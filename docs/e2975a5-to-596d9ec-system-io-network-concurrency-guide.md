# e2975a5 -> 596d9ec로 이해하는 Mini DBMS SQL API Server

이 문서는 초기 커밋 `e2975a56217b81d3b6f7e1a3419ce05d94b2e333`과 최신 커밋 `596d9ec33fcbefd0b538873cc9fad346d4ff46f2`를 비교해서, 현재 코드베이스가 어떻게 커졌는지 설명한다.

대상 독자는 C 언어 초심자다. `struct`, 포인터, 소켓, thread, lock 같은 말이 낯설어도 전체 흐름을 먼저 잡을 수 있게 설명한다.

함께 연결해서 읽을 PDF는 아래 세 장이다.

- `docs/Chapter 10- System-Level I-O.pdf`: 파일 descriptor, `read`, `write`, short count, RIO, 표준 입출력
- `docs/Chapter 11- Network Programming.pdf`: client-server 모델, socket, `bind`, `listen`, `accept`, HTTP
- `docs/Chapter 12- Concurrent Programming.pdf`: process, I/O multiplexing, thread, shared variable, bounded buffer, synchronization, readers-writers

## 읽기 전 용어 미니 사전

처음 읽을 때는 아래 단어만 먼저 잡고 가도 충분하다.

| 용어 | 짧은 뜻 | 이 프로젝트에서 보는 곳 |
|---|---|---|
| descriptor | 운영체제가 열어 둔 입출력 대상을 가리키는 번호 | socket, `stdin`, `stdout` |
| socket | 네트워크 연결의 끝점 | `src/server/http_server.c` |
| listening socket | 새 연결 요청을 받는 서버용 socket | `listen_socket` |
| connected socket | client 한 명과 실제로 읽고 쓰는 socket | `client_socket` |
| short count | 요청한 바이트보다 적게 읽히거나 써지는 상황 | `send_all()` 반복문 |
| bounded buffer | 크기가 제한된 대기열 | `HTTPRequestQueue` |
| worker thread | queue에서 일을 꺼내 처리하는 실행 흐름 | `http_server_worker_main()` |
| mutex | 공유 데이터를 한 번에 한 thread만 만지게 하는 잠금 | `metrics_mutex`, queue mutex |
| rwlock | 읽기는 여러 개, 쓰기는 하나만 허용하는 잠금 | `db_lock` |
| metrics | 서버가 처리한 요청 수 같은 운영 숫자 | `DBServerMetrics` |

## 1. 한 줄 요약

초기 커밋은 **터미널에서 SQL을 입력하면 in-memory `users` 테이블에 실행하는 작은 SQL 엔진**이었다.

최신 커밋은 그 엔진을 거의 그대로 보존하면서, 그 위에 **HTTP API 서버, 요청 큐, worker thread, read/write lock, metrics, smoke test**를 얹었다.

```mermaid
flowchart LR
    A["e2975a5\nCLI SQL 엔진"] --> B["core 보존\nsql/table/bptree"]
    B --> C["596d9ec\nHTTP SQL API 서버"]

    subgraph "초기 커밋"
        A1["main.c\nstdin으로 SQL 입력"]
        A2["sql.c\nSQL 파싱/실행"]
        A3["table.c\nRecord 저장"]
        A4["bptree.c\nid 인덱스"]
        A1 --> A2 --> A3 --> A4
    end

    subgraph "최신 커밋"
        C1["src/cli/main.c\n기존 REPL"]
        C2["src/server/server.c\nCLI 하네스 + HTTP 모드"]
        C3["src/server/http_server.c\nsocket + queue + workers"]
        C4["src/server/db_server.c\nshared Table + rwlock + metrics"]
        C5["src/core/*\n기존 SQL 엔진"]
        C2 --> C3 --> C4 --> C5
        C1 --> C5
    end
```

커밋 차이로 보면 `33 files changed`, `4053 insertions`, `361 deletions`이다. 핵심 엔진 파일들은 대부분 `src/core/` 아래로 이동했지만 내용은 유지되었다. 즉, 프로젝트가 "엔진을 갈아엎은 것"이 아니라 "엔진 위에 서버 계층을 얹은 것"에 가깝다.

## 2. 먼저 큰 구조부터 보기

현재 프로젝트는 세 층으로 나누어 보면 쉽다.

```text
src/core/      SQL 엔진, 테이블 저장소, B+Tree 인덱스
src/cli/       기존 REPL 진입점
src/server/    HTTP API 서버, 요청 큐, worker thread, lock, metrics
```

전체 요청 흐름은 아래처럼 이어진다.

```mermaid
flowchart TB
    Client["HTTP client\ncurl / browser / test script"]
    Listen["listening socket\nsocket -> bind -> listen"]
    Accept["accept()\nconnected socket 생성"]
    Queue["bounded request queue\nHTTPRequestQueue"]
    Worker["worker thread\nhttp_server_worker_main()"]
    Read["recv()\nHTTP 요청 읽기"]
    Parse["api_parse_http_request()\nmethod/path/body 파싱"]
    Route{"endpoint"}
    Health["GET /health"]
    Metrics["GET /metrics"]
    Query["POST /query"]
    DB["DBServer\nTable + rwlock + metrics"]
    SQL["sql_execute()\nSQL 파싱/실행"]
    Table["Table\nRecord 배열"]
    Index["B+Tree\nid primary index"]
    JSON["APIResponse\nJSON body"]
    Send["send_all()\nHTTP 응답 전송"]

    Client --> Listen --> Accept --> Queue --> Worker --> Read --> Parse --> Route
    Route --> Health --> JSON
    Route --> Metrics --> DB
    Route --> Query --> DB
    DB --> SQL --> Table
    Table --> Index
    DB --> JSON --> Send --> Client
```

초심자용으로 더 짧게 말하면:

- `src/core/`: SQL을 실제로 처리하는 뇌
- `src/server/db_server.c`: 여러 요청이 같은 테이블을 안전하게 쓰도록 감싸는 문
- `src/server/api.c`: HTTP 문자열과 JSON 응답을 만드는 번역기
- `src/server/http_server.c`: 소켓, 요청 큐, worker thread를 담당하는 서버 몸통
- `src/server/platform.c`: Windows와 POSIX의 thread/lock API 차이를 숨기는 어댑터

## 3. 초기 커밋에는 무엇이 있었나

초기 커밋 `e2975a5`의 중심은 아래 파일들이었다.

| 파일 | 역할 | 초심자 설명 |
|---|---|---|
| `main.c` | REPL 진입점 | `fgets`로 한 줄 SQL을 읽고 `sql_execute`를 호출한다. |
| `sql.c`, `sql.h` | SQL 파서/실행기 | 문자열을 읽어 `INSERT`인지 `SELECT`인지 판단한다. |
| `table.c`, `table.h` | in-memory 테이블 | `Record`를 동적 배열에 저장하고 찾는다. |
| `bptree.c`, `bptree.h` | B+Tree 인덱스 | `id`로 빠르게 찾기 위한 자료구조다. |
| `unit_test.c` | 단위 테스트 | B+Tree, table, SQL 실행이 맞는지 확인한다. |
| `perf_test.c` 등 | benchmark | 인덱스 검색과 선형 검색의 차이를 확인한다. |

초기 흐름은 단순하다.

```mermaid
sequenceDiagram
    participant User as 사용자
    participant Main as main.c
    participant SQL as sql_execute()
    participant Table as Table
    participant BPTree as B+Tree

    User->>Main: SQL 한 줄 입력
    Main->>SQL: sql_execute(table, input)
    alt INSERT
        SQL->>Table: table_insert(name, age)
        Table->>BPTree: bptree_insert(id, record)
    else SELECT WHERE id
        SQL->>Table: table_find_by_id_condition()
        Table->>BPTree: bptree_search(id)
    else SELECT WHERE name/age
        SQL->>Table: rows 배열 선형 검색
    end
    SQL-->>Main: SQLResult
    Main-->>User: printf로 결과 출력
```

#### 화살표 라벨 해설

| 다이어그램 라벨 | 현재 코드 기준 실제 의미 | 매개변수/값의 뜻 |
|---|---|---|
| `SQL 한 줄 입력` | `fgets(input, sizeof(input), stdin)`로 터미널에서 SQL 한 줄을 읽는다. | `input`은 입력을 담을 문자 배열, `sizeof(input)`은 최대 읽기 크기, `stdin`은 표준 입력이다. |
| `sql_execute(table, input)` | `src/core/sql.c`의 `sql_execute(Table *table, const char *input)` 호출이다. | `table`은 메모리 안의 users 테이블, `input`은 사용자가 입력한 SQL 문자열이다. |
| `table_insert(name, age)` | 실제 호출은 `table_insert(table, name, age)`이다. `sql_execute_insert()`가 `INSERT` 문에서 이름과 나이를 파싱한 뒤 호출한다. | `table`은 저장 대상 테이블, `name`은 예를 들어 `"Alice"`, `age`는 예를 들어 `20`이다. |
| `bptree_insert(id, record)` | 실제 호출은 `bptree_insert(table->pk_index, record->id, record)`이다. 새 row를 id 인덱스에도 넣는다. | `table->pk_index`는 기본키 B+Tree, `record->id`는 자동 증가 id, `record`는 저장한 `Record *` 포인터다. |
| `table_find_by_id_condition()` | 실제 호출은 `table_find_by_id_condition(table, comparison, int_value, &result.records, &result.row_count)`이다. | `comparison`은 `=`, `>`, `>=` 같은 비교 연산, `int_value`는 WHERE의 id 값, `result.records`와 `result.row_count`는 찾은 row 목록과 개수다. |
| `bptree_search(id)` | 실제 호출은 `bptree_search(table->pk_index, id)`이다. | `table->pk_index`에서 `id` key를 찾아 저장된 `Record *`를 돌려준다. |
| `rows 배열 선형 검색` | `table_find_by_name_matches()`, `table_find_by_age_condition()`, `table_collect_all()`처럼 `table->rows` 배열을 순서대로 훑는 경로다. | B+Tree key가 아닌 `name`, `age`, 전체 SELECT는 배열의 row 포인터들을 직접 검사한다. |
| `SQLResult` | `sql_execute()`가 돌려주는 결과 구조체다. | `status`, `action`, `record`, `records`, `row_count`, `inserted_id`, `error_message` 같은 필드로 성공/실패와 결과를 표현한다. |
| `printf로 결과 출력` | `src/cli/main.c`에서 `printf()` 또는 `table_print_records()`로 결과를 터미널에 보여준다. | INSERT면 `inserted_id`, SELECT면 `records`와 `row_count`를 사용한다. |

이 단계는 Chapter 10의 표준 입출력과 가장 가깝다. 사용자는 터미널에 입력하고, 프로그램은 `stdin`에서 읽고, `stdout`으로 출력한다.

## 4. 최신 커밋에는 무엇이 추가되었나

최신 커밋 `596d9ec` 기준으로는 서버 기능이 붙었다.

