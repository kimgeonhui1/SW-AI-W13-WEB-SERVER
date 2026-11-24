// [test_server.c] handle_connection 단위 테스트 — socketpair 한쪽을 "클라이언트", 다른 쪽을 connfd로 넘겨 검증
// 요청 읽기 → 분기(정적 파일 / CGI / Forward / Forward 비활성 403) → 응답, keep-alive 반복, 시간 제한, 연결 정리
#include "test.h"
#include "http_request.h"
#include "server.h"
#include "rio.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define INDEX_BODY "<h1>Index</h1>\n"          // 15바이트

static char g_base[64], g_root[96];
static server_config g_test_cfg;

static void make_pair(int sv[2]) {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        perror("socketpair");
        exit(1);
    }
}

static void write_file(const char *path, const char *content) {
    FILE *fp = fopen(path, "wb");
    CHECK(fp != NULL);
    if (!fp) return;
    fputs(content, fp);
    fclose(fp);
}

// 테스트용 정적 파일 루트를 임시 폴더에 만들고 서버 설정으로 등록
static void setup(int forward_enabled) {
    snprintf(g_base, sizeof(g_base), "/tmp/test_server_XXXXXX");
    CHECK(mkdtemp(g_base) != NULL);
    snprintf(g_root, sizeof(g_root), "%s/www", g_base);
    CHECK(mkdir(g_root, 0755) == 0);
    char p[160];
    snprintf(p, sizeof(p), "%s/index.html", g_root);
    write_file(p, INDEX_BODY);
    snprintf(p, sizeof(p), "%s/a.css", g_root);
    write_file(p, "b{}");

    memset(&g_test_cfg, 0, sizeof(g_test_cfg));
    g_test_cfg.port = "0";
    g_test_cfg.root = g_root;
    g_test_cfg.cgi_dir = "./cgi-bin";
    g_test_cfg.forward_enabled = forward_enabled;
    server_configure(&g_test_cfg);
}

static void teardown(void) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_base);
    CHECK(system(cmd) == 0);
}

// 클라이언트 쪽에서 EOF(또는 에러)까지 받은 바이트를 문자열로
static size_t read_all(int fd, char *buf, size_t cap) {
    size_t total = 0;
    while (total < cap - 1) {
        ssize_t r = read(fd, buf + total, cap - 1 - total);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        total += (size_t)r;
    }
    buf[total] = '\0';
    return total;
}

// 요청 바이트를 모두 보내고 쓰기 방향을 닫은(FIN) 뒤 handle_connection 실행 → 응답 전체 수집
static size_t exchange(const char *req, char *resp, size_t cap) {
    int sv[2];
    make_pair(sv);
    if (req[0] != '\0') rio_writen(sv[0], req, strlen(req));
    shutdown(sv[0], SHUT_WR);
    handle_connection(sv[1]);                               // 끝나면 sv[1]을 닫아야 함
    size_t n = read_all(sv[0], resp, cap);
    close(sv[0]);
    return n;
}

static int count_responses(const char *resp) {
    int n = 0;
    for (const char *p = resp; (p = strstr(p, "HTTP/1.1 ")) != NULL; p++) n++;
    return n;
}

// ───────────── 정상 요청 ─────────────

// GET / → index.html (200, text/html, 정확한 Content-Length, 본문), HTTP/1.1 기본 keep-alive
static void test_get_index(void) {
    char resp[2048];
    exchange("GET / HTTP/1.1\r\nHost: x\r\n\r\n", resp, sizeof(resp));
    CHECK(strncmp(resp, "HTTP/1.1 200 OK\r\n", 17) == 0);
    CHECK(strstr(resp, "Content-Type: text/html; charset=utf-8\r\n") != NULL);
    CHECK(strstr(resp, "Content-Length: 15\r\n") != NULL);
    CHECK(strstr(resp, "Connection: keep-alive\r\n") != NULL);
    char *body = strstr(resp, "\r\n\r\n");
    CHECK(body != NULL && strcmp(body + 4, INDEX_BODY) == 0);   // 본문 뒤에 다른 바이트 없음
}

// 한 연결에 요청 3개를 이어 보냄(pipelining) → 순서대로 응답 3개 (keep-alive 반복)
static void test_keepalive_pipelined(void) {
    char resp[4096];
    exchange("GET / HTTP/1.1\r\nHost: x\r\n\r\n"
             "GET /a.css HTTP/1.1\r\nHost: x\r\n\r\n"
             "GET /missing HTTP/1.1\r\nHost: x\r\n\r\n", resp, sizeof(resp));
    CHECK(count_responses(resp) == 3);
    char *r2 = strstr(resp + 1, "HTTP/1.1 200 OK");
    CHECK(r2 != NULL && strstr(r2, "Content-Type: text/css; charset=utf-8") != NULL);
    CHECK(strstr(resp, "HTTP/1.1 404 Not Found") != NULL);  // 404 뒤에도 연결 유지 (keep-alive)
}

