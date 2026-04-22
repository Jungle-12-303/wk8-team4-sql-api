# 발표 대본 — 전동환

> `*` 는 슬라이드 전환 기준입니다.

여러 클라이언트가 동시에 같은 in-memory DB에 SQL 요청을 보내면 어떤 일이 생길까요?

단순히 `POST /query` 하나를 만드는 것처럼 보이지만, 서버가 되는 순간 문제는 달라집니다. 요청이 한꺼번에 몰렸을 때 어디에서 줄을 세울지, worker가 동시에 같은 테이블을 만질 때 어떻게 보호할지, 그리고 서버가 막히는 상황을 client에게 어떻게 알려줄지가 핵심이 됩니다.

안녕하세요. 미니 DBMS SQL API 서버 구현을 발표할 4조 전동환입니다.

*
# 요구사항과 구현 매핑

저희 팀은 기존 C SQL 엔진을 다시 작성하지 않고, 그 앞에 HTTP API 서버 계층을 얹었습니다. 기존 `sql_execute()`, Table, B+Tree는 재사용하고, `src/server/`에서 endpoint, bounded queue, worker thread pool, read/write lock, timeout, metrics, JSON 응답을 담당하게 했습니다.

즉 이번 발표의 중심은 SQL core 자체가 아니라, 기존 엔진을 여러 client가 동시에 사용할 수 있게 만드는 API 서버 경계입니다.

# 프로젝트 구조
# API 서버 아키텍처

구조를 보시면 크게 두 레이어입니다. 위쪽 `API 서버`는 HTTP 요청을 받고, queue에 넣고, worker thread가 처리하며, 공유 DB 진입 전에 lock과 metrics를 관리합니다. 아래쪽 `DB 엔진`은 lock을 모르는 순수 SQL 실행 영역으로 남겨 두었습니다.

*
# 동시 요청 흐름 
## 1. 단일 요청 (SELECT 또는 INSERT)

이제 요청 하나가 서버 안에서 어떻게 흐르는지 보겠습니다.

client가 연결하면 accept loop는 소켓을 bounded `HTTPRequestQueue`에 넣고 바로 다음 연결을 받으러 돌아갑니다. worker thread pool은 미리 정해진 개수로 떠 있고, 각 worker가 queue에서 소켓을 꺼내 HTTP 파싱, 라우팅, SQL 실행 요청, JSON 응답 전송까지 한 요청을 끝까지 처리합니다.

여기서 queue를 둔 이유는 backpressure입니다. worker가 모두 바쁘면 accept loop까지 멈추는 대신, queue가 버틸 만큼만 요청을 받고 가득 차면 즉시 `503 queue_full`로 거절합니다. 그래서 부하가 몰려도 서버는 “멈춤”이 아니라 “명시적인 실패”로 응답할 수 있습니다.

*

worker가 `/query` 요청을 파싱하면 SQL 문자열을 `db_server_execute()`로 넘깁니다. 이 함수가 API 서버와 기존 DB 엔진 사이의 가장 중요한 경계입니다.

`db_server_execute()`는 SQL이 `SELECT`인지 `INSERT`인지 분류하고, 알맞은 read/write lock을 timeout 안에 획득한 뒤, 기존 `sql_execute()`를 호출합니다. 실행 전후로 metrics도 기록합니다.

반대로 JSON 응답은 이 함수가 직접 만들지 않습니다. `db_server_execute()`는 `DBServerExecution` 결과를 돌려주고, API/server 레이어가 그 결과를 `api_build_execution_response()`와 HTTP response로 직렬화합니다. 이 경계를 나눈 덕분에 core SQL 엔진은 HTTP나 JSON을 몰라도 됩니다.

# HTTP API

외부에 공개한 endpoint는 세 가지입니다. `GET /health`는 서버 상태, `GET /metrics`는 요청과 오류 지표, `POST /query`는 SQL 실행입니다. 예를 들어 `SELECT * FROM users WHERE id = 1`을 실행하면 응답에는 `rows`, `rowCount`, 그리고 id index 사용 여부를 나타내는 `usedIndex: true`가 포함됩니다.

*

이제 이 서버에서 가장 중요한 동시성 문제를 보겠습니다. 여러 worker가 하나의 `Table *`와 B+Tree를 동시에 접근하면 data race가 생깁니다. 대표적인 위험은 세 가지입니다.