| 새 영역 | 대표 파일 | 역할 |
|---|---|---|
| 서버 상태 경계 | `src/server/db_server.c`, `src/server/db_server.h` | 하나의 공유 `Table`을 보관하고, SQL 실행 전후로 lock과 metrics를 관리한다. |
| HTTP 계약 | `src/server/api.c`, `src/server/api.h` | HTTP request를 파싱하고 JSON response를 만든다. |
| 네트워크 서버 | `src/server/http_server.c`, `src/server/http_server.h` | `socket`, `bind`, `listen`, `accept`, `recv`, `send`를 사용한다. |
| OS 차이 흡수 | `src/server/platform.c`, `src/server/platform.h` | POSIX `pthread_*`와 Windows API 차이를 숨긴다. |
| 서버 CLI | `src/server/server.c` | `--query` 하네스와 `--serve` HTTP 서버 모드를 제공한다. |
| 검증 | `tests/smoke/*`, `docs/http-smoke-test.md` | 실제 서버를 띄워 API 계약과 queue/lock 시그널을 확인한다. |

가장 중요한 새 구조체는 `DBServer`다.

```c
typedef struct DBServer {
    Table *table;
    PlatformRWLock db_lock;
    PlatformMutex metrics_mutex;
    DBServerConfig config;
    DBServerMetrics metrics;
} DBServer;
```

초기 커밋에서는 `main.c`가 `Table *table`을 직접 들고 있었다. 최신 커밋에서는 `DBServer`가 테이블과 lock과 metrics를 함께 들고 있다. 그래서 여러 worker thread가 같은 테이블을 보더라도 `db_server_execute()`라는 한 문을 통해 들어가게 된다.

## 5. 현재 코드를 읽는 추천 순서

C 초심자라면 구현 파일부터 바로 읽기보다 header와 test를 먼저 보는 편이 덜 막힌다.

```mermaid
flowchart TD
    A["1. README.md\n지원 기능 먼저 보기"]
    B["2. src/core/table.h\nRecord와 Table 모양 보기"]
    C["3. src/core/sql.h\nSQLResult 모양 보기"]
    D["4. tests/unit/unit_test.c\n사용 예시 보기"]
    E["5. src/core/table.c\n데이터 저장/검색 보기"]
    F["6. src/core/sql.c\n문자열 파싱 보기"]
    G["7. src/server/db_server.h/c\nlock과 metrics 보기"]
    H["8. src/server/api.h/c\nHTTP/JSON 계약 보기"]
    I["9. src/server/http_server.c\nsocket/thread/queue 보기"]
    J["10. src/server/platform.c\nOS별 thread/lock 차이 보기"]

    A --> B --> C --> D --> E --> F --> G --> H --> I --> J
```

왜 이 순서가 좋을까?

- `*.h` 파일은 "이 모듈을 어떻게 쓰는가"를 보여준다.
- `tests/unit/unit_test.c`는 "실제로 어떤 입력과 출력이 기대되는가"를 보여준다.
- `http_server.c`는 가장 어렵다. 소켓, 큐, thread, lock이 한 파일에 모여 있으므로 마지막에 보는 편이 좋다.

## 6. Chapter 10: System-Level I/O와 코드 연결

Chapter 10의 핵심은 "입출력은 결국 descriptor를 읽고 쓰는 일"이라는 관점이다.

초기 커밋에서는 표준 입출력을 주로 쓴다.

| PDF 개념 | 초기 코드 | 최신 코드 | 설명 |
|---|---|---|---|
| 표준 입력 | `fgets(input, sizeof(input), stdin)` | `server_run_stdin()`에서도 사용 | 터미널이나 파이프로 들어온 SQL 한 줄을 읽는다. |
| 표준 출력 | `printf(...)` | CLI 하네스 응답 출력 | 사람이 읽는 결과를 화면에 쓴다. |
| 표준 에러 | `fprintf(stderr, ...)` | 서버 초기화 실패, 인자 오류 출력 | 정상 응답과 오류 메시지 채널을 나눈다. |
| descriptor 기반 I/O | 직접 노출 적음 | socket도 descriptor처럼 다룸 | 네트워크 연결도 읽고 쓸 수 있는 대상이다. |
| short count | 크게 드러나지 않음 | `send_all()`과 request 읽기 루프 | 한 번의 `send`/`recv`가 전체 데이터를 처리한다고 가정하면 위험하다. |
| RIO | 구현 없음 | 비슷한 의식은 있음 | `http_server_socket_send_all()`은 `rio_writen`처럼 끝까지 보내려 한다. |

최신 코드에서 Chapter 10과 가장 직접 연결되는 부분은 `src/server/http_server.c`다.

```mermaid
flowchart LR
    A["connected socket"] --> B["recv()"]
    B --> C["request_buffer[8192]"]
    C --> D["headers 끝 찾기\n\\r\\n\\r\\n"]
    D --> E["Content-Length 계산"]
    E --> F["body까지 다 읽었는지 확인"]
    F --> G["api_parse_http_request()"]

    H["APIResponse"] --> I["api_render_http_response()"]
    I --> J["send() 반복"]
    J --> K["client socket"]
```

`http_server_socket_send_all()`은 특히 중요하다. 네트워크에서는 한 번의 `send()`가 요청한 길이를 전부 보내지 못할 수 있다. 그래서 `offset`을 증가시키며 전체 길이를 보낼 때까지 반복한다. Chapter 10의 RIO 패키지가 해결하려는 문제와 같은 방향이다.

다만 이 프로젝트는 textbook의 RIO 함수를 그대로 쓰지는 않는다. `send_all()`은 `rio_writen`의 "끝까지 쓰기" 아이디어와 비슷하지만, `recv` 쪽은 고정 크기 buffer와 HTTP framing을 직접 확인하는 단순 구현이다. RIO처럼 모든 `EINTR` 재시도와 범용 robust read 패키지를 제공하는 것은 아니다. 현재 코드는 직접 `recv`, `send`, `select`를 사용한다.

### 초심자 포인트: `fgets`와 `recv`의 차이

`fgets`는 "한 줄"을 읽는 느낌이 강하다. 반면 `recv`는 네트워크에서 도착한 "바이트 덩어리"를 읽는다. HTTP 요청이 한 번에 다 들어올 수도 있고, 나뉘어 들어올 수도 있다. 그래서 최신 코드는 헤더 끝과 `Content-Length`를 확인하면서 요청이 충분히 들어왔는지 판단한다.

## 7. Chapter 11: Network Programming과 코드 연결

Chapter 11의 핵심 흐름은 client-server transaction이다.

```text
client가 요청한다 -> server가 처리한다 -> server가 응답한다
```

현재 서버는 이 모델을 그대로 따른다.

| PDF 개념 | 현재 코드 | 설명 |
|---|---|---|
| client-server transaction | `GET /health`, `GET /metrics`, `POST /query` | 요청 하나를 받고 응답 하나를 보낸 뒤 연결을 닫는다. |
| socket | `socket(AF_INET, SOCK_STREAM, 0)` | TCP 연결용 endpoint를 만든다. |
| bind | `bind(listen_socket, ...)` | 서버 포트를 socket에 붙인다. |
| listen | `listen(listen_socket, 16)` | 연결 요청을 받을 수 있는 listening socket으로 만든다. |
| accept | `accept(listen_socket, NULL, NULL)` | 클라이언트별 connected socket을 만든다. |
| HTTP request line | `api_parse_http_request()` | `GET /health HTTP/1.1` 같은 첫 줄을 해석한다. |
| HTTP response | `api_render_http_response()` | status line, header, body를 문자열로 만든다. |
| Tiny Web Server | `http_server.c` | 정적 파일 서버는 아니지만 Tiny처럼 HTTP transaction을 처리한다. |

현재 서버 생성 흐름은 아래와 같다.

```mermaid
sequenceDiagram
    participant Main as server.c
    participant HTTP as http_server_run()
    participant OS as OS socket API
    participant Queue as HTTPRequestQueue
    participant Worker as worker threads

    Main->>HTTP: --serve 옵션으로 진입
    HTTP->>OS: socket()
    HTTP->>OS: setsockopt(SO_REUSEADDR)
    HTTP->>OS: bind(port)
    HTTP->>OS: listen()
    HTTP->>Worker: platform_thread_create() x N
    loop 서버 실행 중
        HTTP->>OS: select(listen_socket)
        OS-->>HTTP: 새 연결 준비됨
        HTTP->>OS: accept()
        OS-->>HTTP: client_socket
        HTTP->>Queue: push(client_socket)
        Worker->>Queue: pop()
        Worker->>OS: recv() 요청 읽기
        Worker->>Worker: API/SQL 처리
        Worker->>OS: send() 응답 쓰기
        Worker->>OS: close(client_socket)
    end
```

#### 화살표 라벨 해설

| 다이어그램 라벨 | 현재 코드 기준 실제 의미 | 매개변수/값의 뜻 |
|---|---|---|
| `--serve 옵션으로 진입` | `src/server/server.c`의 `main()`이 `--serve`를 발견하면 `http_server_run(&http_options)`를 호출한다. | `http_options`에는 `port`, `worker_count`, `queue_capacity`, lock timeout, simulate delay 값이 들어 있다. |
| `socket()` | `socket(AF_INET, SOCK_STREAM, 0)`으로 listening socket을 만든다. | `AF_INET`은 IPv4, `SOCK_STREAM`은 TCP, `0`은 기본 프로토콜 선택이다. |
| `setsockopt(SO_REUSEADDR)` | `setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, ...)`로 포트 재사용 옵션을 켠다. | 서버 재시작 직후에도 같은 포트를 다시 bind하기 쉽게 만든다. |
| `bind(port)` | `bind(listen_socket, (struct sockaddr *)&address, sizeof(address))`로 socket을 포트에 붙인다. | `address.sin_addr.s_addr = htonl(INADDR_ANY)`, `address.sin_port = htons(port)`가 핵심 값이다. |
| `listen()` | `listen(listen_socket, 16)`으로 연결 요청을 받을 수 있는 listening socket으로 전환한다. | `16`은 OS가 대기시킬 연결 요청 backlog 크기다. |
| `platform_thread_create() x N` | worker 수만큼 `platform_thread_create(&context.workers[worker_index], http_server_worker_main, &context)`를 호출한다. | 첫 인자는 thread handle 저장 위치, 둘째는 worker 시작 함수, 셋째는 모든 worker가 공유할 `HTTPServerContext *`다. |
| `select(listen_socket)` | 실제로는 `http_server_socket_wait_for_read(listen_socket, 200)` 안에서 `select()`를 호출한다. | `listen_socket`에 새 연결이 왔는지 최대 `200ms` 기다린다. |
| `새 연결 준비됨` | `select()` 결과가 0보다 크다는 뜻이다. | 0은 timeout, 음수는 에러, 양수는 읽을 수 있는 socket 존재를 의미한다. |
| `accept()` | `accept(listen_socket, NULL, NULL)`로 client별 connected socket을 만든다. | 주소 정보를 따로 쓰지 않기 때문에 두 번째, 세 번째 인자는 `NULL`이다. 반환값이 `client_socket`이다. |
| `push(client_socket)` | `http_request_queue_push(&context.queue, client_socket)`로 connected socket을 worker queue에 넣는다. | `context.queue`는 mutex/condition variable로 보호되는 bounded queue다. |
| `pop()` | worker가 `http_request_queue_pop(&context->queue)`로 처리할 socket을 꺼낸다. | queue가 비어 있으면 condition variable에서 기다린다. |
| `recv() 요청 읽기` | `http_server_read_request(client_socket, request_buffer, sizeof(request_buffer), error_message, sizeof(error_message))`가 HTTP 요청 전체를 읽는다. | `request_buffer`는 raw HTTP 문자열 저장소, `error_message`는 실패 이유 저장소다. |
| `API/SQL 처리` | `http_server_handle_client(context, client_socket)` 안에서 `api_parse_http_request()`, `db_server_execute()`, `api_build_execution_response()`가 이어진다. | `context` 안에는 공유 `DBServer`, queue 상태, 완료 요청 수 등이 들어 있다. |
| `send() 응답 쓰기` | 최종적으로 `http_server_socket_send_all(client_socket, raw_response, strlen(raw_response))`가 응답 바이트를 모두 보낸다. | `raw_response`는 status line, header, JSON body를 합친 HTTP 응답 문자열이다. |
| `close(client_socket)` | worker loop가 `http_server_socket_close(client_socket)`로 connected socket을 닫는다. | 현재 서버는 요청 하나에 응답 하나를 보낸 뒤 연결을 닫는 구조다. |

