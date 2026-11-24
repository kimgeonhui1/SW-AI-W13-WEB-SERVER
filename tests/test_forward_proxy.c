// [test_forward_proxy.c] Forward Proxy 절대 URI 파싱 단위 테스트
#include "test.h"
#include "forward_proxy.h"

static char g_host[64], g_port[8], g_path[64];

static int parse(const char *uri) {
    g_host[0] = g_port[0] = g_path[0] = '\0';
    return parse_absolute_uri(uri, g_host, sizeof(g_host), g_port, sizeof(g_port), g_path, sizeof(g_path));
}

// "http://..." / "HTTP://..." → 1, "/index.html", "https://..." → 0
static void test_is_absolute_uri(void) {
    CHECK(is_absolute_uri("http://example.com/") == 1);
    CHECK(is_absolute_uri("HTTP://example.com/") == 1);
    CHECK(is_absolute_uri("HtTp://x") == 1);
    CHECK(is_absolute_uri("/index.html") == 0);
    CHECK(is_absolute_uri("https://example.com/") == 0);       // HTTPS(CONNECT)는 지원 안 함
    CHECK(is_absolute_uri("http:/x") == 0);
    CHECK(is_absolute_uri("") == 0);
    CHECK(is_absolute_uri(NULL) == 0);
}

// "http://example.com/a.html" → host "example.com", port "80", path "/a.html"
static void test_parse_default_port(void) {
    CHECK(parse("http://example.com/a.html") == 0);
    CHECK_STR(g_host, "example.com");
    CHECK_STR(g_port, "80");
    CHECK_STR(g_path, "/a.html");
    CHECK(parse("http://example.com:/a") == 0);                 // 빈 포트 = 기본값 (RFC 3986)
    CHECK_STR(g_port, "80");
}

// "http://localhost:8001/api?x=1" → port "8001", path "/api?x=1" (쿼리 유지)
static void test_parse_port_and_query(void) {
    CHECK(parse("http://localhost:8001/api?x=1") == 0);
    CHECK_STR(g_host, "localhost");
    CHECK_STR(g_port, "8001");
    CHECK_STR(g_path, "/api?x=1");
    CHECK(parse("http://h:65535/") == 0 && strcmp(g_port, "65535") == 0);
    CHECK(parse("http://h:1/") == 0 && strcmp(g_port, "1") == 0);
    CHECK(parse("http://h/a#frag") == 0);                       // 조각은 서버로 보내지 않음
    CHECK_STR(g_path, "/a");
    CHECK(parse("http://[::1]:8080/x") == 0);                   // IPv6
    CHECK_STR(g_host, "::1");
    CHECK_STR(g_port, "8080");
    CHECK(parse("http://[::1]/") == 0 && strcmp(g_host, "::1") == 0 && strcmp(g_port, "80") == 0);
}

// "http://example.com" (경로 없음) → path "/"
static void test_parse_no_path(void) {
    CHECK(parse("http://example.com") == 0);
    CHECK_STR(g_host, "example.com");
    CHECK_STR(g_path, "/");
    CHECK(parse("http://example.com?a=1") == 0);                // 경로 없이 쿼리
    CHECK_STR(g_path, "/?a=1");
    CHECK(parse("http://example.com:8080") == 0);
    CHECK_STR(g_port, "8080");
    CHECK_STR(g_path, "/");
}

// 오류: "http:///a" (호스트 없음), "http://h:99999/" (포트 범위 밖), 버퍼보다 긴 호스트 → -1
static void test_parse_errors(void) {
    CHECK(parse("http:///a") == -1);
    CHECK(parse("http://") == -1);
    CHECK(parse("http://:80/") == -1);
    CHECK(parse("http://h:99999/") == -1);
    CHECK(parse("http://h:65536/") == -1);
    CHECK(parse("http://h:0/") == -1);
    CHECK(parse("http://h:8a/") == -1);
    CHECK(parse("http://h:+80/") == -1);
    CHECK(parse("http://h:80:81/") == -1);                      // ':' 두 개
    CHECK(parse("http://user:pw@h/") == -1);                    // userinfo
    CHECK(parse("http://good.com@evil.com/") == -1);            // 접속 호스트 속이기
    CHECK(parse("http://[::1/") == -1);                         // ']' 없음
    CHECK(parse("http://[::1]x/") == -1);                       // ']' 뒤에 ':포트'가 아님
    CHECK(parse("http://[]/") == -1);
    CHECK(parse("https://h/") == -1);
    CHECK(parse("/a") == -1);
    char longhost[128] = "http://";
    memset(longhost + 7, 'a', 80);                              // 호스트 80자 > 버퍼 64
    longhost[87] = '\0';
    CHECK(parse(longhost) == -1);
    char longpath[128] = "http://h/";
    memset(longpath + 9, 'p', 70);                              // 경로 71자 > 버퍼 64
    longpath[79] = '\0';
    CHECK(parse(longpath) == -1);
    CHECK(parse_absolute_uri("http://h/", NULL, 0, g_port, sizeof(g_port), g_path, sizeof(g_path)) == -1);
}

int main(void) {
    test_is_absolute_uri();
    test_parse_default_port();
    test_parse_port_and_query();
    test_parse_no_path();
    test_parse_errors();
    return TEST_RESULT();
}
