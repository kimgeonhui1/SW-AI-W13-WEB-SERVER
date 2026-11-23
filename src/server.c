// [server.c] 서버 본체 구현
#include "server.h"
#include <signal.h>
#include "router.h"

static const server_config *g_cfg;            // run_server가 설정, 워커들은 읽기만 함
static route_table g_routes;                  // 시작 시 한 번 로드, 이후 읽기 + next 카운터(atomic)만 변경
static volatile sig_atomic_t g_stop = 0;      // 시그널 핸들러에서 안전하게 쓸 수 있는 타입

int run_server(const server_config *cfg) {
    g_cfg = cfg;
    (void)g_routes;
    // TODO:
    //  1. routes_path가 있으면 load_routes
    //  2. listenfd = open_listenfd(port). 실패하면 -1
    //  3. pool = threadpool_create(threads, queue_size, handle_connection)
    //  4. while (!g_stop): connfd = accept(listenfd)
    //       - 실패 시 EINTR(시그널)이면 루프 조건 재확인, 그 외(EMFILE 등)는 로그만 남기고 계속
    //       - threadpool_submit 실패(큐 가득) → send_error(503) 후 close
    //  5. close(listenfd) → threadpool_destroy (남은 연결 처리 후 종료)
    return -1;
}

void handle_connection(int connfd) {
    (void)connfd;
    // TODO:
    //  1. rio_readinitb(&rio, connfd), SO_RCVTIMEO(KEEPALIVE_TIMEOUT_SEC) 설정 → 유휴 연결은 read가 시간 초과로 끝남
    //  2. keep_alive인 동안 반복:
    //       read_request → -1이면 종료(EOF·타임아웃), >0이면 send_error 후 종료
    //       분기: is_absolute_uri(uri)   → forward_enabled면 forward_proxy_request, 아니면 403
    //             match_route(path) 있음 → reverse_proxy_request
    //             path가 CGI_PREFIX로 시작 → serve_cgi
    //             그 외                   → serve_static
    //       핸들러가 >0 반환 → send_error
    //       free_request
    //  3. close(connfd)   ← 빠뜨리면 fd 누수
}

void request_stop(void) {
    g_stop = 1;
}
