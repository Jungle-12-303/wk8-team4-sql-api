# Mini DBMS SQL API Server

C 기반 in-memory `users` SQL 엔진을 HTTP API 서버로 감싼 프로젝트입니다. 발표의 핵심은 SQL 파서 자체가 아니라, **하나의 공유 DB를 여러 HTTP 요청이 동시에 사용할 때 서버가 어떻게 안전하게 받고, 줄 세우고, 실행하고, 실패 신호를 돌려주는가**입니다.

기존 `src/core/`의 `sql_execute()`, `Table`, `B+Tree`는 재사용했습니다. 새로 추가한 중심은 `src/server/`의 HTTP endpoint, bounded request queue, worker thread pool, read/write lock wrapper, timeout, backpressure, metrics, JSON 응답입니다.

## 핵심 메시지

| 발표 포인트 | 구현 요약 |
|---|---|
| 외부 클라이언트 접근 | `GET /health`, `GET /metrics`, `POST /query` |
| 동시 요청 처리 | accept loop가 socket을 받고 bounded queue에 적재, worker thread pool이 소비 |
| 공유 DB 보호 | `SELECT`는 read 경로, `INSERT`는 write 경로로 분류해 read/write lock wrapper 사용 |
| 공정성/대기 제어 | phase gate 상태로 reader/writer 진입 순서를 관리하고, try-lock 반복 루프에서 timeout 계산 |
| Backpressure | queue가 가득 차면 worker에 넘기지 않고 즉시 `503 queue_full` |
| 운영 관찰 | `/metrics`에서 요청 수, 오류 수, queue full, lock timeout, active query 확인 |
| 검증 초점 | API 계약, 동시 read/write, queue 포화, lock timeout, metrics 누적 |

## 아키텍처 경계

```text
src/server/
  HTTP server
    accept loop + fixed worker pool + bounded request queue

  API boundary
    HTTP parsing -> DB execution result -> JSON/HTTP response

  DB server boundary
    shared Table 보호, read/write lock wrapper, timeout, metrics

src/core/
  기존 SQL engine
    sql_execute(), Table, B+Tree
```

`src/core/`는 lock, socket, HTTP를 모릅니다. 동시성 정책은 `src/server/db_server.c`에 모아 두었습니다. 덕분에 기존 CLI와 SQL 엔진은 유지하고, 외부 API 서버의 병렬 처리와 운영 신호만 별도 레이어에서 검증할 수 있습니다.

## 동시성 제어

공유 자원은 하나의 in-memory `Table *`입니다. 여러 worker가 동시에 접근하면 다음 문제가 생길 수 있습니다.

| 상황 | 위험 |
|---|---|
| `INSERT` 중 `SELECT` | 반쯤 갱신된 record나 index를 읽을 수 있음 |
| `INSERT` / `INSERT` 동시 실행 | 같은 `next_id`를 읽거나 B+Tree 갱신이 충돌할 수 있음 |
| B+Tree 갱신 중 조회 | node split 중인 구조를 다른 worker가 따라갈 수 있음 |

그래서 서버는 SQL을 먼저 read/write 성격으로 분류합니다.

| SQL | 동시성 정책 | 결과 |
|---|---|---|
| `SELECT` | read lock 경로 | 다른 `SELECT`와 함께 실행 가능 |
| `INSERT` | write lock 경로 | 테이블 변경은 한 번에 하나씩 실행 |
| 기타/오류 SQL | lock 없이 core가 오류 판정 | syntax/query error를 JSON으로 반환 |

실제 구현은 OS lock의 blocking/timed 호출에 바로 맡기는 구조가 아닙니다. `db_server_execute()`에서 phase gate 상태를 보고, `platform_rwlock_try_read_lock()` 또는 `platform_rwlock_try_write_lock()`을 자체 대기 루프에서 반복 시도합니다. 루프 안에서 경과 시간을 계산해 `--lock-timeout-ms`를 넘으면 SQL 실행 없이 `503 lock_timeout`으로 끝냅니다.

