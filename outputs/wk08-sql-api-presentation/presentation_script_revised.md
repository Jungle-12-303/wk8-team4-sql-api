# 발표 대본 (수정본) — 전동환

> `*` 는 슬라이드 전환 기준입니다.

---

미니 DBMS SQL API 서버 구현을 주제로 발표를 맡게된 4조 전동환입니다.

*

저희 팀은 이번에 기존 SQL 엔진을 HTTP API 서버로 확장하고, 멀티스레드 요청 처리와 read/write 동시성 제어를 통해 여러 클라이언트의 SQL 요청을 안정적으로 처리할 수 있는 서버 구조를 구현하였습니다.

*

먼저 전체 구조를 간단히 설명드리자면, 저희 프로젝트는 기존에 REPL 환경에서만 동작하던 SQL 실행 엔진을 그대로 재사용하면서, 그 앞단에 HTTP 서버 계층을 추가한 구조입니다.

크게 두 레이어로 나뉩니다. `src/server/`는 HTTP 처리, 대기열, 워커 스레드, 동시성 제어를 담당하고, `src/core/`는 lock을 전혀 모르는 순수 SQL 엔진 영역으로 유지됩니다. 이 분리 덕분에 동시성 정책을 바꾸더라도 DB 엔진 코드는 건드리지 않아도 됩니다.

*

그래서 클라이언트 여러 명이 동시에 서버로 요청을 보내면, accept loop가 각 연결의 소켓을 bounded queue에 넣고 다음 연결을 받으러 돌아갑니다. 그러면 미리 만들어진 여러 개의 워커 스레드가 queue에서 소켓을 꺼내 HTTP 요청을 읽고 처리합니다.

accept loop가 worker에게 직접 넘기지 않고 queue를 두는 이유는, 모든 worker가 바쁜 상황에서도 accept loop가 블록되지 않도록 하기 위해서입니다. queue가 가득 찰 경우에는 즉시 503 queue_full 응답을 돌려주어, 서버가 계속 응답할 수 있게 합니다.

*

각 워커 스레드는 HTTP 요청을 파싱한 뒤, 그 SQL이 SELECT인지 INSERT인지를 판별하여 `db_server_execute()`로 전달합니다. 이 함수가 서버 레이어와 DB 엔진 레이어의 경계입니다.

`db_server_execute()`는 SQL 종류에 따라 적절한 lock을 획득한 뒤 `sql_execute()`를 호출하고, 결과를 JSON으로 직렬화하여 응답합니다. `src/core/`의 SQL 엔진은 이 과정을 전혀 모르고 순수하게 SQL 실행만 담당합니다.

*

이 때, 여러 worker가 하나의 공유 테이블에 동시에 접근하면 data race가 발생할 수 있습니다. 예를 들어 INSERT가 테이블 슬롯을 절반만 쓴 상태에서 SELECT가 읽으면 쓰레기값이 반환되고, 두 INSERT가 동시에 같은 id에 쓰면 충돌이 생깁니다.

저희는 이를 해결하기 위해 동기화 기법 세 가지를 비교한 뒤, 이 프로젝트의 접근 패턴에 맞는 RwLock을 선택하였습니다.

*

뮤텍스는 한 번에 하나의 스레드만 진입시키는 상호 배제 잠금이고, 세마포어는 카운터로 N개의 스레드 동시 진입을 허용하는 신호 기법입니다.

RwLock은 읽기는 여러 스레드가 동시에, 쓰기는 하나만 배타적으로 진입할 수 있는 분리된 잠금입니다.

저희가 Mutex 대신 RwLock을 선택한 이유는, SELECT 요청은 테이블을 수정하지 않으므로 SELECT끼리는 data race가 없기 때문입니다. Mutex는 SELECT끼리도 직렬화하여 불필요한 대기가 생기지만, RwLock은 SELECT에 shared read lock을 적용해 병렬 실행을 허용하고, INSERT에만 exclusive write lock을 적용해 직렬화합니다.

*

한 가지 추가로 해결한 문제가 있습니다. 기본 POSIX rwlock은 reader가 계속 유입되면 writer가 영원히 대기하는 writer starvation이 발생할 수 있습니다.

저희는 이를 방지하기 위해 fair rwlock을 직접 구현하였습니다. write lock 요청이 들어오면 이후 신규 reader를 대기시켜 writer가 먼저 진입할 수 있도록, phase gate와 reader batch 카운터를 활용해 진입 순서를 제어합니다.

또한 lock 대기가 무한정 이어지면 서버 전체가 멈출 수 있으므로, lock 획득에 timeout을 두어 대기 시간을 초과하면 SQL 실행 없이 503 lock_timeout을 반환합니다.

*

시연을 보시면, 실제로 여러 요청이 동시에 들어오는 상황에서도 서버가 이를 대기열과 워커 스레드를 통해 분산 처리하는 것을 확인하실 수 있습니다.

SELECT 요청은 동시에 처리되지만, INSERT 요청은 write lock에 의해 순차적으로 처리되기 때문에 결과가 충돌하지 않고 정상적으로 유지되는 것을 확인할 수 있습니다.

그리고 `/metrics` 엔드포인트를 통해 총 요청 수, SELECT/INSERT 수, queue_full, lock_timeout 발생 횟수 등 서버 상태를 실시간으로 확인할 수 있습니다.

*

마지막으로 저희 팀은 이번에 gstack이라는 스킬을 사용해 작업을 진행해보았는데요.
gstack은 AI를 가상 엔지니어링 팀으로 만들어서 기획 단계부터 개발 및 테스트, 최종 출시까지의 과정을 모두 각 단계에 특화된 전문 에이전트에게 분업해서 맡기는 스킬이라고 볼 수 있습니다.
이 과정을 통해서 저희는 기존 일반 코덱스와 비교했을 때, 작업 속도와 퀄리티 개선을 더 빠르고 효과적으로 할 수 있었고, 이로 인해 남은 시간을 개념과 코드 학습에 더 집중할 수 있었습니다.

---

## 원본 대비 수정 내역

| 위치 | 원본 | 수정 이유 |
|---|---|---|
| 동기화 기법 설명 | "뮤텍스와 **세마포어**를 활용한 동기화 방식으로 데이터 충돌 방지" | 코드에 세마포어 없음. 실제 구현은 `PlatformRWLock` + `PlatformMutex` 조합 |
| Mutex/Semaphore → read/write lock 전환 | 설명 없이 바로 "이를 바탕으로 lock 적용" | RwLock을 선택한 이유(SELECT끼리는 data race 없음)를 명시하여 흐름 연결 |
| accept loop 설명 | "메인 서버는 소켓을 생성한 뒤 queue에 넣음" | queue가 가득 찰 때 503 즉시 반환(backpressure) 내용 추가 |
| DB 처리 흐름 | "기존 DB 엔진으로 전달되어 SQL 실행" | SQL 분류(`classify_query`) → lock 획득 → `sql_execute()` 순서 명시 |
| 동시성 보완 | writer starvation 미언급 | fair rwlock 구현과 phase gate 설명 추가 |
| 동시성 보완 | lock_timeout 미언급 | timeout 초과 시 503 반환 내용 추가 |
| 시연 섹션 | `/metrics` 미언급 | 운영 관찰 가능 지표로 추가 |
| 대본 중간 메모 | "파싱해와서 그게 어떤 요청인지 구분해서 DB서버로 보내는거야..." | 미완성 메모 삭제 |
