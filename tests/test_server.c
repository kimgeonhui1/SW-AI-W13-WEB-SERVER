// [test_server.c] handle_connection 단위 테스트 — socketpair 한쪽을 "클라이언트", 다른 쪽을 connfd로 넘겨 검증
// 1단계 임시 동작 기준: 요청 헤더를 빈 줄까지 읽고 고정 응답(200, "Hello, W13!") 후 연결을 닫는다
// 3단계에서 실제 요청 처리로 바뀌면 기대 응답만 바꾸면 된다
#include "test.h"
#include "http_request.h"   // MAX_LINE
#include "server.h"
#include "rio.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define EXPECTED_STATUS "HTTP/1.1 200 OK\r\n"
#define EXPECTED_BODY   "Hello, W13!\r\n"

static void make_pair(int sv[2]) {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        perror("socketpair");
        exit(1);
    }
}

// 클라이언트 쪽에서 응답을 EOF(또는 에러)까지 읽어 문자열로. 받은 바이트 수 반환
// (서버가 안 읽은 데이터를 남기고 닫으면 마지막에 ECONNRESET이 올 수 있어, 그 전까지 받은 양을 센다)
static ssize_t read_all(int fd, char *buf, size_t cap) {
    size_t total = 0;
    while (total < cap - 1) {
        ssize_t r = read(fd, buf + total, cap - 1 - total);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        total += (size_t)r;
    }
    buf[total] = '\0';
    return (ssize_t)total;
}

// 요청을 다 보낸 뒤 handle_connection 실행 → 응답 확인
static void run_with_request(const char *req, size_t len, char *resp, size_t cap, ssize_t *resp_len) {
    int sv[2];
    make_pair(sv);
    if (len > 0) rio_writen(sv[0], req, len);
    shutdown(sv[0], SHUT_WR);              // 클라이언트가 더 보낼 게 없음 (FIN)
    handle_connection(sv[1]);              // 끝나면 sv[1]을 닫아야 함
    *resp_len = read_all(sv[0], resp, cap);
    close(sv[0]);
}

// 정상 GET → 상태 라인, 헤더, 빈 줄, 본문
static void test_normal_request(void) {
    char resp[512];
    ssize_t n;
    const char *req = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    run_with_request(req, strlen(req), resp, sizeof(resp), &n);
    CHECK(n > 0);
    CHECK(strncmp(resp, EXPECTED_STATUS, strlen(EXPECTED_STATUS)) == 0);
    CHECK(strstr(resp, "Content-Length: 13\r\n") != NULL);
    CHECK(strstr(resp, "Connection: close\r\n") != NULL);
    CHECK(strstr(resp, "\r\n\r\n" EXPECTED_BODY) != NULL);   // 헤더 끝 빈 줄 바로 뒤에 본문
}

// Content-Length 값이 실제 본문 길이와 일치해야 함 (keep-alive 클라이언트가 응답 끝을 판단하는 근거)
static void test_content_length_matches_body(void) {
    char resp[512];
    ssize_t n;
    const char *req = "GET / HTTP/1.1\r\n\r\n";
    run_with_request(req, strlen(req), resp, sizeof(resp), &n);
    char *body = strstr(resp, "\r\n\r\n");
    CHECK(body != NULL);
    if (body) CHECK(strlen(body + 4) == 13);
}

// 줄 끝이 "\n"만 있는 요청(비표준)도 빈 줄을 인식해 응답
static void test_lf_only_request(void) {
    char resp[512];
    ssize_t n;
    const char *req = "GET / HTTP/1.0\nHost: x\n\n";
    run_with_request(req, strlen(req), resp, sizeof(resp), &n);
    CHECK(n > 0 && strncmp(resp, EXPECTED_STATUS, strlen(EXPECTED_STATUS)) == 0);
}

// 헤더 끝(빈 줄) 전에 클라이언트가 끊으면 → 응답 없이 연결만 닫음
static void test_incomplete_request_no_response(void) {
    char resp[512];
    ssize_t n;
    const char *req = "GET / HTTP/1.1\r\nHost: x\r\n";
    run_with_request(req, strlen(req), resp, sizeof(resp), &n);
    CHECK(n == 0);
}

// 아무것도 안 보내고 끊음 → 응답 없음
static void test_empty_connection(void) {
    char resp[64];
    ssize_t n;
    run_with_request("", 0, resp, sizeof(resp), &n);
    CHECK(n == 0);
}

