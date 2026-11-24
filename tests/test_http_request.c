// [test_http_request.c] HTTP 요청 파서 단위 테스트
#include "http_request.h"
#include "test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void feed_and_close(const char *raw, rio_t *rp, int sv[2]) {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        perror("socketpair");
        exit(1);
    }
    size_t len = strlen(raw);
    if (len > 0) {
        CHECK(rio_writen(sv[0], raw, len) == (ssize_t)len);
    }
    close(sv[0]);
    rio_readinitb(rp, sv[1]);
}

static void test_valid_get_and_query(void) {
    http_request req = { 0 };
    int rc = parse_request_line("GET /search?q=hello&page=2 HTTP/1.1\r\n", &req);
    CHECK(rc == 0);
    CHECK_STR(req.method, "GET");
    CHECK_STR(req.uri, "/search?q=hello&page=2");
    CHECK_STR(req.path, "/search");
    CHECK_STR(req.query, "q=hello&page=2");
    CHECK_STR(req.version, "HTTP/1.1");
    CHECK(req.keep_alive == 1);
    free_request(&req);
}

static void test_forward_proxy_abs_uri(void) {
    http_request req = { 0 };
    int rc = parse_request_line("GET http://example.com:8080/api/data?k=v HTTP/1.1\r\n", &req);
    CHECK(rc == 0);
    CHECK_STR(req.uri, "http://example.com:8080/api/data?k=v");
    CHECK_STR(req.path, "/api/data");
    CHECK_STR(req.query, "k=v");
    free_request(&req);

    http_request req2 = { 0 };
    rc = parse_request_line("HEAD http://example.com HTTP/1.0\r\n", &req2);
    CHECK(rc == 0);
    CHECK_STR(req2.method, "HEAD");
    CHECK_STR(req2.path, "/");
    CHECK_STR(req2.query, "");
    CHECK(req2.keep_alive == 0);
    free_request(&req2);
}

static void test_request_line_errors(void) {
    http_request req = { 0 };
    CHECK(parse_request_line("BADLINE\r\n", &req) == 400);
    CHECK(parse_request_line("GET  /a HTTP/1.1\r\n", &req) == 400);
    CHECK(parse_request_line("PUT /a HTTP/1.1\r\n", &req) == 501);
    CHECK(parse_request_line("DELETE /a HTTP/1.1\r\n", &req) == 501);
    CHECK(parse_request_line("GET /a HTTP/2.0\r\n", &req) == 505);
    CHECK(parse_request_line("GET /a HTTP/0.9\r\n", &req) == 505);

    char too_long[MAX_URI_LEN + 64];
    snprintf(too_long, sizeof(too_long), "GET /");
    memset(too_long + 5, 'a', MAX_URI_LEN + 10);
    snprintf(too_long + 5 + MAX_URI_LEN + 10, 20, " HTTP/1.1\r\n");
    CHECK(parse_request_line(too_long, &req) == 414);
}

