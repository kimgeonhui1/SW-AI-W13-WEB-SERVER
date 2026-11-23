#!/usr/bin/env bash
# [load.sh] 부하 테스트 — 동시 연결 수에 따른 처리량(RPS)과 지연 시간 측정
# 사용: 서버 실행 후 tests/load.sh
set -u

URL=${URL:-http://127.0.0.1:8080/index.html}

# TODO: 동시 연결 수를 바꿔 가며 측정하고 결과를 README에 표로 기록
#   wrk -t4 -c10  -d10s "$URL"
#   wrk -t4 -c100 -d10s "$URL"
#   ab -n 10000 -c 100 "$URL"
echo "TODO: load tests against $URL"
