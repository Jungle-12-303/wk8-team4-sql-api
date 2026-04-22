# Mini DBMS SQL API Server

`users` 단일 테이블을 다루는 C 기반 미니 SQL 엔진에 CLI 하네스와 HTTP API 서버를 얹은 프로젝트입니다.

핵심 엔진 경계는 `sql_execute(Table *, const char *)`와 `SQLResult`입니다. `src/core/`는 SQL 파싱/실행, 테이블 저장소, B+Tree 인덱스만 책임지고, 서버 계층인 `src/server/`가 shared table, 요청 큐, worker thread, rwlock, metrics, JSON 응답 계약을 담당합니다.

## 핵심 포인트

- 단일 in-memory `users(id, name, age)` 테이블
- `id` 자동 증가와 B+Tree primary index
- `INSERT`, `SELECT *`, 단순 `WHERE` 조건 지원
- `SELECT`는 read lock, `INSERT`는 write lock 아래에서 실행
- HTTP 엔드포인트: `GET /health`, `GET /metrics`, `POST /query`
- 운영 시그널: `usedIndex`, `queue_full`, `lock_timeout`, metrics
- 단위 테스트, PowerShell smoke test, benchmark 타깃 포함

## 프로젝트 구조

```text
src/
  cli/       기존 REPL 엔트리포인트
  core/      SQL 파서/실행기, table 저장소, B+Tree
  server/    HTTP 서버, API 직렬화, shared DB 서버 경계, platform wrapper
tests/
  unit/      엔진과 서버 경계 단위 테스트
  smoke/     Windows PowerShell 기반 CLI/HTTP smoke test
benchmarks/ 성능 확인용 벤치마크
docs/       설계 문서, 다이어그램, 검증 가이드
```

요청 흐름은 아래 구조입니다.

```text
client
  -> socket accept
  -> bounded request queue
  -> worker thread
  -> db_server_execute()
  -> sql_execute()
  -> table / bptree
  -> JSON response
```

![Project request sequence](./docs/images/server-request-sequence.svg)

## 지원 SQL

세미콜론은 선택 사항이고, 키워드/테이블명/컬럼명은 대소문자를 구분하지 않습니다.

- `src/core/`: SQL 파싱/실행, table 저장소, B+Tree 인덱스
- `src/server/`: CLI 하네스, HTTP 서버, API 직렬화, shared DB 서버 경계, 플랫폼 래퍼
- `src/cli/main.c`: 기존 REPL 엔트리포인트
- `tests/unit/unit_test.c`: 엔진과 서버 경계 단위 테스트
- `tests/smoke/`: PowerShell 기반 CLI/HTTP smoke test, Postman collection
- `benchmarks/`: 성능 확인용 벤치마크
- `docs/`: 설계 문서, 다이어그램, 발표/검증 가이드

```sql
INSERT INTO users VALUES ('Alice', 20);
```

- `id`는 1부터 자동 증가합니다.
- 문자열은 작은따옴표 기반 단순 문자열만 지원합니다.

### SELECT

```sql
SELECT * FROM users;
SELECT * FROM users WHERE id = 1;
SELECT * FROM users WHERE id >= 10;
SELECT * FROM users WHERE name = 'Alice';
SELECT * FROM users WHERE age = 20;
SELECT * FROM users WHERE age > 20;
SELECT * FROM users WHERE age <= 20;
```

- 현재 select list는 `*`만 지원합니다.
- `id` 조건은 `=`, `<`, `<=`, `>`, `>=`를 지원하고 B+Tree 인덱스를 사용합니다.
- `age` 조건은 `=`, `<`, `<=`, `>`, `>=`를 지원하고 linear scan으로 찾습니다.
- `name` 조건은 `=`만 지원하고 linear scan으로 찾습니다.
- 존재하지 않는 컬럼은 `query_error`로 분류됩니다.

### 종료 명령

```sql
EXIT
QUIT
```

- CLI/REPL에서는 종료 명령으로 처리합니다.
- HTTP API에서는 `400 query_error`로 거절합니다.

## 동시성 모델

`src/core/sql.c`, `src/core/table.c`, `src/core/bptree.c`는 lock-unaware 상태로 유지합니다. 동기화는 `src/server/db_server.c`의 shared DB 서버 경계에서만 수행합니다.

1. `SELECT`는 read lock을 획득한 뒤 실행합니다.
2. `INSERT`는 write lock을 획득한 뒤 실행합니다.
3. lock 대기 시간이 `--lock-timeout-ms`를 넘으면 `lock_timeout`으로 실패합니다.
4. HTTP 서버는 bounded queue와 worker thread pool로 요청을 처리합니다.
5. queue가 가득 차면 즉시 `503 queue_full` 응답을 반환합니다.

