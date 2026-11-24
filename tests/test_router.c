// [test_router.c] Reverse Proxy 라우팅 단위 테스트
#include "test.h"
#include "router.h"

#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

// "/api/ 127.0.0.1:8001 127.0.0.1:8002" → prefix, 백엔드 2개, host/port 분리
static void test_parse_route_line(void) {
    route r;
    CHECK(parse_route_line("/api/ 127.0.0.1:8001 127.0.0.1:8002\n", &r) == 1);
    CHECK_STR(r.prefix, "/api/");
    CHECK(r.backend_count == 2);
    CHECK_STR(r.backends[0].host, "127.0.0.1");
    CHECK_STR(r.backends[0].port, "8001");
    CHECK_STR(r.backends[1].port, "8002");
    CHECK(parse_route_line("  /\tlocalhost:80   # 뒤는 주석\r\n", &r) == 1);   // 탭·앞뒤 공백·CRLF·끝 주석
    CHECK_STR(r.prefix, "/");
    CHECK(r.backend_count == 1);
    CHECK_STR(r.backends[0].host, "localhost");
    CHECK(parse_route_line("/v6/ [::1]:9000", &r) == 1);                       // IPv6는 괄호
    CHECK_STR(r.backends[0].host, "::1");
    CHECK_STR(r.backends[0].port, "9000");
    CHECK(parse_route_line("/x/ a:1 a:2 a:3 a:4 a:5 a:6 a:7 a:8", &r) == 1);   // MAX_BACKENDS(8)개까지
    CHECK(r.backend_count == MAX_BACKENDS);
}

// 빈 줄·주석 → 0, 백엔드 없음·':' 없음·포트 범위 밖 → -1
static void test_parse_route_line_errors(void) {
    route r;
    CHECK(parse_route_line("", &r) == 0);
    CHECK(parse_route_line("   \n", &r) == 0);
    CHECK(parse_route_line("# comment\n", &r) == 0);
    CHECK(parse_route_line("  # indented comment", &r) == 0);
    CHECK(parse_route_line("/api/", &r) == -1);                      // 백엔드 없음
    CHECK(parse_route_line("/api/ # 주석만", &r) == -1);
    CHECK(parse_route_line("/api/ 127.0.0.1", &r) == -1);            // ':' 없음
    CHECK(parse_route_line("/api/ :8001", &r) == -1);                // 호스트 없음
    CHECK(parse_route_line("/api/ h:", &r) == -1);                   // 포트 없음
    CHECK(parse_route_line("/api/ h:0", &r) == -1);
    CHECK(parse_route_line("/api/ h:65536", &r) == -1);
    CHECK(parse_route_line("/api/ h:80x", &r) == -1);
    CHECK(parse_route_line("/api/ h:+80", &r) == -1);
    CHECK(parse_route_line("/api/ h:123456", &r) == -1);
    CHECK(parse_route_line("api/ h:80", &r) == -1);                  // prefix는 '/'로 시작
    CHECK(parse_route_line("/v6/ [::1:9000", &r) == -1);             // 괄호 안 닫힘
    CHECK(parse_route_line("/x/ a:1 a:2 a:3 a:4 a:5 a:6 a:7 a:8 a:9", &r) == -1);   // 백엔드 9개
    CHECK(parse_route_line(NULL, &r) == -1);
    CHECK(parse_route_line("/api/#x 127.0.0.1:8001", &r) == -1);    // 공백 없이 붙은 '#'
    CHECK(parse_route_line("/api/ 127.0.0.1:8001#x", &r) == -1);
    char longp[400];
    memset(longp, 'a', sizeof(longp));
    longp[0] = '/';
    memcpy(longp + 300, " h:80", 6);                                 // prefix 300바이트 (상한 255)
    CHECK(parse_route_line(longp, &r) == -1);
}

// "/api/" 와 "/api/v2/" 규칙이 있을 때 "/api/v2/x" → 긴 쪽, 맞는 규칙 없음 → NULL
static void test_match_longest_prefix(void) {
    route_table t;
    memset(&t, 0, sizeof(t));
    parse_route_line("/api/v2/ h:2", &t.routes[0]);                  // 긴 규칙을 먼저 넣어도 / 나중에 넣어도 같아야 함
    parse_route_line("/api/ h:1", &t.routes[1]);
    parse_route_line("/ h:0", &t.routes[2]);
    t.count = 2;
    CHECK(match_route(&t, "/api/v2/x") == &t.routes[0]);
    CHECK(match_route(&t, "/api/x") == &t.routes[1]);
    CHECK(match_route(&t, "/api/v2") == &t.routes[1]);               // "/api/v2/"에는 안 맞음
    CHECK(match_route(&t, "/api") == NULL);                          // prefix "/api/"보다 짧음
    CHECK(match_route(&t, "/other") == NULL);
    t.count = 3;
    CHECK(match_route(&t, "/other") == &t.routes[2]);                // "/" 규칙 = 모두
    CHECK(match_route(&t, "/api/v2/x") == &t.routes[0]);
    CHECK(match_route(NULL, "/x") == NULL);
    // 짧은 규칙이 먼저 있어도 긴 쪽 (첫 번째로 맞는 규칙을 고르면 안 됨)
    route_table s;
    memset(&s, 0, sizeof(s));
    parse_route_line("/ h:0", &s.routes[0]);
    parse_route_line("/api/ h:1", &s.routes[1]);
    parse_route_line("/api/v2/ h:2", &s.routes[2]);
    s.count = 3;
    CHECK(match_route(&s, "/api/v2/x") == &s.routes[2]);
    CHECK(match_route(&s, "/api/x") == &s.routes[1]);
    CHECK(match_route(&s, "/x") == &s.routes[0]);
}