// Connection: close → 응답 하나만 보내고 닫음 (뒤에 이어 보낸 요청은 무시)
static void test_connection_close(void) {
    char resp[4096];
    exchange("GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"
             "GET /a.css HTTP/1.1\r\nHost: x\r\n\r\n", resp, sizeof(resp));
    CHECK(count_responses(resp) == 1);
    CHECK(strstr(resp, "Connection: close\r\n") != NULL);
}

// HTTP/1.0은 기본이 close
static void test_http10_closes(void) {
    char resp[4096];
    exchange("GET / HTTP/1.0\r\n\r\nGET / HTTP/1.0\r\n\r\n", resp, sizeof(resp));
    CHECK(count_responses(resp) == 1);
    CHECK(strstr(resp, "Connection: close\r\n") != NULL);
}

// HEAD → 헤더만 (Content-Length는 GET과 같음), 없는 파일 HEAD도 본문 없음
static void test_head(void) {
    char resp[2048];
    exchange("HEAD / HTTP/1.1\r\nHost: x\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "Content-Length: 15\r\n") != NULL);
    char *sep = strstr(resp, "\r\n\r\n");
    CHECK(sep != NULL && sep[4] == '\0');
    exchange("HEAD /missing HTTP/1.1\r\nHost: x\r\n\r\n", resp, sizeof(resp));
    CHECK(strncmp(resp, "HTTP/1.1 404 Not Found\r\n", 24) == 0);
    sep = strstr(resp, "\r\n\r\n");
    CHECK(sep != NULL && sep[4] == '\0');                    // 404여도 HEAD에는 본문을 붙이지 않음
}

// ───────────── 오류 응답 ─────────────

// 형식이 깨진 요청 → 400 보내고 닫음 (뒤에 온 정상 요청에는 응답하지 않음 — 경계를 믿을 수 없으므로)
static void test_bad_request_closes(void) {
    char resp[4096];
    exchange("GARBAGE\r\n\r\nGET / HTTP/1.1\r\nHost: x\r\n\r\n", resp, sizeof(resp));
    CHECK(strncmp(resp, "HTTP/1.1 400 Bad Request\r\n", 26) == 0);
    CHECK(count_responses(resp) == 1);
    CHECK(strstr(resp, "Connection: close\r\n") != NULL);
}

// 각종 오류 코드가 실제 응답으로 나감
static void test_error_codes(void) {
    char resp[4096];
    exchange("PUT / HTTP/1.1\r\nHost: x\r\n\r\n", resp, sizeof(resp));
    CHECK(strncmp(resp, "HTTP/1.1 501", 12) == 0);
    exchange("GET / HTTP/1.1\r\n\r\n", resp, sizeof(resp));                         // Host 없음
    CHECK(strncmp(resp, "HTTP/1.1 400", 12) == 0);
    exchange("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\nhi", resp, sizeof(resp));
    CHECK(strncmp(resp, "HTTP/1.1 405", 12) == 0);                                   // 정적 파일에 POST
    // 루트(/tmp/test_server_X/www)에서 충분히 올라가 실제로 존재하는 /etc/passwd를 가리키게
    exchange("GET /../../../../../../../../etc/passwd HTTP/1.1\r\nHost: x\r\n\r\n", resp, sizeof(resp));
    CHECK(strncmp(resp, "HTTP/1.1 403", 12) == 0);
    CHECK(strstr(resp, "root:") == NULL);
    exchange("GET /cgi-bin/adder?a=1&b=2 HTTP/1.1\r\nHost: x\r\n\r\n", resp, sizeof(resp));
    CHECK(strncmp(resp, "HTTP/1.1 200", 12) == 0 && strstr(resp, "1 + 2 = 3") != NULL); // CGI (cgi-bin/adder)
    exchange("GET /cgi-bin/nope HTTP/1.1\r\nHost: x\r\n\r\n", resp, sizeof(resp));
    CHECK(strncmp(resp, "HTTP/1.1 404", 12) == 0);                                   // 없는 CGI
    exchange("GET http://example.com/ HTTP/1.1\r\nHost: example.com\r\n\r\n", resp, sizeof(resp));
    CHECK(strncmp(resp, "HTTP/1.1 403", 12) == 0);                                   // -f 없이 Forward 요청
}

static void test_forward_enabled(void) {
    teardown();
    setup(1);
    char resp[2048];
    // -f면 원 서버로 연결 시도 (포트 1은 닫혀 있어 연결 거부 → 502). 외부 네트워크에 의존하지 않도록 루프백
    exchange("GET http://127.0.0.1:1/ HTTP/1.1\r\nHost: 127.0.0.1:1\r\n\r\n", resp, sizeof(resp));
    CHECK(strncmp(resp, "HTTP/1.1 502", 12) == 0);
    teardown();
    setup(0);
}

// 본문 상한(1MB) 초과 → 본문을 읽지 않고 413. 남은 본문이 있어도 응답이 클라이언트에 도착해야 함(lingering close)
struct big_writer { int fd; };
static void *write_big_body(void *p) {
    struct big_writer *w = p;
    char hdr[128];
    snprintf(hdr, sizeof(hdr), "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n", 3 * 1024 * 1024);
    rio_writen(w->fd, hdr, strlen(hdr));
    static char chunk[65536];
    for (int i = 0; i < 48; i++)                            // 3MB 본문 (서버가 안 읽으면 버퍼가 차서 막힘)
        if (rio_writen(w->fd, chunk, sizeof(chunk)) < 0) break;
    shutdown(w->fd, SHUT_WR);
    return NULL;
}

static void test_body_too_large_still_gets_413(void) {
    signal(SIGPIPE, SIG_IGN);
    int sv[2];
    make_pair(sv);
    struct big_writer w = { .fd = sv[0] };
    pthread_t t;
    if (pthread_create(&t, NULL, write_big_body, &w) != 0) {
        CHECK(!"pthread_create failed");
        close(sv[0]);
        close(sv[1]);
        return;
    }
    handle_connection(sv[1]);
    CHECK(pthread_join(t, NULL) == 0);
    char resp[2048];
    read_all(sv[0], resp, sizeof(resp));
    close(sv[0]);
    const char *want = "HTTP/1.1 413 Payload Too Large\r\n";
    CHECK(strncmp(resp, want, strlen(want)) == 0);
}

// ───────────── 연결 관리 ─────────────

// handle_connection이 끝나면 connfd가 반드시 닫혀 있음 (fd 누수 방지)
static void test_closes_connfd(void) {
    int sv[2];
    make_pair(sv);
    rio_writen(sv[0], "GET / HTTP/1.1\r\nHost: x\r\n\r\n", 27);
    shutdown(sv[0], SHUT_WR);
    handle_connection(sv[1]);
    errno = 0;
    CHECK(fcntl(sv[1], F_GETFD) == -1 && errno == EBADF);
    close(sv[0]);
}

// 응답을 받기 전에 클라이언트가 완전히 닫아도 프로세스가 죽지 않음 (SIGPIPE 무시 상태)
static void test_client_gone_before_response(void) {
    signal(SIGPIPE, SIG_IGN);
    int sv[2];
    make_pair(sv);
    rio_writen(sv[0], "GET / HTTP/1.1\r\nHost: x\r\n\r\n", 27);
    close(sv[0]);
    handle_connection(sv[1]);
    CHECK(1);
}

// 요청이 조각조각 천천히 도착해도 합쳐서 처리
struct slow_arg { int fd; };
static void *slow_client(void *p) {
    struct slow_arg *a = p;
    const char *parts[] = { "GE", "T / HT", "TP/1.1\r", "\nHost: x\r\n", "\r", "\n" };
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        rio_writen(a->fd, parts[i], strlen(parts[i]));
        usleep(30 * 1000);
    }
    shutdown(a->fd, SHUT_WR);
    return NULL;
}

