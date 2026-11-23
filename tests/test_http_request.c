// [test_http_request.c] HTTP 파서 단위 테스트 — 소켓 없이 문자열로 parse_request_line / parse_header_line 검증
#include "test.h"
#include "http_request.h"

// 정상: "GET /index.html?a=1 HTTP/1.1\r\n" → method, path "/index.html", query "a=1", keep_alive 1
static void test_request_line_ok(void) { /* TODO */ }

// 정상: POST, HTTP/1.0 (keep_alive 기본 0)
static void test_request_line_post_http10(void) { /* TODO */ }

// 오류: 토큰 개수 틀림 → 400, 지원 안 하는 메서드 → 501, HTTP/2.0 → 505, 너무 긴 URI → 414
static void test_request_line_errors(void) { /* TODO */ }

// 정상: "Host: localhost\r\n" 저장, get_header("host") 대소문자 무시
static void test_header_ok(void) { /* TODO */ }

// Content-Length: 숫자 → content_length, 숫자 아님 → 400, MAX_BODY_LEN 초과 → 413
static void test_header_content_length(void) { /* TODO */ }

// Connection: close → keep_alive 0, keep-alive → 1
static void test_header_connection(void) { /* TODO */ }

// 오류: ':' 없음 → 400, 헤더 MAX_HEADERS 초과 → 431
static void test_header_errors(void) { /* TODO */ }

int main(void) {
    test_request_line_ok();
    test_request_line_post_http10();
    test_request_line_errors();
    test_header_ok();
    test_header_content_length();
    test_header_connection();
    test_header_errors();
    return TEST_RESULT();
}
