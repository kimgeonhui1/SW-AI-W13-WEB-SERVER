// [test_cgi.c] serve_cgi 단위 테스트 — 임시 폴더에 셸 스크립트 CGI를 만들어 실행 결과·오류 코드·자원 정리를 검증
// 응답은 socketpair로 받는다. 큰 응답이 소켓 버퍼를 채워 serve_cgi가 멈추지 않도록 읽기 스레드가 동시에 비운다
#include "test.h"
#include "cgi.h"
#include "http_request.h"
#include "rio.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define RESP_CAP (2 * 1024 * 1024 + 4096)

static char g_dir[64];
static http_request g_req;                 // 구조체가 커서(헤더 배열) 스택 대신 정적 영역
static char *g_resp;                       // 마지막 응답 전체
static size_t g_resp_len;

static void write_script(const char *name, const char *body, mode_t mode) {
    char p[160];
    snprintf(p, sizeof(p), "%s/%s", g_dir, name);
    FILE *fp = fopen(p, "wb");
    CHECK(fp != NULL);
    if (!fp) return;
    fprintf(fp, "#!/bin/sh\n%s", body);
    fclose(fp);
    chmod(p, mode);
}

static void make_req(const char *method, const char *path, const char *query, const char *body) {
    free(g_req.body);
    memset(&g_req, 0, sizeof(g_req));
    snprintf(g_req.method, sizeof(g_req.method), "%s", method);
    snprintf(g_req.path, sizeof(g_req.path), "%s", path);
    snprintf(g_req.query, sizeof(g_req.query), "%s", query);
    snprintf(g_req.version, sizeof(g_req.version), "HTTP/1.1");
    g_req.keep_alive = 1;
    if (body != NULL) {
        g_req.content_length = strlen(body);
        g_req.body = strdup(body);
    }
}