### listening socket과 connected socket

Chapter 11에서 초심자가 자주 헷갈리는 부분이 listening descriptor와 connected descriptor의 차이다.

현재 코드에서는 이렇게 보면 된다.

```text
listen_socket:
  서버가 켜져 있는 동안 계속 살아 있다.
  새 연결 요청을 받는 문지기다.

client_socket:
  accept()가 요청 하나마다 새로 만들어 준다.
  worker가 요청을 읽고 응답을 보낸 뒤 닫는다.
```

이 차이를 이해하면 `accept()` 이후에 왜 queue에 `client_socket`을 넣는지 보인다. queue에 넣는 것은 서버 전체의 문지기 socket이 아니라, "이번 클라이언트와 대화할 전용 socket"이다.

### textbook 예제와 다른 점

PDF의 helper 함수들은 보통 `getaddrinfo` 기반으로 더 portable한 listen socket 생성을 설명한다. 현재 코드는 `sockaddr_in`, `AF_INET`, `INADDR_ANY`, `htons(port)`를 직접 사용한다. 즉 IPv4 중심의 직접 구현이다.

또 Tiny Web Server는 정적 파일과 CGI 동적 콘텐츠를 다룬다. 이 프로젝트는 파일을 서빙하지 않고, `POST /query`로 받은 SQL을 JSON으로 응답하는 API 서버다.

## 8. Chapter 12: Concurrent Programming과 코드 연결

최신 커밋에서 가장 큰 변화는 동시성이다. 서버는 worker thread를 여러 개 만들고, 각 worker가 queue에서 socket을 꺼내 처리한다.

```mermaid
flowchart TB
    Main["main accept loop\nproducer"]
    Queue["bounded queue\nmutex + condition variable"]
    W1["worker 1\nconsumer"]
    W2["worker 2\nconsumer"]
    W3["worker N\nconsumer"]
    DB["shared DBServer\nTable + metrics"]

    Main -->|"accept 후 push"| Queue
    Queue -->|"pop"| W1
    Queue -->|"pop"| W2
    Queue -->|"pop"| W3
    W1 --> DB
    W2 --> DB
    W3 --> DB
```

이 구조는 Chapter 12의 prethreaded concurrent server와 bounded buffer 아이디어와 잘 맞는다.

| PDF 개념 | 현재 코드 | 설명 |
|---|---|---|
| threads | `platform_thread_create()` | worker thread를 미리 여러 개 만든다. |
| shared variables | `HTTPServerContext`, `DBServer`, `metrics` | 여러 thread가 같은 구조체를 본다. |
| bounded buffer | `HTTPRequestQueue` | 연결 socket을 제한된 크기의 원형 큐에 넣는다. |
| mutex | `PlatformMutex` | queue 상태와 metrics 상태를 보호한다. |
| condition variable | `PlatformCond` | queue가 비었을 때 worker를 재우고, push되면 깨운다. |
| readers-writers | `PlatformRWLock db_lock` | `SELECT`는 read lock, `INSERT`는 write lock을 사용한다. |
| race 방지 | lock으로 공유 상태 보호 | 동시에 같은 `Table`이나 metrics를 고치다 깨지는 일을 막는다. |
| deadlock 주의 | lock 범위 단순화 | DB lock과 metrics lock을 복잡하게 중첩하지 않도록 구조화되어 있다. |

### SELECT와 INSERT가 lock을 다르게 쓰는 이유

`SELECT`는 데이터를 읽기만 한다. 여러 reader가 동시에 읽어도 데이터가 바뀌지 않으면 안전하다.

`INSERT`는 데이터를 바꾼다. `rows` 배열에 새 `Record`를 넣고, B+Tree에도 새 key를 넣고, `next_id`도 증가한다. 그래서 writer는 혼자 들어가야 한다.

```mermaid
stateDiagram-v2
    [*] --> Request
    Request --> Classify: SELECT or INSERT?
    Classify --> ReadLock: SELECT
    Classify --> WriteLock: INSERT
    ReadLock --> ExecuteSQL: 여러 SELECT 동시 가능
    WriteLock --> ExecuteSQL: INSERT 단독 실행
    ExecuteSQL --> ReleaseLock
    ReleaseLock --> Metrics
    Metrics --> [*]
```

`src/server/db_server.c`의 핵심 아이디어는 다음 순서다.

```text
1. SQL이 SELECT인지 INSERT인지 가볍게 분류한다.
2. SELECT면 read lock, INSERT면 write lock 획득을 시도한다.
3. lock timeout이 지나면 lock_timeout 응답으로 끝낸다.
4. lock을 얻으면 기존 sql_execute(server->table, query)를 호출한다.
5. lock을 해제하고 metrics를 업데이트한다.
```

정확히는 현재 구현에서 lock은 분류 가능한 `SELECT`와 `INSERT` 중심으로 적용된다. `QUIT`, `EXIT`, 문법 오류처럼 `db_server_classify_query()`가 read/write로 분류하지 못하는 입력은 DB lock 없이 `sql_execute()`로 넘어간다. 현재 지원 SQL 범위에서는 테이블을 바꾸는 문장이 `INSERT`뿐이라 이 구조가 맞지만, 나중에 `UPDATE`나 `DELETE`가 추가되면 이들도 write lock 쪽으로 분류해야 한다.

### queue_full은 왜 필요한가

worker가 처리할 수 있는 속도보다 client 연결이 더 빨리 들어오면 무한히 쌓을 수 없다. 그래서 현재 서버는 queue 크기를 제한한다. queue가 가득 차면 `503 queue_full`을 응답한다.

이것은 Chapter 12의 bounded buffer와 연결된다. 제한이 있는 buffer는 시스템이 과부하일 때 "더 못 받겠다"는 신호를 명확히 줄 수 있다.

## 9. 동시 접근 시나리오

아래 세 diagram은 여러 worker가 동시에 `POST /query`를 처리할 때 `db_lock`이 어떻게 동작하는지 보여준다. `accept(listen_socket, NULL, NULL)`와 `http_request_queue_push(&context.queue, client_socket)`를 지나 서로 다른 worker가 `http_server_handle_client(context, client_socket)`에 들어온 뒤, 각 worker는 자기 stack에 `APIRequest request`, `APIResponse response`, `DBServerExecution execution`을 따로 가진다. 그림에서는 동시에 실행되는 worker를 구분하려고 `request_r1`, `execution_r1`, `request_w1`, `execution_w1`처럼 이름을 붙였다.

핵심 함수는 모두 `src/server/db_server.c`의 같은 흐름을 탄다.

```text
db_server_execute(&context->db_server, request.query, &execution)
  -> query_kind = db_server_classify_query(request.query)
  -> execution.used_index = db_server_guess_uses_index(request.query)
  -> execution.is_write = (query_kind == DB_SERVER_QUERY_KIND_WRITE)
  -> db_server_metrics_query_started(server, query_kind)
  -> db_server_try_acquire_lock(server, query_kind)
       -> platform_rwlock_try_read_lock(&server->db_lock)
       -> or platform_rwlock_try_write_lock(&server->db_lock)
       -> platform_sleep_ms(1) while waiting
  -> db_server_apply_simulated_delay(server, query_kind)
  -> execution.result = sql_execute(server->table, request.query)
  -> execution.server_status = DB_SERVER_EXEC_STATUS_OK
  -> db_server_release_lock(server, query_kind)
  -> db_server_metrics_query_finished(server, &execution)
```

중요한 점은 `db_server_try_acquire_lock()`이 `pthread_rwlock_rdlock()`처럼 한 번에 잠들어 기다리는 blocking 호출이 아니라는 것이다. 현재 코드는 `platform_rwlock_try_read_lock()` 또는 `platform_rwlock_try_write_lock()`을 시도하고, 실패하면 `platform_sleep_ms(1)`로 1ms 쉬었다가 다시 시도한다. 기다림이 `server->config.lock_timeout_ms` 이상 길어지면 `execution.server_status = DB_SERVER_EXEC_STATUS_LOCK_TIMEOUT`이 되고, `sql_execute()`는 호출되지 않는다. 이 경우에는 잡은 lock이 없으므로 unlock도 하지 않는다.

각 worker 안에서 HTTP body의 SQL 문자열은 먼저 `api_parse_http_request(request_buffer, &request, error_message, sizeof(error_message))`로 `request.query`에 들어간다. 그래서 아래 diagram의 `request_r1.query = "SELECT ..."` 같은 줄은 실제 코드의 `request.query` 값을 worker별로 이름만 바꿔 보여주는 것이다.

diagram을 읽을 때는 `->>`를 "함수 호출", `-->>`를 "값이 돌아옴" 정도로 보면 된다. `par`는 두 worker가 동시에 실행되는 구간, `alt`는 timeout 여부에 따른 갈림길, `loop`는 lock을 얻을 때까지 1ms씩 쉬며 다시 시도하는 구간이다.

### 읽기 + 읽기

두 요청이 모두 `SELECT`라면 둘 다 `DB_SERVER_QUERY_KIND_READ`로 분류된다. read lock은 writer가 없을 때 여러 worker가 동시에 잡을 수 있다.

```mermaid
sequenceDiagram
    participant R1 as worker R1
    participant R2 as worker R2
    participant D as db_server.c
    participant L as PlatformRWLock db_lock
    participant S as sql.c
    participant T as table.c
    participant B as bptree.c

    par R1 SELECT by id
        R1->>R1: set request_r1.query to SELECT by id
        R1->>D: db_server_execute with request_r1 and execution_r1
        D->>D: classify request_r1 as READ
        D->>D: mark execution_r1.used_index as true
        D->>D: execution_r1.is_write = 0
        D->>D: db_server_metrics_query_started(server, DB_SERVER_QUERY_KIND_READ)
        D->>L: try read lock succeeds
        D->>D: db_server_apply_simulated_delay(server, DB_SERVER_QUERY_KIND_READ)
        D->>S: sql_execute with request_r1.query
        S->>T: table_find_by_id_condition with id equals 1
        T->>T: table_find_by_id(table, 1)
        T->>B: bptree_search with key 1
        B-->>T: Record pointer
        T-->>S: result.records points to one Record
        S-->>D: execution_r1.result.action = SQL_ACTION_SELECT_ROWS
        D->>L: unlock read lock
        D->>D: finish metrics for execution_r1
    and R2 SELECT by age
        R2->>R2: set request_r2.query to SELECT by age
        R2->>D: db_server_execute with request_r2 and execution_r2
        D->>D: classify request_r2 as READ
        D->>D: mark execution_r2.used_index as false
        D->>D: execution_r2.is_write = 0
        D->>D: db_server_metrics_query_started(server, DB_SERVER_QUERY_KIND_READ)
        D->>L: try read lock succeeds
        D->>D: db_server_apply_simulated_delay(server, DB_SERVER_QUERY_KIND_READ)
        D->>S: sql_execute with request_r2.query
        S->>T: table_find_by_age_condition with age greater than 20
        T-->>S: Record pointer array and row count
        S-->>D: execution_r2.result.action = SQL_ACTION_SELECT_ROWS
        D->>L: unlock read lock
        D->>D: finish metrics for execution_r2
    end
```

