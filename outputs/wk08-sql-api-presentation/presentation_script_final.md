# 발표 대본 - 전동환

> README.md를 화면에 띄운 기준입니다. `*`는 다음 섹션 전환 기준입니다.

여러 클라이언트가 동시에 같은 in-memory DB에 SQL 요청을 보내면 어떤 일이 생길까요?

`POST /query` endpoint 하나를 붙이는 일처럼 보일 수 있지만, 서버가 되는 순간 문제는 달라집니다. 요청을 어디에서 줄 세우고, worker들이 같은 테이블을 어떻게 안전하게 만지고, 실패 상황을 client에게 어떻게 알려줄지가 핵심입니다.

안녕하세요. 미니 DBMS SQL API 서버를 발표할 4조 전동환입니다. 오늘 발표의 핵심은 SQL 엔진을 새로 만든 것이 아니라, 기존 C SQL 처리기 앞에 안전한 API 서버 경계를 세운 과정입니다.

*
# 1. 요구사항과 구현 매핑

먼저 과제 요구사항과 구현을 연결해서 보겠습니다.

이번 과제는 외부 클라이언트가 DBMS 기능을 사용할 수 있어야 하고, thread pool로 SQL 요청을 병렬 처리해야 하며, 이전 차수의 SQL 처리기와 B+Tree 인덱스를 그대로 활용해야 했습니다. 그래서 `src/core/`의 `sql_execute()`, `Table`, `B+Tree`는 유지하고, `src/server/`에 HTTP endpoint, bounded queue, worker thread pool, read/write lock, timeout, metrics, JSON 응답을 추가했습니다.

즉 발표의 중심은 SQL 문법 자체가 아니라, 여러 client가 하나의 DB 엔진을 동시에 사용할 수 있게 만드는 서버 구조입니다.

*
# 2. 전체 구조

README의 첫 번째 다이어그램을 왼쪽에서 오른쪽으로 보시면 됩니다.

client 연결은 accept loop가 받고, 바로 SQL을 실행하지 않습니다. 먼저 bounded request queue에 socket을 넣습니다. worker thread는 queue에서 socket을 꺼내 HTTP 요청을 읽고, `api_parse_http_request()`로 method, path, JSON body의 `query`를 파싱합니다.

그 다음 `db_server_execute()`로 들어갑니다. 여기에서 SQL을 분류하고, 공유 DB에 들어가기 전에 lock과 timeout, metrics를 관리합니다. 마지막으로 기존 `sql_execute()`가 `Table`과 B+Tree를 사용해 실제 SQL을 실행합니다.

중요한 점은 `src/core/`가 HTTP, socket, lock을 모른다는 것입니다. 동시성 정책을 서버 경계에 모아 두었기 때문에 기존 엔진을 크게 흔들지 않고 API 서버만 설명할 수 있었습니다.

*
# 3. 요청 하나가 처리되는 길

요청 하나를 더 작게 따라가 보겠습니다.

client가 연결하면 `accept()`가 socket을 받고, `http_request_queue_push()`가 queue에 넣습니다. worker는 queue에서 꺼낸 요청을 HTTP 파싱부터 JSON 응답 전송까지 끝까지 처리합니다.

queue를 둔 이유는 backpressure입니다. worker가 처리할 수 있는 양보다 요청이 더 빨리 들어오면 무한히 쌓지 않고, queue가 가득 찬 순간 `503 queue_full`로 거절합니다. 이 실패는 worker에게 넘어가기 전, accept loop 쪽에서 발생합니다.

반대로 worker까지 배정됐지만 DB lock을 너무 오래 기다리는 경우도 있습니다. 이때는 `db_server_execute()`가 설정된 시간을 넘겼다고 판단하고 SQL을 실행하지 않은 채 `503 lock_timeout`을 돌려줍니다.

*
# 4. 동시성 문제와 해결

이제 왜 lock이 필요한지 보겠습니다.

