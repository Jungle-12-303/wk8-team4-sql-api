# HTTP Smoke Test Guide

이 문서는 팀 로컬 환경에서 `server.exe` 기반 smoke 검증을 어떻게 돌릴지 빠르게 정리한 가이드입니다.

## 목적

이 smoke test의 목적은 두 가지입니다.

1. HTTP API 계약이 실제로 맞는지 확인
2. queue / worker / rwlock 관련 운영 시그널이 실제 응답으로 내려오는지 확인

즉 "코드가 있다"를 넘어서, "서버를 켜고 실제 요청을 보내도 문서에 적은 모양으로 응답한다"를 확인하는 절차입니다.

## 준비

먼저 바이너리를 빌드합니다.

```bash
make server
```

Windows MinGW에서 `make`가 없으면 아래처럼 실행합니다.

```bash
mingw32-make server
```

직접 컴파일도 가능합니다.

```bash
gcc -std=c11 -Wall -Wextra -Werror -pedantic -O2 -o server.exe server.c http_server.c db_server.c api.c platform.c bptree.c table.c sql.c -lws2_32
```

## 1차 smoke: 정상/에러/metrics

아래 스크립트를 실행합니다.

```powershell
powershell -ExecutionPolicy Bypass -File .\server_http_smoke_test.ps1
```

스크립트 1단계에서는 아래를 확인합니다.

- `GET /health`가 `200`으로 뜨는지
- `POST /query` INSERT가 성공하는지
- `POST /query` SELECT가 성공하고 `usedIndex: true`가 맞는지
- 빈 SELECT가 `200 + rows: []`로 내려오는지
- syntax error가 `400`으로 고정되는지
- `GET /metrics` 값이 요청 수와 맞는지

직관적으로 보면, "기본 기능이 붙었는지"를 확인하는 단계입니다.

## 2차 smoke: queue_full

같은 스크립트의 2단계에서는 아래 설정으로 서버를 띄웁니다.

- worker 1개
- queue 1개
- read delay 300ms

그 다음 동시에 여러 `SELECT`를 보내서 아래를 확인합니다.

- 일부 요청은 정상 `200`
- 최소 1개는 `503 queue_full`

이 단계는 "bounded queue가 실제로 걸리는지"를 확인하는 데 목적이 있습니다.

## 기대 결과

정상 종료되면 아래 메시지가 출력됩니다.

```text
server HTTP smoke test passed.
```

## 실패할 때 먼저 볼 것

### 1. `server.exe not found`

- 바이너리를 아직 빌드하지 않은 경우입니다.
- `make server` 또는 직접 `gcc` 빌드를 먼저 실행합니다.

### 2. 포트 충돌

- 기본 스크립트는 `18080`, `18081` 포트를 사용합니다.
- 이미 같은 포트를 누가 쓰고 있으면 다른 프로세스를 정리하거나 스크립트 포트를 바꿉니다.

### 3. `server.exe` 실행이 정책에 막힘

- 일부 회사/실습 환경에서는 Device Guard / App Control 정책 때문에 새로 만든 `.exe` 실행이 막힐 수 있습니다.
- 이 경우 코드 문제라기보다 실행 정책 문제일 가능성이 큽니다.
- 팀원 PC나 정책이 없는 환경에서 다시 확인해 보면 보통 바로 재현됩니다.

## 발표 전에 추천하는 최종 체크

아래 순서로 한 번만 통과시키면 시연 안정성이 많이 올라갑니다.

1. `unit_test` 통과
2. `server_cli_smoke_test.ps1` 통과
3. `server_http_smoke_test.ps1` 통과
4. `curl`로 `GET /health`, `GET /metrics`, `POST /query`를 직접 한 번 더 확인

이 순서가 좋은 이유는, 엔진 회귀와 서버 회귀를 따로 보다가 마지막에 end-to-end를 한 번 더 확인할 수 있기 때문입니다.