static void test_fragmented_request(void) {
    int sv[2];
    make_pair(sv);
    struct slow_arg a = { .fd = sv[0] };
    pthread_t t;
    if (pthread_create(&t, NULL, slow_client, &a) != 0) {
        CHECK(!"pthread_create failed");
        close(sv[0]);
        close(sv[1]);
        return;
    }
    handle_connection(sv[1]);
    CHECK(pthread_join(t, NULL) == 0);
    char resp[2048];
    read_all(sv[0], resp, sizeof(resp));
    close(sv[0]);
    CHECK(strncmp(resp, "HTTP/1.1 200 OK\r\n", 17) == 0);
}

// 아무것도 안 보내는 클라이언트 → KEEPALIVE_TIMEOUT_SEC 뒤 응답 없이 반환 (무한 대기 금지)
static void test_idle_client_times_out(void) {
    int sv[2];
    make_pair(sv);
    time_t start = time(NULL);
    handle_connection(sv[1]);
    time_t elapsed = time(NULL) - start;
    CHECK(elapsed >= KEEPALIVE_TIMEOUT_SEC - 1 && elapsed <= KEEPALIVE_TIMEOUT_SEC + 3);
    char resp[64];
    CHECK(read_all(sv[0], resp, sizeof(resp)) == 0);
    close(sv[0]);
}

