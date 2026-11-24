// [test_proxy.c] proxy_relay / reverse_proxy_request 단위 테스트
// 가짜 백엔드: 스레드가 듣기 소켓에서 연결을 받아 요청을 기록하고, 정해 둔 응답 바이트를 그대로 보낸다
// 클라이언트 쪽은 socketpair. 큰 응답에도 막히지 않도록 읽기 스레드가 동시에 비운다
#include "test.h"
#include "forward_proxy.h"
#include "http_request.h"
#include "net.h"
#include "proxy.h"
#include "rio.h"
#include "router.h"

#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#define RESP_CAP (4 * 1024 * 1024)

// ───────────── 가짜 백엔드 ─────────────
typedef struct {
    int listenfd;
    char port[8];
    const char *resp;         // 보낼 응답 (NULL이면 아무것도 안 보냄)
    size_t resp_len;
    int hold_ms;              // 응답 후 연결을 닫기 전에 기다리는 시간 (닫힘으로 끝을 알리지 않는 경우 시험)
    int drip_ms;              // 0이 아니면 응답을 1바이트씩 이 간격으로 보냄 (헤더를 천천히 흘리는 백엔드)
    int conns;                // 받을 연결 수
    char got[512 * 1024];     // 마지막으로 받은 요청 원문
    size_t got_len;
    int served;
    pthread_t t;
} fake_backend;

static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

// 요청 하나를 끝까지 읽기: 헤더 끝(\r\n\r\n) + Content-Length만큼
static void read_one_request(int fd, fake_backend *fb) {
    fb->got_len = 0;
    size_t need = (size_t)-1;
    while (fb->got_len < sizeof(fb->got) - 1 && fb->got_len < need) {
        ssize_t n = read(fd, fb->got + fb->got_len, sizeof(fb->got) - 1 - fb->got_len);
        if (n <= 0) break;
        fb->got_len += (size_t)n;
        fb->got[fb->got_len] = '\0';
        char *end = strstr(fb->got, "\r\n\r\n");
        if (end != NULL && need == (size_t)-1) {
            size_t hl = (size_t)(end + 4 - fb->got);
            char *cl = strstr(fb->got, "Content-Length: ");
            need = hl + ((cl != NULL && cl < end) ? strtoul(cl + 16, NULL, 10) : 0);
        }
    }
    fb->got[fb->got_len] = '\0';
}

static void *fake_main(void *p) {
    fake_backend *fb = p;
    for (int i = 0; i < fb->conns; i++) {
        int c = accept(fb->listenfd, NULL, NULL);
        if (c < 0) break;
        read_one_request(c, fb);
        if (fb->resp != NULL && fb->drip_ms > 0) {
            for (size_t k = 0; k < fb->resp_len; k++) {
                if (write(c, fb->resp + k, 1) != 1) break;    // 프록시가 포기하고 닫으면 멈춤
                sleep_ms(fb->drip_ms);
            }
        } else if (fb->resp != NULL) {
            rio_writen(c, fb->resp, fb->resp_len);
        }
        if (fb->hold_ms > 0) sleep_ms(fb->hold_ms);
        fb->served++;
        close(c);
    }
    return NULL;
}

static int g_next_drip_ms;   // 다음 fake_start 한 번에만 적용 (스레드 시작 전에 정해 두어 경쟁 없음)

static fake_backend *fake_start(const char *resp, size_t len, int hold_ms, int conns) {
    fake_backend *fb = calloc(1, sizeof(*fb));
    fb->drip_ms = g_next_drip_ms;
    g_next_drip_ms = 0;
    fb->listenfd = open_listenfd("0");
    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    getsockname(fb->listenfd, (struct sockaddr *)&ss, &sl);
    snprintf(fb->port, sizeof(fb->port), "%d", ntohs(((struct sockaddr_in *)&ss)->sin_port));
    fb->resp = resp;
    fb->resp_len = len;
    fb->hold_ms = hold_ms;
    fb->conns = conns;
    pthread_create(&fb->t, NULL, fake_main, fb);
    return fb;
}

static void fake_stop(fake_backend *fb) {
    pthread_join(fb->t, NULL);
    close(fb->listenfd);
    free(fb);
}

// 연결을 받지 않는 포트 (열었다 바로 닫음 → 연결 거부)
static void dead_port(char *out, size_t cap) {
    int fd = open_listenfd("0");
    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    getsockname(fd, (struct sockaddr *)&ss, &sl);
    snprintf(out, cap, "%d", ntohs(((struct sockaddr_in *)&ss)->sin_port));
    close(fd);
}

