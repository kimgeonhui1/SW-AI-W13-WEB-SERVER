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

resp=$(curl -s -i "http://127.0.0.1:$PORT/" | tr -d '\r')
expect_eq "상태 라인" "$(echo "$resp" | head -1)" "HTTP/1.1 200 OK"
echo "$resp" | grep -q '^Content-Type: text/plain$' && ok "Content-Type 헤더" || bad "Content-Type 헤더" "$resp"
echo "$resp" | grep -q '^Content-Length: 13$' && ok "Content-Length 헤더" || bad "Content-Length 헤더" "$resp"
echo "$resp" | grep -q '^Connection: close$' && ok "Connection 헤더" || bad "Connection 헤더" "$resp"
expect_eq "본문 (CRLF로 끝남)" "$(curl -s "http://127.0.0.1:$PORT/" | od -An -c | tr -s ' ' | tr -d '\n')" " H e l l o , W 1 3 ! \r \n"
expect_eq "HTTP/1.0 요청" "$(curl -s -0 -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/")" "200"
expect_eq "POST 요청" "$(curl -s -o /dev/null -w '%{http_code}' -d 'a=1' "http://127.0.0.1:$PORT/")" "200"
expect_eq "localhost 이름으로 접속" "$(curl -s -o /dev/null -w '%{http_code}' "http://localhost:$PORT/")" "200"

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
expect_eq "20KB 헤더 줄 (1단계는 잘라 읽고 응답)" "$got" "HTTP/1.1 200 OK"

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
