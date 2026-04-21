#!/bin/sh

set -eu

BASE_DIR=$(mktemp -d /tmp/sqlproc-api-test.XXXXXX)
SCHEMA_DIR="$BASE_DIR/schemas"
DATA_DIR="$BASE_DIR/data"
PORT=$((18000 + ($$ % 20000)))
SERVER_PID=""

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        sleep 0.1
        kill -9 "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$BASE_DIR"
}

trap cleanup EXIT INT TERM

mkdir -p "$SCHEMA_DIR" "$DATA_DIR"
printf 'id:int,name:string,age:int\n' > "$SCHEMA_DIR/users.schema"

./build/sqlproc \
    --schema-dir "$SCHEMA_DIR" \
    --data-dir "$DATA_DIR" \
    --server \
    --port "$PORT" \
    --threads 4 \
    --queue-size 8 \
    > "$BASE_DIR/server.log" 2>&1 &
SERVER_PID=$!

i=0
while [ "$i" -lt 50 ]; do
    if curl -fsS "http://127.0.0.1:$PORT/health" > "$BASE_DIR/health.txt" 2>/dev/null; then
        break
    fi
    i=$((i + 1))
    sleep 0.1
done

if ! grep -q '^OK$' "$BASE_DIR/health.txt" 2>/dev/null; then
    echo "health check failed"
    cat "$BASE_DIR/server.log" 2>/dev/null || true
    exit 1
fi

insert_response=$(curl -sS \
    -X POST \
    --data-binary "INSERT INTO users (name, age) VALUES ('kim', 20);" \
    "http://127.0.0.1:$PORT/query")

if [ "$insert_response" != "OK" ]; then
    echo "unexpected insert response: $insert_response"
    exit 1
fi

select_response=$(curl -sS \
    -X POST \
    --data-binary "SELECT * FROM users WHERE id = 1;" \
    "http://127.0.0.1:$PORT/query")

printf '%s\n' "$select_response" > "$BASE_DIR/select.txt"
grep -q '\[INDEX\] WHERE id = 1' "$BASE_DIR/select.txt"
grep -q 'id	name	age' "$BASE_DIR/select.txt"
grep -q '1	kim	20' "$BASE_DIR/select.txt"

multiline_sql=$(printf "INSERT INTO users (name, age) VALUES ('min', 25);\nSELECT name FROM users WHERE id = 2;")
multiline_response=$(curl -sS \
    -X POST \
    --data-binary "$multiline_sql" \
    "http://127.0.0.1:$PORT/query")

printf '%s\n' "$multiline_response" > "$BASE_DIR/multiline.txt"
grep -q '\[INDEX\] WHERE id = 2' "$BASE_DIR/multiline.txt"
grep -q 'name' "$BASE_DIR/multiline.txt"
grep -q 'min' "$BASE_DIR/multiline.txt"

not_found_status=$(curl -sS -o "$BASE_DIR/not-found.txt" -w '%{http_code}' \
    "http://127.0.0.1:$PORT/missing")
if [ "$not_found_status" != "404" ]; then
    echo "expected 404, got $not_found_status"
    exit 1
fi

method_status=$(curl -sS -o "$BASE_DIR/method.txt" -w '%{http_code}' \
    "http://127.0.0.1:$PORT/query")
if [ "$method_status" != "405" ]; then
    echo "expected 405, got $method_status"
    exit 1
fi

empty_status=$(curl -sS -o "$BASE_DIR/empty.txt" -w '%{http_code}' \
    -X POST \
    --data-binary "" \
    "http://127.0.0.1:$PORT/query")
if [ "$empty_status" != "400" ]; then
    echo "expected 400 for empty body, got $empty_status"
    exit 1
fi

missing_length_status=$(printf 'POST /query HTTP/1.0\r\n\r\nSELECT * FROM users;' |
    nc 127.0.0.1 "$PORT" |
    awk 'NR == 1 { print $2 }')
if [ "$missing_length_status" != "400" ]; then
    echo "expected 400 for missing Content-Length, got $missing_length_status"
    exit 1
fi

invalid_length_status=$(printf 'POST /query HTTP/1.0\r\nContent-Length: abc\r\n\r\n' |
    nc 127.0.0.1 "$PORT" |
    awk 'NR == 1 { print $2 }')
if [ "$invalid_length_status" != "400" ]; then
    echo "expected 400 for invalid Content-Length, got $invalid_length_status"
    exit 1
fi

large_sql=$(awk 'BEGIN { for (i = 0; i < 9000; i++) printf "x" }')
large_status=$(curl -sS -o "$BASE_DIR/large.txt" -w '%{http_code}' \
    -X POST \
    --data-binary "$large_sql" \
    "http://127.0.0.1:$PORT/query")
if [ "$large_status" != "413" ]; then
    echo "expected 413, got $large_status"
    exit 1
fi

sql_error_status=$(curl -sS -o "$BASE_DIR/sql-error.txt" -w '%{http_code}' \
    -X POST \
    --data-binary "SELECT * FROM users WHERE missing = 1;" \
    "http://127.0.0.1:$PORT/query")
if [ "$sql_error_status" != "400" ]; then
    echo "expected 400 for SQL error, got $sql_error_status"
    exit 1
fi
grep -q '오류:' "$BASE_DIR/sql-error.txt"

parallel_pids=""
for name in lee park choi jung; do
    curl -sS \
        -X POST \
        --data-binary "INSERT INTO users (name, age) VALUES ('$name', 30);" \
        "http://127.0.0.1:$PORT/query" > "$BASE_DIR/parallel-$name.txt" &
    parallel_pids="$parallel_pids $!"
done

for pid in $parallel_pids; do
    wait "$pid"
done

all_response=$(curl -sS \
    -X POST \
    --data-binary "SELECT * FROM users;" \
    "http://127.0.0.1:$PORT/query")

printf '%s\n' "$all_response" > "$BASE_DIR/all.txt"
grep -q 'kim' "$BASE_DIR/all.txt"
grep -q 'min' "$BASE_DIR/all.txt"
grep -q 'lee' "$BASE_DIR/all.txt"
grep -q 'park' "$BASE_DIR/all.txt"
grep -q 'choi' "$BASE_DIR/all.txt"
grep -q 'jung' "$BASE_DIR/all.txt"

echo "API server tests passed."
