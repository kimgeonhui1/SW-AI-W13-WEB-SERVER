// [test_router.c] Reverse Proxy 라우팅 단위 테스트
#include "test.h"
#include "router.h"

// "/api/ 127.0.0.1:8001 127.0.0.1:8002" → prefix, 백엔드 2개, host/port 분리
static void test_parse_route_line(void) { /* TODO */ }

// 빈 줄·주석 → 0, 백엔드 없음·':' 없음·포트 범위 밖 → -1
static void test_parse_route_line_errors(void) { /* TODO */ }

// "/api/" 와 "/api/v2/" 규칙이 있을 때 "/api/v2/x" → 긴 쪽, 맞는 규칙 없음 → NULL
static void test_match_longest_prefix(void) { /* TODO */ }

// 백엔드 2개면 next_backend가 8001 → 8002 → 8001 순서
static void test_round_robin(void) { /* TODO */ }

int main(void) {
    test_parse_route_line();
    test_parse_route_line_errors();
    test_match_longest_prefix();
    test_round_robin();
    return TEST_RESULT();
}
