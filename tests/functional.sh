#!/usr/bin/env bash
# [functional.sh] 기능 테스트 — 실제 서버를 띄우고 curl / nc로 응답 확인
# 사용: make && bash tests/functional.sh        (필요 도구: curl, nc, ss)
#       SERVER=build/asan/server bash tests/functional.sh   ← ASan 빌드 서버로 실행
set -u
cd "$(dirname "$0")/.." || exit 1

SERVER=${SERVER:-./bin/server}
PORT=${PORT:-18080}
LOG=$(mktemp)
PASS=0
FAIL=0
PID=

ok()   { echo "  ok   $1"; PASS=$((PASS + 1)); }
bad()  { echo "  FAIL $1 — $2"; FAIL=$((FAIL + 1)); }
expect_eq() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "기대 '$3', 실제 '$2'"; fi; }
now_ms() { date +%s%3N; }

start_server() {
    "$SERVER" -p "$PORT" "$@" >"$LOG" 2>&1 &
    PID=$!
    for _ in $(seq 50); do                      # 최대 5초 동안 포트가 열리기를 기다림
        curl -s -o /dev/null "http://127.0.0.1:$PORT/" && return 0
        kill -0 "$PID" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}
# 서버 종료를 최대 $1초만 기다린다. 넘으면 강제 종료(kill -9)하고 137을 돌려준다 → 테스트가 무한 대기하지 않고 실패로 기록
# (kill -0은 좀비 프로세스에도 성공하므로 쓰지 않고, 감시 프로세스가 시간 초과 시 죽이는 방식)
wait_exit() {
    ( sleep "$1"; kill -9 "$PID" 2>/dev/null ) &
    local wd=$!
    wait "$PID" 2>/dev/null
    local rc=$?
    kill "$wd" 2>/dev/null
    wait "$wd" 2>/dev/null
    PID=
    return $rc
}
stop_server() { [ -n "$PID" ] && kill -INT "$PID" 2>/dev/null && wait_exit 10; PID=; }
cleanup() { [ -n "$PID" ] && kill -9 "$PID" 2>/dev/null; rm -f "$LOG"; }
trap cleanup EXIT

[ -x "$SERVER" ] || { echo "서버 실행 파일 없음: $SERVER (먼저 make)"; exit 1; }

# ───────────── 1. 인자 검사 ─────────────
echo "== 1. 인자 검사 (잘못된 값이면 시작하지 않고 exit 1)"
for args in "-p 0" "-p -1" "-p 65536" "-p 70000" "-p 99999" "-p abc" "-p 80x" "-p 8080abc" "-p ''" \
            "-p ' 80'" "-p +80" "-p 999999999999999999999999" \
            "-t 0" "-t -5" "-t 1025" "-t x" "-t 8abc" "-t ' 8'" "-t +8" "-t 99999999999999999999" \
            "-q 0" "-q -1" "-q 65537" "-q abc" "-q 64abc" "-q 99999999999999999999" \
            "-z" "-p" "-t" "extra" "-t 8 extra"; do
    eval "timeout 3 $SERVER $args" >/dev/null 2>&1
    expect_eq "인자 $args → 거부" "$?" "1"
done

# 유효한 값은 인자 검사를 통과해 서버가 계속 실행됨 (timeout이 끊으면 124)
vport=$((PORT + 100))
for args in "-t 1" "-t 1024" "-q 1" "-q 65536" "-f" "-r /tmp -g /tmp" "-c conf/routes.conf -f -t 4" \
            "-q 8 -t 2 -r ./www"; do
    vport=$((vport + 1))
    eval "timeout 1 $SERVER -p $vport $args" >/dev/null 2>&1
    expect_eq "인자 $args → 허용(실행 유지)" "$?" "124"
done
for p in 1024 65535; do                         # 포트 경계값 (사용 중이면 bind 실패라 실행 유지 여부만 확인)
    out=$(timeout 1 "$SERVER" -p $p 2>&1)
    echo "$out" | grep -q "invalid -p" && bad "포트 $p 경계값 허용" "$out" || ok "포트 $p 경계값 허용"
done

# ───────────── 2. 기본 응답 ─────────────
echo "== 2. 기본 응답"
start_server && ok "서버 시작" || { bad "서버 시작" "$(cat "$LOG")"; exit 1; }