## 요청이 몰릴 때의 서버 반응

| 압박 지점 | 서버 반응 | 클라이언트가 받는 신호 |
|---|---|---|
| queue 여유 있음 | socket을 queue에 넣고 worker가 처리 | 정상 응답 또는 SQL/API 오류 |
| queue 포화 | accept loop가 즉시 실패 응답 | `503 queue_full` |
| read/write lock 대기 길어짐 | try-lock 루프에서 timeout 계산 | `503 lock_timeout` |
| 정상 SELECT | 여러 reader가 함께 core 실행 가능 | `200`, `action: "select"` |
| 정상 INSERT | writer 하나만 core 실행 | `200`, `action: "insert"` |

동시에 도착한 요청의 응답 순서는 고정되어 있지 않습니다. 다만 공유 DB에 들어가는 순간에는 read/write lock wrapper와 phase gate가 `SELECT` 동시 읽기, `INSERT` 단독 쓰기, timeout 실패를 명확히 나눕니다.

```mermaid
sequenceDiagram
    autonumber
    participant Alice as "Alice (SELECT)"
    participant Bob as "Bob (INSERT)"
    participant WA as "worker A"
    participant WB as "worker B"
    participant DB as "db_server_execute()"
    participant Core as "sql_execute()"

    par worker 단계
        Alice->>WA: POST /query
        WA->>WA: HTTP parse
    and
        Bob->>WB: POST /query
        WB->>WB: HTTP parse
    end

    WA->>DB: execute SELECT
    DB->>DB: read lock 획득
    WB->>DB: execute INSERT
    Note over WB,DB: write lock은 read lock 해제까지 대기
    DB->>Core: SELECT 실행
    Core-->>DB: SQLResult
    DB-->>WA: DBServerExecution
    WA-->>Alice: HTTP 200 JSON
    DB->>DB: read lock 해제

    DB->>DB: write lock 획득
    DB->>Core: INSERT 실행
    Core-->>DB: SQLResult
    DB-->>WB: DBServerExecution
    WB-->>Bob: HTTP 200 JSON
```

## 핵심 경계 함수

발표 중 코드로 연결해서 볼 함수는 이 정도면 충분합니다.

| 경계 | 함수 | 역할 |
|---|---|---|
| HTTP 서버 시작 | `http_server_run()` | listen socket, worker pool, queue 초기화 |
| 요청 파싱 | `api_parse_http_request()` | HTTP raw request에서 method/path/query 추출 |
| 공유 DB 진입 | `db_server_execute()` | SQL 분류, lock 대기, timeout, metrics, core 호출 |
| 기존 SQL 실행 | `sql_execute()` | 기존 `Table`과 B+Tree를 이용해 SQL 처리 |
| API 응답 생성 | `api_build_execution_response()` | 실행 결과를 JSON body로 변환 |
| HTTP 응답 렌더링 | `api_render_http_response()` | status line/header/body 문자열 생성 |

## HTTP API

모든 응답 body는 JSON이고 `Content-Type: application/json; charset=utf-8`을 사용합니다.

| Endpoint | 용도 |
|---|---|
| `GET /health` | 서버 생존 확인 |
| `GET /metrics` | 요청/오류/queue/timeout/active query 관찰 |
| `POST /query` | SQL 실행 |

`POST /query` 요청 예시:

```json
{"query":"SELECT * FROM users WHERE id = 1;"}
```

성공 응답 예시:

```json
{"ok":true,"status":"ok","action":"select","rowCount":1,"usedIndex":true,"rows":[{"id":1,"name":"Alice","age":20}]}
```

대표 오류:

