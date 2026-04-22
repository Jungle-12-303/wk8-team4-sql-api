FROM debian:bookworm-slim AS devcontainer

RUN apt-get update \
    && apt-get install -y --no-install-recommends bash build-essential ca-certificates curl git make \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

FROM devcontainer AS builder

WORKDIR /app

COPY Makefile ./
COPY src ./src
COPY tests ./tests

RUN make unit_test server

FROM debian:bookworm-slim AS runtime

RUN useradd --create-home --uid 10001 appuser

WORKDIR /app

COPY --from=builder /app/build/bin/server ./server

USER appuser

EXPOSE 8080

ENTRYPOINT ["./server"]
CMD ["--serve", "--port", "8080", "--workers", "4", "--queue", "16"]