// keep-alive: 첫 응답 뒤 다음 요청 없이 연결만 유지 → 약 KEEPALIVE_TIMEOUT_SEC 뒤 조용히 종료
// (응답은 1개, 408 같은 추가 응답 없음 — 요청 시작 전 대기는 정상적인 유휴이므로)
static void test_keepalive_idle_after_response(void) {
    int sv[2];
    make_pair(sv);
    rio_writen(sv[0], "GET / HTTP/1.1\r\nHost: x\r\n\r\n", 27);   // shutdown 안 함 → 연결 유지
    time_t start = time(NULL);
    handle_connection(sv[1]);
    time_t elapsed = time(NULL) - start;
    CHECK(elapsed >= KEEPALIVE_TIMEOUT_SEC - 1 && elapsed <= KEEPALIVE_TIMEOUT_SEC + 3);
    char resp[2048];
    read_all(sv[0], resp, sizeof(resp));
    close(sv[0]);
    CHECK(strncmp(resp, "HTTP/1.1 200 OK\r\n", 17) == 0);
    CHECK(count_responses(resp) == 1);
}

// 요청 도중(요청 라인만 받고) 멈춤 → 수신 타임아웃(5초)에 걸려도 응답 없이 끊지 말고 408을 알려 줌
static void test_stalled_mid_request_gets_408(void) {
    int sv[2];
    make_pair(sv);
    rio_writen(sv[0], "GET / HTTP/1.1\r\n", 16);
    time_t start = time(NULL);
    handle_connection(sv[1]);
    time_t elapsed = time(NULL) - start;
    CHECK(elapsed >= KEEPALIVE_TIMEOUT_SEC - 1 && elapsed <= KEEPALIVE_TIMEOUT_SEC + 3);
    char resp[2048];
    read_all(sv[0], resp, sizeof(resp));
    close(sv[0]);
    CHECK(strncmp(resp, "HTTP/1.1 408 Request Timeout\r\n", 30) == 0);
}

// 한 줄 안에서 1바이트씩 천천히 보내는 slowloris → read 하나하나는 수신 타임아웃에 안 걸려도
// 요청 전체 시간 제한(REQUEST_TIMEOUT_SEC)에 걸려 408 후 종료
struct drip_arg { int fd; volatile int stop; };
static void *drip_client(void *p) {
    struct drip_arg *a = p;
    rio_writen(a->fd, "GET / HTTP/1.1\r\nX-Slow: ", 24);
    while (!a->stop) {                                      // 헤더 값을 1초에 1바이트씩, 줄을 끝내지 않음
        if (rio_writen(a->fd, "a", 1) < 0) break;
        sleep(1);
    }
    return NULL;
}

static void test_slowloris_gets_408(void) {
    signal(SIGPIPE, SIG_IGN);
    int sv[2];
    make_pair(sv);
    struct drip_arg a = { .fd = sv[0], .stop = 0 };
    pthread_t t;
    if (pthread_create(&t, NULL, drip_client, &a) != 0) {
        CHECK(!"pthread_create failed");
        close(sv[0]);
        close(sv[1]);
        return;
    }
    time_t start = time(NULL);
    handle_connection(sv[1]);
    time_t elapsed = time(NULL) - start;
    a.stop = 1;
    CHECK(pthread_join(t, NULL) == 0);
    // 408 후 정리(lingering close)도 전체 1초 안에 끝나야 함 — 계속 들어오는 바이트에 붙잡히지 않음
    CHECK(elapsed >= REQUEST_TIMEOUT_SEC && elapsed <= REQUEST_TIMEOUT_SEC + 3);
    char resp[2048];
    read_all(sv[0], resp, sizeof(resp));
    close(sv[0]);
    CHECK(strncmp(resp, "HTTP/1.1 408 Request Timeout\r\n", 30) == 0);
}

int main(void) {
    setup(0);
    test_get_index();
    test_keepalive_pipelined();
    test_connection_close();
    test_http10_closes();
    test_head();
    test_bad_request_closes();
    test_error_codes();
    test_forward_enabled();
    test_body_too_large_still_gets_413();
    test_closes_connfd();
    test_client_gone_before_response();
    test_fragmented_request();
    fprintf(stderr, "  (시간 제한 테스트: 약 %d초 걸림)\n", 3 * KEEPALIVE_TIMEOUT_SEC + REQUEST_TIMEOUT_SEC);
    test_idle_client_times_out();
    test_keepalive_idle_after_response();
    test_stalled_mid_request_gets_408();
    test_slowloris_gets_408();
    teardown();
    return TEST_RESULT();
}