static void test_headers_and_connection(void) {
    http_request req = { 0 };
    req.keep_alive = 1;
    CHECK(parse_header_line("Host: localhost:8080\r\n", &req) == 0);
    CHECK(parse_header_line("Connection: close\r\n", &req) == 0);
    CHECK_STR(get_header(&req, "host"), "localhost:8080");
    CHECK(req.keep_alive == 0);

    CHECK(parse_header_line("InvalidHeaderWithoutColon\r\n", &req) == 400);
    CHECK(parse_header_line(" Content-Length: 10\r\n", &req) == 400);
    CHECK(parse_header_line("Content-Length: abc\r\n", &req) == 400);
    CHECK(parse_header_line("Content-Length: -5\r\n", &req) == 400);
    // 값 안의 제어 문자: 프록시가 넘기면 백엔드가 CR을 줄바꿈으로 읽어 헤더가 늘어날 수 있음
    CHECK(parse_header_line("X-A: a\rX-Evil: 1\r\n", &req) == 400);
    CHECK(parse_header_line("X-A: a\x01" "b\r\n", &req) == 400);
    CHECK(parse_header_line("X-A: a\x7f\r\n", &req) == 400);
    CHECK(parse_header_line("X-A: a\tb\r\n", &req) == 0);              // 탭은 허용

    // close와 keep-alive가 함께 오면 close가 이긴다 (순서·대소문자·여러 헤더 줄과 무관)
    http_request both = { 0 };
    both.keep_alive = 1;
    CHECK(parse_header_line("Connection: close, keep-alive\r\n", &both) == 0);
    CHECK(both.keep_alive == 0);
    http_request both2 = { 0 };
    both2.keep_alive = 1;
    CHECK(parse_header_line("Connection: CLOSE\r\n", &both2) == 0);
    CHECK(parse_header_line("Connection: keep-alive\r\n", &both2) == 0);   // 두 번째 줄이 close를 덮으면 안 됨
    CHECK(both2.keep_alive == 0);

    // HTTP/1.0에서 Connection: keep-alive 명시 시 활성화
    http_request h10_req = { 0 };
    CHECK(parse_request_line("GET / HTTP/1.0\r\n", &h10_req) == 0);
    CHECK(h10_req.keep_alive == 0);
    CHECK(parse_header_line("Connection: keep-alive\r\n", &h10_req) == 0);
    CHECK(h10_req.keep_alive == 1);

    // 중복 Content-Length 헤더 -> 400
    http_request dup_req = { 0 };
    CHECK(parse_header_line("Content-Length: 5\r\n", &dup_req) == 0);
    CHECK(parse_header_line("Content-Length: 5\r\n", &dup_req) == 400);

    // 헤더 값 길이(MAX_HEADER_VALUE) 초과 -> 431
    http_request long_val_req = { 0 };
    char long_val_line[MAX_HEADER_VALUE + 64];
    size_t pos = (size_t)snprintf(long_val_line, sizeof(long_val_line), "X-Long: ");
    memset(long_val_line + pos, 'v', MAX_HEADER_VALUE + 8);
    long_val_line[pos + MAX_HEADER_VALUE + 8] = '\r';
    long_val_line[pos + MAX_HEADER_VALUE + 9] = '\n';
    long_val_line[pos + MAX_HEADER_VALUE + 10] = '\0';
    CHECK(parse_header_line(long_val_line, &long_val_req) == 431);

    // 헤더 개수(MAX_HEADERS) 초과 -> 431
    http_request many_hdr_req = { 0 };
    char hline[64];
    for (int i = 0; i < MAX_HEADERS; i++) {
        snprintf(hline, sizeof(hline), "X-Hdr-%d: 1\r\n", i);
        CHECK(parse_header_line(hline, &many_hdr_req) == 0);
    }
    CHECK(parse_header_line("X-Overflow: 1\r\n", &many_hdr_req) == 431);
}

