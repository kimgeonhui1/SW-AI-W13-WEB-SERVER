// [http_response.c] HTTP 응답 생성 구현
#include "http_response.h"

const char *status_text(int status) {
    (void)status;
    return "Unknown"; // TODO: 200, 400, 403, 404, 405, 411, 413, 414, 431, 500, 501, 502, 503, 504, 505
}

int send_headers(int fd, int status, const char *content_type, size_t content_length, int keep_alive) {
    (void)fd; (void)status; (void)content_type; (void)content_length; (void)keep_alive;
    // TODO: "HTTP/1.1 %d %s\r\n" + Content-Type + Content-Length + Connection(keep-alive/close) + "\r\n"
    //       → snprintf로 만들고 rio_writen으로 송신
    return -1;
}

int send_response(int fd, int status, const char *content_type, const void *body, size_t len, int keep_alive) {
    (void)fd; (void)status; (void)content_type; (void)body; (void)len; (void)keep_alive;
    return -1; // TODO: send_headers 후 body를 rio_writen
}

int send_error(int fd, int status, int keep_alive) {
    (void)fd; (void)status; (void)keep_alive;
    return -1; // TODO: "<h1>404 Not Found</h1>" 같은 본문을 만들어 send_response (text/html)
}
