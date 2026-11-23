// [server.c] 서버 본체 구현
#include "server.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "cgi.h"
#include "forward_proxy.h"
#include "http_request.h"
#include "http_response.h"
#include "net.h"
#include "rio.h"
#include "proxy.h"
#include "router.h"
#include "static_file.h"
#include "threadpool.h"

// 종료 플래그: 시그널 핸들러(메인 스레드)가 쓰고 워커 스레드들이 읽는다.
// volatile sig_atomic_t는 "시그널 ↔ 같은 스레드"만 보장하고 스레드 사이에선 데이터 경쟁(정의되지 않은 동작)이므로
// C11 atomic을 쓴다. lock-free여야 시그널 핸들러 안에서 써도 안전하다
#if ATOMIC_INT_LOCK_FREE != 2
#error "atomic_int must be lock-free to be used from a signal handler"
#endif
static atomic_int g_stop = 0;

static const server_config *g_cfg;            // run_server가 설정, 워커들은 읽기만 함
static route_table g_routes;                  // 시작 시 한 번 로드, 이후 읽기 + next 카운터(atomic)만 변경
static threadpool *g_pool;                    // 워커가 "큐에 기다리는 연결이 있는지" 볼 때 사용 (단위 테스트에선 NULL)

static int stopping(void) { return atomic_load(&g_stop); }

// 큐가 가득 참 → 짧게 503을 알리고 닫는다 (메인 스레드에서 실행되므로 오래 막히면 안 됨: 송신 1초 제한)
static void reject_busy(int connfd) {
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(connfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    send_error(connfd, 503, 0);
    close(connfd);
}

static void set_nonblocking(int fd, int on) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) return;
    fcntl(fd, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}

