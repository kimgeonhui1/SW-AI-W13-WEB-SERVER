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
# 경과 시간은 부팅 후 시간(/proc/uptime, 거꾸로 가지 않음)으로 잰다.
# date(시스템 시계)는 WSL2가 Windows 시계와 맞추며 뒤로 갈 수 있어 음수 경과 시간이 나와 "빠름" 검사를 잘못 통과시켰다
now_ms() { awk '{ printf "%d", $1 * 1000 }' /proc/uptime; }
# URL이 200을 줄 때까지 최대 $2초 재시도하고 마지막 상태 코드를 출력 (고정 sleep 대신 조건이 될 때까지 대기)
until_200() {
    local code=000
    for _ in $(seq $(( $2 * 5 ))); do
        code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 2 "$1")
        [ "$code" = 200 ] && break
        sleep 0.2
    done
    echo "$code"
}

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
expect_eq "CGI GET adder" "$(curl -s "$U/cgi-bin/adder?a=1&b=2")" "1 + 2 = 3"
expect_eq "CGI POST adder (본문 → 표준 입력)" "$(curl -s -d 'a=40&b=2' "$U/cgi-bin/adder")" "40 + 2 = 42"
expect_eq "CGI Status 헤더 반영 (잘못된 입력 → 400)" "$(curl -s -o /dev/null -w '%{http_code}' "$U/cgi-bin/adder?a=x&b=2")" "400"
head_cgi=$(curl -s -I "$U/cgi-bin/adder?a=1&b=2" | tr -d '\r')
# 응답이 빈 줄(\r\n\r\n)로 끝나야 본문이 없는 것 ($(...)는 끝 줄바꿈을 지우므로 마지막 4바이트를 16진수로 비교)
tail4=$(printf 'HEAD /cgi-bin/adder?a=1&b=2 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' | nc -q 2 127.0.0.1 "$PORT" | tail -c 4 | od -An -tx1 | tr -d ' \n')
echo "$head_cgi" | grep -q '^Content-Length: 10$' && [ "$tail4" = "0d0a0d0a" ] \
    && ok "CGI HEAD: 본문 없음, Content-Length는 GET과 같음" || bad "CGI HEAD" "$head_cgi"
for cl in 10abc 99999999999999999999 -1 ""; do
    got=$(CONTENT_LENGTH="$cl" REQUEST_METHOD=POST ./cgi-bin/adder </dev/null | head -1 | tr -d '\r')
    expect_eq "adder 단독 실행, 잘못된 CONTENT_LENGTH='$cl' → 400" "$got" "Status: 400 Bad Request"
done
expect_eq "없는 CGI → 404" "$(curl -s -o /dev/null -w '%{http_code}' "$U/cgi-bin/nope")" "404"
expect_eq "CGI 하위 경로 탈출 → 404" "$(curl -s --path-as-is -o /dev/null -w '%{http_code}' "$U/cgi-bin/../Makefile")" "404"
expect_eq "CGI Content-Length 있음" "$(curl -s -o /dev/null -w '%{size_download}' "$U/cgi-bin/adder?a=1&b=2")" "10"
conns=$(curl -s -o /dev/null -o /dev/null -w '%{num_connects} ' "$U/cgi-bin/adder?a=1&b=2" "$U/cgi-bin/adder?a=3&b=4")
expect_eq "CGI 응답 뒤 keep-alive 유지 (새 연결 수)" "$conns" "1 0 "
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
expect_eq "응답 안 읽고 끊기 30회 뒤" "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/")" "200"

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
echo "== 6. 유휴 클라이언트 · 동시 처리 (스레드 풀)"
NCPIDS=()
for _ in 1 2 3; do nc -d 127.0.0.1 "$PORT" >/dev/null & NCPIDS+=($!); done   # 워커 3개를 붙잡는 유휴 연결
sleep 0.3
t0=$(now_ms)
code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 10 "http://127.0.0.1:$PORT/")
elapsed=$(( $(now_ms) - t0 ))
expect_eq "유휴 연결 3개가 있어도 다른 요청 처리됨" "$code" "200"
[ "$elapsed" -le 1000 ] && ok "기다리지 않고 바로 응답 (${elapsed}ms, 다른 워커가 처리)" || bad "응답 지연" "${elapsed}ms"
kill "${NCPIDS[@]}" 2>/dev/null; wait "${NCPIDS[@]}" 2>/dev/null

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
nc -d 127.0.0.1 "$PORT" >/dev/null &
NCPID=$!
sleep 0.3
t0=$(now_ms)
kill -INT "$PID"; wait_exit 10; rc=$?
elapsed=$(( $(now_ms) - t0 ))
kill "$NCPID" 2>/dev/null; wait "$NCPID" 2>/dev/null
expect_eq "유휴 클라이언트가 붙어 있어도 SIGINT → exit 0" "$rc" "0"
[ "$elapsed" -le 7000 ] && ok "타임아웃 안에 종료 (${elapsed}ms)" || bad "종료 지연" "${elapsed}ms"