| 오류 | HTTP status | 의미 |
|---|---:|---|
| `syntax_error` | `400` | SQL 문법 오류 |
| `query_error` | `400` | 지원하지 않는 SQL 동작 또는 컬럼 |
| `malformed_http` | `400` | HTTP/JSON 요청 형식 오류 |
| `method_not_allowed` | `405` | endpoint와 맞지 않는 method |
| `not_found` | `404` | 지원하지 않는 path |
| `queue_full` | `503` | request queue 포화 |
| `lock_timeout` | `503` | DB lock 대기 초과 |
| `internal_error` | `500` | 내부 처리 오류 |

## 지원 SQL 범위

발표에서는 SQL core를 새로 만든 부분으로 강조하지 않습니다. 이 프로젝트에서는 기존 엔진을 아래 범위로 재사용합니다.

```sql
INSERT INTO users VALUES ('Alice', 20);

SELECT * FROM users;
SELECT * FROM users WHERE id = 1;
SELECT * FROM users WHERE id >= 10;
SELECT * FROM users WHERE name = 'Alice';
SELECT * FROM users WHERE age <= 20;
```

- 테이블은 `users(id, name, age)` 하나입니다.
- `id`는 자동 증가 primary key이며 `id` 조건 조회는 B+Tree index를 사용합니다.
- `name`, `age` 조건은 linear scan입니다.
- HTTP에서는 `EXIT`, `QUIT`를 실행 명령으로 받지 않고 `400 query_error`로 처리합니다.

## 빌드와 실행

```bash
make
./build/bin/server --serve --port 8080 --workers 4 --queue 16
```

발표용 확인은 아래 요청이면 충분합니다.

```bash
curl http://127.0.0.1:8080/health

curl -X POST http://127.0.0.1:8080/query \
  -H "Content-Type: application/json" \
  -d "{\"query\":\"INSERT INTO users VALUES ('Alice', 20);\"}"

curl -X POST http://127.0.0.1:8080/query \
  -H "Content-Type: application/json" \
  -d "{\"query\":\"SELECT * FROM users WHERE id = 1;\"}"

curl http://127.0.0.1:8080/metrics
```

주요 서버 옵션:

| 옵션 | 기본값 | 설명 |
|---|---:|---|
| `--workers <n>` | `4` | worker thread 수 |
| `--queue <n>` | `16` | request queue 크기 |
| `--lock-timeout-ms <ms>` | `1000` | DB lock 대기 상한 |
| `--simulate-read-delay-ms <ms>` | `0` | 검증용 read 지연 |
| `--simulate-write-delay-ms <ms>` | `0` | 검증용 write 지연 |
| `--max-requests <n>` | `0` | 지정 응답 수 이후 종료 |

## 검증 요약

| 검증 항목 | 확인한 내용 |
|---|---|
| Unit test | SQL 실행 결과, B+Tree 조회, API parser/response builder, `db_server_execute()` metrics |
| 동시 SELECT | 여러 reader가 동시에 read 경로로 진입하고 결과가 유지되는지 확인 |
| read/write 충돌 | writer 대기, reader batch, phase 전환이 깨지지 않는지 확인 |
| lock timeout | lock 대기가 상한을 넘으면 `503 lock_timeout`과 metrics가 기록되는지 확인 |
| HTTP smoke | `/health`, INSERT, indexed SELECT, syntax error, `/metrics` counter 확인 |
| Backpressure | 작은 queue와 지연 옵션으로 `503 queue_full` 응답 확인 |
| Postman collection | 정상 API 계약, edge case, burst 요청 후 metrics 안정성 확인 시나리오 구성 |

실행 명령:

```bash
make unit_test
./build/bin/unit_test
```

Windows smoke test:

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\smoke\server_http_smoke_test.ps1
```

## 프로젝트 구조

```text
src/
  core/      기존 SQL parser/executor, Table, B+Tree
  server/    HTTP server, API response builder, DB concurrency boundary
  cli/       기존 REPL
tests/
  unit/      core/server boundary 단위 테스트
  smoke/     HTTP smoke와 Postman collection