#### 화살표 라벨 해설

| 다이어그램 라벨 | 현재 코드 기준 실제 의미 | 매개변수/값의 뜻 |
|---|---|---|
| `set request_r1.query to SELECT by id` | HTTP 요청 파싱 뒤 `request.query`에 `SELECT * FROM users WHERE id = 1;`이 들어간 상황을 worker별 이름으로 표현한 것이다. | `request_r1`은 설명용 이름이고, 실제 구조체 타입은 `APIRequest`다. |
| `db_server_execute with request_r1 and execution_r1` | 실제 HTTP 경로에서는 `db_server_execute(&context->db_server, request.query, &execution)`이다. | `context->db_server`는 공유 테이블과 lock을 가진 서버 상태, `request.query`는 SQL 문자열, `execution`은 실행 결과를 받을 구조체다. |
| `classify request_r1 as READ` | `db_server_classify_query(request.query)`가 앞 단어 `SELECT`를 보고 `DB_SERVER_QUERY_KIND_READ`를 돌려준다. | 이 값이 read lock을 잡을지 write lock을 잡을지 결정한다. |
| `mark execution_r1.used_index as true` | `db_server_guess_uses_index(request.query)`가 `WHERE id`를 발견해서 `1`을 돌려준다. | HTTP JSON 응답의 `usedIndex:true`로 이어지는 값이다. |
| `execution_r1.is_write = 0` | `execution->is_write = (query_kind == DB_SERVER_QUERY_KIND_WRITE)` 결과가 0이다. | SELECT는 쓰기 요청이 아니므로 metrics와 응답에서 read로 취급된다. |
| `db_server_metrics_query_started(server, DB_SERVER_QUERY_KIND_READ)` | 실행 시작 시 metrics를 갱신한다. | `server`는 `DBServer *`, 두 번째 인자는 SELECT 요청 수를 올리기 위한 분류값이다. |
| `try read lock succeeds` | 실제 호출은 `platform_rwlock_try_read_lock(&server->db_lock)`이다. | `&server->db_lock`은 공유 `Table`을 보호하는 readers-writer lock 주소다. reader끼리는 동시에 성공할 수 있다. |
| `db_server_apply_simulated_delay(server, DB_SERVER_QUERY_KIND_READ)` | 읽기 지연 옵션이 있으면 `platform_sleep_ms(server->config.simulate_read_delay_ms)`를 호출한다. | 동시성 테스트에서 SELECT가 lock을 잡은 채 오래 머무르게 만들 수 있다. |
| `sql_execute with request_r1.query` | 실제 호출은 `sql_execute(server->table, request.query)`이다. | `server->table`은 공유 users 테이블, `request.query`는 SELECT 문자열이다. |
| `table_find_by_id_condition with id equals 1` | 실제 호출은 `table_find_by_id_condition(table, TABLE_COMPARISON_EQ, 1, &result.records, &result.row_count)`이다. | `TABLE_COMPARISON_EQ`는 `=`, `1`은 WHERE의 id 값, 뒤 두 인자는 결과 배열과 row 수를 받을 출력 포인터다. |
| `table_find_by_id(table, 1)` | `table_find_by_id()`가 id 단건 검색을 B+Tree 검색으로 넘긴다. | `table`은 조회 대상, `1`은 찾을 primary key다. |
| `bptree_search with key 1` | 실제 호출은 `bptree_search(table->pk_index, 1)`이다. | `table->pk_index`는 id 인덱스, `1`은 검색 key다. |
| `Record pointer` | `bptree_search()`가 저장되어 있던 `Record *`를 반환한다. | row가 없으면 `NULL`이 될 수 있다. |
| `result.records points to one Record` | `table_find_by_id_condition()`이 찾은 `Record *`를 `result.records` 배열에 담는다. | `result.records`는 `Record **`, `result.row_count`는 찾은 row 개수다. |
| `execution_r1.result.action = SQL_ACTION_SELECT_ROWS` | `sql_execute_select()`가 SELECT 결과임을 표시한다. | API 응답의 `"action":"select"`로 변환된다. |
| `unlock read lock` | 실제 호출은 `platform_rwlock_read_unlock(&server->db_lock)`이다. | SELECT가 끝났으므로 다른 writer가 들어올 수 있게 read lock을 놓는다. |
| `finish metrics for execution_r1` | 실제 호출은 `db_server_metrics_query_finished(server, &execution)`이다. | active query 수를 줄이고, 오류/timeout/not found 같은 결과별 metrics를 반영한다. |
| `set request_r2.query to SELECT by age` | 두 번째 worker의 `request.query`가 `SELECT * FROM users WHERE age > 20;`인 상황이다. | 실제 변수명은 worker마다 로컬 `APIRequest request`이고, `request_r2`는 설명용 이름이다. |
| `mark execution_r2.used_index as false` | `db_server_guess_uses_index()`가 `WHERE age`는 id 인덱스 조건이 아니라고 판단한다. | 그래서 HTTP 응답의 `usedIndex`는 `false`가 된다. |
| `table_find_by_age_condition with age greater than 20` | 실제 호출은 `table_find_by_age_condition(table, TABLE_COMPARISON_GT, 20, &result.records, &result.row_count)`이다. | `TABLE_COMPARISON_GT`는 `>`, `20`은 WHERE의 age 값이며, 내부적으로 `table->rows`를 선형 검색한다. |
| `Record pointer array and row count` | age 조건에 맞는 여러 `Record *`를 결과 배열로 돌려준다. | `Record **records`는 row 포인터 목록, `size_t row_count`는 개수다. |

초심자 관점에서는 "읽는 사람끼리는 동시에 들어갈 수 있다"고 기억하면 된다. `SELECT WHERE id = 1`은 B+Tree를 쓰기 때문에 `usedIndex:true`가 되고, `SELECT WHERE age > 20`은 rows 배열을 선형으로 훑기 때문에 `usedIndex:false`가 된다.

### 읽기 + 쓰기

`SELECT`와 `INSERT`가 동시에 오면 둘 중 먼저 lock을 잡은 쪽이 먼저 실행된다. 아래 그림은 reader가 먼저 read lock을 잡고, writer가 그 뒤에 write lock을 기다리는 대표 상황이다. `--simulate-read-delay-ms`를 주거나 SELECT가 오래 걸리면 이런 흐름을 더 쉽게 관찰할 수 있다.

```mermaid
sequenceDiagram
    participant R as reader worker
    participant W as writer worker
    participant DR as db_server.c read path
    participant DW as db_server.c write path
    participant L as PlatformRWLock db_lock
    participant S as sql.c
    participant T as table.c
    participant B as bptree.c

    R->>R: set request_r.query to SELECT by id
    R->>DR: db_server_execute with read request
    DR->>DR: classify request_r as READ
    DR->>DR: mark execution_r.used_index as true
    DR->>DR: execution_r.is_write = 0
    DR->>DR: db_server_metrics_query_started(server, DB_SERVER_QUERY_KIND_READ)
    DR->>L: try read lock succeeds

    W->>W: set request_w.query to INSERT Bob
    W->>DW: db_server_execute with write request
    DW->>DW: classify request_w as WRITE
    DW->>DW: mark execution_w.used_index as false
    DW->>DW: execution_w.is_write = 1
    DW->>DW: db_server_metrics_query_started(server, DB_SERVER_QUERY_KIND_WRITE)
    DW->>L: try write lock fails

    par reader keeps the read lock while executing SQL
        DR->>DR: db_server_apply_simulated_delay(server, DB_SERVER_QUERY_KIND_READ)
        DR->>S: sql_execute with request_r.query
        S->>T: table_find_by_id_condition with id equals 1
        T->>T: table_find_by_id(table, 1)
        T->>B: bptree_search with key 1
        B-->>T: Record pointer
        T-->>S: result.records points to one Record
        S-->>DR: execution_r.result.action = SQL_ACTION_SELECT_ROWS
    and writer polls for write lock
        loop until read lock is released or lock timeout expires
            DW->>DW: platform_sleep_ms(1)
            DW->>L: try write lock again
        end
    end

    DR->>L: unlock read lock
    DR->>DR: finish metrics for execution_r

    alt writer obtains lock before timeout
        DW->>L: try write lock succeeds
        DW->>DW: db_server_apply_simulated_delay(server, DB_SERVER_QUERY_KIND_WRITE)
        DW->>S: sql_execute with request_w.query
        S->>T: table_insert with Bob age 30
        T->>T: assign next id to record
        T->>B: bptree_insert with new record id
        B-->>T: 1
        T->>T: increase table size
        T-->>S: Record pointer
        S-->>DW: execution_w.result.action = SQL_ACTION_INSERT
        DW->>L: unlock write lock
        DW->>DW: finish metrics for execution_w
    else writer waits until lock timeout
        DW-->>W: execution_w.server_status = DB_SERVER_EXEC_STATUS_LOCK_TIMEOUT
        DW->>DW: finish metrics for execution_w
    end
```

#### 화살표 라벨 해설