int run_server(const server_config *cfg) {
    server_configure(cfg);
    // 설정 파일이 있으면 시작할 때 한 번 읽는다. 형식이 틀리면 잘못된 라우팅으로 도는 대신 시작하지 않는다
    if (cfg->routes_path != NULL) {
        if (load_routes(cfg->routes_path, &g_routes) != 0) return -1;
        printf("reverse proxy: %d route(s) from %s\n", g_routes.count, cfg->routes_path);
    }

    int listenfd = open_listenfd(cfg->port);
    if (listenfd < 0) return -1;
    printf("listening on port %s\n", cfg->port);
    fflush(stdout);

    // 종료 시그널 race 방지:
    //   "while (!g_stop) 확인" 직후, accept로 잠들기 직전에 Ctrl+C가 오면 플래그는 서지만
    //   accept는 다음 연결이 올 때까지 계속 잔다 → 종료가 지연된다.
    //   그래서 평소엔 SIGINT/SIGTERM을 막아 두고, pselect가 기다리는 동안에만 원자적으로 풀어 준다.
    //   (4단계: 이 막힌 마스크를 워커 스레드가 물려받으므로 시그널은 이 메인 스레드만 받는다)
    sigset_t stop_sigs, orig_mask;
    sigemptyset(&stop_sigs);
    sigaddset(&stop_sigs, SIGINT);
    sigaddset(&stop_sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &stop_sigs, &orig_mask);

    // pselect가 "읽을 수 있음"이라 해도 그 사이 클라이언트가 RST로 끊으면 대기 연결이 사라져
    // accept가 다음 연결까지 막힐 수 있다 → 듣기 소켓만 non-blocking으로 두고 EAGAIN이면 다시 기다린다
    set_nonblocking(listenfd, 1);

    // 워커는 만들어지는 순간의 시그널 마스크를 물려받는다 → 위에서 막은 뒤에 만들어야
    // SIGINT/SIGTERM이 워커가 아니라 pselect로 기다리는 메인 스레드에만 전달된다
    threadpool *pool = threadpool_create(cfg->threads, cfg->queue_size, handle_connection);
    g_pool = pool;
    if (pool == NULL) {
        fprintf(stderr, "threadpool_create failed\n");
        close(listenfd);
        pthread_sigmask(SIG_SETMASK, &orig_mask, NULL);
        return -1;
    }

    while (!stopping()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listenfd, &rfds);
        // 대기하는 동안만 orig_mask(시그널 허용)로 바뀐다 — 플래그 확인과 대기 사이에 틈이 없음
        if (pselect(listenfd + 1, &rfds, NULL, NULL, NULL, &orig_mask) < 0) {
            if (errno == EINTR) continue;   // 시그널로 깨어남 → 루프 조건에서 g_stop 확인
            perror("pselect");
            break;
        }

        int connfd = accept(listenfd, NULL, NULL);
        if (connfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED || errno == EINTR) continue;
            if (errno == EMFILE || errno == ENFILE) {
                // fd 한도: 대기 연결은 큐에 남아 있어 pselect가 곧바로 또 "읽을 수 있음"을 알려 준다
                // → 그대로 돌면 CPU 100%로 헛돈다. 워커가 연결을 닫아 fd가 생길 때까지 잠깐 쉰다
                struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000 };
                nanosleep(&ts, NULL);
                continue;
            }
            perror("accept");               // 그 밖의 오류도 서버 전체를 죽이지 않고 다음 연결을 기다린다
            continue;
        }
        // shortcut: accept와 set_cloexec 사이에 다른 워커가 CGI를 fork하면 이 fd가 그 CGI에 샐 수 있다(아주 짧은 틈).
        //           Linux 전용으로 가도 되면 accept4(..., SOCK_CLOEXEC)로 바꿔 틈을 없앤다 (macOS에는 accept4 없음)
        set_cloexec(connfd);
        set_nonblocking(connfd, 0);         // macOS 등은 듣기 소켓의 O_NONBLOCK을 물려주므로 명시적으로 blocking
        if (threadpool_submit(pool, connfd) != 0) reject_busy(connfd);
    }

    // 종료: 새 연결부터 막고 → 워커가 처리 중인 연결과 큐에 남은 연결을 마치길 기다린다
    // (워커의 keep-alive 루프는 stopping()을 보고 지금 요청까지만 응답한 뒤 Connection: close로 끝낸다)
    close(listenfd);
    threadpool_destroy(pool);
    g_pool = NULL;
    pthread_sigmask(SIG_SETMASK, &orig_mask, NULL);
    printf("server stopped\n");
    return 0;
}

void server_configure(const server_config *cfg) {
    g_cfg = cfg;
}

// 요청 하나를 알맞은 핸들러로 보낸다. 반환 규약은 static_file.h와 같음 (0 / HTTP 오류 코드 / -1)
static int dispatch(int connfd, const http_request *req) {
    if (is_absolute_uri(req->uri)) {            // "GET http://host/..." = Forward Proxy 요청
        if (!g_cfg->forward_enabled) return 403;    // -f 없이 열린 프록시가 되지 않도록
        return 501;                             // TODO(7단계): forward_proxy_request
    }
    // 라우팅 규칙에 맞으면 백엔드로 (CGI·정적 파일보다 먼저: 설정한 경로는 이 서버가 직접 처리하지 않는다)
    route *r = match_route(&g_routes, req->path);
    if (r != NULL) return reverse_proxy_request(connfd, req, r);
    if (strncmp(req->path, CGI_PREFIX, strlen(CGI_PREFIX)) == 0) {
        return serve_cgi(connfd, g_cfg->cgi_dir, req);
    }
    return serve_static(connfd, g_cfg->root, req);
}

// 오류 응답. HEAD 요청에는 본문을 붙이면 안 되므로(다음 응답과 섞임) 헤더만 보낸다
static int reply_error(int connfd, int status, const http_request *req, int keep_alive) {
    if (req != NULL && strcmp(req->method, "HEAD") == 0)
        return send_headers(connfd, status, "text/html; charset=utf-8", 0, keep_alive);
    return send_error(connfd, status, keep_alive);
}