benchmarks/ 성능 확인용 benchmark target
docs/       추가 설계/검증 참고 문서
```

### macOS / Ubuntu 테스트 실행

macOS와 Ubuntu에서는 PowerShell smoke script 대신 `make`, `unit_test`, `curl`, Postman collection 조합으로 같은 기본 API 계약을 확인할 수 있습니다.

1. 빌드

```bash
make unit_test server
```

2. unit test 실행

```bash
./build/bin/unit_test
```

3. HTTP 서버 실행

```bash
./build/bin/server --serve --port 8080 --workers 2 --queue 4
```

4. 다른 터미널에서 `curl` 예시 또는 아래 Postman collection으로 smoke 확인

```bash
curl http://127.0.0.1:8080/health
curl http://127.0.0.1:8080/metrics
curl -X POST http://127.0.0.1:8080/query \
  -H "Content-Type: application/json" \
  -d "{\"query\":\"INSERT INTO users VALUES ('Alice', 20);\"}"
```

즉, macOS와 Ubuntu에서는 현재 PowerShell 스크립트 대신 `make`, `unit_test`, `curl`, Postman 조합으로 같은 기본 경로를 확인하는 방식입니다.

### Postman smoke collection

Postman으로도 같은 기본 HTTP 계약을 빠르게 확인할 수 있습니다.

1. 먼저 서버를 실행합니다.

```bash
./build/bin/server --serve --port 8080 --workers 2 --queue 4
```

2. [tests/smoke/server_http_smoke_test.postman_collection.json](./tests/smoke/server_http_smoke_test.postman_collection.json)을 Postman에 import 합니다.
3. 컬렉션 변수 `baseUrl`이 `http://127.0.0.1:8080`인지 확인합니다.
4. 컬렉션 전체를 위에서 아래 순서대로 실행합니다.

각 항목이 보는 의미는 아래와 같습니다.

| 항목 | 검증 의미 |
| --- | --- |
| `GET /health` | 서버가 살아 있고 HTTP 라우팅이 정상인지 확인 |
| `POST /query` INSERT | JSON body 파싱, 쓰기 경로, INSERT 응답 계약 확인 |
| `POST /query` indexed SELECT | 조회 성공과 `usedIndex: true` 인덱스 경로 확인 |
| 빈 SELECT 결과 | 결과가 없어도 `200`과 `rows: []`로 안정적으로 응답하는지 확인 |
| syntax error 응답 | 잘못된 SQL이 `400 syntax_error`로 매핑되는지 확인 |
| metrics delta 검증 | health/query/error/not-found 카운터가 실제 요청 흐름에 맞게 증가하는지 확인 |

즉 이 Postman smoke collection은 "기본 기능이 실제 클라이언트 요청 기준으로 끝까지 동작하는지"를 검증하는 빠른 end-to-end 체크입니다.

`queue_full`처럼 동시 요청이 필요한 시나리오는 Postman 컬렉션보다 기존 `tests/smoke/server_http_smoke_test.ps1`가 더 적합합니다.

### Postman edge and burst collection

우선순위 edge case와 가벼운 burst 시나리오를 Postman에서 바로 돌리고 싶다면 아래 고급 컬렉션을 사용하면 됩니다.

1. 서버를 아래처럼 실행합니다.

```bash
./build/bin/server --serve --port 8080 --workers 4 --queue 64 \
  --lock-timeout-ms 5000 \
  --simulate-read-delay-ms 40 \
  --simulate-write-delay-ms 100
```

2. [tests/smoke/server_http_edge_burst_test.postman_collection.json](./tests/smoke/server_http_edge_burst_test.postman_collection.json)을 Postman에 import 합니다.
3. 컬렉션 변수 `baseUrl`이 서버 주소와 맞는지 확인합니다.
4. `01 Edge Cases` 폴더를 먼저 실행하고, 이어서 `02 Burst Tests` 폴더를 실행합니다.

각 항목이 보는 의미는 아래와 같습니다.