| 다이어그램 라벨 | 현재 코드 기준 실제 의미 | 매개변수/값의 뜻 |
|---|---|---|
| `set request_r.query to SELECT by id` | reader worker의 `request.query`가 `SELECT * FROM users WHERE id = 1;`인 상황이다. | `request_r`은 설명용 이름이고 실제로는 `http_server_handle_client()`의 로컬 `APIRequest request`다. |
| `db_server_execute with read request` | 실제 호출은 `db_server_execute(&context->db_server, request.query, &execution)`이다. | 공유 `DBServer`, SELECT 문자열, 결과를 받을 `DBServerExecution`을 넘긴다. |
| `classify request_r as READ` | `db_server_classify_query(request.query)`가 `DB_SERVER_QUERY_KIND_READ`를 반환한다. | SELECT이므로 read lock 경로로 간다. |
| `mark execution_r.used_index as true` | `db_server_guess_uses_index()`가 `WHERE id`를 보고 1을 반환한다. | `execution.used_index`는 응답 JSON의 `usedIndex` 값이 된다. |
| `try read lock succeeds` | `platform_rwlock_try_read_lock(&server->db_lock)`가 성공한 상태다. | 이 순간 reader는 공유 `Table`을 읽을 권한을 얻는다. |
| `set request_w.query to INSERT Bob` | writer worker의 `request.query`가 `INSERT INTO users VALUES ('Bob', 30);`인 상황이다. | SQL 안의 `"Bob"`과 `30`이 나중에 `table_insert()`의 `name`, `age`가 된다. |
| `db_server_execute with write request` | writer도 같은 `db_server_execute(&context->db_server, request.query, &execution)`로 들어온다. | 같은 공유 `DBServer`를 사용하므로 reader와 같은 `db_lock`을 경쟁한다. |
| `classify request_w as WRITE` | `db_server_classify_query()`가 앞 단어 `INSERT`를 보고 `DB_SERVER_QUERY_KIND_WRITE`를 반환한다. | INSERT는 테이블을 바꾸므로 write lock이 필요하다. |
| `mark execution_w.used_index as false` | INSERT는 SELECT가 아니므로 `db_server_guess_uses_index()`가 0을 반환한다. | INSERT 응답은 항상 `usedIndex:false`로 만들어진다. |
| `try write lock fails` | `platform_rwlock_try_write_lock(&server->db_lock)`가 실패한 상태다. | reader가 read lock을 잡고 있으므로 writer는 동시에 들어갈 수 없다. |
| `db_server_apply_simulated_delay(server, DB_SERVER_QUERY_KIND_READ)` | reader가 read lock을 잡은 상태에서 선택적으로 지연된다. | `server->config.simulate_read_delay_ms`가 0보다 크면 그만큼 sleep한다. |
| `sql_execute with request_r.query` | reader가 `sql_execute(server->table, request.query)`로 SELECT를 실행한다. | `server->table`은 공유 테이블, `request.query`는 SELECT 문자열이다. |
| `table_find_by_id_condition with id equals 1` | `table_find_by_id_condition(table, TABLE_COMPARISON_EQ, 1, &result.records, &result.row_count)`이다. | id 1을 B+Tree 경로로 찾아 결과 배열에 담는다. |
| `platform_sleep_ms(1)` | writer가 lock 재시도 사이에 1ms 쉰다. | busy waiting으로 CPU를 계속 태우지 않도록 짧게 양보한다. |
| `try write lock again` | loop 안에서 `platform_rwlock_try_write_lock(&server->db_lock)`를 반복 호출한다. | read lock이 풀리면 성공하고, timeout이 먼저 오면 실패로 끝난다. |
| `unlock read lock` | reader가 `platform_rwlock_read_unlock(&server->db_lock)`를 호출한다. | 이 시점 이후 writer가 write lock을 얻을 수 있다. |
| `try write lock succeeds` | writer가 `platform_rwlock_try_write_lock(&server->db_lock)`에 성공한다. | 이제 writer가 테이블 변경을 독점한다. |
| `db_server_apply_simulated_delay(server, DB_SERVER_QUERY_KIND_WRITE)` | 쓰기 지연 옵션이 있으면 `platform_sleep_ms(server->config.simulate_write_delay_ms)`를 호출한다. | INSERT가 write lock을 잡은 채 오래 머무르는 상황을 만들 수 있다. |
| `sql_execute with request_w.query` | writer가 `sql_execute(server->table, request.query)`로 INSERT를 실행한다. | `request.query`는 `INSERT INTO users VALUES ('Bob', 30);`이다. |
| `table_insert with Bob age 30` | 실제 호출은 `table_insert(table, "Bob", 30)`이다. | `table`은 공유 테이블, `"Bob"`은 새 row의 name, `30`은 age다. |
| `assign next id to record` | `record->id = table->next_id++`가 실행된다. | write lock 덕분에 동시에 두 writer가 같은 id를 가져가지 않는다. |
| `bptree_insert with new record id` | 실제 호출은 `bptree_insert(table->pk_index, record->id, record)`이다. | 새 row를 id 인덱스에 추가한다. |
| `increase table size` | `table->size++`로 row 개수를 늘린다. | rows 배열에 새 `Record *`가 들어간 뒤 수행된다. |
| `execution_w.server_status = DB_SERVER_EXEC_STATUS_LOCK_TIMEOUT` | writer가 timeout까지 lock을 못 얻은 실패 분기다. | 이 경우 `sql_execute()`는 호출되지 않고 API 응답은 `503 lock_timeout`이 된다. |

반대로 `INSERT`가 먼저 write lock을 잡으면 `SELECT`가 `platform_rwlock_try_read_lock(&server->db_lock)`에서 기다린다. 즉, 읽기와 쓰기는 동시에 테이블에 들어가지 않는다.

### 쓰기 + 쓰기

두 요청이 모두 `INSERT`라면 둘 다 write lock이 필요하다. write lock은 한 번에 하나만 허용하므로, 두 INSERT는 순서대로 실행된다.

```mermaid
sequenceDiagram
    participant W1 as writer worker W1
    participant W2 as writer worker W2
    participant D1 as db_server.c write path W1
    participant D2 as db_server.c write path W2
    participant L as PlatformRWLock db_lock
    participant S as sql.c
    participant T as table.c
    participant B as bptree.c

    W1->>W1: set request_w1.query to INSERT Alice
    W1->>D1: db_server_execute with first write request
    D1->>D1: classify request_w1 as WRITE
    D1->>D1: mark execution_w1.used_index as false
    D1->>D1: execution_w1.is_write = 1
    D1->>D1: db_server_metrics_query_started(server, DB_SERVER_QUERY_KIND_WRITE)
    D1->>L: try write lock succeeds

    W2->>W2: set request_w2.query to INSERT Bob
    W2->>D2: db_server_execute with second write request
    D2->>D2: classify request_w2 as WRITE
    D2->>D2: mark execution_w2.used_index as false
    D2->>D2: execution_w2.is_write = 1
    D2->>D2: db_server_metrics_query_started(server, DB_SERVER_QUERY_KIND_WRITE)
    D2->>L: try write lock fails

    par W1 keeps the write lock while inserting Alice
        D1->>D1: db_server_apply_simulated_delay(server, DB_SERVER_QUERY_KIND_WRITE)
        D1->>S: sql_execute with request_w1.query
        S->>T: table_insert with Alice age 20
        T->>T: assign next id to record
        T->>B: bptree_insert with new record id
        B-->>T: 1
        T->>T: increase table size
        T-->>S: Record pointer
        S-->>D1: execution_w1.result.action = SQL_ACTION_INSERT
    and W2 polls for write lock
        loop until W1 releases write lock or lock timeout expires
            D2->>D2: platform_sleep_ms(1)
            D2->>L: try write lock again
        end
    end

    D1->>L: unlock write lock
    D1->>D1: finish metrics for execution_w1

    alt W2 obtains lock before timeout
        D2->>L: try write lock succeeds
        D2->>D2: db_server_apply_simulated_delay(server, DB_SERVER_QUERY_KIND_WRITE)
        D2->>S: sql_execute with request_w2.query
        S->>T: table_insert with Bob age 30
        T->>T: assign next id to record
        T->>B: bptree_insert with new record id
        B-->>T: 1
        T->>T: increase table size
        T-->>S: Record pointer
        S-->>D2: execution_w2.result.action = SQL_ACTION_INSERT
        D2->>L: unlock write lock
        D2->>D2: finish metrics for execution_w2
    else W2 waits until lock timeout
        D2-->>W2: execution_w2.server_status = DB_SERVER_EXEC_STATUS_LOCK_TIMEOUT
        D2->>D2: finish metrics for execution_w2
    end
```

#### 화살표 라벨 해설

| 다이어그램 라벨 | 현재 코드 기준 실제 의미 | 매개변수/값의 뜻 |
|---|---|---|
| `set request_w1.query to INSERT Alice` | 첫 번째 writer의 `request.query`가 `INSERT INTO users VALUES ('Alice', 20);`인 상황이다. | `"Alice"`와 `20`은 SQL 파서가 꺼내서 `table_insert()`로 넘길 값이다. |
| `db_server_execute with first write request` | 실제 호출은 `db_server_execute(&context->db_server, request.query, &execution)`이다. | 첫 번째 worker도 공유 `DBServer`의 같은 `db_lock`을 사용한다. |
| `classify request_w1 as WRITE` | `db_server_classify_query()`가 `INSERT`를 보고 `DB_SERVER_QUERY_KIND_WRITE`를 반환한다. | write lock을 사용하라는 신호다. |
| `mark execution_w1.used_index as false` | `db_server_guess_uses_index()`가 INSERT에 대해 0을 반환한다. | INSERT는 SELECT 인덱스 조회가 아니므로 `usedIndex:false`다. |
| `try write lock succeeds` | W1의 `platform_rwlock_try_write_lock(&server->db_lock)`가 먼저 성공한다. | W1이 테이블 변경 권한을 독점한다. |
| `set request_w2.query to INSERT Bob` | 두 번째 writer의 `request.query`가 `INSERT INTO users VALUES ('Bob', 30);`인 상황이다. | `"Bob"`과 `30`은 두 번째 INSERT의 row 값이다. |
| `db_server_execute with second write request` | W2도 `db_server_execute(&context->db_server, request.query, &execution)`로 들어온다. | W1과 같은 공유 테이블을 쓰려 하므로 같은 write lock을 기다린다. |
| `try write lock fails` | W2의 `platform_rwlock_try_write_lock(&server->db_lock)`가 실패한다. | 이미 W1이 write lock을 잡고 있어서 writer끼리는 동시에 실행될 수 없다. |
| `db_server_apply_simulated_delay(server, DB_SERVER_QUERY_KIND_WRITE)` | W1이 write lock을 잡은 상태에서 선택적으로 sleep한다. | `server->config.simulate_write_delay_ms`가 쓰기 지연 시간이다. |
| `sql_execute with request_w1.query` | W1이 `sql_execute(server->table, request.query)`로 Alice INSERT를 실행한다. | `server->table`은 공유 테이블, `request.query`는 Alice INSERT SQL이다. |
| `table_insert with Alice age 20` | 실제 호출은 `table_insert(table, "Alice", 20)`이다. | 새 `Record`를 만들고 `id`, `name`, `age`를 채운다. |
| `assign next id to record` | `record->id = table->next_id++`가 실행된다. | write lock 안에서 실행되므로 id 증가가 원자적인 순서처럼 보호된다. |
| `bptree_insert with new record id` | 실제 호출은 `bptree_insert(table->pk_index, record->id, record)`이다. | rows 배열뿐 아니라 id 인덱스에도 새 record를 연결한다. |
| `increase table size` | `table->size++`로 저장된 row 개수를 늘린다. | 이후 SELECT 전체 조회나 선형 검색 범위가 넓어진다. |
| `platform_sleep_ms(1)` | W2가 lock 재시도 사이에 1ms 쉰다. | `db_server_try_acquire_lock()` loop의 대기 동작이다. |
| `try write lock again` | W2가 `platform_rwlock_try_write_lock(&server->db_lock)`를 반복 시도한다. | W1이 unlock하기 전에는 계속 실패할 수 있다. |
| `unlock write lock` | W1 또는 W2가 작업을 마치고 `platform_rwlock_write_unlock(&server->db_lock)`를 호출한다. | 다음 reader 또는 writer가 lock을 얻을 수 있게 된다. |
| `sql_execute with request_w2.query` | W2가 lock을 얻은 뒤 Bob INSERT를 실행한다. | SQL 문자열은 `INSERT INTO users VALUES ('Bob', 30);`이다. |
| `table_insert with Bob age 30` | 실제 호출은 `table_insert(table, "Bob", 30)`이다. | Alice가 먼저 들어갔다면 Bob은 다음 `next_id` 값을 받는다. |
| `execution_w2.server_status = DB_SERVER_EXEC_STATUS_LOCK_TIMEOUT` | W2가 timeout까지 write lock을 얻지 못한 분기다. | SQL은 실행되지 않고, API 응답은 `503 lock_timeout`으로 만들어진다. |

