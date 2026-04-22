# HTTP Smoke Test Guide

이 문서는 Docker 기반 Linux 환경에서 HTTP smoke 검증을 어떻게 돌릴지 빠르게 정리한 가이드입니다.

## 목적

이 smoke test의 목적은 두 가지입니다.

1. HTTP API 계약이 실제로 맞는지 확인
2. queue / worker / rwlock 관련 운영 시그널이 실제 응답으로 내려오는지 확인

즉 "코드가 있다"를 넘어서, "서버를 켜고 실제 요청을 보내도 문서에 적은 모양으로 응답한다"를 확인하는 절차입니다.

## 준비

먼저 Docker Compose로 서버 이미지를 빌드하고 실행합니다.

```bash
docker compose up --build sql-api
```

## 1차 smoke: 정상/에러/metrics

다른 터미널에서 아래 요청을 보냅니다.

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

이 단계에서는 아래를 확인합니다.

- `GET /health`가 `200`으로 뜨는지
- `POST /query` INSERT가 성공하는지
- `POST /query` SELECT가 성공하고 `usedIndex: true`가 맞는지
- 빈 SELECT가 `200 + rows: []`로 내려오는지
- syntax error가 `400`으로 고정되는지
- `GET /metrics` 값이 요청 수와 맞는지

직관적으로 보면, "기본 기능이 붙었는지"를 확인하는 단계입니다.

## 2차 smoke: queue_full

queue 포화를 확인하려면 서버를 아래 설정으로 다시 띄웁니다.

```bash
docker compose down
docker build -t mini-dbms-sql-api .
docker run --rm -p 8080:8080 mini-dbms-sql-api \
  --serve --port 8080 --workers 1 --queue 1 --simulate-read-delay-ms 300
```

그 다음 동시에 여러 `SELECT`를 보내서 아래를 확인합니다.

- 일부 요청은 정상 `200`
- 최소 1개는 `503 queue_full`

이 단계는 "bounded queue가 실제로 걸리는지"를 확인하는 데 목적이 있습니다.

## 정리

검증이 끝나면 아래 명령으로 정리합니다.

```bash
docker compose down
```

## 실패할 때 먼저 볼 것

### 1. Docker가 실행 중이지 않음

- Docker Desktop 또는 Docker Engine이 올라와 있는지 먼저 확인합니다.
- `docker compose up --build sql-api`가 바로 실패하면 Docker daemon 상태를 먼저 확인합니다.

### 2. 포트 충돌

- 기본 예시는 `8080` 포트를 사용합니다.
- 이미 같은 포트를 누가 쓰고 있으면 `-p 18080:8080`처럼 다른 호스트 포트로 바꿉니다.

### 3. `queue_full` 재현이 안 됨

- `--workers 1 --queue 1 --simulate-read-delay-ms 300`처럼 의도적으로 작은 설정을 써야 재현이 쉽습니다.
- 동시에 보내는 요청 수가 너무 적으면 모두 정상 응답으로 끝날 수 있습니다.

## 발표 전에 추천하는 최종 체크

아래 순서로 한 번만 통과시키면 시연 안정성이 많이 올라갑니다.

1. `docker compose --profile test run --rm unit-test` 통과
2. `docker compose up --build sql-api`로 서버 기동 확인
3. `curl`로 `GET /health`, `GET /metrics`, `POST /query`를 직접 확인
4. 필요하면 Postman collection으로 edge/burst 시나리오를 한 번 더 확인

이 순서가 좋은 이유는, 엔진 회귀와 서버 회귀를 따로 보다가 마지막에 end-to-end를 한 번 더 확인할 수 있기 때문입니다.
