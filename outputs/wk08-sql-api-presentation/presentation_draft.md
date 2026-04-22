# Mini DBMS SQL API Server 발표 초안

## 발표 정보

- 예상 발표 시간: 약 4분
- 발표 대상: 수요 코딩회 과제 발표 청중
- 핵심 주장: 기존 C 기반 SQL 처리기와 B+Tree 인덱스를 유지하면서, HTTP API 서버와 thread pool, bounded queue, read/write lock을 추가해 외부 클라이언트가 병렬로 DBMS 기능을 사용할 수 있게 만들었다.

## Slide 1. Mini DBMS를 API 서버로 확장

**화면 문구**

- C 기반 in-memory `users` SQL 엔진
- HTTP API + worker thread pool
- 기존 `sql_execute()`, `Table`, `B+Tree` 재사용

**발표 메모**

이번 과제는 미니 DBMS를 외부에서 사용할 수 있는 API 서버로 만드는 것이었습니다. 저희 구현은 기존에 있던 C 기반 SQL 엔진을 갈아엎지 않고, 그 위에 HTTP 서버 계층을 얹는 방식으로 접근했습니다. 그래서 핵심은 두 가지입니다. 하나는 `src/core/`에 있는 `sql_execute()`, `Table`, `B+Tree`를 그대로 살리는 것, 다른 하나는 `src/server/`에서 요청 큐, worker thread, read/write lock, JSON 응답 계약을 추가하는 것입니다.

## Slide 2. 요구사항과 구현 매핑

**화면 문구**

| 요구사항 | 구현 |
|---|---|
| 외부 클라이언트에서 DBMS 사용 | `GET /health`, `GET /metrics`, `POST /query` |
| SQL 요청 병렬 처리 | bounded queue + worker thread pool |
| 기존 SQL/B+Tree 활용 | `sql_execute()`, `table_find_by_id_condition()` |
| C 언어 구현 | `src/core/*.c`, `src/server/*.c` |
| 품질 검증 | unit test, HTTP smoke, Postman edge/burst |

**발표 메모**

요구사항별로 보면, 외부 클라이언트는 HTTP endpoint로 접근합니다. 실제 SQL 실행은 `POST /query`가 담당하고, 상태 확인은 `/health`, 운영 지표는 `/metrics`로 분리했습니다. 병렬성은 accept loop가 요청을 queue에 넣고, 여러 worker thread가 꺼내 처리하는 구조입니다. 내부 DB 엔진은 기존 SQL 파서와 테이블, B+Tree를 재사용했습니다. 품질 쪽은 단위 테스트와 실제 HTTP smoke 확인, 그리고 edge/burst Postman collection으로 보완했습니다.

## Slide 3. 전체 아키텍처

**화면 문구**

```text
client
  -> accept()
  -> HTTPRequestQueue
  -> worker thread
  -> db_server_execute()
  -> sql_execute()
  -> Table / B+Tree
  -> JSON response
```

**발표 메모**

전체 흐름은 이렇게 볼 수 있습니다. 클라이언트가 HTTP 요청을 보내면 서버는 `accept()`로 연결을 받고, 이 `client_socket`을 bounded queue에 넣습니다. worker thread는 queue에서 socket을 꺼내 HTTP 요청을 읽고 파싱한 다음, `/query`라면 `db_server_execute()`로 넘깁니다. 이 함수가 공유 DB 경계입니다. 여기서 lock과 metrics를 처리하고, 그 안쪽에서 기존 `sql_execute()`가 실제 INSERT나 SELECT를 수행합니다. 결과는 다시 JSON 응답으로 직렬화되어 클라이언트에게 돌아갑니다.

## Slide 4. HTTP API 계약

**화면 문구**

- `GET /health` -> `{"ok":true,"status":"healthy"}`
- `GET /metrics` -> request/select/insert/error counters
- `POST /query` -> SQL 실행
- 성공: `insertedId`, `rowCount`, `usedIndex`
- 실패: `syntax_error`, `query_error`, `queue_full`, `lock_timeout`

**발표 메모**

API는 일부러 작게 잡았습니다. `/health`는 서버가 살아 있는지 보는 endpoint이고, `/metrics`는 요청 수, SELECT/INSERT 수, 오류 수, queue full, lock timeout 같은 운영 지표를 보여줍니다. `/query`는 JSON body의 `query` 문자열을 읽어 SQL을 실행합니다. 성공 응답에는 insert의 경우 `insertedId`, select의 경우 `rowCount`, `rows`, 그리고 `usedIndex`가 들어갑니다. 오류도 문자열만 던지지 않고 `syntax_error`, `query_error`, `queue_full`, `lock_timeout`처럼 분류해서 HTTP status와 함께 내려줍니다.