static void test_read_request_post_and_errors(void) {
    rio_t rp;
    int sv[2];
    http_request req;

    const char *post_ok =
        "POST /cgi-bin/adder HTTP/1.1\r\n"
        "Host: 127.0.0.1:8080\r\n"
        "Content-Length: 7\r\n"
        "\r\n"
        "a=1&b=2";
    feed_and_close(post_ok, &rp, sv);
    CHECK(read_request(&rp, &req) == 0);
    CHECK_STR(req.method, "POST");
    CHECK_STR(req.path, "/cgi-bin/adder");
    CHECK(req.content_length == 7);
    CHECK_STR(req.body, "a=1&b=2");
    free_request(&req);
    close(sv[1]);

    // Content-Length: 0인 POST 요청 -> 0, body는 NULL
    const char *post_zero =
        "POST /ping HTTP/1.1\r\n"
        "Host: 127.0.0.1:8080\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    feed_and_close(post_zero, &rp, sv);
    CHECK(read_request(&rp, &req) == 0);
    CHECK(req.content_length == 0);
    CHECK(req.body == NULL);
    free_request(&req);
    close(sv[1]);

    // 헤더 중간에 EOF -> 400
    feed_and_close("GET / HTTP/1.1\r\nHost: localhost\r\n", &rp, sv);
    CHECK(read_request(&rp, &req) == 400);
    close(sv[1]);

    // HTTP/1.1 Host 누락 -> 400
    feed_and_close("GET / HTTP/1.1\r\n\r\n", &rp, sv);
    CHECK(read_request(&rp, &req) == 400);
    close(sv[1]);

    // POST Content-Length 누락 -> 411
    feed_and_close("POST /submit HTTP/1.1\r\nHost: localhost\r\n\r\n", &rp, sv);
    CHECK(read_request(&rp, &req) == 411);
    close(sv[1]);

    // chunked -> 501
    feed_and_close("POST /submit HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n", &rp, sv);
    CHECK(read_request(&rp, &req) == 501);
    close(sv[1]);

    // body 길이 부족(EOF) -> 400
    feed_and_close("POST /submit HTTP/1.1\r\nHost: localhost\r\nContent-Length: 10\r\n\r\nhi", &rp, sv);
    CHECK(read_request(&rp, &req) == 400);
    close(sv[1]);

    // 빈 연결 EOF -> -1
    feed_and_close("", &rp, sv);
    CHECK(read_request(&rp, &req) == -1);
    close(sv[1]);

    // HTTP/1.1에서 Host 헤더가 둘 이상 -> 400 (RFC 7230 5.4). 대소문자만 다른 경우도 같은 헤더
    feed_and_close("GET / HTTP/1.1\r\nHost: a.example\r\nHost: b.example\r\n\r\n", &rp, sv);
    CHECK(read_request(&rp, &req) == 400);
    close(sv[1]);
    feed_and_close("GET / HTTP/1.1\r\nHost: a.example\r\nhOsT: a.example\r\n\r\n", &rp, sv);
    CHECK(read_request(&rp, &req) == 400);
    close(sv[1]);
    feed_and_close("GET / HTTP/1.0\r\nHost: a\r\nHost: b\r\n\r\n", &rp, sv);   // 1.0도 중복은 모호하므로 거부
    CHECK(read_request(&rp, &req) == 400);
    close(sv[1]);

    // Content-Length가 MAX_BODY_LEN 초과 -> 413 (본문을 읽기 전에 거부, 메모리 할당 없음)
    char big_cl[128];
    snprintf(big_cl, sizeof(big_cl), "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n", MAX_BODY_LEN + 1);
    feed_and_close(big_cl, &rp, sv);
    CHECK(read_request(&rp, &req) == 413);
    close(sv[1]);
    // 정확히 MAX_BODY_LEN은 허용 범위 → 본문이 모자라 EOF이므로 400 (413이 아님)
    snprintf(big_cl, sizeof(big_cl), "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\nab", MAX_BODY_LEN);
    feed_and_close(big_cl, &rp, sv);
    CHECK(read_request(&rp, &req) == 400);
    close(sv[1]);
    // unsigned long long도 넘는 값 -> 400 (오버플로를 413으로 오인하지 않음)
    feed_and_close("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 99999999999999999999999\r\n\r\n", &rp, sv);
    CHECK(read_request(&rp, &req) == 400);
    close(sv[1]);

    // Transfer-Encoding은 chunked가 아니어도(gzip 등) 지원하지 않음 -> 501 (Content-Length와 섞인 경계 해석 방지)
    feed_and_close("POST /submit HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip\r\nContent-Length: 2\r\n\r\nhi", &rp, sv);
    CHECK(read_request(&rp, &req) == 501);
    close(sv[1]);
}

// 요청 라인이 MAX_LINE(8192)보다 길어 잘리는 경우 -> 400이 아니라 414
static void test_request_line_longer_than_buffer(void) {
    size_t uri_len = MAX_LINE + 100;
    char *raw = malloc(uri_len + 64);
    CHECK(raw != NULL);
    if (raw == NULL) return;
    memcpy(raw, "GET /", 5);
    memset(raw + 5, 'a', uri_len);
    strcpy(raw + 5 + uri_len, " HTTP/1.1\r\nHost: x\r\n\r\n");

    // 소켓 버퍼(수백 KB)에 다 들어가는 크기라 먼저 쓰고 읽는다
    rio_t rp;
    int sv[2];
    http_request req;
    feed_and_close(raw, &rp, sv);
    CHECK(read_request(&rp, &req) == 414);
    close(sv[1]);
    free(raw);
}

int main(void) {
    test_valid_get_and_query();
    test_forward_proxy_abs_uri();
    test_request_line_errors();
    test_headers_and_connection();
    test_read_request_post_and_errors();
    test_request_line_longer_than_buffer();
    return TEST_RESULT();
}