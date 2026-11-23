// [test.h] 최소 단위 테스트 도구 — 외부 라이브러리 없이 CHECK 매크로로 검증
#ifndef TEST_H
#define TEST_H

#include <stdio.h>
#include <string.h>

static int test_failures = 0;

// 조건이 거짓이면 파일:줄과 조건식을 출력하고 실패로 센다 (테스트는 계속 진행)
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        test_failures++; \
    } \
} while (0)

// 문자열 비교용
#define CHECK_STR(actual, expected) CHECK((actual) != NULL && strcmp((actual), (expected)) == 0)

// main의 마지막에 return TEST_RESULT(); — 실패가 있으면 1 (make test 중단)
#define TEST_RESULT() \
    (test_failures ? (fprintf(stderr, "  %d check(s) failed\n", test_failures), 1) : (printf("  ok\n"), 0))

#endif