# 대기 중 연속 시그널 여러 번 → 한 번만 정상 종료
start_server
kill -INT "$PID"; kill -INT "$PID" 2>/dev/null; kill -TERM "$PID" 2>/dev/null
wait_exit 10; rc=$?
expect_eq "시그널 연속 3번 → exit 0" "$rc" "0"
expect_eq "종료 후 새 연결은 거부됨" "$(curl -s -o /dev/null -w '%{http_code}' --max-time 2 "http://127.0.0.1:$PORT/")" "000"

# ───────────── 9. 종료 시그널 경쟁 조건 (플래그 확인 ~ 대기 시작 사이) ─────────────
# 훅 없이 그 틈을 정확히 맞출 수는 없으므로, 틈이 원리적으로 없는지를 시그널 마스크로 결정적으로 검증한다
echo "== 9. 종료 시그널 경쟁 조건 · 시그널을 받는 스레드"
mask_of() { awk -v k="$2:" '$1 == k { print $2 }' /proc/"$1"/status; }
has_bits() { [ $(( 0x$1 & 0x4002 )) -eq $(( 0x4002 )) ]; }  # SIGINT(2) → bit1, SIGTERM(15) → bit14

# 스레드 풀 구조에서의 기대:
#   (a) 메인 스레드는 pselect 대기 중이므로 SIGINT/SIGTERM이 열려 있고
#   (b) 워커 스레드들은 모두 막고 있어서 시그널이 처리 중인 워커로 가지 않는다
#   (c) 연결 처리 중에 SIGINT → 메인이 바로 받아 새 연결을 막고, 워커가 그 연결을 끝내면 종료
task_mask() { awk -v k="$2:" '$1 == k { print $2 }' /proc/"$1"/task/"$3"/status; }

start_server
blk=$(mask_of "$PID" SigBlk)
has_bits "$blk" && bad "(a) 메인 스레드는 대기 중 SIGINT/SIGTERM 허용" "SigBlk=$blk" || ok "(a) 메인 스레드는 대기 중 SIGINT/SIGTERM 허용 (SigBlk=$blk)"
nworkers=0; unblocked=0
for tid in $(ls /proc/"$PID"/task); do
    [ "$tid" = "$PID" ] && continue
    nworkers=$((nworkers + 1))
    has_bits "$(task_mask "$PID" SigBlk "$tid")" || unblocked=$((unblocked + 1))
done
expect_eq "(b) 워커 스레드 수 (-t 기본 8)" "$nworkers" "8"
expect_eq "(b) SIGINT/SIGTERM을 받을 수 있는 워커 수" "$unblocked" "0"

nc -d 127.0.0.1 "$PORT" >/dev/null &
NCPID=$!
sleep 0.5                                       # 워커 하나가 이 연결을 처리하며 read에서 막힘
t0=$(now_ms)
kill -INT "$PID"
sleep 0.3
code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 2 "http://127.0.0.1:$PORT/")
expect_eq "(c) SIGINT 직후 새 연결은 받지 않음 (듣기 소켓 닫음)" "$code" "000"
kill -0 "$PID" 2>/dev/null && ok "(c) 처리 중인 연결이 끝날 때까지는 살아 있음" || bad "(c) 처리 중 생존" "이미 종료"
wait_exit 10; rc=$?
elapsed=$(( $(now_ms) - t0 ))
kill "$NCPID" 2>/dev/null; wait "$NCPID" 2>/dev/null
expect_eq "(c) 처리 중인 연결을 마치고 정상 종료" "$rc" "0"
[ "$elapsed" -le 7000 ] && ok "(c) 유휴 타임아웃 안에 종료 (${elapsed}ms)" || bad "(c) 종료 지연" "${elapsed}ms"