## Slide 5. Thread Pool과 Backpressure

**화면 문구**

- main thread: listen socket + `accept()`
- queue: fixed-size `HTTPRequestQueue`
- workers: `http_server_worker_main()`
- full queue: `503 queue_full`

**발표 메모**

동시성에서 첫 번째 축은 thread pool입니다. 서버는 요청마다 thread를 새로 만드는 대신, 시작할 때 worker thread들을 만들어 둡니다. main thread는 네트워크 연결을 받고 queue에 넣는 역할에 집중하고, worker들은 queue에서 일을 꺼내 처리합니다. queue는 크기가 정해져 있기 때문에 worker가 감당하지 못할 만큼 요청이 몰리면 무한히 쌓지 않습니다. 대신 `http_request_queue_push()`가 실패하고, 서버는 즉시 `503 queue_full`을 응답합니다. 이 부분은 API 서버에서 backpressure를 드러내는 장치입니다.

## Slide 6. 공유 DB 보호: Read/Write Lock

**화면 문구**

- `SELECT`: read lock
- `INSERT`: write lock
- `src/core/*`: lock-unaware
- lock 대기 초과: `503 lock_timeout`

**발표 메모**

두 번째 축은 공유 DB 보호입니다. 여러 worker가 동시에 같은 in-memory table에 접근하므로 lock이 필요합니다. 여기서 중요한 설계 선택은 core 엔진에 lock을 흩뿌리지 않았다는 점입니다. `src/core/sql.c`, `table.c`, `bptree.c`는 여전히 단일 실행 엔진처럼 동작하고, `src/server/db_server.c`가 그 앞에서 read/write lock을 잡습니다. `SELECT`는 여러 개가 동시에 가능하도록 read lock을 쓰고, `INSERT`는 table과 B+Tree를 수정하므로 write lock을 씁니다. lock을 너무 오래 기다리면 `lock_timeout`으로 실패시켜 서버가 멈춘 것처럼 보이지 않게 했습니다.

## Slide 7. SQL 엔진과 B+Tree 재사용

**화면 문구**

```sql
INSERT INTO users VALUES ('Alice', 20);
SELECT * FROM users WHERE id = 1;
SELECT * FROM users WHERE age >= 20;
```

- `id` 조건: B+Tree primary index
- `name`, `age` 조건: linear scan
- HTTP 응답에 `usedIndex` 노출

**발표 메모**

DB 엔진 쪽은 단일 `users(id, name, age)` 테이블을 다룹니다. insert를 하면 id가 자동 증가하고, B+Tree primary index에도 같이 들어갑니다. select는 현재 `SELECT *` 형태와 단순 WHERE 조건을 지원합니다. 특히 `WHERE id ...` 조건은 B+Tree를 사용하고, `name`과 `age` 조건은 linear scan으로 처리합니다. 그래서 HTTP 응답에 `usedIndex`를 넣어, 이 요청이 인덱스 경로를 탔는지 외부에서도 확인할 수 있게 했습니다.

## Slide 8. 검증 결과와 마무리

**화면 문구**

- `make unit_test server`: up to date
- `./build/bin/unit_test`: `All unit tests passed.`
- HTTP smoke 확인:
  - `/health`: healthy
  - insert: `insertedId=1`
  - indexed select: `usedIndex=true`
  - `/metrics`: totalRequests=4, errors=0
- 비범위: DDL, 다중 테이블, update/delete, persistence, auth/TLS

**발표 메모**

마지막으로 검증입니다. 현재 세션에서 `make unit_test server`는 최신 상태였고, `./build/bin/unit_test`는 `All unit tests passed.`로 통과했습니다. 추가로 로컬 HTTP 서버를 띄워 `/health`, insert, id 기반 select, `/metrics`까지 확인했습니다. metrics에서는 총 4개 요청, query 2개, select 1개, insert 1개, error 0으로 나왔습니다. 이번 구현의 초점은 DBMS 전체 기능을 넓히는 것이 아니라, 작은 SQL 엔진을 외부 API와 동시성 경계로 안전하게 감싸는 것이었습니다. 그래서 DDL, 다중 테이블, update/delete, 영속화, 인증과 TLS는 명시적으로 비범위로 남겼습니다.