## 빌드

### 개발 도구 준비

#### macOS

먼저 Xcode Command Line Tools를 설치합니다.

```bash
xcode-select --install
```

기본 `clang`, `make`, `curl`로 바로 빌드할 수 있습니다.

#### Ubuntu

필수 빌드 도구와 `curl`을 설치합니다.

```bash
sudo apt update
sudo apt install -y build-essential curl
```

#### Windows MinGW

MinGW shell 또는 PowerShell에서 `gcc`, `mingw32-make`를 사용할 수 있어야 합니다.

```bash
make
```

macOS와 Ubuntu에서는 위 명령을 그대로 사용하면 됩니다.  
Windows MinGW 환경에서 `make`가 없으면 `mingw32-make`를 사용하면 됩니다.

Makefile을 쓰는 것을 권장하지만, 직접 컴파일도 가능합니다.

POSIX/macOS/Linux:

### macOS / Ubuntu 직접 빌드

```bash
mkdir -p build/bin
cc -std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_XOPEN_SOURCE=700 -pthread -Isrc/core -Isrc/server -o build/bin/unit_test tests/unit/unit_test.c src/server/db_server.c src/server/api.c src/server/platform.c src/core/bptree.c src/core/table.c src/core/sql.c
cc -std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_XOPEN_SOURCE=700 -pthread -Isrc/core -Isrc/server -o build/bin/server src/server/server.c src/server/http_server.c src/server/db_server.c src/server/api.c src/server/platform.c src/core/bptree.c src/core/table.c src/core/sql.c
cc -std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_XOPEN_SOURCE=700 -pthread -Isrc/core -Isrc/server -o build/bin/main src/cli/main.c src/core/bptree.c src/core/table.c src/core/sql.c
```

### Windows MinGW 직접 빌드

```bash
mkdir -p build/bin
gcc -std=c11 -Wall -Wextra -Werror -pedantic -O2 -pthread -Isrc/core -Isrc/server -o build/bin/unit_test tests/unit/unit_test.c src/server/db_server.c src/server/api.c src/server/platform.c src/core/bptree.c src/core/table.c src/core/sql.c
gcc -std=c11 -Wall -Wextra -Werror -pedantic -O2 -pthread -Isrc/core -Isrc/server -o build/bin/server src/server/server.c src/server/http_server.c src/server/db_server.c src/server/api.c src/server/platform.c src/core/bptree.c src/core/table.c src/core/sql.c
```

Windows MinGW:

```powershell
mkdir build\bin
gcc -std=c11 -Wall -Wextra -Werror -pedantic -O2 -Isrc/core -Isrc/server -o build/bin/unit_test.exe tests/unit/unit_test.c src/server/db_server.c src/server/api.c src/server/platform.c src/core/bptree.c src/core/table.c src/core/sql.c
gcc -std=c11 -Wall -Wextra -Werror -pedantic -O2 -Isrc/core -Isrc/server -o build/bin/server.exe src/server/server.c src/server/http_server.c src/server/db_server.c src/server/api.c src/server/platform.c src/core/bptree.c src/core/table.c src/core/sql.c -lws2_32
gcc -std=c11 -Wall -Wextra -Werror -pedantic -O2 -Isrc/core -Isrc/server -o build/bin/main.exe src/cli/main.c src/core/bptree.c src/core/table.c src/core/sql.c
```

## 실행

macOS와 Ubuntu에서는 아래 명령을 그대로 사용하면 됩니다.  
Windows MinGW에서는 같은 경로에 `.exe`를 붙여 실행하면 됩니다.

### 1. 기존 REPL

```bash
./build/bin/main
```

Windows:

```powershell
.\build\bin\main.exe
```

### 서버 CLI 하네스

`server` 바이너리는 같은 shared table 상태에 여러 SQL을 순서대로 실행할 수 있습니다.

```bash
./build/bin/server \
  --query "INSERT INTO users VALUES ('Alice', 20);" \
  --query "SELECT * FROM users WHERE id = 1;" \
  --query "QUIT"
```

인자 없이 실행하면 stdin에서 한 줄씩 SQL을 입력받습니다.

```bash
./build/bin/server
```

대표 출력:

```text
OK INSERT id=1 used_index=false
OK SELECT rows=1 used_index=true
ROW id=1 name=Alice age=20
BYE
```

### HTTP 서버

```bash
./build/bin/server --serve --port 8080 --workers 4 --queue 16
```

옵션:

