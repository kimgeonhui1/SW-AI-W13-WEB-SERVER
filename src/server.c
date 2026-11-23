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

#include "http_request.h"
#include "net.h"
#include "rio.h"
#include "router.h"

static const server_config *g_cfg;            // run_server가 설정, 워커들은 읽기만 함
static route_table g_routes;                  // 시작 시 한 번 로드, 이후 읽기 + next 카운터(atomic)만 변경
static volatile sig_atomic_t g_stop = 0;      // 시그널 핸들러에서 안전하게 쓸 수 있는 타입

static void set_nonblocking(int fd, int on) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) return;
    fcntl(fd, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}

int run_server(const server_config *cfg) {
    g_cfg = cfg;
    (void)g_cfg;
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

void handle_connection(int connfd) {
    // TODO(3단계): 아래 임시 처리를 read_request + 분기(forward / reverse / cgi / static)로 교체
    //              SO_RCVTIMEO(KEEPALIVE_TIMEOUT_SEC), keep-alive 반복, free_request
    rio_t rio;
    char line[MAX_LINE];
    ssize_t n;

    rio_readinitb(&rio, connfd);

    // 수신 타임아웃: 연결만 하고 요청을 안 보내는 클라이언트가 이 스레드(1단계에선 서버 전체)와
    // 종료(SIGINT/SIGTERM)를 무기한 막지 않도록, read가 KEEPALIVE_TIMEOUT_SEC 뒤 -1(EAGAIN)로 끝나게 한다
    struct timeval tv = { .tv_sec = KEEPALIVE_TIMEOUT_SEC, .tv_usec = 0 };
    setsockopt(connfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // 1단계 임시: 요청 라인과 헤더를 빈 줄("\r\n")까지 읽고 버린 뒤 고정 응답을 보낸다
    // 빈 줄까지 읽어야 하는 이유: 요청을 다 받기 전에 응답하고 닫으면 클라이언트가 RST를 받을 수 있음
    while ((n = rio_readlineb(&rio, line, sizeof(line))) > 0) {
        if (strcmp(line, "\r\n") == 0 || strcmp(line, "\n") == 0) break;
    }

    if (n > 0) {
        static const char resp[] =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 13\r\n"
            "Connection: close\r\n"
            "\r\n"
            "Hello, W13!\r\n";
        rio_writen(connfd, resp, sizeof(resp) - 1);   // 실패(EPIPE)해도 아래에서 닫으면 됨
    }

    close(connfd);                          // ← 빠뜨리면 fd 누수
}

void request_stop(void) {
    g_stop = 1;
}
