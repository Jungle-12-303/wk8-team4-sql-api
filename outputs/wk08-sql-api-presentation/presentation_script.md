# Mini DBMS SQL API Server 4분 발표 스크립트

## 시간 배분

| 구간 | 내용 | 예상 |
|---|---|---:|
| 1 | 문제와 구현 방향 | 25초 |
| 2 | 요구사항 매핑 | 35초 |
| 3 | 전체 아키텍처 | 40초 |
| 4 | API 계약 | 30초 |
| 5 | thread pool / queue | 40초 |
| 6 | read/write lock | 35초 |
| 7 | SQL 엔진과 B+Tree | 35초 |
| 8 | 검증과 마무리 | 40초 |

## 최종 스크립트

안녕하세요. 이번 과제는 기존 미니 DBMS를 외부 클라이언트가 사용할 수 있는 API 서버로 확장하는 것이었습니다. 저희 구현의 핵심은 기존 C 기반 SQL 엔진을 갈아엎지 않고, 그 앞에 HTTP 서버와 동시성 경계를 세운 것입니다.

구조는 크게 두 영역입니다. `src/core/`는 `sql_execute()`, `Table`, `B+Tree`처럼 SQL 처리와 저장소를 담당합니다. `src/server/`는 HTTP 요청 처리, bounded queue, worker thread pool, read/write lock, metrics, JSON 응답을 담당합니다. 그래서 과제 요구사항인 기존 SQL 처리기와 B+Tree 재사용, 외부 API 제공, 병렬 요청 처리를 각각 코드의 책임 경계로 나눠 설명할 수 있습니다.

외부 클라이언트는 세 endpoint를 사용합니다. `GET /health`는 서버 상태, `GET /metrics`는 요청과 오류 지표, `POST /query`는 SQL 실행입니다. 예를 들어 `POST /query` body에 `{"query":"SELECT * FROM users WHERE id = 1;"}`를 보내면, 응답에는 `rowCount`, `rows`, `usedIndex`가 포함됩니다.

전체 요청 흐름은 `client -> accept() -> HTTPRequestQueue -> worker thread -> db_server_execute() -> sql_execute() -> Table / B+Tree -> JSON response`입니다. main thread는 연결을 받고 `client_socket`을 queue에 넣습니다. worker thread들은 미리 만들어져 있고, queue에서 socket을 꺼내 요청을 읽고 처리합니다. 즉 요청마다 새 thread를 만드는 방식이 아니라, thread pool이 요청을 배정받는 구조입니다.

요청이 몰릴 때도 무한히 쌓지 않습니다. queue 크기가 정해져 있어서 `http_request_queue_push()`가 실패하면 바로 `503 queue_full`을 응답합니다. 이 부분은 서버가 감당 가능한 처리량을 넘었음을 클라이언트에게 명확히 알려주는 backpressure 역할을 합니다.

DB 일관성은 `src/server/db_server.c`의 `db_server_execute()`에서 제어합니다. HTTP 요청 처리는 병렬로 진행되지만, 공유 in-memory table에 접근할 때는 read/write lock을 잡습니다. `SELECT`는 read lock이라 여러 읽기가 동시에 가능하고, `INSERT`는 table과 B+Tree를 수정하므로 write lock을 사용합니다. lock을 오래 기다리면 `503 lock_timeout`으로 실패시켜 서버가 멈춘 것처럼 보이지 않게 했습니다.

DB 엔진은 단일 `users(id, name, age)` 테이블을 다룹니다. `INSERT INTO users VALUES ('Alice', 20);`를 실행하면 id가 자동 증가하고 B+Tree primary index에도 저장됩니다. `WHERE id` 조건은 B+Tree 경로를 사용하고, `name`과 `age` 조건은 linear scan으로 처리합니다. 여기서 `usedIndex`는 범용 실행 계획 분석기가 아니라, 현재 지원하는 SQL 범위에서 id 인덱스 경로를 탔는지 보여주는 표시입니다.

검증도 진행했습니다. 현재 세션에서 `make unit_test server`는 최신 상태였고, `./build/bin/unit_test`는 `All unit tests passed.`로 통과했습니다. 또 로컬 HTTP 서버를 띄워 `/health`, insert, id 기반 select, `/metrics`를 확인했고, metrics는 총 요청 4개, query 2개, select 1개, insert 1개, error 0으로 나왔습니다.

정리하면 이번 구현은 DBMS 기능을 무작정 넓히기보다, 작은 SQL 엔진을 외부 API와 동시성 경계로 안전하게 감싸는 데 집중했습니다. DDL, 다중 테이블, update/delete, 영속화, 인증과 TLS는 비범위로 남겼지만, 과제의 핵심인 API 서버 아키텍처, 내부 DB 엔진 연결, 멀티스레드 동시성, 검증 가능한 품질은 현재 코드베이스 안에서 충족했습니다.

## 세 관점 리뷰 반영 요약

- 세 관점 모두 요구사항 매핑, 아키텍처 흐름, 검증 근거는 충분하다고 봤습니다.
- 공통 보완점은 “HTTP 요청 처리는 병렬화하지만 공유 DB 접근은 read/write lock으로 제어한다”는 표현을 더 명확히 하는 것이었습니다.
- 초보자 관점에서는 `bounded queue`, `backpressure`, `usedIndex`가 헷갈릴 수 있어, 최종 스크립트에서는 실제 요청 예시와 `usedIndex`의 의미를 풀어 설명했습니다.
- 시니어 관점에서는 Postman collection은 “준비/포함”과 “실제 실행”을 구분하라는 조언이 있어, 최종 스크립트의 검증 결과는 이번 세션에서 실제 확인한 unit test와 HTTP smoke 중심으로 정리했습니다.
