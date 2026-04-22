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

### INSERT

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

기본 빌드는 REPL, 서버, 단위 테스트, 기본 benchmark 실행 파일을 만듭니다.

```bash
make
```

주요 타깃:

```bash
make main
make server
make unit_test
make benchmarks
make clean
```

생성되는 주요 바이너리:

- POSIX/macOS/Linux: `build/bin/main`, `build/bin/server`, `build/bin/unit_test`
- Windows MinGW: `build\bin\main.exe`, `build\bin\server.exe`, `build\bin\unit_test.exe`

Windows MinGW 환경에서 `make`가 없으면 `mingw32-make`를 사용합니다.

```powershell
mingw32-make
```

## 직접 컴파일

Makefile을 쓰는 것을 권장하지만, 직접 컴파일도 가능합니다.

POSIX/macOS/Linux:

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
```

## 실행

### REPL

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

Windows:

```powershell
mingw32-make unit_test
.\build\bin\unit_test.exe
```

현재 unit test는 아래를 확인합니다.

- B+Tree insert/search/split/leaf link
- table auto increment, index lookup, linear scan, 조건 검색
- SQL 실행 결과, detailed syntax/query error
- `db_server` shared table 유지
- `usedIndex` 분류
- lock timeout과 metrics 집계
- HTTP request parsing과 JSON response contract

### Smoke test

PowerShell smoke test는 Windows/PowerShell 환경에서 `build\bin\server.exe`를 대상으로 실행합니다.

```powershell
mingw32-make server
powershell -ExecutionPolicy Bypass -File .\tests\smoke\server_cli_smoke_test.ps1
powershell -ExecutionPolicy Bypass -File .\tests\smoke\server_http_smoke_test.ps1
```

HTTP smoke test는 두 단계를 검증합니다.

1. 정상 INSERT/SELECT, 빈 조회, syntax error, metrics 응답
2. 작은 queue와 read 지연을 이용한 `queue_full` 응답

자세한 절차는 [docs/http-smoke-test.md](./docs/http-smoke-test.md)에 정리되어 있습니다.

### Benchmarks

```bash
make benchmarks
./build/bin/perf10
./build/bin/cond10
```

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
