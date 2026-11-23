# SW-AI-W13-WEB-SERVER

POSIX 소켓 API만으로 처음부터 만드는 C 웹서버 + Forward / Reverse Proxy (WEEK13 팀 과제)

> 현재 상태: **뼈대(skeleton)** — 모든 모듈의 인터페이스(헤더)와 빈 구현(`TODO`)만 있습니다.

## 설계 (팀 확정)

| 항목 | 선택 | 이유 |
|---|---|---|
| 동시성 | 스레드 풀 (`pthread`, 생산자-소비자 큐) | 스레드 생성 비용 없음, 워커 수로 동시 처리량 제한, 공유 상태(Round-Robin 카운터) 사용 쉬움 |
| 프록시 | **Reverse + Forward** | Reverse: 경로 prefix 라우팅 + Round-Robin + 502/504. Forward: 절대 URI 요청을 원 서버로 중계 (`-f`로 켤 때만) |
| 동적 처리 | CGI (`fork` + `execve`) | 서버와 분리된 프로그램 실행, CSAPP Tiny와 같은 방식 |
| 반환 규칙 | `0` 성공 · 양수 = 보낼 HTTP 오류 코드 · `-1` 연결 종료 | 오류 응답을 `handle_connection` 한 곳에서만 보냄 |
| 헤더 저장 | 고정 배열 32개 | malloc/free 없음, 상한 초과 시 바로 431 |
| 크기 상한 | URI 2KB(414) · 헤더 줄 8KB · 헤더 32개(431) · 본문 1MB(413) | 과도한 요청으로 메모리 고갈 방지 |
| HTTP 범위 | GET · POST · HEAD (그 외 501), HTTP/1.0·1.1, keep-alive + 유휴 타임아웃 5초, chunked 미지원 | |
| 설정 | CLI 인자 + `conf/routes.conf` | |

## 요청 처리 흐름

```
main ─ 인자 파싱, 시그널 설정 (SIGPIPE 무시, SIGINT/SIGTERM → 안전 종료)
 └ run_server ─ open_listenfd → threadpool_create → accept 루프 → threadpool_submit
      └ (워커) handle_connection ─ read_request (keep-alive 동안 반복)
            ├ 절대 URI (http://...)   → forward_proxy_request  (-f 없으면 403)
            ├ 라우팅 규칙과 일치       → reverse_proxy_request  (→ 백엔드, Round-Robin)
            ├ /cgi-bin/...            → serve_cgi
            └ 그 외                   → serve_static
```

## 폴더 구조

```
src/
  main.c                 진입점: 인자 파싱, 시그널
  server.{h,c}           accept 루프, 연결 하나 처리(분기), keep-alive
  net.{h,c}              open_listenfd / open_clientfd
  rio.{h,c}              short count 처리 read/write
  http_request.{h,c}     요청 파서            ⚠ 팀 공통 인터페이스
  http_response.{h,c}    응답 생성, 오류 응답
  mime.{h,c}             확장자 → Content-Type
  static_file.{h,c}      정적 파일 서빙
  cgi.{h,c}              CGI 실행
  router.{h,c}           라우팅 규칙, Round-Robin
  proxy.{h,c}            프록시 공통 중계 + Reverse Proxy, 502/504
  forward_proxy.{h,c}    절대 URI 파싱 + Forward Proxy
  threadpool.{h,c}       스레드 풀
tests/                   단위 테스트(test_*.c), 기능(functional.sh), 부하(load.sh)
www/                     정적 파일 루트
cgi-bin/                 CGI 프로그램
conf/routes.conf         Reverse Proxy 라우팅 규칙
.github/workflows/ci.yml PR마다 make(-Werror) → make test → make asan
```

## 빌드 · 실행

WSL2(Ubuntu) / Linux / macOS

```bash
make            # bin/server, cgi-bin/adder
make test       # 단위 테스트
make asan       # AddressSanitizer로 빌드 후 테스트
make clean

./bin/server -p 8080 -r ./www                          # 웹서버
./bin/server -p 8001 -r ./www                          # 백엔드 1
./bin/server -p 8002 -r ./www                          # 백엔드 2
./bin/server -p 8080 -r ./www -c conf/routes.conf      # Reverse Proxy
./bin/server -p 8080 -r ./www -f                       # Forward Proxy 허용

curl http://127.0.0.1:8080/api/                                   # Reverse
curl -x 127.0.0.1:8080 http://127.0.0.1:8001/index.html           # Forward
```

| 옵션 | 기본값 | 의미 |
|---|---|---|
| `-p` | 8080 | 포트 |
| `-r` | ./www | 정적 파일 루트 |
| `-g` | ./cgi-bin | CGI 폴더 |
| `-c` | 없음 | 라우팅 설정 파일 (지정하면 Reverse Proxy 동작) |
| `-f` | 꺼짐 | Forward Proxy 허용 |
| `-t` | 8 | 워커 스레드 수 |
| `-q` | 1024 | 연결 대기 큐 크기 (차면 503) |

## 모듈 담당

| 묶음 | 파일 | 담당 |
|---|---|---|
| 서버 코어 | server, net, rio, threadpool, main(시그널) | |
| HTTP | http_request, http_response, mime, static_file | |
| 프록시 | router, proxy, forward_proxy | |
| 동적 · 검증 | cgi, cgi-bin/, tests/functional.sh, load.sh, README | |

## 협업 규칙 (fork 방식)

```bash
# 처음 한 번
git clone https://github.com/<내계정>/SW-AI-W13-WEB-SERVER.git
cd SW-AI-W13-WEB-SERVER
git remote add upstream https://github.com/kimgeonhui1/SW-AI-W13-WEB-SERVER.git

# 작업마다
git fetch upstream
git switch -c feat/<이름>-<모듈> upstream/dev
# ... 작업, 커밋 ...
git fetch upstream && git merge upstream/dev   # PR 전에 최신 dev 반영, 충돌은 직접 해결
git push origin feat/<이름>-<모듈>
# GitHub에서 원본 repo의 dev로 PR
```

- **PR 대상:** 원본 repo의 `dev`. 발표 전(목요일 오전)에 `dev` → `main`
- **merge:** 승인 1명 이상 → PR 작성자가 **Squash and merge**
- **PR 크기:** 모듈 하나 또는 기능 하나. 설명에 "무엇을 했는지 + 어떻게 테스트했는지"
- **리뷰:** 반나절 안에 응답
- **완료 기준:** `make` 경고 0 · `make test` 통과 · `make asan` 오류·누수 없음 · 새 기능에 단위 테스트 (CI가 자동 확인)
- **공통 헤더**(`http_request.h` 등 여러 모듈이 쓰는 헤더): 이슈로 먼저 합의 → 그 변경만 담은 작은 PR
- **커밋:** `feat:` `fix:` `test:` `docs:` `refactor:` `chore:`
- **이슈:** 작업마다 이슈 → Projects 보드, PR 본문에 `Closes #번호`
