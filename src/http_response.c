// [http_response.c] HTTP 응답 생성 구현
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "http_response.h"
#include "rio.h"

#include <stdio.h>
#include <string.h>

const char *status_text(int status) {
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default:  return "Unknown";
    }
}

int send_headers(int fd, int status, const char *content_type, size_t content_length, int keep_alive) {
    if (fd < 0) return -1;

    const char *reason = status_text(status);
    const char *ctype = (content_type != NULL && content_type[0] != '\0')
                            ? content_type
                            : "application/octet-stream";
    const char *conn_val = keep_alive ? "keep-alive" : "close";

    char header_buf[2048];
    int n = snprintf(
        header_buf,
        sizeof(header_buf),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: %s\r\n"
        "\r\n",
        status,
        reason,
        ctype,
        content_length,
        conn_val
    );

    if (n < 0 || (size_t)n >= sizeof(header_buf)) {
        return -1;
    }

    size_t total = (size_t)n;
    if (rio_writen(fd, header_buf, total) != (ssize_t)total) {
        return -1;
    }

    return 0;
}

int send_response(int fd, int status, const char *content_type, const void *body, size_t len, int keep_alive) {
    if (fd < 0) return -1;
    if (len > 0 && body == NULL) return -1;

    if (send_headers(fd, status, content_type, len, keep_alive) != 0) {
        return -1;
    }

    if (len > 0) {
        if (rio_writen(fd, body, len) != (ssize_t)len) {
            return -1;
        }
    }

    return 0;
}

int send_error(int fd, int status, int keep_alive) {
    if (fd < 0) return -1;

    const char *reason = status_text(status);
    char html_buf[512];
    int n = snprintf(
        html_buf,
        sizeof(html_buf),
        "<!DOCTYPE html>\n"
        "<html><head><meta charset=\"utf-8\"><title>%d %s</title></head>\n"
        "<body><h1>%d %s</h1></body></html>\n",
        status,
        reason,
        status,
        reason
    );

    if (n < 0 || (size_t)n >= sizeof(html_buf)) {
        return -1;
    }

    return send_response(fd, status, "text/html; charset=utf-8", html_buf, (size_t)n, keep_alive);
}