| 항목 | 검증 의미 |
| --- | --- |
| `GET /does-not-exist`의 `404 not_found` | 존재하지 않는 경로가 올바른 routing error로 분리되는지 확인 |
| `POST /health`, `GET /query`의 `405 method_not_allowed` | 경로는 맞지만 HTTP 메서드가 틀렸을 때 메서드 오류로 정확히 매핑되는지 확인 |
| 빈 body 또는 잘못된 `query` 타입의 `400 malformed_http` | `POST /query`의 body 계약이 깨졌을 때 SQL 실행 전에 요청 자체를 거절하는지 확인 |
| HTTP API에서 `EXIT`, `QUIT`를 거절하는 `400 query_error` | CLI 전용 명령이 HTTP 경계에서는 질의 오류로 안전하게 차단되는지 확인 |
| routing error와 `malformed_http`, `EXIT`/`QUIT` metrics 분리 | 라우팅/파싱 오류는 DB query metrics에 섞이지 않고, 실제 쿼리로 들어간 `EXIT`/`QUIT`만 query metrics에 잡히는지 확인 |
| `pm.sendRequest` fan-out read-only burst | 여러 읽기 요청을 동시에 보내도 read 경로가 안정적으로 응답하는지 확인 |
| mixed 80/20 burst | read 위주 트래픽 속에 일부 write가 섞였을 때도 응답 계약과 metrics가 무너지지 않는지 확인 |
| write-heavy burst | 여러 INSERT가 동시에 들어와도 쓰기 경로가 깨지지 않고 결과가 일관되게 내려오는지 확인 |

즉 이 Postman edge and burst collection은 기본 smoke test보다 한 단계 더 나아가, 라우팅/메서드/body 계약의 edge case와 가벼운 동시 burst 상황에서 API 계약과 metrics 분리가 유지되는지를 빠르게 검증하는 테스트입니다.

burst 시나리오는 Postman request 하나가 내부에서 여러 `pm.sendRequest`를 병렬로 보내는 방식입니다.
그래서 metrics 비교가 흔들리지 않도록 각 burst 직전마다 baseline metrics를 다시 저장합니다.

Postman은 request line이나 header framing 자체가 깨진 "raw malformed HTTP"를 만들기에는 적합하지 않습니다.
이 컬렉션의 `malformed_http` 검증은 `POST /query`의 body/query 계약 실패를 통해 확인합니다.
완전히 깨진 raw HTTP 헤더/바디 framing은 기존 PowerShell smoke나 수동 socket 테스트가 더 적합합니다.

## 현재 확인 상태

- `make` 전체 빌드는 현재 작업 세션에서 통과 확인
- `build/bin/unit_test`는 현재 작업 세션에서 통과 확인
- `make benchmarks`로 `benchmarks/perf10.c`, `benchmarks/cond10.c` 빌드 확인
- `build/bin/server --query ...` CLI 하네스 기본 흐름 확인
- `tests/smoke/server_cli_smoke_test.ps1`, `tests/smoke/server_http_smoke_test.ps1`, `tests/smoke/server_http_smoke_test.postman_collection.json`, `tests/smoke/server_http_edge_burst_test.postman_collection.json`는 저장소에 포함
- PowerShell smoke test는 현재 Windows/PowerShell 환경용 검증 경로입니다
- macOS와 Ubuntu에서는 `make`, `build/bin/unit_test`, `curl` 또는 Postman collection으로 같은 기본 API 계약과 edge/burst 시나리오를 확인할 수 있습니다

기본 `make`는 `perf_test`, `condition_perf_test`도 함께 빌드합니다.
## 비범위

- 다중 테이블, DDL, `UPDATE`, `DELETE`
- join, aggregate, order by
- 영속화, transaction, WAL
- TLS, auth, 인터넷 배포

이 프로젝트의 목표는 새 DBMS 전체를 만드는 것이 아닙니다. 작은 SQL 엔진을 HTTP API 서버로 노출했을 때 필요한 **thread, queue, lock, timeout, backpressure, metrics** 경계를 구현하고 검증하는 것입니다.