이 흐름에서 `record->id = table->next_id++`가 한 worker씩만 실행되는 이유는 write lock 덕분이다. 빈 테이블에서 시작했다면 예시처럼 첫 INSERT는 `insertedId:1`, 두 번째 INSERT는 `insertedId:2`가 되지만, 이미 데이터가 있으면 현재 `table->next_id` 값부터 이어진다.

## 10. 핵심 자료구조를 초심자 눈높이로 보기

### `Record`

```c
typedef struct Record {
    int id;
    char name[RECORD_NAME_SIZE];
    int age;
} Record;
```

테이블의 한 행이다. SQL로 보면 `users(id, name, age)` 한 줄이다.

### `Table`

```c
typedef struct Table {
    int next_id;
    Record **rows;
    size_t size;
    size_t capacity;
    BPTree *pk_index;
} Table;
```

`rows`는 `Record *`들을 담는 동적 배열이다. `pk_index`는 `id`로 빠르게 찾기 위한 B+Tree다. `next_id`는 다음 INSERT에 붙일 자동 증가 id다.

### `SQLResult`

```c
typedef struct SQLResult {
    SQLStatus status;
    SQLAction action;
    Record *record;
    Record **records;
    int inserted_id;
    size_t row_count;
    int error_code;
    char sql_state[SQL_SQLSTATE_SIZE];
    char error_message[SQL_ERROR_MESSAGE_SIZE];
} SQLResult;
```

SQL 실행 결과를 담는 봉투다. 성공인지, INSERT인지 SELECT인지, 몇 행이 나왔는지, 오류 메시지가 무엇인지 한 번에 담는다.

### `DBServerExecution`

```c
typedef struct DBServerExecution {
    SQLResult result;
    int used_index;
    int is_write;
    DBServerExecStatus server_status;
    char message[128];
} DBServerExecution;
```

`SQLResult`에 서버 관점의 정보를 더 붙인 결과다. 예를 들어 SQL 자체는 맞아도 lock을 못 얻으면 `server_status`가 `LOCK_TIMEOUT`이 될 수 있다.

### `Record *record`와 `Record **records`

초심자에게 가장 헷갈릴 수 있는 부분이다.

```text
Record *record:
  Record 하나를 가리킨다.
  SELECT 결과의 첫 번째 행을 빠르게 보려고 둔 convenience pointer다.

Record **records:
  Record* 여러 개를 담는 배열을 가리킨다.
  SELECT 결과가 여러 행일 때 사용한다.
```

중요한 점은 `SQLResult.records` 배열은 `SQLResult`가 해제해야 하지만, 그 안의 `Record` 자체는 `Table`이 소유한다는 것이다. 그래서 호출자는 `sql_result_destroy()`를 호출해 결과 배열만 정리하고, 실제 row 객체는 `table_destroy()` 때 정리된다.

### 소유권 그림

C에는 자동 메모리 관리가 없다. 그래서 "누가 만들고 누가 해제하는가"가 중요하다.

```mermaid
flowchart TB
    DB["DBServer"] -->|"owns"| Table["Table"]
    DB -->|"owns"| Lock["db_lock / metrics_mutex"]
    Table -->|"owns"| Rows["rows 배열"]
    Rows -->|"owns"| Records["Record 객체들"]
    Table -->|"owns"| Tree["B+Tree"]
    SQLResult["SQLResult"] -->|"may own"| ResultRows["records 결과 배열"]
    APIResponse["APIResponse"] -->|"owns"| Body["malloc된 JSON body"]

    D1["db_server_destroy()"] -.해제.-> DB
    D2["sql_result_destroy()"] -.해제.-> SQLResult
    D3["api_response_destroy()"] -.해제.-> APIResponse
```

읽을 때는 `*_destroy()` 함수가 무엇을 해제하는지 꼭 같이 보면 좋다.

## 11. HTTP 요청 하나가 실제로 처리되는 길

`POST /query`는 `http_server_handle_client()`까지는 같은 길로 들어온다. 이후 `request.query` 문자열이 `INSERT`인지 `SELECT`인지에 따라 `db_server_execute()` 안에서 lock 종류와 table 함수 호출이 달라진다.

아래 diagram은 실제 코드의 함수 이름과 대표 파라미터를 그대로 써서 그린 것이다.

### INSERT 요청

```mermaid
sequenceDiagram
    participant C as Client
    participant W as worker thread
    participant H as http_server.c
    participant A as api.c
    participant D as db_server.c
    participant S as sql.c
    participant T as table.c
    participant B as bptree.c

    C->>H: TCP connection request to server port
    H->>H: http_server_socket_wait_for_read(listen_socket, 200)
    H->>H: accept(listen_socket, NULL, NULL)
    H->>H: push client_socket into request queue
    W->>H: pop client_socket from request queue
    H-->>W: client_socket
    W->>H: http_server_handle_client(context, client_socket)
    C->>H: POST query with INSERT Alice body
    H->>H: http_server_read_request(client_socket, request_buffer, sizeof(request_buffer), error_message, sizeof(error_message))
    H->>A: api_parse_http_request with request_buffer
    A-->>H: request contains POST path and INSERT query
    H->>D: db_server_execute with parsed request
    D->>D: classify request as WRITE
    D->>D: used_index is false
    D->>D: db_server_metrics_query_started(server, DB_SERVER_QUERY_KIND_WRITE)
    D->>D: db_server_try_acquire_lock(server, DB_SERVER_QUERY_KIND_WRITE)
    D->>D: try write lock on db_lock
    D->>S: sql_execute with INSERT Alice query
    S->>S: sql_execute_insert(table, input)
    S->>T: table_insert with Alice age 20
    T->>B: bptree_insert with new record id
    B-->>T: 1
    T-->>S: inserted Record with id 1
    S-->>D: SQLResult
    D->>D: db_server_release_lock(server, DB_SERVER_QUERY_KIND_WRITE)
    D->>D: finish metrics for execution
    D-->>H: execution.result.action = SQL_ACTION_INSERT, execution.result.inserted_id = 1
    H->>A: build execution response
    A-->>H: response status 200 with insert JSON
    H->>H: send API response
    H->>A: render raw HTTP response
    H->>H: http_server_socket_send_all(client_socket, raw_response, strlen(raw_response))
    H-->>C: HTTP/1.1 200 OK + JSON insert result
    H->>D: destroy DB execution
    H->>A: destroy API response
```

#### 화살표 라벨 해설

| 다이어그램 라벨 | 현재 코드 기준 실제 의미 | 매개변수/값의 뜻 |
|---|---|---|
| `TCP connection request to server port` | client가 서버 port로 TCP 연결을 요청한다. | 서버 쪽에서는 listening socket이 이 연결 요청을 감지한다. |
| `http_server_socket_wait_for_read(listen_socket, 200)` | `select()`로 listening socket에 새 연결이 왔는지 기다린다. | `listen_socket`은 서버용 socket, `200`은 timeout 밀리초다. |
| `accept(listen_socket, NULL, NULL)` | 새 client 연결을 받아 `client_socket`을 만든다. | `NULL, NULL`은 client 주소 정보를 따로 받지 않겠다는 뜻이다. |
| `push client_socket into request queue` | 실제 호출은 `http_request_queue_push(&context.queue, client_socket)`이다. | `context.queue`는 worker가 꺼내 갈 socket queue, `client_socket`은 방금 accept한 연결이다. |
| `pop client_socket from request queue` | 실제 호출은 `http_request_queue_pop(&context->queue)`이다. | worker thread가 처리할 socket 하나를 queue에서 꺼낸다. |
| `client_socket` | queue에서 꺼낸 connected socket 값이 worker에게 전달된다는 뜻이다. | 이후 `recv()`와 `send()`는 모두 이 `client_socket`에 대해 수행된다. |
| `http_server_handle_client(context, client_socket)` | worker가 요청 하나를 처리하는 핵심 함수다. | `context`에는 공유 `DBServer`, queue 상태, 완료 카운터가 있고, `client_socket`은 요청/응답용 연결이다. |
| `POST query with INSERT Alice body` | 실제 HTTP body는 예를 들어 `{"query":"INSERT INTO users VALUES ('Alice', 20);"}`이다. | `POST /query` endpoint는 JSON body 안의 `query` 문자열만 SQL로 사용한다. |
| `http_server_read_request(client_socket, request_buffer, sizeof(request_buffer), error_message, sizeof(error_message))` | socket에서 header와 body를 읽어 raw HTTP 문자열로 만든다. | `request_buffer`는 수신 버퍼, `sizeof(request_buffer)`는 버퍼 한계, `error_message`는 실패 이유 저장 버퍼다. |
| `api_parse_http_request with request_buffer` | 실제 호출은 `api_parse_http_request(request_buffer, &request, error_message, sizeof(error_message))`이다. | raw HTTP를 `APIRequest request` 구조체로 파싱한다. |
| `request contains POST path and INSERT query` | 파싱 결과가 `request.method = API_METHOD_POST`, `request.path = "/query"`, `request.query = "INSERT INTO users VALUES ('Alice', 20);"`가 된다는 뜻이다. | `request.query`가 다음 단계의 SQL 입력이 된다. |
| `db_server_execute with parsed request` | 실제 호출은 `db_server_execute(&context->db_server, request.query, &execution)`이다. | 공유 DB 서버 상태, SQL 문자열, 실행 결과를 받을 `DBServerExecution`을 넘긴다. |
| `classify request as WRITE` | `db_server_classify_query(request.query)`가 `INSERT`를 보고 `DB_SERVER_QUERY_KIND_WRITE`를 반환한다. | write lock을 잡아야 한다는 의미다. |
| `used_index is false` | `db_server_guess_uses_index(request.query)`가 0을 반환한다. | INSERT는 id 인덱스를 조회하는 SELECT가 아니므로 `usedIndex:false`다. |
| `db_server_metrics_query_started(server, DB_SERVER_QUERY_KIND_WRITE)` | INSERT 요청 시작 metrics를 올린다. | `total_query_requests`, `active_query_requests`, `total_insert_requests`가 증가한다. |
| `db_server_try_acquire_lock(server, DB_SERVER_QUERY_KIND_WRITE)` | write lock을 얻을 때까지 시도하거나 timeout을 판단한다. | `server`는 lock과 timeout 설정을 가진 `DBServer *`, 두 번째 인자는 write lock 경로 선택값이다. |
| `try write lock on db_lock` | 내부 호출은 `platform_rwlock_try_write_lock(&server->db_lock)`이다. | 다른 reader/writer가 없을 때만 성공한다. |
| `sql_execute with INSERT Alice query` | 실제 호출은 `sql_execute(server->table, request.query)`이다. | `server->table`은 공유 테이블, `request.query`는 INSERT SQL이다. |
| `sql_execute_insert(table, input)` | `sql_execute()` 안에서 INSERT 문법을 파싱하고 실행하는 static 함수다. | `table`은 저장 대상, `input`은 `INSERT INTO users VALUES ('Alice', 20);` 문자열이다. |
| `table_insert with Alice age 20` | 실제 호출은 `table_insert(table, "Alice", 20)`이다. | SQL에서 파싱한 name과 age를 새 `Record`에 저장한다. |
| `bptree_insert with new record id` | 실제 호출은 `bptree_insert(table->pk_index, record->id, record)`이다. | 자동 생성된 id를 key로 삼아 B+Tree에 `Record *`를 저장한다. |
| `1` | `bptree_insert()`가 성공을 뜻하는 `1`을 반환한다. | 중복 key이거나 내부 오류면 0이 될 수 있지만, `next_id`로 만든 새 id라 일반적으로 성공한다. |
| `inserted Record with id 1` | `table_insert()`가 새 `Record *`를 반환하고, 그 record의 id가 1인 예시다. | 빈 테이블에서 첫 INSERT라면 `record->id = 1`이 된다. |
| `SQLResult` | `sql_execute()`가 `SQLResult`를 반환한다. | INSERT 성공이면 `status = SQL_STATUS_OK`, `action = SQL_ACTION_INSERT`, `inserted_id = 1`이다. |
| `db_server_release_lock(server, DB_SERVER_QUERY_KIND_WRITE)` | SQL 실행 후 write lock을 해제한다. | 내부에서 `platform_rwlock_write_unlock(&server->db_lock)`가 호출된다. |
| `finish metrics for execution` | 실제 호출은 `db_server_metrics_query_finished(server, &execution)`이다. | active query 수를 줄이고 실행 결과별 metrics를 반영한다. |
| `execution.result.action = SQL_ACTION_INSERT, execution.result.inserted_id = 1` | `db_server_execute()`가 HTTP layer로 넘기는 실행 결과 요약이다. | API 응답 생성 함수가 `inserted_id`를 JSON의 `insertedId`로 사용한다. |
| `build execution response` | 실제 호출은 `api_build_execution_response(&execution, &response)`이다. | `execution`을 읽어 HTTP status와 JSON body를 채운 `APIResponse`를 만든다. |
| `response status 200 with insert JSON` | INSERT 성공 시 body는 `{"ok":true,"status":"ok","action":"insert","insertedId":1,"usedIndex":false}` 모양이다. | `response.status_code = 200`, `response.body`가 동적 할당된 JSON 문자열이다. |
| `send API response` | 실제 호출은 `http_server_send_response(client_socket, &response)`이다. | 이 wrapper 안에서 HTTP 문자열 렌더링과 socket 전송이 일어난다. |
| `render raw HTTP response` | 실제 호출은 `api_render_http_response(&response, &raw_response)`이다. | `APIResponse`를 `HTTP/1.1 200 OK`, header, body가 합쳐진 문자열로 만든다. |
| `http_server_socket_send_all(client_socket, raw_response, strlen(raw_response))` | raw HTTP 응답 전체를 TCP socket으로 보낸다. | `strlen(raw_response)`만큼 모두 전송할 때까지 `send()`를 반복한다. |
| `HTTP/1.1 200 OK + JSON insert result` | client가 받는 최종 응답이다. | status line, `Content-Type`, `Content-Length`, JSON body가 포함된다. |
| `destroy DB execution` | 실제 호출은 `db_server_execution_destroy(&execution)`이다. | `SQLResult`가 가진 `records` 같은 heap 메모리를 정리하고 구조체를 초기화한다. |
| `destroy API response` | 실제 호출은 `api_response_destroy(&response)`이다. | `response.body`를 `free()`하고 response 필드를 초기화한다. |