# ───────────── 10. 스레드 풀 포화 → 503 ─────────────
echo "== 10. 스레드 풀 포화"
start_server -t 1 -q 1
nc -d 127.0.0.1 "$PORT" >/dev/null & B1=$!
sleep 0.3                                       # 워커 1개가 이 연결에 묶임
nc -d 127.0.0.1 "$PORT" >/dev/null & B2=$!
sleep 0.3                                       # 큐 1칸도 참
t0=$(now_ms)
code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "http://127.0.0.1:$PORT/")
elapsed=$(( $(now_ms) - t0 ))
expect_eq "워커·큐 모두 차면 503" "$code" "503"
[ "$elapsed" -le 1000 ] && ok "기다리지 않고 바로 거절 (${elapsed}ms)" || bad "503 지연" "${elapsed}ms"
kill "$B1" "$B2" 2>/dev/null; wait "$B1" "$B2" 2>/dev/null   # 클라이언트 종료를 확실히 기다린 뒤 복구 확인
expect_eq "포화가 풀리면 다시 200" "$(until_200 "http://127.0.0.1:$PORT/" 15)" "200"
stop_server

# 붐빌 때 keep-alive 양보: 워커 1개가 keep-alive 연결 A를 처리 중일 때 B가 큐에서 기다리면,
# A에 응답한 뒤 연결을 끊고 워커를 B에게 넘겨야 한다 (안 그러면 B는 A의 유휴 타임아웃 5초를 기다림)
start_server -t 1 -q 4
{ sleep 0.6; printf 'GET / HTTP/1.1\r\nHost: x\r\n\r\n'; sleep 10; } | nc 127.0.0.1 "$PORT" >/tmp/w13_conn_a 2>&1 & A=$!
sleep 0.2                                       # A가 워커를 먼저 차지 (아직 요청 전)
t0=$(now_ms)
code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 10 "http://127.0.0.1:$PORT/")   # B: 큐에서 대기
elapsed=$(( $(now_ms) - t0 ))
expect_eq "큐에서 기다린 요청도 처리됨" "$code" "200"
[ "$elapsed" -le 2000 ] && ok "keep-alive 연결이 응답 후 워커를 양보 (${elapsed}ms)" || bad "워커 양보 안 함" "${elapsed}ms (A의 유휴 타임아웃까지 대기)"
grep -q '^Connection: close' /tmp/w13_conn_a && ok "양보한 연결 A의 응답은 Connection: close" || bad "A 응답 헤더" "$(head -5 /tmp/w13_conn_a)"
kill "$A" 2>/dev/null; wait "$A" 2>/dev/null
rm -f /tmp/w13_conn_a
stop_server

# ───────────── 11. fd 한도(EMFILE) ─────────────
echo "== 11. fd 한도 (ulimit -n 64)"
( ulimit -n 64; exec "$SERVER" -p "$PORT" ) >"$LOG" 2>&1 &
PID=$!
sleep 0.4
HOLD=()
for _ in $(seq 90); do nc -d 127.0.0.1 "$PORT" >/dev/null 2>&1 & HOLD+=($!); done   # fd 64개를 넘김
sleep 1
ticks() { awk '{ print $14 + $15 }' /proc/"$PID"/stat; }   # 사용자+커널 CPU 시간 (1/100초)
c0=$(ticks); sleep 1; c1=$(ticks)
kill -0 "$PID" 2>/dev/null && ok "fd 한도에 걸려도 서버 생존" || bad "EMFILE 생존" "종료됨"
[ $((c1 - c0)) -le 30 ] && ok "헛돌지 않음 (1초 동안 CPU $((c1 - c0))/100초)" || bad "EMFILE 바쁜 대기" "CPU $((c1 - c0))/100초"
kill "${HOLD[@]}" 2>/dev/null; wait "${HOLD[@]}" 2>/dev/null
expect_eq "fd가 돌아오면 다시 200" "$(until_200 "http://127.0.0.1:$PORT/" 20)" "200"
left=$(for p in "${HOLD[@]}"; do kill -0 "$p" 2>/dev/null && echo "$p"; done | wc -l)
expect_eq "연결을 붙잡던 클라이언트 프로세스가 모두 정리됨" "$left" "0"
stop_server

