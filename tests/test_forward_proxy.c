// [test_forward_proxy.c] Forward Proxy 절대 URI 파싱 단위 테스트
#include "test.h"
#include "forward_proxy.h"

// "http://..." / "HTTP://..." → 1, "/index.html", "https://..." → 0
static void test_is_absolute_uri(void) { /* TODO */ }

// "http://example.com/a.html" → host "example.com", port "80", path "/a.html"
static void test_parse_default_port(void) { /* TODO */ }

// "http://localhost:8001/api?x=1" → port "8001", path "/api?x=1" (쿼리 유지)
static void test_parse_port_and_query(void) { /* TODO */ }

// "http://example.com" (경로 없음) → path "/"
static void test_parse_no_path(void) { /* TODO */ }

// 오류: "http:///a" (호스트 없음), "http://h:99999/" (포트 범위 밖), 버퍼보다 긴 호스트 → -1
static void test_parse_errors(void) { /* TODO */ }

int main(void) {
    test_is_absolute_uri();
    test_parse_default_port();
    test_parse_port_and_query();
    test_parse_no_path();
    test_parse_errors();
    return TEST_RESULT();
}