U="http://127.0.0.1:$PORT"
resp=$(curl -s -i "$U/" | tr -d '\r')
expect_eq "GET / 상태 라인" "$(echo "$resp" | head -1)" "HTTP/1.1 200 OK"
echo "$resp" | grep -q '^Content-Type: text/html; charset=utf-8$' && ok "Content-Type text/html" || bad "Content-Type" "$resp"
echo "$resp" | grep -q "^Content-Length: $(wc -c < www/index.html)$" && ok "Content-Length = 파일 크기" || bad "Content-Length" "$resp"
echo "$resp" | grep -q '^Connection: keep-alive$' && ok "HTTP/1.1 기본 keep-alive" || bad "Connection" "$resp"
curl -s "$U/" | cmp -s - www/index.html && ok "본문 = www/index.html (바이트 동일)" || bad "본문" "다름"
curl -s "$U/index.html" | cmp -s - www/index.html && ok "/index.html 직접 요청" || bad "/index.html" "다름"
expect_eq "CSS Content-Type" "$(curl -s -o /dev/null -w '%{content_type}' "$U/style.css")" "text/css; charset=utf-8"
expect_eq "JS Content-Type" "$(curl -s -o /dev/null -w '%{content_type}' "$U/app.js")" "application/javascript; charset=utf-8"
expect_eq "쿼리스트링 무시하고 파일 서빙" "$(curl -s -o /dev/null -w '%{http_code}' "$U/style.css?v=1")" "200"
expect_eq "없는 파일 → 404" "$(curl -s -o /dev/null -w '%{http_code}' "$U/nope.html")" "404"
expect_eq "루트 탈출 ../ → 403" "$(curl -s --path-as-is -o /dev/null -w '%{http_code}' "$U/../Makefile")" "403"
expect_eq "정적 파일에 POST → 405" "$(curl -s -o /dev/null -w '%{http_code}' -d 'a=1' "$U/")" "405"
expect_eq "CGI 경로 (5단계 전) → 501" "$(curl -s -o /dev/null -w '%{http_code}' "$U/cgi-bin/adder?a=1&b=2")" "501"
expect_eq "Forward 요청, -f 없음 → 403" "$(curl -s -o /dev/null -w '%{http_code}' -x "127.0.0.1:$PORT" http://example.com/)" "403"
head_out=$(curl -s -I "$U/" | tr -d '\r')
echo "$head_out" | grep -q "^Content-Length: $(wc -c < www/index.html)$" && ok "HEAD: 헤더만, 길이는 GET과 같음" || bad "HEAD" "$head_out"
r10=$(curl -s -0 -i "$U/" | tr -d '\r')
echo "$r10" | grep -q '^Connection: close$' && ok "HTTP/1.0 → Connection: close" || bad "HTTP/1.0" "$r10"
# curl 로그 문구는 버전마다 달라서, 전송별 "새로 맺은 연결 수"로 판정 (두 번째가 0이면 재사용)
conns=$(curl -s -o /dev/null -o /dev/null -w '%{num_connects} ' "$U/" "$U/style.css")
expect_eq "keep-alive: 두 번째 요청이 같은 연결 재사용 (새 연결 수)" "$conns" "1 0 "
expect_eq "localhost 이름으로 접속" "$(curl -s -o /dev/null -w '%{http_code}' "http://localhost:$PORT/")" "200"
bigf=$(mktemp)
head -c 3000000 /dev/zero > "$bigf"
expect_eq "본문 3MB(상한 1MB 초과) → 413 응답이 실제로 도착" \
    "$(curl -s -o /dev/null -w '%{http_code}' -H 'Expect:' --data-binary @"$bigf" "$U/")" "413"
rm -f "$bigf"

# ───────────── 3. 비정상 클라이언트 ─────────────
echo "== 3. 비정상 클라이언트 (서버가 죽지 않고 다음 요청을 처리해야 함)"
got=$({ printf 'GET / HTT'; sleep 0.3; printf 'P/1.1\r\nHost: x\r\n'; sleep 0.3; printf '\r\n'; sleep 0.3; } \
      | nc -q 1 127.0.0.1 "$PORT" | head -1 | tr -d '\r')
expect_eq "조각난 요청 합쳐서 처리" "$got" "HTTP/1.1 200 OK"

got=$(printf 'GET / HTTP/1.1\nHost: x\n\n' | nc -q 1 127.0.0.1 "$PORT" | head -1 | tr -d '\r')
expect_eq "LF만 쓰는 요청" "$got" "HTTP/1.1 200 OK"

printf 'GET / HTTP/1.1\r\nHost:' | nc -q 0 127.0.0.1 "$PORT" >/dev/null
expect_eq "헤더 도중 끊긴 뒤 다음 요청" "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/")" "200"

nc -z 127.0.0.1 "$PORT"
expect_eq "연결만 하고 바로 끊은 뒤" "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/")" "200"

for _ in $(seq 30); do printf 'GET / HTTP/1.0\r\n\r\n' | timeout 0.05 nc 127.0.0.1 "$PORT" >/dev/null 2>&1; done
expect_eq "응답 안 읽고 끊기 30회(SIGPIPE) 뒤" "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/")" "200"