// ───────────── 클라이언트 쪽 ─────────────
static http_request g_req;
static char *g_resp;
static size_t g_resp_len;

static void make_req(const char *method, const char *uri, int keep_alive) {
    free(g_req.body);
    memset(&g_req, 0, sizeof(g_req));
    snprintf(g_req.method, sizeof(g_req.method), "%s", method);
    snprintf(g_req.uri, sizeof(g_req.uri), "%s", uri);
    snprintf(g_req.path, sizeof(g_req.path), "%s", uri);
    char *q = strchr(g_req.path, '?');
    if (q) *q = '\0';
    snprintf(g_req.version, sizeof(g_req.version), "HTTP/1.1");
    g_req.keep_alive = keep_alive;
}

static void add_header(const char *name, const char *value) {
    http_header *h = &g_req.headers[g_req.header_count++];
    snprintf(h->name, sizeof(h->name), "%s", name);
    snprintf(h->value, sizeof(h->value), "%s", value);
}

static void *reader_main(void *p) {
    int fd = *(int *)p;
    g_resp_len = 0;
    for (;;) {
        ssize_t n = read(fd, g_resp + g_resp_len, RESP_CAP - 1 - g_resp_len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        g_resp_len += (size_t)n;
    }
    g_resp[g_resp_len] = '\0';
    return NULL;
}

// fb로 직접 연결해 proxy_relay (route가 있으면 reverse_proxy_request). 클라이언트가 받은 바이트는 g_resp
static int relay_via(fake_backend *fb, route *r) {
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    pthread_t t;
    pthread_create(&t, NULL, reader_main, &sv[0]);
    int rc;
    if (r != NULL) {
        rc = reverse_proxy_request(sv[1], &g_req, r);
    } else {
        int up = open_clientfd("127.0.0.1", fb->port);
        rc = proxy_relay(sv[1], up, &g_req, g_req.uri, NULL);
        close(up);
    }
    close(sv[1]);
    pthread_join(t, NULL);
    close(sv[0]);
    return rc;
}

static const char *body_of(void) {
    char *p = strstr(g_resp, "\r\n\r\n");
    return p ? p + 4 : "";
}

#define RESP(s) (s), (sizeof(s) - 1)

// ───────────── 테스트 ─────────────

// 요청 재구성(hop-by-hop 제거, Connection: close)과 응답 재구성(백엔드 Connection 대신 keep-alive)
static void test_basic_rewrite(void) {
    fake_backend *fb = fake_start(RESP("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\nKeep-Alive: timeout=1\r\nX-B: 1\r\n\r\nhello"), 0, 1);
    make_req("GET", "/api/x?q=1", 1);
    add_header("Host", "front.example");
    add_header("Connection", "keep-alive");
    add_header("Keep-Alive", "timeout=5");
    add_header("Proxy-Connection", "keep-alive");
    add_header("Upgrade", "h2c");
    add_header("X-A", "1");
    CHECK(relay_via(fb, NULL) == 0);
    // 백엔드가 받은 요청
    CHECK(strncmp(fb->got, "GET /api/x?q=1 HTTP/1.1\r\n", 25) == 0);
    CHECK(strstr(fb->got, "\r\nHost: front.example\r\n") != NULL);      // Host는 그대로
    CHECK(strstr(fb->got, "\r\nX-A: 1\r\n") != NULL);
    CHECK(strstr(fb->got, "\r\nConnection: close\r\n") != NULL);
    CHECK(strstr(fb->got, "keep-alive") == NULL);
    CHECK(strstr(fb->got, "Keep-Alive") == NULL && strstr(fb->got, "Proxy-Connection") == NULL);
    CHECK(strstr(fb->got, "Upgrade") == NULL);
    // 클라이언트가 받은 응답
    CHECK(strncmp(g_resp, "HTTP/1.1 200 OK\r\n", 17) == 0);
    CHECK(strstr(g_resp, "\r\nX-B: 1\r\n") != NULL);
    CHECK(strstr(g_resp, "\r\nContent-Length: 5\r\n") != NULL);
    CHECK(strstr(g_resp, "\r\nConnection: keep-alive\r\n") != NULL);
    CHECK(strstr(g_resp, "Connection: close") == NULL && strstr(g_resp, "timeout=1") == NULL);
    CHECK_STR(body_of(), "hello");
    fake_stop(fb);
}

// 클라이언트가 keep-alive를 원하지 않으면 Connection: close, 반환 -1 (호출한 쪽이 닫음)
static void test_client_close(void) {
    fake_backend *fb = fake_start(RESP("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"), 0, 1);
    make_req("GET", "/", 0);
    CHECK(relay_via(fb, NULL) == -1);
    CHECK(strstr(g_resp, "\r\nConnection: close\r\n") != NULL);
    CHECK_STR(body_of(), "ok");
    fake_stop(fb);
}

// POST 본문을 백엔드로 (소켓 버퍼보다 큰 300KB)
static void test_post_body(void) {
    fake_backend *fb = fake_start(RESP("HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n"), 0, 1);
    make_req("POST", "/upload", 1);
    size_t n = 300 * 1024;
    g_req.body = malloc(n);
    for (size_t i = 0; i < n; i++) g_req.body[i] = (char)('A' + i % 26);
    g_req.content_length = n;
    char cl[32];
    snprintf(cl, sizeof(cl), "%zu", n);
    add_header("Content-Length", cl);
    CHECK(relay_via(fb, NULL) == 0);
    char *hend = strstr(fb->got, "\r\n\r\n");
    CHECK(hend != NULL && fb->got_len - (size_t)(hend + 4 - fb->got) == n);
    CHECK(hend != NULL && memcmp(hend + 4, g_req.body, n) == 0);
    CHECK(strncmp(g_resp, "HTTP/1.1 201 Created\r\n", 22) == 0);
    fake_stop(fb);
}

// 길이를 모르는 응답: 백엔드가 닫을 때까지 보내고 클라이언트도 닫음
static void test_until_close(void) {
    fake_backend *fb = fake_start(RESP("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nstream-until-close"), 0, 1);
    make_req("GET", "/", 1);
    CHECK(relay_via(fb, NULL) == -1);
    CHECK(strstr(g_resp, "\r\nConnection: close\r\n") != NULL);
    CHECK_STR(body_of(), "stream-until-close");
    fake_stop(fb);
    fb = fake_start(RESP("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n"), 0, 1);
    make_req("GET", "/", 1);
    CHECK(relay_via(fb, NULL) == -1);                                   // chunked는 그대로 통과, 끝은 닫힘으로
    CHECK(strstr(g_resp, "\r\nTransfer-Encoding: chunked\r\n") != NULL);
    CHECK_STR(body_of(), "5\r\nhello\r\n0\r\n\r\n");
    fake_stop(fb);
}

// 본문이 없는 응답(HEAD, 204, 304)은 백엔드가 연결을 안 닫아도 기다리지 않는다
static void test_no_body_responses(void) {
    struct { const char *method; const char *resp; } cases[] = {
        { "HEAD", "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n" },
        { "GET",  "HTTP/1.1 204 No Content\r\n\r\n" },
        { "GET",  "HTTP/1.1 304 Not Modified\r\nContent-Length: 50\r\n\r\n" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        fake_backend *fb = fake_start(cases[i].resp, strlen(cases[i].resp), 2000, 1);
        make_req(cases[i].method, "/", 1);
        double t0 = rio_now();
        CHECK(relay_via(fb, NULL) == 0);
        CHECK(rio_now() - t0 < 1.0);
        CHECK_STR(body_of(), "");
        fake_stop(fb);
    }
}

// 큰 응답 (2MB) 그대로 전달
static void test_large_response(void) {
    size_t n = 2 * 1024 * 1024;
    char head[128];
    int hl = snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n", n);
    char *resp = malloc((size_t)hl + n);
    memcpy(resp, head, (size_t)hl);
    for (size_t i = 0; i < n; i++) resp[hl + i] = (char)(i * 7);
    fake_backend *fb = fake_start(resp, (size_t)hl + n, 0, 1);
    make_req("GET", "/big", 1);
    CHECK(relay_via(fb, NULL) == 0);
    const char *b = body_of();
    CHECK(g_resp_len - (size_t)(b - g_resp) == n && memcmp(b, resp + hl, n) == 0);
    fake_stop(fb);
    free(resp);
}

// 약속한 길이보다 일찍 끊긴 응답 → 받은 만큼 보내고 -1 (클라이언트가 잘린 것을 알 수 있게 닫음)
static void test_truncated(void) {
    fake_backend *fb = fake_start(RESP("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nonly10byte"), 0, 1);
    make_req("GET", "/", 1);
    CHECK(relay_via(fb, NULL) == -1);
    CHECK_STR(body_of(), "only10byte");
    fake_stop(fb);
}

// 잘못된 응답 → 502, 클라이언트에는 아무것도 보내지 않음 (호출한 쪽이 502 응답)
static void test_bad_responses(void) {
    static char bigh[PROXY_MAX_RESP_HEADER + 4096];
    size_t off = (size_t)snprintf(bigh, sizeof(bigh), "HTTP/1.1 200 OK\r\n");
    while (off < PROXY_MAX_RESP_HEADER + 100) off += (size_t)snprintf(bigh + off, sizeof(bigh) - off, "X-Pad: %0100d\r\n", 0);
    off += (size_t)snprintf(bigh + off, sizeof(bigh) - off, "\r\n");
    struct { const char *resp; size_t len; } cases[] = {
        { RESP("") },                                                 // 응답 없이 닫음
        { RESP("FOO 200 OK\r\n\r\n") },
        { RESP("ABCDEFGH 200 OK\r\nContent-Length: 0\r\n\r\n") },     // 9번째 글자부터는 정상이지만 "HTTP/1."이 아님
        { RESP("HTTP/1.1 abc OK\r\n\r\n") },
        { RESP("HTTP/1.1 999 Huh\r\n\r\n") },
        { RESP("HTTP/1.1 100 Continue\r\n\r\n") },                    // 1xx 미지원
        { RESP("HTTP/1.1 200 OK\r\nno colon line\r\n\r\n") },
        { RESP("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello") },
        { RESP("HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n") },
        { RESP("HTTP/1.1 200 OK\r\nContent-Length: 5") },             // 헤더 도중 끊김
        { bigh, off },                                                // 헤더 64KB 초과
        { RESP("HTTP/1.1 200abc OK\r\nContent-Length: 0\r\n\r\n") },   // 상태 코드 뒤에 글자
        { RESP("HTTP/1.1 2000 OK\r\nContent-Length: 0\r\n\r\n") },
        { RESP("HTTP/1.1200 OK\r\nContent-Length: 0\r\n\r\n") },      // 버전과 상태 코드 사이 공백 없음
        { RESP("HTTP/1.x 200 OK\r\nContent-Length: 0\r\n\r\n") },
        { RESP("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n0\r\n\r\n") },   // TE + CL
        { RESP("HTTP/1.1 200 OK\r\nX-A: a\rX-Evil: 1\r\nContent-Length: 0\r\n\r\n") },   // 값 중간의 CR
        { RESP("HTTP/1.1 200 OK\r\nContent-Length : 0\r\n\r\n") },    // 이름 뒤 공백
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        fake_backend *fb = fake_start(cases[i].resp, cases[i].len, 0, 1);
        make_req("GET", "/", 1);
        int rc = relay_via(fb, NULL);
        if (rc != 502) fprintf(stderr, "  bad response case %zu → %d\n", i, rc);
        CHECK(rc == 502);
        CHECK(g_resp_len == 0);
        fake_stop(fb);
    }
    // 같은 값의 중복 Content-Length는 허용하되 클라이언트에는 하나만 (RFC 9112 6.3)
    fake_backend *fb = fake_start(RESP("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\nok"), 0, 1);
    make_req("GET", "/", 1);
    CHECK(relay_via(fb, NULL) == 0);
    char *first = strstr(g_resp, "Content-Length: 2\r\n");
    CHECK(first != NULL && strstr(first + 1, "Content-Length") == NULL);
    CHECK_STR(body_of(), "ok");
    fake_stop(fb);
    // 상태 코드 바로 뒤가 줄 끝이어도 정상 (이유 문구는 생략 가능)
    fb = fake_start(RESP("HTTP/1.1 200\r\nContent-Length: 2\r\n\r\nok"), 0, 1);
    make_req("GET", "/", 1);
    CHECK(relay_via(fb, NULL) == 0);
    fake_stop(fb);
}

// 응답하지 않는 백엔드 → 504 (PROXY_TIMEOUT_SEC 근처에서)
static void test_timeout(void) {
    fake_backend *fb = fake_start(NULL, 0, (PROXY_TIMEOUT_SEC + 2) * 1000, 1);
    make_req("GET", "/", 1);
    double t0 = rio_now();
    CHECK(relay_via(fb, NULL) == 504);
    double el = rio_now() - t0;
    CHECK(el >= PROXY_TIMEOUT_SEC - 0.2 && el < PROXY_TIMEOUT_SEC + 1.5);
    CHECK(g_resp_len == 0);
    fake_stop(fb);
}

// 헤더를 1바이트씩 흘리는 백엔드: read 한 번은 제한 안에 오지만 헤더 전체 제한(PROXY_TIMEOUT_SEC)에 걸려 504
static void test_header_drip(void) {
    static const char drip[] = "HTTP/1.1 200 OK\r\nX-Slow: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n\r\n";
    g_next_drip_ms = 200;                                             // 전체 약 16초
    fake_backend *fb = fake_start(drip, sizeof(drip) - 1, 0, 1);
    make_req("GET", "/", 1);
    double t0 = rio_now();
    CHECK(relay_via(fb, NULL) == 504);
    double el = rio_now() - t0;
    CHECK(el >= PROXY_TIMEOUT_SEC - 0.2 && el < PROXY_TIMEOUT_SEC + 1.5);
    fake_stop(fb);
}

// Round-Robin으로 번갈아, 꺼진 백엔드는 건너뛰고, 모두 꺼지면 502
static void test_reverse_round_robin_failover(void) {
    fake_backend *a = fake_start(RESP("HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nA"), 0, 2);
    fake_backend *b = fake_start(RESP("HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nB"), 0, 2);
    route r;
    char line[128];
    snprintf(line, sizeof(line), "/api/ 127.0.0.1:%s 127.0.0.1:%s", a->port, b->port);
    CHECK(parse_route_line(line, &r) == 1);
    char seq[5] = "";
    for (int i = 0; i < 4; i++) {
        make_req("GET", "/api/x", 1);
        CHECK(relay_via(NULL, &r) == 0);
        seq[i] = body_of()[0];
    }
    CHECK_STR(seq, "ABAB");
    CHECK(strncmp(a->got, "GET /api/x HTTP/1.1\r\n", 21) == 0);   // 경로는 바꾸지 않음
    fake_stop(a);
    fake_stop(b);

    char dead[8];
    dead_port(dead, sizeof(dead));
    fake_backend *c = fake_start(RESP("HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nC"), 0, 2);
    snprintf(line, sizeof(line), "/ 127.0.0.1:%s 127.0.0.1:%s", dead, c->port);
    CHECK(parse_route_line(line, &r) == 1);
    for (int i = 0; i < 2; i++) {                                     // 첫 차례가 꺼진 쪽이어도 살아 있는 쪽으로
        make_req("GET", "/", 1);
        CHECK(relay_via(NULL, &r) == 0);
        CHECK_STR(body_of(), "C");
    }
    fake_stop(c);

    snprintf(line, sizeof(line), "/ 127.0.0.1:%s 127.0.0.1:%s", dead, dead);
    CHECK(parse_route_line(line, &r) == 1);
    make_req("GET", "/", 1);
    CHECK(relay_via(NULL, &r) == 502);
    CHECK(g_resp_len == 0);
}

// ───────────── Forward Proxy ─────────────
static int forward_once(void) {
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    pthread_t t;
    pthread_create(&t, NULL, reader_main, &sv[0]);
    int rc = forward_proxy_request(sv[1], &g_req);
    close(sv[1]);
    pthread_join(t, NULL);
    close(sv[0]);
    return rc;
}

// 절대 URI → 원 서버에는 경로만, Host는 URI의 authority로, 프록시용 헤더는 빼고
static void test_forward_rewrite(void) {
    fake_backend *fb = fake_start(RESP("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"), 0, 1);
    char uri[128];
    snprintf(uri, sizeof(uri), "http://127.0.0.1:%s/a.html?x=1#frag", fb->port);
    make_req("GET", uri, 1);
    add_header("Host", "spoofed.example");
    add_header("Proxy-Connection", "keep-alive");
    add_header("Proxy-Authorization", "Basic c2VjcmV0");
    add_header("User-Agent", "t");
    CHECK(forward_once() == 0);
    CHECK(strncmp(fb->got, "GET /a.html?x=1 HTTP/1.1\r\n", 26) == 0);    // origin-form, 조각 없음
    char host[64];
    snprintf(host, sizeof(host), "\r\nHost: 127.0.0.1:%s\r\n", fb->port);
    CHECK(strstr(fb->got, host) != NULL);
    CHECK(strstr(fb->got, "spoofed") == NULL);
    CHECK(strstr(fb->got, "Proxy-") == NULL);                            // 프록시 인증 정보가 원 서버로 새지 않음
    CHECK(strstr(fb->got, "\r\nUser-Agent: t\r\n") != NULL);
    CHECK(strstr(g_resp, "\r\nConnection: keep-alive\r\n") != NULL);
    CHECK_STR(body_of(), "ok");
    fake_stop(fb);

    // 클라이언트가 Host를 안 보내도(HTTP/1.0) URI의 호스트로 채움
    fb = fake_start(RESP("HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nok"), 0, 1);
    snprintf(uri, sizeof(uri), "http://127.0.0.1:%s", fb->port);
    make_req("GET", uri, 0);
    snprintf(g_req.version, sizeof(g_req.version), "HTTP/1.0");
    CHECK(forward_once() == -1);
    CHECK(strncmp(fb->got, "GET / HTTP/1.0\r\n", 16) == 0);
    snprintf(host, sizeof(host), "\r\nHost: 127.0.0.1:%s\r\n", fb->port);
    CHECK(strstr(fb->got, host) != NULL);
    fake_stop(fb);
}

static void test_forward_errors(void) {
    make_req("GET", "http://user@127.0.0.1:1/", 1);
    CHECK(forward_once() == 400);                                       // URI 형식 오류
    make_req("GET", "http://127.0.0.1:99999/", 1);
    CHECK(forward_once() == 400);
    char dead[8], uri[64];
    dead_port(dead, sizeof(dead));
    snprintf(uri, sizeof(uri), "http://127.0.0.1:%s/", dead);
    make_req("GET", uri, 1);
    CHECK(forward_once() == 502);                                       // 연결 거부
    make_req("GET", "http://no-such-host.invalid/", 1);
    CHECK(forward_once() == 502);                                       // DNS 실패 (.invalid는 존재하지 않는 도메인으로 예약됨)
    CHECK(g_resp_len == 0);
}

// 연결을 받지 않는 호스트(SYN을 버림) → CONNECT_TIMEOUT_SEC 뒤 504
// 듣기 큐를 꽉 채우면 Linux는 새 SYN에 응답하지 않으므로 패킷을 버리는 호스트처럼 동작한다
static void test_forward_connect_timeout(void) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK), .sin_port = 0 };
    bind(lfd, (struct sockaddr *)&a, sizeof(a));
    listen(lfd, 0);
    socklen_t sl = sizeof(a);
    getsockname(lfd, (struct sockaddr *)&a, &sl);
    int fill[8], nfill = 0;
    for (int i = 0; i < 8; i++) {                                       // 큐를 채움 (accept하지 않음)
        fill[nfill] = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        connect(fill[nfill], (struct sockaddr *)&a, sizeof(a));
        nfill++;
    }
    sleep_ms(200);
    char uri[64];
    snprintf(uri, sizeof(uri), "http://127.0.0.1:%d/", ntohs(a.sin_port));
    make_req("GET", uri, 1);
    double t0 = rio_now();
    int rc = forward_once();
    double el = rio_now() - t0;
    CHECK(rc == 504);
    if (rc != 504 || el > CONNECT_TIMEOUT_SEC + 1.5) fprintf(stderr, "  connect timeout: rc=%d %.2fs\n", rc, el);
    CHECK(el >= CONNECT_TIMEOUT_SEC - 0.2 && el < CONNECT_TIMEOUT_SEC + 1.5);
    for (int i = 0; i < nfill; i++) close(fill[i]);
    close(lfd);
}

static int count_fds(void) {
    DIR *d = opendir("/proc/self/fd");
    int n = 0;
    if (d == NULL) return -1;
    while (readdir(d) != NULL) n++;
    closedir(d);
    return n;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);                                         // 서버(main.c)와 같은 조건
    g_resp = malloc(RESP_CAP);
    int fds = count_fds();
    test_basic_rewrite();
    test_client_close();
    test_post_body();
    test_until_close();
    test_no_body_responses();
    test_large_response();
    test_truncated();
    test_bad_responses();
    test_reverse_round_robin_failover();
    test_timeout();
    test_header_drip();
    test_forward_rewrite();
    test_forward_errors();
    test_forward_connect_timeout();
    CHECK(count_fds() == fds);                                        // 백엔드 소켓이 남지 않음
    free(g_req.body);
    free(g_resp);
    return TEST_RESULT();
}
