#!/usr/bin/env bash
# [functional.sh] 기능 테스트 — 실제 서버를 띄우고 curl로 응답 확인
# 사용: make && tests/functional.sh
set -u

PORT=${PORT:-18080}

# TODO: 서버를 백그라운드로 실행하고 종료 시 kill (trap)
# TODO: 확인할 시나리오 (상태 코드는 curl -s -o /dev/null -w '%{http_code}')
#   - GET /index.html → 200, Content-Type: text/html
#   - GET /style.css, /app.js → 200, 올바른 Content-Type
#   - GET /nope.html → 404
#   - GET /../Makefile (curl --path-as-is) → 403
#   - POST /cgi-bin/adder (body a=1&b=2) → 200, 결과 3
#   - 잘못된 요청 (printf 'BAD\r\n\r\n' | nc) → 400
#   - Reverse Proxy: 백엔드 2개(8001, 8002) 띄우고 /api/ 요청이 번갈아 가는지
#   - 백엔드 꺼짐 → 502
#   - Forward Proxy: 서버를 -f로 실행, curl -x 127.0.0.1:$PORT http://127.0.0.1:8001/index.html → 200
#   - Forward Proxy 꺼짐(-f 없음) 상태에서 같은 요청 → 403
#   - 동시 접속: curl 여러 개를 & 로 동시에 실행해 모두 200
echo "TODO: functional tests"