long=$(head -c 20000 /dev/zero | tr '\0' 'a')
got=$(printf 'GET / HTTP/1.1\r\nX-Long: %s\r\n\r\n' "$long" | nc -q 1 127.0.0.1 "$PORT" | head -1 | tr -d '\r')
expect_eq "20KB 헤더 줄 → 431" "$got" "HTTP/1.1 431 Request Header Fields Too Large"
got=$(printf 'GARBAGE\r\n\r\n' | nc -q 1 127.0.0.1 "$PORT" | head -1 | tr -d '\r')
expect_eq "깨진 요청 라인 → 400" "$got" "HTTP/1.1 400 Bad Request"

head -c 100000 /dev/urandom | nc -q 1 127.0.0.1 "$PORT" >/dev/null 2>&1
expect_eq "100KB 쓰레기 바이트 뒤" "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/")" "200"

# ───────────── 4. 동시 접속 / 반복 ─────────────
echo "== 4. 동시 접속 · 반복"
pids=()
tmpd=$(mktemp -d)
for i in $(seq 20); do curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/" >"$tmpd/$i" & pids+=($!); done
wait "${pids[@]}"
expect_eq "동시 20개 모두 200" "$(cat "$tmpd"/* | tr -d '\n' | sed 's/200//g')" ""
rm -rf "$tmpd"

codes=$(for _ in $(seq 200); do curl -s -o /dev/null -w '%{http_code}\n' "http://127.0.0.1:$PORT/"; done | sort | uniq -c | tr -s ' ')
expect_eq "연속 200회 모두 200" "$codes" " 200 200"

# ───────────── 5. 자원 누수 ─────────────
echo "== 5. 자원 누수"
expect_eq "요청 수백 번 뒤 열린 소켓 = listen 1개" "$(ls -l /proc/"$PID"/fd 2>/dev/null | grep -c socket)" "1"
fds_before=$(ls /proc/"$PID"/fd | wc -l)
for _ in $(seq 100); do nc -z 127.0.0.1 "$PORT"; done
for _ in $(seq 100); do curl -s -o /dev/null "http://127.0.0.1:$PORT/"; done
expect_eq "fd 개수 변화 없음" "$(ls /proc/"$PID"/fd | wc -l)" "$fds_before"

# ───────────── 6. 유휴 클라이언트 / 타임아웃 ─────────────
echo "== 6. 유휴 클라이언트 (수신 타임아웃 5초)"
sleep 15 | nc 127.0.0.1 "$PORT" >/dev/null &
NCPID=$!
sleep 0.3
t0=$(now_ms)
code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 10 "http://127.0.0.1:$PORT/")
elapsed=$(( $(now_ms) - t0 ))
expect_eq "유휴 클라이언트 뒤 요청도 처리됨" "$code" "200"
[ "$elapsed" -ge 3500 ] && [ "$elapsed" -le 7000 ] && ok "대기 시간 약 5초 (${elapsed}ms)" || bad "대기 시간" "${elapsed}ms"
kill "$NCPID" 2>/dev/null

# 요청 라인만 보내고 멈춤 → 요청 도중의 타임아웃이므로 조용히 끊지 않고 408
got=$({ printf 'GET / HTTP/1.1\r\n'; sleep 8; } | nc -q 1 127.0.0.1 "$PORT" | head -1 | tr -d '\r')
expect_eq "요청 도중 멈춘 클라이언트 → 408" "$got" "HTTP/1.1 408 Request Timeout"

# 1바이트씩 천천히 보내는 slowloris → 요청 전체 제한(10초)에 걸려 408
got=$({ printf 'GET / HTTP/1.1\r\nX-Slow: '; for _ in $(seq 14); do printf 'a'; sleep 1; done; } \
      | nc -q 1 127.0.0.1 "$PORT" | head -1 | tr -d '\r')
expect_eq "slowloris(1초에 1바이트) → 408" "$got" "HTTP/1.1 408 Request Timeout"

# ───────────── 7. 같은 포트 중복 실행 ─────────────
echo "== 7. 포트 충돌"
timeout 3 "$SERVER" -p "$PORT" >/dev/null 2>&1
expect_eq "사용 중인 포트로 두 번째 실행 → exit 1" "$?" "1"

# ───────────── 8. 종료 처리 ─────────────
echo "== 8. 종료 처리"
t0=$(now_ms)
kill -INT "$PID"; wait_exit 10; rc=$?
elapsed=$(( $(now_ms) - t0 ))
expect_eq "SIGINT → exit 0" "$rc" "0"
[ "$elapsed" -le 1000 ] && ok "SIGINT 즉시 종료 (${elapsed}ms)" || bad "SIGINT 종료 시간" "${elapsed}ms"
grep -q "server stopped" "$LOG" && ok "종료 메시지 출력" || bad "종료 메시지" "$(cat "$LOG")"

