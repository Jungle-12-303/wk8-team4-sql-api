# 세션 로그

## 요청 요약

- WK08 HTTP SQL API 서버 계획 대비 남은 부분을 구현 완료한다.
- 서버/HTTP/큐 테스트 보강, benchmark 모드 분리, 세션 로그 누락을 정리한다.
- `commit-convention` 규칙에 따라 기능별 커밋을 남긴다.

## 결정 사항

- API 서버의 bounded queue는 기존 구현처럼 `pthread_mutex_t`와 `pthread_cond_t`를 사용한다.
  CS:APP `sbuf`의 생산자-소비자 의미는 유지하고, macOS에서 다루기 쉬운 조건 변수 방식으로 검증한다.
- 테스트 전용 빌드는 `SQLPROC_TEST`를 정의해 서버 내부의 HTTP header builder,
  `Content-Length` 파서, queue 기본 동작을 직접 확인한다.
- `--benchmark`는 계획의 모드 분리 원칙에 맞춰 결과 표를 출력한 뒤 종료한다.
  interactive 사용은 `--interactive`로 별도 실행한다.

## 현재 브랜치 상태

- 브랜치: `feature/api-server-thread-pool`
- 기준 작업: API 서버 MVP 구현 뒤 남은 테스트/문서/모드 분리 보강

## 완료한 작업

- 서버 CLI 인자 경계값 테스트를 추가했다.
  - 잘못된 port
  - 잘못된 thread count
  - 잘못된 queue size
  - server 전용 옵션을 server 모드 없이 사용하는 경우
- HTTP 단위 성격 테스트를 추가했다.
  - `Content-Length` 정상/오류 파싱
  - `500 Internal Server Error` 포함 response header 생성
  - bounded queue insert/remove 순서
- API 통합 테스트를 보강했다.
  - multiline SQL body
  - 여러 SQL 문장 body
  - missing `Content-Length`
  - SQL 실행 오류의 `400 Bad Request`
- benchmark 모드가 interactive로 이어지지 않고 종료되도록 수정했다.
- README/GUIDE의 benchmark 설명을 현재 동작과 맞췄다.

## 리뷰 결과

- 정확성/버그:
  - SQL 엔진 critical section은 기존처럼 `sql_engine_mutex`로 보호된다.
  - 병렬 HTTP 요청은 통합 테스트에서 계속 확인한다.
- 자료구조 무결성:
  - queue는 FIFO round trip 테스트를 추가해 기본 순서를 검증했다.
  - B+ Tree와 CSV append 보호 정책은 변경하지 않았다.
- 초심자 가독성/C99 난이도:
  - 테스트 전용 hook은 `SQLPROC_TEST`에서만 공개해 production 인터페이스를 복잡하게 만들지 않았다.
  - benchmark 문서에서 모드가 섞이지 않는다는 점을 더 직접적으로 설명했다.

## 검증

- `make`
- `make test`
- `make api-test`
- `make bench BENCH_DATA_DIR=/tmp/sqlproc-bench-check`
  - 입력값: `10`

## 다음 작업

- GitHub Issue/Project/PR 절차를 진행할 때 이 로그와 테스트 결과를 Issue 코멘트에 요약한다.
- 필요하면 HTTP 500 live fault injection은 별도 테스트 hook 또는 실패 주입 옵션으로 확장한다.

## 남은 리스크

- HTTP 500은 response builder 단위 테스트로 검증했지만, 실제 운영 경로의 `tmpfile()` 실패를 강제로 재현하지는 않았다.
- 서버는 graceful shutdown 없이 테스트 스크립트에서 프로세스를 종료한다.
