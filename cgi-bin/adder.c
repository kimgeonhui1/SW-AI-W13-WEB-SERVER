// [adder.c] CGI 예제 — 두 수를 더한 결과를 출력 (동적 요청 처리 데모)
// 입력: GET이면 QUERY_STRING, POST면 표준 입력(CONTENT_LENGTH 바이트). 형식은 "a=1&b=2" 또는 "1&2"
// 출력: CGI 헤더 + 빈 줄 + 본문. 서버가 이 출력을 파이프로 받아 Content-Length를 붙여 응답한다
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ADDER_MAX_INPUT 1024

// "a=12" 또는 "12" → long. 숫자 뒤에 다른 글자가 있거나 범위를 넘으면 실패(0 반환)
static int parse_operand(const char *s, long *out) {
    const char *eq = strchr(s, '=');
    if (eq != NULL) s = eq + 1;
    if (*s == '\0') return 0;
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno == ERANGE || *end != '\0') return 0;
    *out = v;
    return 1;
}

static void bad_request(const char *why) {
    printf("Status: 400 Bad Request\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n%s\n", why);
}

int main(void) {
    char input[ADDER_MAX_INPUT + 1];
    const char *method = getenv("REQUEST_METHOD");
    if (method != NULL && strcmp(method, "POST") == 0) {
        const char *cl = getenv("CONTENT_LENGTH");
        // 서버 밖에서 직접 실행될 수도 있으므로 환경변수를 믿지 않는다: 숫자만, 범위 안
        char *end = NULL;
        errno = 0;
        long want = (cl != NULL) ? strtol(cl, &end, 10) : -1;
        if (cl == NULL || end == cl || *end != '\0' || errno == ERANGE) { bad_request("bad CONTENT_LENGTH"); return 0; }
        if (want < 0 || want > ADDER_MAX_INPUT) { bad_request("body too large"); return 0; }
        size_t got = 0;
        while (got < (size_t)want) {                         // 파이프는 나눠서 올 수 있으므로 다 받을 때까지
            ssize_t n = read(STDIN_FILENO, input + got, (size_t)want - got);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            got += (size_t)n;
        }
        input[got] = '\0';
    } else {
        const char *q = getenv("QUERY_STRING");
        if (q == NULL) q = "";
        if (strlen(q) > ADDER_MAX_INPUT) { bad_request("query too long"); return 0; }
        strcpy(input, q);
    }

    // 정확히 두 항목("x&y")이어야 한다
    char *amp = strchr(input, '&');
    if (amp == NULL || strchr(amp + 1, '&') != NULL) { bad_request("usage: ?a=1&b=2"); return 0; }
    *amp = '\0';
    long a, b;
    if (!parse_operand(input, &a) || !parse_operand(amp + 1, &b)) { bad_request("operands must be integers"); return 0; }
    // 덧셈 오버플로는 정의되지 않은 동작 → 미리 검사
    if ((b > 0 && a > LONG_MAX - b) || (b < 0 && a < LONG_MIN - b)) { bad_request("overflow"); return 0; }

    printf("Content-Type: text/plain; charset=utf-8\r\n\r\n%ld + %ld = %ld\n", a, b, a + b);
    return 0;
}