# ───────────── 12. CGI 격리 ─────────────
echo "== 12. CGI 격리"
CGID=$(mktemp -d)
cat > "$CGID/fds" <<'SH'
#!/bin/sh
printf 'Content-Type: text/plain\n\n'
n=0
for f in /proc/$$/fd/*; do case "$(readlink "$f")" in socket:*) n=$((n + 1));; esac; done
echo "$n"
SH
printf '#!/bin/sh\nprintf "Content-Type: a\\n\\n"\nsleep 31\n' > "$CGID/hang"
chmod 755 "$CGID/fds" "$CGID/hang"
start_server -g "$CGID"
expect_eq "CGI가 서버의 소켓(듣기·클라이언트 연결)을 물려받지 않음" "$(curl -s "http://127.0.0.1:$PORT/cgi-bin/fds")" "0"
t0=$(now_ms)
expect_eq "멈춘 CGI → 504" "$(curl -s -o /dev/null -w '%{http_code}' --max-time 15 "http://127.0.0.1:$PORT/cgi-bin/hang")" "504"
elapsed=$(( $(now_ms) - t0 ))
[ "$elapsed" -le 7000 ] && ok "CGI 시간 제한 안에 응답 (${elapsed}ms)" || bad "CGI 시간 제한" "${elapsed}ms"
sleep 0.2
expect_eq "시간 초과 CGI가 띄운 자식(sleep)까지 종료" "$(pgrep -fc 'sleep 31$')" "0"
stop_server
rm -rf "$CGID"

# ───────────── 12-2. SIGPIPE ─────────────
# 응답이 작으면 커널 송신 버퍼에 한 번에 들어가 SIGPIPE가 나지 않는다.
# 20MB를 보내는 도중 클라이언트가 끊으면 서버의 다음 write가 EPIPE → SIGPIPE를 무시하지 않으면 서버가 죽는다
echo "== 12-2. 큰 응답 도중 클라이언트가 끊음 (SIGPIPE)"
SP=$(mktemp -d)
head -c 20000000 /dev/zero > "$SP/big.bin"
printf 'ok' > "$SP/index.html"
start_server -r "$SP"
# 요청을 보내자마자(응답이 오기 전에) 정상 종료(FIN)한다. 서버가 FIN을 받은 뒤 쓰면 상대가 RST로 답하고,
# 그 다음 write가 EPIPE → SIGPIPE. (응답을 받은 채로 닫으면 RST가 먼저 가서 ECONNRESET이 되고 SIGPIPE는 안 난다)
for _ in $(seq 10); do
    ( exec 3<>"/dev/tcp/127.0.0.1/$PORT" && printf 'GET /big.bin HTTP/1.1\r\nHost: x\r\n\r\n' >&3 ) 2>/dev/null
done
sleep 0.3
kill -0 "$PID" 2>/dev/null && ok "큰 응답 도중 끊겨도 서버 생존 (SIGPIPE 무시)" || bad "SIGPIPE" "서버 종료됨"
expect_eq "그 뒤 요청도 정상" "$(curl -s --max-time 3 "http://127.0.0.1:$PORT/")" "ok"
stop_server
rm -rf "$SP"

# ───────────── 13. Reverse Proxy ─────────────
echo "== 13. Reverse Proxy"
RP=$(mktemp -d)
B1=$((PORT + 11)); B2=$((PORT + 12))
for i in 1 2; do mkdir -p "$RP/b$i/api"; printf "b$i" > "$RP/b$i/api/who.txt"; done
"$SERVER" -p $B1 -r "$RP/b1" >/dev/null 2>&1 & BP1=$!
"$SERVER" -p $B2 -r "$RP/b2" >/dev/null 2>&1 & BP2=$!
printf '# test\n/api/ 127.0.0.1:%s 127.0.0.1:%s\n' $B1 $B2 > "$RP/routes.conf"
until_200 "http://127.0.0.1:$B1/api/who.txt" 5 >/dev/null; until_200 "http://127.0.0.1:$B2/api/who.txt" 5 >/dev/null
start_server -c "$RP/routes.conf" && ok "routes.conf로 시작" || bad "Reverse Proxy 시작" "$(cat "$LOG")"
grep -q "1 route(s)" "$LOG" && ok "규칙 1개 로드 메시지" || bad "로드 메시지" "$(cat "$LOG")"
RU="http://127.0.0.1:$PORT"
two="$(curl -s "$RU/api/who.txt") $(curl -s "$RU/api/who.txt")"
[ "$two" = "b1 b2" ] || [ "$two" = "b2 b1" ] && ok "Round-Robin: 두 백엔드가 번갈아 응답 ($two)" || bad "Round-Robin" "$two"
curl -s "$RU/" | cmp -s - www/index.html && ok "규칙에 없는 경로는 프록시가 직접 정적 파일" || bad "직접 처리" "다름"
expect_eq "백엔드의 404를 그대로 전달" "$(curl -s -o /dev/null -w '%{http_code}' "$RU/api/nope")" "404"
expect_eq "백엔드의 405(POST)를 그대로 전달" "$(curl -s -o /dev/null -w '%{http_code}' -d x=1 "$RU/api/who.txt")" "405"
expect_eq "HEAD 전달 (본문 없이 바로 끝남)" "$(curl -s -I --max-time 2 -o /dev/null -w '%{http_code}' "$RU/api/who.txt")" "200"
conns=$(curl -s -o /dev/null -o /dev/null -w '%{num_connects} ' "$RU/api/who.txt" "$RU/api/who.txt")
expect_eq "프록시를 거쳐도 클라이언트 keep-alive 유지 (새 연결 수)" "$conns" "1 0 "
kill -INT $BP1; wait $BP1
got=$(for _ in 1 2 3 4; do curl -s "$RU/api/who.txt"; done)
expect_eq "백엔드 1 종료 → 모든 요청이 백엔드 2로" "$got" "b2b2b2b2"
kill -INT $BP2; wait $BP2
expect_eq "백엔드 모두 종료 → 502" "$(curl -s -o /dev/null -w '%{http_code}' "$RU/api/who.txt")" "502"
expect_eq "백엔드가 없어도 직접 처리 경로는 정상" "$(curl -s -o /dev/null -w '%{http_code}' "$RU/")" "200"
stop_server
printf '/api/ 127.0.0.1:8001\n/bad/ nocolon\n' > "$RP/bad.conf"
out=$(timeout 3 "$SERVER" -p "$PORT" -c "$RP/bad.conf" 2>&1); rc=$?
expect_eq "형식이 틀린 routes.conf → exit 1" "$rc" "1"
echo "$out" | grep -q "bad.conf:2:" && ok "오류 줄 번호 출력 (bad.conf:2)" || bad "오류 줄 번호" "$out"
timeout 3 "$SERVER" -p "$PORT" -c "$RP/none.conf" >/dev/null 2>&1
expect_eq "없는 routes.conf → exit 1" "$?" "1"
rm -rf "$RP"

# ───────────── 14. Forward Proxy ─────────────
echo "== 14. Forward Proxy (-f)"
FP=$(mktemp -d)
O=$((PORT + 21))
mkdir -p "$FP/www"; printf 'origin' > "$FP/www/o.txt"
"$SERVER" -p $O -r "$FP/www" >/dev/null 2>&1 & OP=$!
until_200 "http://127.0.0.1:$O/o.txt" 5 >/dev/null
start_server -f
PX="127.0.0.1:$PORT"
expect_eq "curl -x 로 원 서버 파일 받기" "$(curl -s -x "$PX" "http://127.0.0.1:$O/o.txt")" "origin"
expect_eq "원 서버의 404 전달" "$(curl -s -o /dev/null -w '%{http_code}' -x "$PX" "http://127.0.0.1:$O/nope")" "404"
expect_eq "쿼리 포함 URI" "$(curl -s -x "$PX" "http://127.0.0.1:$O/o.txt?v=1")" "origin"
conns=$(curl -s -o /dev/null -o /dev/null -w '%{num_connects} ' -x "$PX" "http://127.0.0.1:$O/o.txt" "http://127.0.0.1:$O/o.txt")
expect_eq "프록시 연결 keep-alive 유지 (새 연결 수)" "$conns" "1 0 "
raw=$(printf 'GET HTTP://127.0.0.1:%s/o.txt HTTP/1.1\r\nHost: wrong.example\r\nConnection: close\r\n\r\n' $O | nc -q 2 127.0.0.1 "$PORT" | tail -c 6)
expect_eq "스킴 대소문자 무시 + Host가 달라도 URI 기준" "$raw" "origin"
# curl은 userinfo를 URI에서 빼서 보내므로 nc로 직접
st=$(printf 'GET http://a@127.0.0.1:%s/o.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' $O | nc -q 2 127.0.0.1 "$PORT" | head -1 | tr -d '\r')
expect_eq "userinfo(@) URI → 400" "$st" "HTTP/1.1 400 Bad Request"
# 프록시가 자기 자신을 대상으로 받아도 원 서버엔 경로만("GET /") 보내므로 다시 프록시 요청이 되지 않는다 (루프 없음)
expect_eq "자기 자신을 대상으로 → 한 번만 거쳐 정적 파일" "$(curl -s --max-time 5 -o /dev/null -w '%{http_code}' -x "$PX" "http://127.0.0.1:$PORT/")" "200"
kill -INT $OP; wait $OP
expect_eq "원 서버 꺼짐 → 502" "$(curl -s -o /dev/null -w '%{http_code}' -x "$PX" "http://127.0.0.1:$O/o.txt")" "502"
expect_eq "없는 도메인 → 502" "$(curl -s -o /dev/null -w '%{http_code}' -x "$PX" "http://no-such-host.invalid/")" "502"
expect_eq "-f여도 일반 요청은 직접 처리" "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/")" "200"
stop_server
rm -rf "$FP"

echo
echo "결과: 통과 $PASS, 실패 $FAIL"
[ "$FAIL" -eq 0 ]