- `--port <n>`: listen port, 기본값 `8080`
- `--workers <n>`: worker thread 수, 기본값 `4`
- `--queue <n>`: bounded request queue 크기, 기본값 `16`
- `--lock-timeout-ms <ms>`: DB lock 대기 timeout, 기본값 `1000`
- `--simulate-read-delay-ms <ms>`: 테스트용 read 지연, 기본값 `0`
- `--simulate-write-delay-ms <ms>`: 테스트용 write 지연, 기본값 `0`
- `--max-requests <n>`: 지정한 응답 수를 완료하면 서버 종료, 기본값 `0`으로 비활성화

`--serve`와 `--query`는 함께 사용할 수 없습니다.

## HTTP API

모든 HTTP 응답 body는 JSON이며 `Content-Type: application/json; charset=utf-8`을 사용합니다. 연결은 응답 후 닫힙니다.

### `GET /health`

```json
{"ok":true,"status":"healthy"}
```

### `GET /metrics`

```json
{
  "ok": true,
  "status": "ok",
  "metrics": {
    "totalRequests": 5,
    "totalHealthRequests": 1,
    "totalMetricsRequests": 1,
    "totalQueryRequests": 3,
    "totalSelectRequests": 2,
    "totalInsertRequests": 1,
    "totalErrors": 0,
    "totalSyntaxErrors": 0,
    "totalQueryErrors": 0,
    "totalInternalErrors": 0,
    "totalNotFoundResults": 0,
    "totalQueueFull": 0,
    "totalLockTimeouts": 0,
    "activeQueryRequests": 0
  }
}
```

metrics는 DB 서버 경계에서 집계되는 요청/쿼리/queue 지표입니다. routing 단계에서 발생한 `not_found`, `method_not_allowed`, `malformed_http`는 현재 metrics에 포함하지 않고, `queue_full`은 포함합니다.

### `POST /query`

요청 body:

```json
{
  "query": "SELECT * FROM users WHERE id = 1;"
}
```

`query`는 JSON 문자열이어야 하며 최대 1023자까지 저장됩니다.

SELECT 성공:

```json
{
  "ok": true,
  "status": "ok",
  "action": "select",
  "rowCount": 1,
  "usedIndex": true,
  "rows": [
    { "id": 1, "name": "Alice", "age": 20 }
  ]
}
```

빈 조회도 HTTP 관점에서는 성공입니다.

```json
{
  "ok": true,
  "status": "ok",
  "action": "select",
  "rowCount": 0,
  "usedIndex": true,
  "rows": []
}
```

INSERT 성공:

```json
{
  "ok": true,
  "status": "ok",
  "action": "insert",
  "insertedId": 1,
  "usedIndex": false
}
```

오류 응답:

```json
{
  "ok": false,
  "status": "syntax_error",
  "error": "syntax_error",
  "message": "..."
}
```

대표 오류:

- `syntax_error`: SQL 문법 오류, HTTP `400`
- `query_error`: 존재하지 않는 컬럼, HTTP에서의 `EXIT`/`QUIT` 등 질의 오류, HTTP `400`
- `internal_error`: 내부 실행/직렬화 실패, HTTP `500`
- `lock_timeout`: DB lock 대기 timeout, HTTP `503`
- `queue_full`: worker queue 포화, HTTP `503`
- `malformed_http`: 잘못된 request line/header/body 또는 `POST /query` JSON body 오류, HTTP `400`
- `method_not_allowed`: 잘못된 HTTP 메서드, HTTP `405`
- `not_found`: 지원하지 않는 경로, HTTP `404`

## curl 예시

```bash
curl http://127.0.0.1:8080/health
```

```bash
curl http://127.0.0.1:8080/metrics
```

```bash
curl -X POST http://127.0.0.1:8080/query \
  -H "Content-Type: application/json" \
  -d "{\"query\":\"INSERT INTO users VALUES ('Alice', 20);\"}"
```

```bash
curl -X POST http://127.0.0.1:8080/query \
  -H "Content-Type: application/json" \
  -d "{\"query\":\"SELECT * FROM users WHERE id = 1;\"}"
```

## 테스트

### Unit test

```bash
make unit_test
./build/bin/unit_test
```

Windows MinGW에서는 `./build/bin/unit_test.exe`를 사용합니다.

현재 unit test는 아래를 확인합니다.

- B+Tree insert/search/split/leaf link
- table auto increment, index lookup, linear scan, 조건 검색
- SQL 실행 결과, detailed syntax/query error
- `db_server` shared table 유지
- `usedIndex` 분류
- lock timeout과 metrics 집계
- HTTP request parsing과 JSON response contract

현재 CLI smoke script는 Windows PowerShell 경로를 기준으로 작성되어 있습니다.

```bash
powershell -ExecutionPolicy Bypass -File .\tests\smoke\server_cli_smoke_test.ps1
```

PowerShell smoke test는 Windows/PowerShell 환경에서 `build\bin\server.exe`를 대상으로 실행합니다.

현재 HTTP smoke script도 Windows PowerShell + `build\bin\server.exe` 기준입니다.