## 4분 발표 스크립트

안녕하세요. 저희 팀은 이번 과제인 미니 DBMS API 서버를, 기존 C 기반 SQL 엔진 위에 HTTP 서버 계층을 얹는 방식으로 구현했습니다.

핵심 방향은 기존 엔진을 최대한 살리고, 외부 클라이언트가 접근할 수 있는 서버 경계를 추가하는 것이었습니다. 그래서 `src/core/`는 SQL 파싱과 실행, table 저장소, B+Tree 인덱스를 담당하고, `src/server/`는 HTTP, 요청 큐, worker thread, read/write lock, metrics, JSON 응답을 담당하도록 나눴습니다.

요구사항과 연결해 보면, 외부 클라이언트는 `GET /health`, `GET /metrics`, `POST /query` endpoint를 통해 접근합니다. 실제 SQL 실행은 `/query`가 담당하고, body에는 `query` 문자열이 들어갑니다. 병렬 처리 요구사항은 bounded queue와 worker thread pool로 구현했습니다. main thread는 `accept()`로 새 연결을 받고, `client_socket`을 queue에 넣습니다. worker thread들은 queue에서 socket을 꺼내 HTTP 요청을 읽고 처리합니다.

전체 요청 흐름은 클라이언트에서 시작해 `accept()`, `HTTPRequestQueue`, worker thread, `db_server_execute()`, `sql_execute()`, `Table / B+Tree`, 그리고 JSON response로 이어집니다. 여기서 가장 중요한 경계는 `db_server_execute()`입니다. 이 함수가 여러 worker가 공유하는 table 앞에서 lock과 metrics를 처리하고, 그 안쪽의 기존 SQL 엔진을 호출합니다.

동시성 처리에서는 두 가지를 신경 썼습니다. 첫째, queue 크기를 제한했습니다. 요청이 몰렸을 때 무한히 쌓지 않고, queue가 꽉 차면 `503 queue_full`을 바로 응답합니다. 둘째, 공유 DB에는 read/write lock을 적용했습니다. `SELECT`는 read lock으로 여러 요청이 동시에 읽을 수 있게 하고, `INSERT`는 table과 B+Tree를 수정하므로 write lock으로 보호합니다. lock을 너무 오래 기다리면 `503 lock_timeout`으로 실패시켜 장애 상황도 응답으로 드러나게 했습니다.

DB 엔진 자체는 단일 `users(id, name, age)` 테이블을 다룹니다. `INSERT INTO users VALUES ('Alice', 20);`처럼 insert하면 id가 자동 증가하고 B+Tree primary index에도 들어갑니다. `SELECT * FROM users WHERE id = 1;` 같은 id 조건은 B+Tree를 사용하고, `name`이나 `age` 조건은 linear scan으로 처리합니다. 외부 클라이언트가 이 차이를 확인할 수 있도록 select 응답에는 `usedIndex` 값을 포함했습니다.

API 응답도 발표와 테스트가 쉽도록 명확히 나눴습니다. 성공 시 insert는 `insertedId`, select는 `rowCount`, `rows`, `usedIndex`를 반환합니다. 실패 시에는 `syntax_error`, `query_error`, `queue_full`, `lock_timeout` 같은 상태를 HTTP status와 함께 내려줍니다. `/metrics`에서는 요청 수, SELECT/INSERT 수, 오류 수, queue full과 lock timeout 횟수도 볼 수 있습니다.

검증은 단위 테스트와 실제 HTTP 요청으로 확인했습니다. 현재 세션에서 `make unit_test server`는 최신 상태였고, `./build/bin/unit_test`는 `All unit tests passed.`로 통과했습니다. 또 로컬 서버를 띄운 뒤 `/health`, insert, id 기반 select, `/metrics`를 확인했고, metrics는 총 요청 4개, query 2개, select 1개, insert 1개, error 0으로 나왔습니다.

정리하면 이번 구현은 DBMS 기능을 무작정 넓히기보다, 작은 SQL 엔진을 외부 API 서버로 감싸고, thread pool과 lock으로 병렬 요청을 안전하게 처리하는 데 집중했습니다. DDL, 다중 테이블, update/delete, 영속화, 인증과 TLS는 비범위로 남겼지만, 과제의 핵심인 API 서버 아키텍처, 내부 DB 엔진 연결, 동시성 이슈, 검증 가능한 품질 요구사항은 현재 코드베이스 안에서 충족하도록 구성했습니다.
