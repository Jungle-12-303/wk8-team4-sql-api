# Mini DBMS SQL API Server 발표 자료 계획

## Audience

- 수요 코딩회 과제 발표를 듣는 동료 개발자와 멘토
- C, SQL, 자료구조, 네트워크, 동시성 개념을 배우는 중인 청중
- "요구사항을 충족했는가"와 "핵심 로직을 이해하고 설명할 수 있는가"를 확인하려는 평가자

## Objective

현재 코드베이스가 과제 요구사항인 C 기반 미니 DBMS API 서버, 외부 클라이언트 사용, thread pool 기반 병렬 SQL 처리, 기존 SQL 처리기와 B+Tree 재사용, 테스트와 edge case 고려를 어떻게 만족하는지 4분 안에 설명한다.

## Narrative Arc

1. 기존 미니 DBMS 엔진을 HTTP API 서버로 확장했다.
2. 핵심 설계는 `src/core/`와 `src/server/`의 책임 분리다.
3. HTTP 요청은 bounded queue와 worker thread pool을 거쳐 공유 DB 경계로 들어간다.
4. 공유 테이블은 read/write lock으로 보호하고, `queue_full`, `lock_timeout`, metrics로 운영 시그널을 드러낸다.
5. 단위 테스트와 실제 HTTP smoke 확인으로 기능과 계약을 검증했다.

## Slide List

1. **Mini DBMS를 API 서버로 확장**
   - 한 줄 소개: C 기반 in-memory SQL 엔진에 HTTP API와 thread pool을 얹은 프로젝트
   - 핵심 메시지: "기존 엔진 재사용 + 서버 계층 추가"

2. **과제 요구사항과 구현 매핑**
   - 외부 클라이언트: `GET /health`, `GET /metrics`, `POST /query`
   - 병렬 처리: worker thread pool + bounded queue
   - 내부 DB 엔진: `sql_execute()`, `Table`, `B+Tree`
   - 품질: unit test, smoke, Postman collection, edge/burst scenario

3. **전체 아키텍처**
   - `client -> accept -> queue -> worker -> db_server_execute() -> sql_execute() -> table/bptree -> JSON`
   - `src/core/`와 `src/server/` 경계 표시

4. **HTTP API 계약**
   - `/health`, `/metrics`, `/query`
   - 성공 응답: `insertedId`, `rowCount`, `usedIndex`
   - 오류 응답: `syntax_error`, `query_error`, `queue_full`, `lock_timeout`

5. **Thread Pool과 Backpressure**
   - main accept loop는 `client_socket`을 queue에 넣는다.
   - worker thread가 queue에서 socket을 꺼내 요청을 읽고 처리한다.
   - queue가 꽉 차면 즉시 `503 queue_full`로 실패한다.

6. **공유 DB 보호: Read/Write Lock**
   - `SELECT`는 read lock, `INSERT`는 write lock
   - `src/core/*`는 lock-unaware로 유지
   - lock 대기 초과는 `503 lock_timeout`

7. **기존 SQL 엔진과 B+Tree 재사용**
   - `INSERT INTO users VALUES ('Alice', 20);`
   - `SELECT * FROM users WHERE id = 1;`
   - `id` 조건은 B+Tree index, `name`/`age` 조건은 linear scan

8. **검증 결과와 마무리**
   - `make unit_test server`: 최신 상태
   - `./build/bin/unit_test`: `All unit tests passed.`
   - 로컬 HTTP smoke: `/health`, insert, indexed select, `/metrics` 확인
   - 비범위: DDL, 다중 테이블, update/delete, persistence, auth/TLS

## Source Plan

- 요구사항: `/Users/donghyunkim/Downloads/wk08_수요코딩회_과제_요구사항.md`
- 구현 요약: `README.md`
- HTTP/thread pool: `src/server/http_server.c`
- DB lock/metrics: `src/server/db_server.c`
- HTTP/JSON contract: `src/server/api.c`
- SQL/B+Tree reuse: `src/core/sql.c`, `src/core/table.c`, `src/core/bptree.c`
- 검증: `tests/unit/unit_test.c`, `docs/http-smoke-test.md`

## Visual System

- 형식: 16:9 editable PowerPoint
- 분위기: 실습 발표용 기술 데크, 차분하지만 선명한 대비
- 색상: ink navy, off-white, teal, amber, coral, muted green
- 서체: 제목 Poppins, 본문 Lato 계열
- 시각 요소: 네트워크 요청 흐름, 모듈 경계, 큐/worker/thread, read/write lock, API contract cards

## Editability Plan

- 모든 제목, 본문, 코드 예시, API 응답, speaker notes는 PowerPoint 텍스트 객체로 유지한다.
- 아키텍처와 흐름도는 editable shape와 connector로 만든다.
- speaker notes에 4분 발표 스크립트를 넣어 발표자가 PPT만 열어도 말할 수 있게 한다.