// 응답 직후 바로 close하면, 아직 안 읽은 입력(이어 보낸 다음 요청, 거절한 큰 본문)이 소켓에 남아 있을 때
// 커널이 FIN 대신 RST를 보내고, 클라이언트는 방금 받은 응답을 버릴 수 있다.
// → 쓰기 방향만 먼저 닫고(FIN), 남은 입력을 잠깐(전체 최대 1초·4MB) 읽어 버린 뒤 close
//   read 한 번마다의 타임아웃만 두면 1바이트씩 계속 보내는 클라이언트가 이 단계에서 워커를 붙잡으므로 전체 마감 시각도 둔다
static void lingering_close(int fd) {
    shutdown(fd, SHUT_WR);
    struct timeval tv = { .tv_sec = 0, .tv_usec = 200 * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    double end = rio_now() + 1.0;
    char buf[4096];
    size_t drained = 0;
    while (drained < 4 * 1024 * 1024 && rio_now() < end) {
        ssize_t r = read(fd, buf, sizeof(buf));
        if (r == 0) break;                      // 클라이언트도 닫음
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;   // 마감 전이면 조금 더 기다림
            break;
        }
        drained += (size_t)r;
    }
    close(fd);
}

void handle_connection(int connfd) {
    rio_t rio;
    rio_readinitb(&rio, connfd);

    // 수신 타임아웃: 다음 요청을 안 보내는 keep-alive 연결·아무것도 안 보내는 연결이 워커와 종료를 막지 않게
    struct timeval rtv = { .tv_sec = KEEPALIVE_TIMEOUT_SEC, .tv_usec = 0 };
    setsockopt(connfd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));
    // 송신 타임아웃: 응답을 읽지 않는 클라이언트 때문에 큰 파일 전송(write)이 영원히 막히지 않게
    struct timeval wtv = { .tv_sec = SEND_TIMEOUT_SEC, .tv_usec = 0 };
    setsockopt(connfd, SOL_SOCKET, SO_SNDTIMEO, &wtv, sizeof(wtv));
    // Nagle 끄기: 응답을 헤더 write + 본문 write로 나눠 보내므로, 켜져 있으면 상대의 지연 ACK와 겹쳐
    // 요청마다 ~40ms씩 멈춘다 (실측: 동시 10연결에서 초당 173건). TCP가 아닌 소켓(테스트의 socketpair)에선 실패해도 무시
    int one = 1;
    setsockopt(connfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    http_request req;
    // keep-alive: 한 연결로 요청을 여러 번 받는다. 클라이언트가 닫거나·close를 원하거나·오류가 나거나·서버 종료 중이면 멈춤
    while (!stopping()) {
        int rc = read_request(&rio, &req);
        if (rc < 0) break;                      // EOF, 유휴 시간 초과, 읽기 에러 → 응답 없이 종료
        if (rc > 0) {                           // 형식 오류 등: 남은 입력의 경계를 믿을 수 없으므로 알려 주고 닫는다
            reply_error(connfd, rc, NULL, 0);
            break;
        }

        // 이번 응답 뒤에도 연결을 유지할지:
        //  - 종료 중이면 끊는다
        //  - 큐에서 기다리는 연결이 있으면 끊고 워커를 넘겨준다 (keep-alive 연결이 워커를 계속 붙잡으면
        //    워커 수를 넘는 클라이언트는 큐에서 굶거나 503을 받는다. 한가할 땐 keep-alive 그대로 유지)
        int busy = (g_pool != NULL && threadpool_queued(g_pool) > 0);
        int keep = req.keep_alive && !stopping() && !busy;
        req.keep_alive = keep;
        int hr = dispatch(connfd, &req);
        if (hr > 0 && reply_error(connfd, hr, &req, keep) != 0) hr = -1;
        free_request(&req);
        if (hr < 0 || !keep) break;
    }

    lingering_close(connfd);                    // ← 어느 경로로 끝나든 여기서 닫는다 (fd 누수 방지)
}

void request_stop(void) {
    atomic_store(&g_stop, 1);                   // lock-free atomic이라 시그널 핸들러에서 호출해도 안전
}