이 INSERT 흐름에서 가장 중요한 실제 파라미터는 `table_insert(table, "Alice", 20)`이다. `id`는 요청 body에 없고, `table_insert()` 안에서 `record->id = table->next_id++`로 자동 생성된다.

### SELECT 요청

아래 예시는 바로 위 INSERT로 `id = 1`인 `Alice` row가 이미 들어간 뒤의 요청이다.

```mermaid
sequenceDiagram
    participant C as Client
    participant W as worker thread
    participant H as http_server.c
    participant A as api.c
    participant D as db_server.c
    participant S as sql.c
    participant T as table.c
    participant B as bptree.c

    C->>H: TCP connection request to server port
    H->>H: http_server_socket_wait_for_read(listen_socket, 200)
    H->>H: accept(listen_socket, NULL, NULL)
    H->>H: push client_socket into request queue
    W->>H: pop client_socket from request queue
    H-->>W: client_socket
    W->>H: http_server_handle_client(context, client_socket)
    C->>H: POST query with SELECT by id body
    H->>H: http_server_read_request(client_socket, request_buffer, sizeof(request_buffer), error_message, sizeof(error_message))
    H->>A: api_parse_http_request with request_buffer
    A-->>H: request contains POST path and SELECT query
    H->>D: db_server_execute with parsed request
    D->>D: classify request as READ
    D->>D: used_index is true
    D->>D: db_server_metrics_query_started(server, DB_SERVER_QUERY_KIND_READ)
    D->>D: db_server_try_acquire_lock(server, DB_SERVER_QUERY_KIND_READ)
    D->>D: try read lock on db_lock
    D->>S: sql_execute with SELECT by id query
    S->>S: sql_execute_select(table, input)
    S->>T: table_find_by_id_condition with id equals 1
    T->>T: table_find_by_id(table, 1)
    T->>B: bptree_search with key 1
    B-->>T: Record* Alice
    T-->>S: one Alice row
    S-->>D: SQLResult
    D->>D: db_server_release_lock(server, DB_SERVER_QUERY_KIND_READ)
    D->>D: finish metrics for execution
    D-->>H: execution.used_index = 1, execution.result.action = SQL_ACTION_SELECT_ROWS
    H->>A: build execution response
    A-->>H: response status 200 with select JSON
    H->>H: send API response
    H->>A: render raw HTTP response
    H->>H: http_server_socket_send_all(client_socket, raw_response, strlen(raw_response))
    H-->>C: HTTP/1.1 200 OK + JSON select result
    H->>D: destroy DB execution
    H->>A: destroy API response
```

#### 화살표 라벨 해설

| 다이어그램 라벨 | 현재 코드 기준 실제 의미 | 매개변수/값의 뜻 |
|---|---|---|
| `TCP connection request to server port` | client가 서버 port로 TCP 연결을 요청한다. | INSERT와 마찬가지로 listening socket이 새 연결을 감지한다. |
| `http_server_socket_wait_for_read(listen_socket, 200)` | `select()`로 새 연결 준비 여부를 확인한다. | `200`은 서버 loop가 stop 요청도 확인할 수 있게 하는 짧은 대기 시간이다. |
| `accept(listen_socket, NULL, NULL)` | 연결 요청을 받아 `client_socket`을 만든다. | 이 socket은 해당 client와의 실제 데이터 송수신에 쓰인다. |
| `push client_socket into request queue` | `http_request_queue_push(&context.queue, client_socket)`로 worker queue에 넣는다. | queue가 가득 차면 `503 queue_full` 경로로 갈 수 있다. |
| `pop client_socket from request queue` | worker가 `http_request_queue_pop(&context->queue)`로 socket을 꺼낸다. | queue가 비어 있으면 worker는 condition variable에서 기다린다. |
| `http_server_handle_client(context, client_socket)` | worker가 HTTP 요청 하나를 읽고, API 처리와 SQL 실행, 응답 전송까지 담당한다. | `context`의 `db_server`가 모든 worker 사이에서 공유된다. |
| `POST query with SELECT by id body` | 실제 HTTP body는 예를 들어 `{"query":"SELECT * FROM users WHERE id = 1;"}`이다. | `request.query`에 들어갈 SQL 문자열이다. |
| `http_server_read_request(client_socket, request_buffer, sizeof(request_buffer), error_message, sizeof(error_message))` | socket에서 raw HTTP 요청을 읽는다. | Content-Length를 보고 body까지 들어왔는지 확인한다. |
| `api_parse_http_request with request_buffer` | 실제 호출은 `api_parse_http_request(request_buffer, &request, error_message, sizeof(error_message))`이다. | request line, headers, JSON body를 해석해 `APIRequest`를 채운다. |
| `request contains POST path and SELECT query` | 파싱 결과가 `request.method = API_METHOD_POST`, `request.path = "/query"`, `request.query = "SELECT * FROM users WHERE id = 1;"`가 된다는 뜻이다. | 이 SELECT 문자열이 DB layer로 전달된다. |
| `db_server_execute with parsed request` | 실제 호출은 `db_server_execute(&context->db_server, request.query, &execution)`이다. | 공유 테이블을 대상으로 SELECT를 실행하고 `execution`에 결과를 담는다. |
| `classify request as READ` | `db_server_classify_query(request.query)`가 `SELECT`를 보고 `DB_SERVER_QUERY_KIND_READ`를 반환한다. | read lock 경로로 들어간다. |
| `used_index is true` | `db_server_guess_uses_index(request.query)`가 `WHERE id`를 보고 1을 반환한다. | API 응답의 `usedIndex:true`가 된다. |
| `db_server_metrics_query_started(server, DB_SERVER_QUERY_KIND_READ)` | SELECT 요청 시작 metrics를 올린다. | `total_query_requests`, `active_query_requests`, `total_select_requests`가 증가한다. |
| `db_server_try_acquire_lock(server, DB_SERVER_QUERY_KIND_READ)` | read lock을 얻을 때까지 시도하거나 timeout을 판단한다. | reader끼리는 동시에 성공할 수 있지만 writer가 있으면 기다린다. |
| `try read lock on db_lock` | 내부 호출은 `platform_rwlock_try_read_lock(&server->db_lock)`이다. | `&server->db_lock`은 공유 `Table` 접근을 보호한다. |
| `sql_execute with SELECT by id query` | 실제 호출은 `sql_execute(server->table, request.query)`이다. | `request.query`는 `SELECT * FROM users WHERE id = 1;`이다. |
| `sql_execute_select(table, input)` | `sql_execute()` 안에서 SELECT 문법을 파싱하고 실행하는 static 함수다. | `table`은 조회 대상, `input`은 SELECT SQL 문자열이다. |
| `table_find_by_id_condition with id equals 1` | 실제 호출은 `table_find_by_id_condition(table, TABLE_COMPARISON_EQ, 1, &result.records, &result.row_count)`이다. | `TABLE_COMPARISON_EQ`와 `1`은 `WHERE id = 1`에서 온 값이다. |
| `table_find_by_id(table, 1)` | id 단건 검색을 수행한다. | 내부에서 B+Tree 인덱스를 사용한다. |
| `bptree_search with key 1` | 실제 호출은 `bptree_search(table->pk_index, 1)`이다. | id 인덱스에서 key 1에 연결된 `Record *`를 찾는다. |
| `Record* Alice` | B+Tree에서 찾은 row 포인터가 Alice record인 예시다. | 직전 INSERT로 Alice가 id 1을 받았다는 전제다. |
| `one Alice row` | `result.records[0]`에 Alice `Record *`가 들어가고 `result.row_count = 1`이 된다는 뜻이다. | row가 없으면 `SQL_STATUS_NOT_FOUND`가 될 수 있다. |
| `SQLResult` | `sql_execute()`가 SELECT 실행 결과를 반환한다. | 성공이면 `action = SQL_ACTION_SELECT_ROWS`, `records`와 `row_count`가 채워진다. |
| `db_server_release_lock(server, DB_SERVER_QUERY_KIND_READ)` | SELECT 실행 후 read lock을 해제한다. | 내부에서 `platform_rwlock_read_unlock(&server->db_lock)`가 호출된다. |
| `finish metrics for execution` | `db_server_metrics_query_finished(server, &execution)`가 실행 종료 metrics를 반영한다. | not found, syntax error, lock timeout 같은 결과도 여기서 집계된다. |
| `execution.used_index = 1, execution.result.action = SQL_ACTION_SELECT_ROWS` | HTTP layer로 돌아온 실행 결과 요약이다. | 응답 JSON에 `usedIndex:true`, `action:"select"`로 들어간다. |
| `build execution response` | 실제 호출은 `api_build_execution_response(&execution, &response)`이다. | SELECT 결과 rows를 JSON 배열로 직렬화한다. |
| `response status 200 with select JSON` | 성공 body는 `{"ok":true,"status":"ok","action":"select","rowCount":1,"usedIndex":true,"rows":[...]}` 모양이다. | rows 배열에는 `id`, `name`, `age`가 들어간다. |
| `send API response` | 실제 호출은 `http_server_send_response(client_socket, &response)`이다. | wrapper 함수가 렌더링과 전송을 묶어서 처리한다. |
| `render raw HTTP response` | 실제 호출은 `api_render_http_response(&response, &raw_response)`이다. | `Content-Length`를 계산해 HTTP 응답 문자열을 만든다. |
| `http_server_socket_send_all(client_socket, raw_response, strlen(raw_response))` | 응답 문자열을 socket에 끝까지 쓴다. | 내부에서 `send()`가 반복 호출될 수 있다. |
| `HTTP/1.1 200 OK + JSON select result` | client가 받는 최종 SELECT 응답이다. | HTTP header와 JSON body가 같이 전송된다. |
| `destroy DB execution` | `db_server_execution_destroy(&execution)`로 SQL 결과 메모리를 정리한다. | 특히 SELECT의 `result.records` 배열을 해제한다. |
| `destroy API response` | `api_response_destroy(&response)`로 JSON body 메모리를 정리한다. | `response.body`를 `free()`한다. |

