// [test_static_response.c] MIME / HTTP 응답 / 정적 파일 서빙 단위 테스트
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "http_request.h"
#include "http_response.h"
#include "mime.h"
#include "static_file.h"
#include "test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_file(const char *path, const char *content) {
    FILE *fp = fopen(path, "wb");
    CHECK(fp != NULL);
    size_t len = strlen(content);
    if (len > 0) {
        CHECK(fwrite(content, 1, len, fp) == len);
    }
    fclose(fp);
}

// 픽스처 구조 (테스트마다 새 임시 폴더 → 동시 실행해도 서로 간섭하지 않음)
//   base/secret_outside.txt      ← 루트 밖 파일
//   base/www/index.html          ← 정적 파일 루트
//   base/www/sub/style.css
//   base/www/escape_link         → ../secret_outside.txt (심볼릭 링크로 루트 탈출 시도)
static void setup_fixture(char *base, size_t base_cap, char *root, size_t root_cap) {
    snprintf(base, base_cap, "/tmp/test_static_XXXXXX");
    CHECK(mkdtemp(base) != NULL);
    snprintf(root, root_cap, "%s/www", base);
    CHECK(mkdir(root, 0755) == 0);

    char path[512];
    snprintf(path, sizeof(path), "%s/sub", root);
    CHECK(mkdir(path, 0755) == 0);
    snprintf(path, sizeof(path), "%s/index.html", root);
    write_file(path, "<h1>Home</h1>");
    snprintf(path, sizeof(path), "%s/sub/style.css", root);
    write_file(path, "body{color:#111;}");
    snprintf(path, sizeof(path), "%s/secret_outside.txt", base);
    write_file(path, "TOP_SECRET");

    char link[512];
    snprintf(link, sizeof(link), "%s/escape_link", root);
    CHECK(symlink(path, link) == 0);
}

static void cleanup_fixture(const char *base) {
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", base);
    CHECK(system(cmd) == 0);
}

// 오류를 반환해야 하는 요청: 반환 코드 확인 + 소켓으로 아무것도 보내지 않았는지 확인
// (fd 1(stdout)을 넘기면 버그로 200이 나올 때 테스트 출력에 HTTP 응답이 섞인다)
static int serve_expect_error(const char *root, const char *method, const char *path) {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    http_request req = { 0 };
    snprintf(req.method, sizeof(req.method), "%s", method);
    snprintf(req.path, sizeof(req.path), "%s", path);
    int rc = serve_static(sv[0], root, &req);
    close(sv[0]);
    char buf[16];
    CHECK(read(sv[1], buf, sizeof(buf)) == 0);              // 오류 응답은 호출한 쪽이 보냄 → 여기선 전송 없음
    close(sv[1]);
    return rc;
}

static void test_response_helpers(void) {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    CHECK(send_response(sv[0], 200, "text/plain; charset=utf-8", "hello", 5, 1) == 0);
    close(sv[0]);

    char buf[1024] = { 0 };
    ssize_t n = read(sv[1], buf, sizeof(buf) - 1);
    close(sv[1]);
    CHECK(n > 0);
    buf[n] = '\0';

    CHECK(strstr(buf, "HTTP/1.1 200 OK\r\n") != NULL);
    CHECK(strstr(buf, "Content-Type: text/plain; charset=utf-8\r\n") != NULL);
    CHECK(strstr(buf, "Content-Length: 5\r\n") != NULL);
    CHECK(strstr(buf, "Connection: keep-alive\r\n") != NULL);
    CHECK(strstr(buf, "\r\n\r\nhello") != NULL);
}

static void test_serve_static_cases(void) {
    char base[256], root[300];
    setup_fixture(base, sizeof(base), root, sizeof(root));

    // 1. GET / -> index.html
    {
        int sv[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

        http_request req = { 0 };
        snprintf(req.method, sizeof(req.method), "GET");
        snprintf(req.path, sizeof(req.path), "/");
        req.keep_alive = 1;

        CHECK(serve_static(sv[0], root, &req) == 0);
        close(sv[0]);

        char buf[1024] = { 0 };
        ssize_t n = read(sv[1], buf, sizeof(buf) - 1);
        close(sv[1]);
        CHECK(n > 0);
        CHECK(strstr(buf, "HTTP/1.1 200 OK\r\n") != NULL);
        CHECK(strstr(buf, "Content-Type: text/html; charset=utf-8\r\n") != NULL);
        CHECK(strstr(buf, "Content-Length: 13\r\n") != NULL);
        CHECK(strstr(buf, "\r\n\r\n<h1>Home</h1>") != NULL);
    }

    // 2. HEAD /sub/style.css -> 헤더만 전송하고 본문 없음
    {
        int sv[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

        http_request req = { 0 };
        snprintf(req.method, sizeof(req.method), "HEAD");
        snprintf(req.path, sizeof(req.path), "/sub/style.css");
        req.keep_alive = 0;

        CHECK(serve_static(sv[0], root, &req) == 0);
        close(sv[0]);

        char buf[1024] = { 0 };
        ssize_t n = read(sv[1], buf, sizeof(buf) - 1);
        close(sv[1]);
        CHECK(n > 0);
        CHECK(strstr(buf, "HTTP/1.1 200 OK\r\n") != NULL);
        CHECK(strstr(buf, "Content-Type: text/css; charset=utf-8\r\n") != NULL);
        CHECK(strstr(buf, "Connection: close\r\n") != NULL);
        char *body_sep = strstr(buf, "\r\n\r\n");
        CHECK(body_sep != NULL);
        CHECK(strlen(body_sep + 4) == 0);
    }

    // 3. 경로 탐색(../, 여러 단계) 및 심볼릭 링크 탈출 -> 403, 아무것도 전송 안 함
    CHECK(serve_expect_error(root, "GET", "/../secret_outside.txt") == 403);
    CHECK(serve_expect_error(root, "GET", "/sub/../../secret_outside.txt") == 403);
    CHECK(serve_expect_error(root, "GET", "/escape_link") == 403);

    // 4. 존재하지 않는 파일·디렉터리 -> 404
    CHECK(serve_expect_error(root, "GET", "/does-not-exist.html") == 404);
    CHECK(serve_expect_error(root, "GET", "/nope/") == 404);
    CHECK(serve_expect_error(root, "GET", "/index.html/x") == 404);   // 파일 뒤에 경로를 붙임(ENOTDIR)

    // 5. 디렉터리를 '/' 없이 요청 -> 일반 파일이 아니므로 403
    CHECK(serve_expect_error(root, "GET", "/sub") == 403);

    // 6. 허용되지 않은 메서드 -> 405
    CHECK(serve_expect_error(root, "POST", "/index.html") == 405);

    // 7. '/'로 시작하지 않는 경로 -> 400
    CHECK(serve_expect_error(root, "GET", "index.html") == 400);

    // 8. 루트 폴더 자체가 없음 -> 500
    char missing_root[400];
    snprintf(missing_root, sizeof(missing_root), "%s/no-such-root", base);
    CHECK(serve_expect_error(missing_root, "GET", "/") == 500);

    cleanup_fixture(base);
}

int main(void) {
    test_response_helpers();
    test_serve_static_cases();
    return TEST_RESULT();
}