공유 자원은 하나의 in-memory `Table *`입니다. `INSERT` 중에 `SELECT`가 읽으면 반쯤 갱신된 record를 볼 수 있고, `INSERT` 두 개가 동시에 실행되면 같은 `next_id`를 쓰거나 B+Tree 갱신이 충돌할 수 있습니다.

그래서 `SELECT`는 read lock, `INSERT`는 write lock 경로로 보냈습니다. `SELECT`끼리는 테이블을 수정하지 않으므로 함께 실행할 수 있고, `INSERT`는 테이블과 B+Tree를 바꾸므로 한 번에 하나만 실행합니다.

mutex만 쓰면 안전하지만 모든 `SELECT`도 한 줄로 서야 합니다. 이 프로젝트에는 "읽기는 같이, 쓰기는 혼자"라는 규칙이 필요했기 때문에 read/write lock이 맞았습니다.

README의 두 번째 sequence diagram은 Alice의 `SELECT`와 Bob의 `INSERT`가 동시에 들어온 상황입니다. Alice는 read lock을 잡고 먼저 읽고, Bob의 write lock은 reader가 끝날 때까지 기다립니다. reader가 끝나면 Bob이 write lock을 잡고 `INSERT`를 실행합니다.

RwLock만으로 끝내지는 않았습니다. reader가 계속 들어오면 writer가 밀릴 수 있기 때문에, writer가 기다리기 시작하면 새 reader가 계속 앞질러 들어가지 못하게 `phase gate`로 차례를 조절했습니다. lock 대기가 길어지면 `503 lock_timeout`으로 실패시켜 서버가 멈춘 것처럼 보이지 않게 했습니다.

*
# 5. HTTP API와 SQL 범위

외부에 공개한 endpoint는 세 가지입니다.

`GET /health`는 서버 생존 확인, `GET /metrics`는 query 수와 queue full, lock timeout 같은 counter 확인, `POST /query`는 SQL 실행입니다.

예를 들어 `SELECT * FROM users WHERE id = 1;`을 보내면 JSON 응답에 `rows`, `rowCount`, `usedIndex: true`가 포함됩니다. 오류도 `syntax_error`, `query_error`, `queue_full`, `lock_timeout`처럼 HTTP status와 함께 구분합니다.

지원 SQL은 기존 엔진의 범위와 같습니다. 테이블은 `users(id, name, age)` 하나이고, `INSERT INTO users VALUES (...)`, 전체 SELECT, 그리고 `id`, `name`, `age` 조건 SELECT를 지원합니다. `id` 조건은 B+Tree index를 사용하고, `name`과 `age` 조건은 linear scan입니다.

*
# 6. 시연과 검증

시연에서는 `make` 후 `./build/bin/server --serve --port 8080 --workers 4 --queue 16`으로 서버를 띄우고, `/health`, `POST /query` INSERT, indexed SELECT, `/metrics` 순서로 확인하면 됩니다.

검증은 `make unit_test`와 `./build/bin/unit_test`로 B+Tree, table, SQL 실행, lock timeout, phase gate, metrics, HTTP parser와 JSON 응답을 확인했습니다. 추가로 smoke script와 Postman collection에서 404/405, malformed request, read-only burst, mixed 80/20, write-heavy burst 흐름을 준비했습니다.

*
# 7. 비범위와 결론

마지막으로 이 프로젝트의 비범위도 분명합니다. 다중 테이블, UPDATE, DELETE, join, transaction, 영속화, auth 같은 기능은 만들지 않았습니다.

결론적으로 저희 구현은 새 DBMS 전체가 아니라, 기존 SQL 엔진 앞에 설명 가능한 API 서버 경계를 세운 프로젝트입니다. thread pool, bounded queue, read/write lock, phase gate, timeout, backpressure, metrics를 연결해서 동시에 요청을 받아도 어디까지 안전한지 말할 수 있게 만든 것이 이번 발표의 핵심입니다.
