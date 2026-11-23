// [server.c] 서버 본체 구현
#include "server.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "cgi.h"
#include "forward_proxy.h"
#include "http_request.h"
#include "http_response.h"
#include "net.h"
#include "rio.h"
#include "router.h"
#include "static_file.h"

static const server_config *g_cfg;            // run_server가 설정, 워커들은 읽기만 함
static route_table g_routes;                  // 시작 시 한 번 로드, 이후 읽기 + next 카운터(atomic)만 변경
static volatile sig_atomic_t g_stop = 0;      // 시그널 핸들러에서 안전하게 쓸 수 있는 타입

static void set_nonblocking(int fd, int on) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) return;
    fcntl(fd, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}

int run_server(const server_config *cfg) {
    server_configure(cfg);
    (void)g_routes;
    // TODO(6단계): routes_path가 있으면 load_routes

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

    // TODO(4단계): threadpool_create 후 아래 handle_connection 직접 호출을 threadpool_submit으로 교체
    while (!g_stop) {
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
            // EMFILE(fd 한도) 등: 서버 전체를 죽이지 않고 다음 연결을 기다린다
            // shortcut: EMFILE이 계속되면 매번 바로 실패하며 루프가 빠르게 돈다. 4단계에서 잠깐 쉬는 처리 추가
            perror("accept");
            continue;
        }
        set_cloexec(connfd);
        set_nonblocking(connfd, 0);         // macOS 등은 듣기 소켓의 O_NONBLOCK을 물려주므로 명시적으로 blocking
        handle_connection(connfd);          // 1단계: 스레드 없이 연결을 하나씩 처리
    }

    pthread_sigmask(SIG_SETMASK, &orig_mask, NULL);
    close(listenfd);
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
    // TODO(6단계): match_route(&g_routes, req->path) → reverse_proxy_request
    if (strncmp(req->path, CGI_PREFIX, strlen(CGI_PREFIX)) == 0) {
        return 501;                             // TODO(5단계): serve_cgi
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

    http_request req;
    // keep-alive: 한 연결로 요청을 여러 번 받는다. 클라이언트가 닫거나·close를 원하거나·오류가 나거나·서버 종료 중이면 멈춤
    while (!g_stop) {
        int rc = read_request(&rio, &req);
        if (rc < 0) break;                      // EOF, 유휴 시간 초과, 읽기 에러 → 응답 없이 종료
        if (rc > 0) {                           // 형식 오류 등: 남은 입력의 경계를 믿을 수 없으므로 알려 주고 닫는다
            reply_error(connfd, rc, NULL, 0);
            break;
        }

        int keep = req.keep_alive && !g_stop;   // 종료 중이면 이번 응답에 Connection: close
        req.keep_alive = keep;
        int hr = dispatch(connfd, &req);
        if (hr > 0 && reply_error(connfd, hr, &req, keep) != 0) hr = -1;
        free_request(&req);
        if (hr < 0 || !keep) break;
    }

    lingering_close(connfd);                    // ← 어느 경로로 끝나든 여기서 닫는다 (fd 누수 방지)
}

void request_stop(void) {
    g_stop = 1;
}