tw=$(ss -tan state time-wait "( sport = :$PORT )" | tail -n +2 | wc -l)
start_server && ok "TIME_WAIT ${tw}개 남은 상태에서 바로 재시작 (SO_REUSEADDR)" || bad "재시작" "$(cat "$LOG")"
kill -TERM "$PID"; wait_exit 10; rc=$?
expect_eq "SIGTERM → exit 0" "$rc" "0"

start_server
sleep 15 | nc 127.0.0.1 "$PORT" >/dev/null &
NCPID=$!
sleep 0.3
t0=$(now_ms)
kill -INT "$PID"; wait_exit 10; rc=$?
elapsed=$(( $(now_ms) - t0 ))
kill "$NCPID" 2>/dev/null
expect_eq "유휴 클라이언트가 붙어 있어도 SIGINT → exit 0" "$rc" "0"
[ "$elapsed" -le 7000 ] && ok "타임아웃 안에 종료 (${elapsed}ms)" || bad "종료 지연" "${elapsed}ms"

# 대기 중 연속 시그널 여러 번 → 한 번만 정상 종료
start_server
kill -INT "$PID"; kill -INT "$PID" 2>/dev/null; kill -TERM "$PID" 2>/dev/null
wait_exit 10; rc=$?
expect_eq "시그널 연속 3번 → exit 0" "$rc" "0"
expect_eq "종료 후 새 연결은 거부됨" "$(curl -s -o /dev/null -w '%{http_code}' --max-time 2 "http://127.0.0.1:$PORT/")" "000"

# ───────────── 9. 종료 시그널 경쟁 조건 (플래그 확인 ~ 대기 시작 사이) ─────────────
# 훅 없이 그 틈을 정확히 맞출 수는 없으므로, 틈이 원리적으로 없는지를 결정적으로 검증한다:
#   (a) 대기(pselect) 중에는 SIGINT/SIGTERM이 열려 있고
#   (b) 그 밖(연결 처리 중)에서는 막혀 있어서, 그때 온 시그널은 사라지지 않고 보류된 뒤
#   (c) 다음 대기 진입 순간 바로 처리되어 종료된다
echo "== 9. 종료 시그널 경쟁 조건"
mask_of() { awk -v k="$2:" '$1 == k { print $2 }' /proc/"$1"/status; }
has_bits() { [ $(( 0x$1 & 0x4002 )) -eq $(( 0x4002 )) ]; }  # SIGINT(2) → bit1, SIGTERM(15) → bit14

start_server
blk=$(mask_of "$PID" SigBlk)
has_bits "$blk" && bad "(a) 대기 중엔 SIGINT/SIGTERM 허용" "SigBlk=$blk" || ok "(a) 대기 중엔 SIGINT/SIGTERM 허용 (SigBlk=$blk)"

sleep 15 | nc 127.0.0.1 "$PORT" >/dev/null &
NCPID=$!
sleep 0.5                                       # 서버가 이 연결을 처리하며 read에서 막힘
blk=$(mask_of "$PID" SigBlk)
has_bits "$blk" && ok "(b) 연결 처리 중엔 SIGINT/SIGTERM 막힘 (SigBlk=$blk)" || bad "(b) 처리 중 막힘" "SigBlk=$blk"
kill -INT "$PID"
sleep 0.2
pnd=$(mask_of "$PID" ShdPnd)
has_bits "$pnd" >/dev/null; [ $(( 0x$pnd & 0x2 )) -ne 0 ] && ok "(b) 처리 중 온 SIGINT는 보류됨 (ShdPnd=$pnd)" || bad "(b) SIGINT 보류" "ShdPnd=$pnd"
kill -0 "$PID" 2>/dev/null && ok "(b) 보류 중엔 아직 살아 있음" || bad "(b) 보류 중 생존" "이미 종료"
t0=$(now_ms)
wait_exit 10; rc=$?
elapsed=$(( $(now_ms) - t0 ))
kill "$NCPID" 2>/dev/null
expect_eq "(c) 보류된 SIGINT로 정상 종료" "$rc" "0"
[ "$elapsed" -le 7000 ] && ok "(c) 연결 처리가 끝나자 바로 종료 (${elapsed}ms)" || bad "(c) 종료 지연" "${elapsed}ms"

echo
echo "결과: 통과 $PASS, 실패 $FAIL"
[ "$FAIL" -eq 0 ]