// 백엔드 2개면 next_backend가 8001 → 8002 → 8001 순서
static void test_round_robin(void) {
    route r;
    parse_route_line("/api/ 127.0.0.1:8001 127.0.0.1:8002", &r);
    // CHECK_STR은 인자를 두 번 평가하므로 호출 결과를 먼저 변수에 받는다
    const char *seq[3];
    for (int i = 0; i < 3; i++) seq[i] = next_backend(&r)->port;
    CHECK_STR(seq[0], "8001");
    CHECK_STR(seq[1], "8002");
    CHECK_STR(seq[2], "8001");
    route empty;
    memset(&empty, 0, sizeof(empty));
    CHECK(next_backend(&empty) == NULL);
}

// 여러 스레드가 동시에 불러도 정확히 고르게 나뉨 (카운터 증가가 원자적이 아니면 같은 번호를 두 번 받아 어긋난다)
#define RR_THREADS 8
#define RR_CALLS   100000
static route g_rr;
static int g_counts[RR_THREADS][MAX_BACKENDS];
static void *rr_worker(void *p) {
    int id = (int)(long)p;
    for (int i = 0; i < RR_CALLS; i++) g_counts[id][next_backend(&g_rr) - g_rr.backends]++;
    return NULL;
}
static void test_round_robin_concurrent(void) {
    parse_route_line("/ a:1 a:2 a:3", &g_rr);
    pthread_t t[RR_THREADS];
    for (long i = 0; i < RR_THREADS; i++) pthread_create(&t[i], NULL, rr_worker, (void *)i);
    for (int i = 0; i < RR_THREADS; i++) pthread_join(t[i], NULL);
    long total[3] = { 0, 0, 0 };
    for (int i = 0; i < RR_THREADS; i++)
        for (int b = 0; b < 3; b++) total[b] += g_counts[i][b];
    long all = (long)RR_THREADS * RR_CALLS;
    CHECK(total[0] + total[1] + total[2] == all);
    // 800000 = 3 × 266666 + 2 → 정확히 266667, 266667, 266666
    CHECK(total[0] == all / 3 + 1 && total[1] == all / 3 + 1 && total[2] == all / 3);
    CHECK(g_rr.next == (unsigned int)all);
}

static void write_conf(const char *path, const char *text) {
    FILE *fp = fopen(path, "w");
    if (fp) { fputs(text, fp); fclose(fp); }
}

static void test_load_routes(void) {
    char path[] = "/tmp/test_router_XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    close(fd);
    route_table t;
    write_conf(path, "# routes\n\n/api/ 127.0.0.1:8001 127.0.0.1:8002\n/img/ 127.0.0.1:8003   # images\n");
    CHECK(load_routes(path, &t) == 0);
    CHECK(t.count == 2);
    CHECK_STR(t.routes[1].prefix, "/img/");
    CHECK(match_route(&t, "/img/a.png") == &t.routes[1]);
    write_conf(path, "/api/ 127.0.0.1:8001\n/bad/ nocolon\n");      // 2번째 줄 오류 → 전체 실패
    CHECK(load_routes(path, &t) == -1);
    write_conf(path, "/api/ h:1\n/api/ h:2\n");                      // 같은 prefix 두 번 → 뒤 규칙이 쓰일 수 없으니 오류
    CHECK(load_routes(path, &t) == -1);
    write_conf(path, "");                                            // 빈 파일 = 규칙 0개 (성공)
    CHECK(load_routes(path, &t) == 0 && t.count == 0);
    char many[2048] = "";
    for (int i = 0; i <= MAX_ROUTES; i++) {                          // MAX_ROUTES + 1개
        char line[64];
        snprintf(line, sizeof(line), "/r%d/ h:80\n", i);
        strcat(many, line);
    }
    write_conf(path, many);
    CHECK(load_routes(path, &t) == -1);
    char longline[1500];
    memset(longline, ' ', sizeof(longline));
    memcpy(longline, "/a/ h:80", 8);
    longline[sizeof(longline) - 2] = '\n';
    longline[sizeof(longline) - 1] = '\0';
    write_conf(path, longline);                                      // 1024바이트를 넘는 줄
    CHECK(load_routes(path, &t) == -1);
    unlink(path);
    CHECK(load_routes(path, &t) == -1);                              // 없는 파일
}

int main(void) {
    test_parse_route_line();
    test_parse_route_line_errors();
    test_match_longest_prefix();
    test_round_robin();
    test_round_robin_concurrent();
    test_load_routes();
    return TEST_RESULT();
}