struct reader { int fd; };
static void *reader_main(void *p) {
    struct reader *r = p;
    g_resp_len = 0;
    for (;;) {
        ssize_t n = read(r->fd, g_resp + g_resp_len, RESP_CAP - 1 - g_resp_len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        g_resp_len += (size_t)n;
    }
    g_resp[g_resp_len] = '\0';
    return NULL;
}

// g_req로 serve_cgi 실행 → 반환값. 보낸 응답은 g_resp
static int run(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { perror("socketpair"); exit(1); }
    struct reader r = { sv[0] };
    pthread_t t;
    if (pthread_create(&t, NULL, reader_main, &r) != 0) { perror("pthread_create"); exit(1); }
    int rc = serve_cgi(sv[1], g_dir, &g_req);
    close(sv[1]);                                          // 읽기 스레드에 EOF
    pthread_join(t, NULL);
    close(sv[0]);
    return rc;
}

static const char *body_of(void) {
    char *p = strstr(g_resp, "\r\n\r\n");
    return p ? p + 4 : "";
}

static int count_fds(void) {
    DIR *d = opendir("/proc/self/fd");
    int n = 0;
    if (d == NULL) return -1;
    while (readdir(d) != NULL) n++;
    closedir(d);
    return n;
}

static void test_env_and_get(void) {
    write_script("env", "printf 'Content-Type: text/plain\\r\\n\\r\\n'\n"
                        "echo \"M=$REQUEST_METHOD Q=$QUERY_STRING L=$CONTENT_LENGTH S=$SCRIPT_NAME G=$GATEWAY_INTERFACE P=$SERVER_PROTOCOL\"\n", 0755);
    make_req("GET", "/cgi-bin/env", "a=1&b=2", NULL);
    CHECK(run() == 0);
    CHECK(strncmp(g_resp, "HTTP/1.1 200 OK\r\n", 17) == 0);
    CHECK(strstr(g_resp, "Content-Type: text/plain\r\n") != NULL);
    CHECK(strstr(g_resp, "Connection: keep-alive\r\n") != NULL);
    const char *want = "M=GET Q=a=1&b=2 L=0 S=/cgi-bin/env G=CGI/1.1 P=HTTP/1.1\n";
    CHECK_STR(body_of(), want);
    char cl[64];
    snprintf(cl, sizeof(cl), "Content-Length: %zu\r\n", strlen(want));
    CHECK(strstr(g_resp, cl) != NULL);                     // 서버가 본문 길이를 계산해 붙임
}

// CONTENT_TYPE은 요청 헤더에서
static void test_content_type_env(void) {
    write_script("ct", "printf 'Content-Type: text/plain\\n\\n'; printf '%s' \"$CONTENT_TYPE\"\n", 0755);
    make_req("POST", "/cgi-bin/ct", "", "x");
    snprintf(g_req.headers[0].name, sizeof(g_req.headers[0].name), "Content-Type");
    snprintf(g_req.headers[0].value, sizeof(g_req.headers[0].value), "application/x-www-form-urlencoded");
    g_req.header_count = 1;
    CHECK(run() == 0);
    CHECK_STR(body_of(), "application/x-www-form-urlencoded");
}

// POST 본문 → 표준 입력. 입출력 모두 파이프 버퍼(64KB)보다 크게 해서 교착이 없는지 확인
static void test_post_large_echo(void) {
    write_script("cat", "printf 'Content-Type: application/octet-stream\\n\\n'; exec cat\n", 0755);
    size_t n = 512 * 1024;
    char *big = malloc(n + 1);
    for (size_t i = 0; i < n; i++) big[i] = (char)('a' + i % 26);
    big[n] = '\0';
    make_req("POST", "/cgi-bin/cat", "", big);
    CHECK(run() == 0);
    CHECK(strncmp(g_resp, "HTTP/1.1 200", 12) == 0);
    const char *b = body_of();
    CHECK(g_resp_len - (size_t)(b - g_resp) == n && memcmp(b, big, n) == 0);
    free(big);
}

// 본문을 읽지 않고 끝나는 CGI → 서버는 EPIPE를 받아도 정상 응답
static void test_post_ignored_body(void) {
    write_script("noread", "printf 'Content-Type: text/plain\\n\\nok'\n", 0755);
    char *big = malloc(1024 * 1024 + 1);
    memset(big, 'z', 1024 * 1024);
    big[1024 * 1024] = '\0';
    make_req("POST", "/cgi-bin/noread", "", big);
    CHECK(run() == 0);
    CHECK_STR(body_of(), "ok");
    free(big);
}

static void test_status_header(void) {
    write_script("st", "printf 'Status: 404 Not Found\\r\\nContent-Type: text/plain\\r\\n\\r\\ngone'\n", 0755);
    make_req("GET", "/cgi-bin/st", "", NULL);
    CHECK(run() == 0);
    CHECK(strncmp(g_resp, "HTTP/1.1 404 Not Found\r\n", 24) == 0);
    CHECK_STR(body_of(), "gone");
    write_script("st2", "printf 'Status: 99\\nContent-Type: text/plain\\n\\nx'\n", 0755);
    make_req("GET", "/cgi-bin/st2", "", NULL);
    CHECK(run() == 502);                                   // 범위 밖 상태 코드
}

static void test_head(void) {
    make_req("HEAD", "/cgi-bin/noread", "", NULL);
    CHECK(run() == 0);
    CHECK(strstr(g_resp, "Content-Length: 2\r\n") != NULL);
    CHECK_STR(body_of(), "");                              // 헤더만
}

static void test_bad_output(void) {
    write_script("nohdr", "echo hello\n", 0755);           // 빈 줄(헤더 끝)이 없음
    make_req("GET", "/cgi-bin/nohdr", "", NULL);
    CHECK(run() == 502);
    write_script("noct", "printf 'X-A: 1\\n\\nbody'\n", 0755);   // Content-Type 없음
    make_req("GET", "/cgi-bin/noct", "", NULL);
    CHECK(run() == 502);
    write_script("nocolon", "printf 'garbage\\n\\nbody'\n", 0755);
    make_req("GET", "/cgi-bin/nocolon", "", NULL);
    CHECK(run() == 502);
    write_script("fail", "printf 'Content-Type: text/plain\\n\\nx'; exit 1\n", 0755);   // 비정상 종료
    make_req("GET", "/cgi-bin/fail", "", NULL);
    CHECK(run() == 502);
    CHECK(g_resp_len == 0);                                // 오류 코드 반환 시 serve_cgi는 아무것도 안 보냄
}

// 출력 상한: 정확히 1MB는 허용, 1바이트라도 넘으면 502
static void test_output_limit(void) {
    char s[256];
    // "Content-Type: a\n\n" = 17바이트
    snprintf(s, sizeof(s), "printf 'Content-Type: a\\n\\n'; head -c %d /dev/zero\n", CGI_MAX_OUTPUT - 17);
    write_script("exact", s, 0755);
    make_req("GET", "/cgi-bin/exact", "", NULL);
    CHECK(run() == 0);
    CHECK(strstr(g_resp, "HTTP/1.1 200") == g_resp);
    snprintf(s, sizeof(s), "printf 'Content-Type: a\\n\\n'; head -c %d /dev/zero\n", CGI_MAX_OUTPUT - 16);
    write_script("over", s, 0755);
    make_req("GET", "/cgi-bin/over", "", NULL);
    CHECK(run() == 502);
    write_script("flood", "printf 'Content-Type: a\\n\\n'; exec cat /dev/zero\n", 0755);   // 끝없는 출력
    make_req("GET", "/cgi-bin/flood", "", NULL);
    CHECK(run() == 502);
}

// 시간 제한: 출력 중 멈춤 / 출력은 닫고 계속 실행 — 둘 다 504, 제한 시간 근처에서 끝나야 함
// 테스트 socketpair는 CLOEXEC가 아니라 sleep(셸의 자식)이 응답 소켓을 물려받는다.
// 셸만 죽이고 sleep이 남으면 읽기 스레드가 30초 동안 EOF를 못 받아 여기서 실패 → 프로세스 그룹 종료를 검증
static void test_timeout(void) {
    write_script("hang", "printf 'Content-Type: a\\n\\n'; sleep 30\n", 0755);
    make_req("GET", "/cgi-bin/hang", "", NULL);
    double t0 = rio_now();
    CHECK(run() == 504);
    double el = rio_now() - t0;
    if (el > CGI_TIMEOUT_SEC + 2) fprintf(stderr, "  hang elapsed %.2fs\n", el);
    CHECK(el >= CGI_TIMEOUT_SEC - 0.1 && el < CGI_TIMEOUT_SEC + 2);
    write_script("detach", "printf 'Content-Type: a\\n\\nx'; exec >&-; sleep 30\n", 0755);
    make_req("GET", "/cgi-bin/detach", "", NULL);
    t0 = rio_now();
    CHECK(run() == 504);
    el = rio_now() - t0;
    if (el > CGI_TIMEOUT_SEC + 2) fprintf(stderr, "  detach elapsed %.2fs\n", el);
    CHECK(el >= CGI_TIMEOUT_SEC - 0.1 && el < CGI_TIMEOUT_SEC + 2);
}

static void test_lookup_errors(void) {
    make_req("GET", "/cgi-bin/missing", "", NULL);
    CHECK(run() == 404);
    make_req("GET", "/cgi-bin/", "", NULL);
    CHECK(run() == 404);
    make_req("GET", "/cgi-bin/..", "", NULL);
    CHECK(run() == 404);
    make_req("GET", "/cgi-bin/../env", "", NULL);          // 하위 경로·탈출 불가
    CHECK(run() == 404);
    make_req("GET", "/cgi-bin/sub/x", "", NULL);
    CHECK(run() == 404);
    write_script("noexec", "printf 'Content-Type: a\\n\\nx'\n", 0644);
    make_req("GET", "/cgi-bin/noexec", "", NULL);
    CHECK(run() == 403);
    char p[128];
    snprintf(p, sizeof(p), "%s/adir", g_dir);
    mkdir(p, 0755);
    make_req("GET", "/cgi-bin/adir", "", NULL);
    CHECK(run() == 403);                                   // 디렉터리
    write_script("badinterp", "", 0755);
    snprintf(p, sizeof(p), "%s/badinterp", g_dir);
    FILE *fp = fopen(p, "wb");
    if (fp) { fputs("#!/nonexistent/interp\n", fp); fclose(fp); }
    make_req("GET", "/cgi-bin/badinterp", "", NULL);
    CHECK(run() == 500);                                   // execve 실패
    make_req("PUT", "/cgi-bin/env", "", NULL);
    CHECK(run() == 405);
}

// 워커처럼 SIGINT/SIGTERM을 막고 SIGPIPE를 무시한 스레드에서 실행해도, CGI는 기본 시그널 상태로 시작해야 함
static void test_signal_state_reset(void) {
    // 셸은 시작할 때 시그널 상태를 스스로 바꾸기도 하므로, 셸이 아닌 grep 자신의 상태(/proc/self)를 본다
    write_script("sig", "printf 'Content-Type: text/plain\\n\\n'; exec grep -E '^Sig(Blk|Ign)' /proc/self/status\n", 0755);
    sigset_t blk, old;
    sigemptyset(&blk);
    sigaddset(&blk, SIGINT);
    sigaddset(&blk, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &blk, &old);
    make_req("GET", "/cgi-bin/sig", "", NULL);
    CHECK(run() == 0);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    unsigned long long blkmask = 1, ignmask = 0;
    const char *b = strstr(g_resp, "SigBlk:");
    const char *i = strstr(g_resp, "SigIgn:");
    CHECK(b != NULL && i != NULL);
    if (b) blkmask = strtoull(b + 7, NULL, 16);
    if (i) ignmask = strtoull(i + 7, NULL, 16);
    CHECK(blkmask == 0);
    CHECK((ignmask & (1ULL << (SIGPIPE - 1))) == 0);
}

// 요청마다 fd·자식 프로세스가 남지 않음 (시간 초과·출력 과다 경로 포함)
static void test_no_leaks(void) {
    int before = count_fds();
    const char *names[] = { "env", "nohdr", "fail", "over", "badinterp", "missing", "noread" };
    for (int k = 0; k < 3; k++) {
        for (size_t j = 0; j < sizeof(names) / sizeof(names[0]); j++) {
            char path[64];
            snprintf(path, sizeof(path), "/cgi-bin/%s", names[j]);
            make_req("POST", path, "", "a=1");
            run();
        }
    }
    CHECK(count_fds() == before);
    CHECK(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);   // 좀비 없음
}

// 실제 예제 cgi-bin/adder (make가 빌드). 저장소 루트에서 실행된다고 가정
static void test_adder(void) {
    char save[sizeof(g_dir)];
    memcpy(save, g_dir, sizeof(g_dir));
    snprintf(g_dir, sizeof(g_dir), "./cgi-bin");
    struct { const char *method, *query, *body; int status; const char *out; } cases[] = {
        { "GET",  "a=1&b=2",     NULL,      200, "1 + 2 = 3\n" },
        { "GET",  "1&2",         NULL,      200, "1 + 2 = 3\n" },
        { "GET",  "a=-5&b=3",    NULL,      200, "-5 + 3 = -2\n" },
        { "POST", "",            "a=40&b=2", 200, "40 + 2 = 42\n" },
        { "GET",  "a=1",         NULL,      400, NULL },
        { "GET",  "a=1&b=2&c=3", NULL,      400, NULL },
        { "GET",  "a=x&b=2",     NULL,      400, NULL },
        { "GET",  "a=&b=2",      NULL,      400, NULL },
        { "GET",  "a=9223372036854775807&b=1", NULL, 400, NULL },          // 덧셈 오버플로
        { "GET",  "a=99999999999999999999&b=1", NULL, 400, NULL },         // strtol 범위 초과
    };
    for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        make_req(cases[k].method, "/cgi-bin/adder", cases[k].query, cases[k].body);
        CHECK(run() == 0);
        char line[32];
        snprintf(line, sizeof(line), "HTTP/1.1 %d", cases[k].status);
        if (strncmp(g_resp, line, strlen(line)) != 0) fprintf(stderr, "  adder case %zu: %.40s\n", k, g_resp);
        CHECK(strncmp(g_resp, line, strlen(line)) == 0);
        if (cases[k].out) CHECK_STR(body_of(), cases[k].out);
    }
    memcpy(g_dir, save, sizeof(g_dir));
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);                              // 서버(main.c)와 같은 조건
    g_resp = malloc(RESP_CAP);
    snprintf(g_dir, sizeof(g_dir), "/tmp/test_cgi_XXXXXX");
    if (g_resp == NULL || mkdtemp(g_dir) == NULL) { perror("setup"); return 1; }

    test_env_and_get();
    test_content_type_env();
    test_post_large_echo();
    test_post_ignored_body();
    test_status_header();
    test_head();
    test_bad_output();
    test_output_limit();
    test_lookup_errors();
    test_signal_state_reset();
    test_no_leaks();
    test_adder();
    test_timeout();

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
    if (system(cmd) != 0) fprintf(stderr, "cleanup failed\n");
    free(g_req.body);
    free(g_resp);
    return TEST_RESULT();
}