```bash
powershell -ExecutionPolicy Bypass -File .\tests\smoke\server_http_smoke_test.ps1
```

HTTP smoke test는 두 단계를 검증합니다.

1. 정상 INSERT/SELECT, 빈 조회, syntax error, metrics 응답
2. 작은 queue와 read 지연을 이용한 `queue_full` 응답

이 smoke test의 목적은 "서버가 실제 HTTP 요청을 받았을 때 문서에 적은 계약대로 응답하는지"를 빠르게 확인하는 것입니다.
즉 내부 함수 단위를 세밀하게 보는 unit test라기보다, 라우팅, JSON body 파싱, SQL 실행, 오류 매핑, 운영 메트릭 집계가 end-to-end로 맞는지 확인하는 빠른 회귀 검증에 가깝습니다.

자세한 절차는 [docs/http-smoke-test.md](./docs/http-smoke-test.md)에 정리되어 있습니다.

### macOS / Ubuntu 테스트 실행

macOS와 Ubuntu에서는 아래 순서로 같은 검증을 진행하면 됩니다.

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

이 컬렉션은 아래를 검증합니다.

- `GET /health`
- `POST /query` INSERT
- `POST /query` indexed SELECT
- 빈 SELECT 결과
- syntax error 응답
- metrics delta 검증

각 항목이 보는 의미는 아래와 같습니다.

- `GET /health`: 서버가 살아 있고 HTTP 라우팅이 정상인지 확인
- `POST /query` INSERT: JSON body 파싱, 쓰기 경로, INSERT 응답 계약 확인
- `POST /query` indexed SELECT: 조회 성공과 `usedIndex: true` 인덱스 경로 확인
- 빈 SELECT 결과: 결과가 없어도 `200`과 `rows: []`로 안정적으로 응답하는지 확인
- syntax error 응답: 잘못된 SQL이 `400 syntax_error`로 매핑되는지 확인
- metrics delta 검증: health/query/error/not-found 카운터가 실제 요청 흐름에 맞게 증가하는지 확인

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

이 컬렉션은 아래를 검증합니다.

- `GET /does-not-exist`의 `404 not_found`
- `POST /health`, `GET /query`의 `405 method_not_allowed`
- 빈 body 또는 잘못된 `query` 타입으로 들어오는 `400 malformed_http`
- HTTP API에서 `EXIT`, `QUIT`를 거절하는 `400 query_error`
- routing error와 `malformed_http`는 DB metrics에 섞이지 않고, `EXIT`/`QUIT`만 query metrics에 잡히는지
- `pm.sendRequest` fan-out으로 실행하는 read-only burst
- read 8 / write 2 비율의 mixed 80/20 burst
- 여러 INSERT를 동시에 보내는 write-heavy burst

각 항목이 보는 의미는 아래와 같습니다.

- `GET /does-not-exist`의 `404 not_found`: 존재하지 않는 경로가 올바른 routing error로 분리되는지 확인
- `POST /health`, `GET /query`의 `405 method_not_allowed`: 경로는 맞지만 HTTP 메서드가 틀렸을 때 메서드 오류로 정확히 매핑되는지 확인
- 빈 body 또는 잘못된 `query` 타입의 `400 malformed_http`: `POST /query`의 body 계약이 깨졌을 때 SQL 실행 전에 요청 자체를 거절하는지 확인
- HTTP API에서 `EXIT`, `QUIT`를 거절하는 `400 query_error`: CLI 전용 명령이 HTTP 경계에서는 질의 오류로 안전하게 차단되는지 확인
- routing error와 `malformed_http`, `EXIT`/`QUIT` metrics 분리: 라우팅/파싱 오류는 DB query metrics에 섞이지 않고, 실제 쿼리로 들어간 `EXIT`/`QUIT`만 query metrics에 잡히는지 확인
- `pm.sendRequest` fan-out read-only burst: 여러 읽기 요청을 동시에 보내도 read 경로가 안정적으로 응답하는지 확인
- mixed 80/20 burst: read 위주 트래픽 속에 일부 write가 섞였을 때도 응답 계약과 metrics가 무너지지 않는지 확인
- write-heavy burst: 여러 INSERT가 동시에 들어와도 쓰기 경로가 깨지지 않고 결과가 일관되게 내려오는지 확인

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

- DDL
- 다중 테이블
- `UPDATE` / `DELETE`
- join / aggregate / order by
- 영속화
- TLS / auth
- 인터넷 배포

이 프로젝트의 초점은 새 DBMS 전체를 만드는 것이 아니라, 작은 SQL 엔진을 명확한 동시성/HTTP 서비스 경계로 감싸고 검증 가능한 API 계약을 제공하는 데 있습니다.