// MAX_LINE(8192)보다 긴 헤더 줄 → 잘려서 여러 번 읽히지만 빈 줄을 만나면 응답 (1단계 임시 동작)
static void test_very_long_header_line(void) {
    size_t long_len = MAX_LINE * 2;
    size_t total = long_len + 64;
    char *req = malloc(total);
    char resp[512];
    ssize_t n;
    strcpy(req, "GET / HTTP/1.1\r\nX-Long: ");
    size_t off = strlen(req);
    memset(req + off, 'a', long_len);
    off += long_len;
    memcpy(req + off, "\r\n\r\n", 4);
    off += 4;

    int sv[2];
    make_pair(sv);
    // 소켓 버퍼보다 클 수 있으므로 별도 스레드 없이 순서대로: 먼저 보내고 처리
    rio_writen(sv[0], req, off);
    shutdown(sv[0], SHUT_WR);
    handle_connection(sv[1]);
    n = read_all(sv[0], resp, sizeof(resp));
    close(sv[0]);
    CHECK(n > 0 && strncmp(resp, EXPECTED_STATUS, strlen(EXPECTED_STATUS)) == 0);
    free(req);
}

// 빈 줄 뒤에 온 데이터(body, 다음 요청)는 1단계에선 무시하고 응답 한 번만
static void test_extra_data_after_headers(void) {
    char resp[512];
    ssize_t n;
    const char *req = "POST / HTTP/1.1\r\nContent-Length: 5\r\n\r\nhelloGET / HTTP/1.1\r\n\r\n";
    run_with_request(req, strlen(req), resp, sizeof(resp), &n);
    CHECK(n > 0);
    char *second = strstr(resp + 1, "HTTP/1.1 200");
    CHECK(second == NULL);                                   // 응답은 하나만
}

// handle_connection이 connfd를 반드시 닫는지 (fd 누수 방지)
static void test_closes_connfd(void) {
    int sv[2];
    make_pair(sv);
    rio_writen(sv[0], "GET / HTTP/1.1\r\n\r\n", 18);
    handle_connection(sv[1]);
    errno = 0;
    CHECK(fcntl(sv[1], F_GETFD) == -1 && errno == EBADF);   // 이미 닫힌 fd
    close(sv[0]);
}

// 응답을 받기 전에 클라이언트가 완전히 닫아도(쓰기 실패 EPIPE) 프로세스가 죽지 않고 반환
static void test_client_gone_before_response(void) {
    signal(SIGPIPE, SIG_IGN);                                // main.c와 같은 설정
    int sv[2];
    make_pair(sv);
    rio_writen(sv[0], "GET / HTTP/1.1\r\n\r\n", 18);
    close(sv[0]);                                            // 응답 받을 사람 없음
    handle_connection(sv[1]);
    CHECK(1);                                                // 여기 도달하면 통과
}

// 요청을 조각조각 천천히 보내도 합쳐서 처리
struct slow_arg { int fd; };
static void *slow_client(void *p) {
    struct slow_arg *a = p;
    const char *parts[] = { "GE", "T / HT", "TP/1.1\r", "\nHost: x\r\n", "\r", "\n" };
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        rio_writen(a->fd, parts[i], strlen(parts[i]));
        usleep(30 * 1000);
    }
    return NULL;
}

static void test_fragmented_request(void) {
    int sv[2];
    make_pair(sv);
    struct slow_arg a = { .fd = sv[0] };
    pthread_t t;
    if (pthread_create(&t, NULL, slow_client, &a) != 0) {
        CHECK(!"pthread_create failed");
        return;
    }
    handle_connection(sv[1]);
    pthread_join(t, NULL);
    char resp[512];
    ssize_t n = read_all(sv[0], resp, sizeof(resp));
    close(sv[0]);
    CHECK(n > 0 && strncmp(resp, EXPECTED_STATUS, strlen(EXPECTED_STATUS)) == 0);
}

// 연결만 하고 아무것도 안 보내는 클라이언트 → KEEPALIVE_TIMEOUT_SEC 뒤 응답 없이 반환 (무한 대기 금지)
static void test_idle_client_times_out(void) {
    int sv[2];
    make_pair(sv);
    time_t start = time(NULL);
    handle_connection(sv[1]);
    time_t elapsed = time(NULL) - start;
    CHECK(elapsed >= KEEPALIVE_TIMEOUT_SEC - 1 && elapsed <= KEEPALIVE_TIMEOUT_SEC + 2);
    char resp[64];
    CHECK(read_all(sv[0], resp, sizeof(resp)) == 0);         // 응답 없이 닫힘
    close(sv[0]);
}

int main(void) {
    test_normal_request();
    test_content_length_matches_body();
    test_lf_only_request();
    test_incomplete_request_no_response();
    test_empty_connection();
    test_very_long_header_line();
    test_extra_data_after_headers();
    test_closes_connfd();
    test_client_gone_before_response();
    test_fragmented_request();
    fprintf(stderr, "  (유휴 클라이언트 타임아웃 테스트: 약 %d초 걸림)\n", KEEPALIVE_TIMEOUT_SEC);
    test_idle_client_times_out();
    return TEST_RESULT();
}