초기 커밋에서는 `Client`, `http_server.c`, `api.c`, `db_server.c`가 없었다. 최신 커밋은 기존 `sql.c -> table.c -> bptree.c` 앞뒤에 서버용 입출력 계층을 붙인 것이다.

## 12. PDF 개념별로 현재 코드에서 찾을 곳

| 읽고 싶은 개념 | 먼저 볼 파일 | 보면 좋은 함수/구조체 |
|---|---|---|
| 표준 입출력 | `src/cli/main.c`, `src/server/server.c` | `fgets`, `printf`, `fprintf` |
| socket 생성 | `src/server/http_server.c` | `http_server_create_listen_socket()` |
| 요청 읽기 | `src/server/http_server.c` | `http_server_read_request()` |
| 끝까지 쓰기 | `src/server/http_server.c` | `http_server_socket_send_all()` |
| HTTP 파싱 | `src/server/api.c` | `api_parse_http_request()` |
| HTTP 응답 만들기 | `src/server/api.c` | `api_render_http_response()` |
| worker thread | `src/server/http_server.c` | `http_server_worker_main()` |
| producer-consumer queue | `src/server/http_server.c` | `HTTPRequestQueue`, `push`, `pop` |
| read/write lock | `src/server/db_server.c` | `db_server_try_acquire_lock()` |
| metrics 보호 | `src/server/db_server.c` | `metrics_mutex` |
| OS별 thread/lock 추상화 | `src/server/platform.c` | `platform_*` 함수들 |
| SQL 파싱 | `src/core/sql.c` | `sql_execute_insert()`, `sql_execute_select()` |
| B+Tree 인덱스 | `src/core/bptree.c` | `bptree_insert()`, `bptree_search()` |

## 13. 초심자가 헷갈리기 쉬운 지점

### 1. `src/core`는 thread-safe하지 않다

`sql.c`, `table.c`, `bptree.c` 내부는 lock을 모른다. 동시성 보호는 `db_server.c`에서 한다. 그래서 서버 경계 밖에서 여러 thread가 같은 `Table *`을 직접 건드리면 위험하다.

### 2. `used_index`는 SQL 문자열을 보고 추정한다

`db_server_guess_uses_index()`는 `SELECT ... WHERE id ...` 형태인지 보고 `used_index`를 표시한다. 실제 B+Tree 실행 경로와 대체로 맞지만, 이름 그대로 "서버 계층의 표시값"이다.

### 3. HTTP parser는 학습용 직접 구현이다

`api.c`는 JSON과 HTTP를 완전한 범용 parser로 처리하지 않는다. 현재 API 계약에 필요한 만큼만 직접 파싱한다. 초심자에게는 오히려 흐름을 보기 좋지만, 실무용 범용 HTTP 서버와는 다르다.

### 4. 요청 buffer 크기가 고정되어 있다

`HTTP_SERVER_REQUEST_BUFFER_SIZE`는 `8192`다. 큰 요청은 제한에 걸릴 수 있다. 이 제한은 단순 서버에서 흔히 쓰는 방어선이다.

### 5. 데이터는 메모리에만 있다

현재 `Table`은 파일이나 DB에 저장하지 않는다. 서버를 끄면 데이터도 사라진다. Chapter 10의 파일 I/O 개념을 배웠더라도, 이 프로젝트의 테이블 데이터는 아직 disk persistence가 아니다.

### 6. `EXIT`와 `QUIT`은 CLI용이다

REPL이나 서버 CLI 하네스에서는 종료 명령으로 처리하지만, HTTP API에서는 `query_error`로 거절한다.

## 14. 현재 코드에서 보이는 설계 방향

이 프로젝트는 한 번에 거대한 서버를 만든 것이 아니라, 아래 순서로 확장된 모양이다.

```mermaid
flowchart TD
    A["1. SQL 엔진\nsql_execute(Table*, const char*)"]
    B["2. 저장소\nTable + B+Tree"]
    C["3. CLI REPL\nstdin/stdout"]
    D["4. 서버 경계\nDBServer가 Table 소유"]
    E["5. API 계약\nHTTP request -> SQL -> JSON"]
    F["6. 네트워크\nsocket/bind/listen/accept"]
    G["7. 동시성\nbounded queue + worker threads"]
    H["8. 운영 신호\nmetrics, queue_full, lock_timeout"]

    A --> B --> C --> D --> E --> F --> G --> H
```

좋은 점은 `src/core`가 서버를 모르도록 유지했다는 것이다. 덕분에 CLI, 단위 테스트, HTTP 서버가 같은 SQL 엔진을 재사용한다.

주의할 점은 서버 기능이 늘어나면서 `http_server.c`에 여러 개념이 모여 있다는 것이다. 이 파일을 읽을 때는 "소켓", "큐", "worker", "응답 직렬화"를 한꺼번에 이해하려 하지 말고, 위 표의 함수 단위로 나눠 보는 편이 좋다.

## 15. 직접 실행해 볼 명령

빌드:

```bash
make
```

단위 테스트:

```bash
./build/bin/unit_test
```

기대 출력:

```text
All unit tests passed.
```

기존 REPL:

```bash
./build/bin/main
```

서버 CLI 하네스:

```bash
./build/bin/server \
  --query "INSERT INTO users VALUES ('Alice', 20);" \
  --query "SELECT * FROM users WHERE id = 1;" \
  --query "QUIT"
```

기대 출력:

```text
OK INSERT id=1 used_index=false
OK SELECT rows=1 used_index=true
ROW id=1 name=Alice age=20
BYE
```

HTTP 서버:

```bash
./build/bin/server --serve --port 8080 --workers 4 --queue 16
```

다른 터미널에서 확인:

```bash
curl http://localhost:8080/health
curl http://localhost:8080/metrics
curl -X POST http://localhost:8080/query \
  -H "Content-Type: application/json" \
  -d "{\"query\":\"INSERT INTO users VALUES ('Alice', 20);\"}"
```

작은 3-step 실습:

```bash
curl -X POST http://localhost:8080/query \
  -H "Content-Type: application/json" \
  -d "{\"query\":\"INSERT INTO users VALUES ('Alice', 20);\"}"

curl -X POST http://localhost:8080/query \
  -H "Content-Type: application/json" \
  -d "{\"query\":\"SELECT * FROM users WHERE id = 1;\"}"

curl http://localhost:8080/metrics
```

기대 흐름은 첫 요청에서 `insertedId`가 나오고, 두 번째 요청에서 `usedIndex:true`와 `Alice` row가 나오고, 세 번째 요청에서 `totalQueryRequests` 같은 metrics 숫자가 증가한 것을 보는 것이다.

## 16. 비슷하지만 같은 것은 아님

PDF 개념과 코드를 연결할 때는 "닮았다"와 "동일하다"를 구분해야 한다.

| PDF 개념 | 현재 코드와 닮은 점 | 다른 점 |
|---|---|---|
| RIO `rio_writen` | `http_server_socket_send_all()`이 전체 응답을 보낼 때까지 반복한다. | textbook RIO 패키지를 구현한 것은 아니고, 범용 robust read/write wrapper도 아니다. |
| Tiny Web Server | HTTP request를 읽고 HTTP response를 보낸다. | 정적 파일, CGI, MIME type 처리는 없다. SQL JSON API 서버다. |
| prethreaded server | worker thread를 미리 만들고 queue에서 일을 꺼낸다. | textbook의 semaphore 기반 `sbuf` 그대로는 아니고, `PlatformMutex`와 `PlatformCond`로 직접 queue를 구현한다. |
| readers-writers problem | `SELECT` reader, `INSERT` writer처럼 공유 `Table` 접근을 나눈다. | textbook semaphore 풀이가 아니라 `pthread_rwlock_t` 또는 Windows wrapper를 사용한다. |

## 17. PDF와 함께 읽는 빠른 체크리스트

Chapter 10을 읽을 때:

- `stdin/stdout/stderr`가 코드에서 어디에 있는지 찾는다.
- `recv`와 `send`가 파일 I/O처럼 "바이트를 읽고 쓰는 함수"라는 점을 연결한다.
- `send_all()`이 왜 반복문을 쓰는지 생각한다.

Chapter 11을 읽을 때:

- `socket -> bind -> listen -> accept` 순서를 `http_server_create_listen_socket()`과 main accept loop에서 찾는다.
- listening socket과 client socket을 구분한다.
- HTTP request line, header, body가 `api_parse_http_request()`에서 어떻게 나뉘는지 본다.

Chapter 12를 읽을 때:

- accept loop가 producer, worker thread가 consumer라는 점을 본다.
- `HTTPRequestQueue`가 bounded buffer라는 점을 본다.
- `metrics_mutex`와 `db_lock`이 어떤 공유 데이터를 보호하는지 표시해 본다.
- `SELECT`와 `INSERT`가 readers-writers 문제의 reader/writer에 대응된다는 점을 연결한다.

## 18. 최종 mental model

현재 코드베이스를 한 문장으로 기억하면 된다.

> 이 프로젝트는 C로 만든 작은 in-memory SQL 엔진을 중심에 두고, 그 앞에 HTTP/network/concurrency 계층을 붙여 여러 클라이언트 요청을 안전하게 처리하는 학습용 SQL API 서버다.

더 짧게는:

```text
문자열 SQL
  -> sql_execute()
  -> Table / B+Tree
  -> SQLResult
  -> JSON
  -> HTTP response
```

그리고 최신 커밋에서 가장 중요한 변화는 이 흐름이 한 사람의 터미널 입력뿐 아니라, 여러 네트워크 클라이언트의 동시 요청에도 대응하도록 확장되었다는 점이다.