첫째, INSERT가 슬롯을 절반만 쓴 상태에서 SELECT가 읽으면 쓰레기값이 반환됩니다. 둘째, 두 INSERT가 동시에 같은 id에 쓰면 충돌이 생깁니다. 셋째, B+Tree 노드 분할이 진행 중인 상태에서 다른 worker가 탐색하면 포인터가 오염될 수 있습니다.

그래서 저희는 공유 DB 경계에서 read/write lock을 적용했습니다.

*

Mutex를 쓰면 가장 단순하게 안전해집니다. 하지만 SELECT끼리도 모두 한 줄로 세우게 됩니다. 이 프로젝트에서 SELECT는 테이블을 수정하지 않기 때문에, 읽기끼리는 동시에 실행해도 됩니다.

Semaphore는 “최대 N개까지 진입”은 표현할 수 있지만, 읽기와 쓰기를 구분해 안전하게 보호하는 모델은 아닙니다. 저희에게 필요한 건 요청 개수 제한이 아니라, SELECT는 함께 들어가고 INSERT는 배타적으로 들어가는 규칙이었습니다.

그래서 RwLock이 맞았습니다. SELECT에는 shared read lock을 적용해 병렬 실행을 허용하고, INSERT에는 exclusive write lock을 적용해 테이블 변경과 B+Tree 갱신을 직렬화했습니다. 이 선택이 안전성과 읽기 병렬성을 동시에 만족시킵니다.

*
# 멀티 스레드 동시성 이슈

RwLock만으로 끝나지는 않았습니다. reader가 계속 들어오면 writer가 계속 밀릴 수 있습니다. 즉 INSERT가 들어왔는데 SELECT가 끊임없이 유입되면 writer starvation이 발생할 수 있습니다.

이를 막기 위해 writer가 기다리기 시작하면 새 reader를 잠시 막고, 이미 들어와 있던 reader들이 끝난 뒤 writer가 들어갈 차례를 보장했습니다. 이렇게 하면 읽기 병렬성은 살리면서도 INSERT가 영원히 굶지 않습니다.

또 하나 중요한 장치가 lock timeout입니다. worker가 lock을 무한정 기다리면 요청이 쌓이고, 사용자는 서버가 멈춘 것처럼 느낍니다. 그래서 lock 대기 시간이 설정값을 넘으면 SQL을 실행하지 않고 `503 lock_timeout`을 반환합니다. 이 이벤트는 `/metrics`의 `totalLockTimeouts`로도 관찰할 수 있습니다.

*
# (시연 진행)
시연에서는 여러 요청이 동시에 들어오는 상황에서 accept loop, bounded queue, worker thread pool이 어떻게 동작하는지 확인하겠습니다. SELECT 요청은 read lock 아래에서 함께 처리되고, INSERT 요청은 write lock 아래에서 순차적으로 처리되어 id와 B+Tree 상태가 충돌하지 않습니다.

그리고 `/metrics` 엔드포인트를 통해 총 요청 수, SELECT/INSERT 수, `queue_full`, `lock_timeout`, active query 수를 확인할 수 있습니다.

검증은 `make unit_test`로 B+Tree, table, SQL 실행, `db_server_execute()`, lock timeout, metrics, HTTP 파싱과 JSON 계약을 확인했습니다. 또한 smoke script와 함께 Postman collection을 구성해서 404/405, malformed request, read-only burst, mixed 80/20, write-heavy burst 같은 edge/burst 검증 흐름을 준비하고 기대 metrics를 확인할 수 있게 했습니다.

# 회고
마지막으로 이번 작업에서 배운 점은, API 서버 구현은 endpoint를 붙이는 것보다 경계를 명확히 세우는 일이 더 중요하다는 점이었습니다.

gstack 기반으로 설계, 구현, 검증 관점을 나누어 점검한 것도 도움이 됐지만, 핵심은 도구 자체가 아니라 “이 서버가 동시에 요청을 받아도 어디까지 안전하다고 말할 수 있는가”를 계속 검증한 과정이었습니다.

결론적으로 저희 구현은 SQL 엔진을 새로 만든 프로젝트가 아니라, 기존 엔진 앞에 안정적인 API 서버 경계를 세운 프로젝트입니다. accept loop, bounded queue, worker thread pool, read/write lock, starvation 방지, timeout, metrics까지 연결해서 “동시에 요청을 받아도 설명 가능한 서버”를 만드는 것이 이번 발표의 핵심입니다.
