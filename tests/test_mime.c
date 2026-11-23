// [test_mime.c] MIME 타입 매핑 단위 테스트
#include "test.h"
#include "mime.h"

// html, css, js, png, jpg, jpeg 기본 매핑
static void test_known_types(void) { /* TODO */ }

// 대문자 확장자(".HTML")도 같은 결과
static void test_case_insensitive(void) { /* TODO */ }

// 확장자 없음, 모르는 확장자, "/dir.v1/file"(디렉터리의 '.') → application/octet-stream
static void test_unknown_types(void) { /* TODO */ }

int main(void) {
    test_known_types();
    test_case_insensitive();
    test_unknown_types();
    return TEST_RESULT();
}
