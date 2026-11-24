#!/usr/bin/env bash
# [load.sh] 부하 테스트 — 동시 연결 수에 따른 처리량(RPS)과 지연 시간 측정
# 사용: make && bash tests/load.sh   (필요 도구: wrk, curl)
set -u
cd "$(dirname "$0")/.." || exit 1

SERVER=${SERVER:-./bin/server}
PORT=${PORT:-18081}
DURATION=${DURATION:-10s}
THREADS=${THREADS:-4}

command -v wrk >/dev/null 2>&1 || { echo "wrk 없음: sudo apt install wrk"; exit 1; }
command -v curl >/dev/null 2>&1 || { echo "curl 없음"; exit 1; }
[ -x "$SERVER" ] || { echo "서버 실행 파일 없음: $SERVER (먼저 make)"; exit 1; }
[ -x ./cgi-bin/adder ] || { echo "CGI 실행 파일 없음: ./cgi-bin/adder (먼저 make)"; exit 1; }

# WSL /mnt/c(9P) 파일 시스템 병목을 배제하고 순수 서버 성능을 재기 위해 /tmp로 복사해 실행
TMPDIR=$(mktemp -d /tmp/w13_load_XXXXXX)
PID=
cleanup() {
    if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
        kill -INT "$PID" 2>/dev/null
        wait "$PID" 2>/dev/null
    fi
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

cp -r ./www "$TMPDIR/www"
mkdir -p "$TMPDIR/cgi-bin"
cp ./cgi-bin/adder "$TMPDIR/cgi-bin/adder"

"$SERVER" -p "$PORT" -r "$TMPDIR/www" -g "$TMPDIR/cgi-bin" >"$TMPDIR/server.log" 2>&1 &
PID=$!

ready=0
for _ in $(seq 50); do
    if curl -s -o /dev/null "http://127.0.0.1:$PORT/"; then
        ready=1
        break
    fi
    kill -0 "$PID" 2>/dev/null || break
    sleep 0.1
done
if [ "$ready" -ne 1 ]; then
    echo "서버 시작 실패:"
    cat "$TMPDIR/server.log"
    exit 1
fi

URL="http://127.0.0.1:$PORT"
for c in 10 100 500; do
    echo "=== 정적 GET / (동시 ${c}연결, ${DURATION}) ==="
    wrk -t"$THREADS" -c"$c" -d"$DURATION" "$URL/"
done

echo "=== CGI GET /cgi-bin/adder?a=1&b=2 (동시 100연결, ${DURATION}) ==="
wrk -t"$THREADS" -c100 -d"$DURATION" "$URL/cgi-bin/adder?a=1&b=2"

kill -INT "$PID"
wait "$PID"
PID=
