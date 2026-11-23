// [http_request.c] HTTP 요청 파서 구현
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "http_request.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void strip_crlf(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[--len] = '\0';
    }
}

static int is_tchar(unsigned char c) {
    if (isalnum(c)) return 1;
    switch (c) {
    case '!': case '#': case '$': case '%': case '&':
    case '\'': case '*': case '+': case '-': case '.':
    case '^': case '_': case '`': case '|': case '~':
        return 1;
    default:
        return 0;
    }
}

static int is_valid_token(const char *s, size_t len) {
    if (len == 0) return 0;
    for (size_t i = 0; i < len; i++) {
        if (!is_tchar((unsigned char)s[i])) return 0;
    }
    return 1;
}

static int is_valid_uri_char(unsigned char c) {
    // 제어문자(0x00~0x1F, 0x7F) 및 공백 금지
    return c > 0x20 && c != 0x7F;
}

static int has_valid_abs_http_host(const char *uri) {
    if (strlen(uri) >= 7 && strncasecmp(uri, "http://", 7) == 0) {
        const char *after = uri + 7;
        // "http://", "http:///path", "http://?a=1" 등 빈 호스트 차단
        if (*after == '\0' || *after == '/' || *after == '?') return 0;
    }
    return 1;
}

static void split_target_path_query(const char *uri,
                                    char *path_out, size_t path_cap,
                                    char *query_out, size_t query_cap) {
    path_out[0] = '\0';
    query_out[0] = '\0';

    const char *target = uri;

    // Forward Proxy absolute-form: "http://" (대소문자 무시)
    if (strlen(uri) > 7 && strncasecmp(uri, "http://", 7) == 0) {
        const char *after_scheme = uri + 7;
        const char *slash = strchr(after_scheme, '/');
        const char *q = strchr(after_scheme, '?');
        if (slash != NULL && (q == NULL || slash < q)) {
            target = slash;
        } else {
            // "http://example.com" 또는 "http://example.com?a=1" 형태
            snprintf(path_out, path_cap, "/");
            if (q != NULL) {
                snprintf(query_out, query_cap, "%s", q + 1);
            }
            return;
        }
    }

    const char *q = strchr(target, '?');
    if (q != NULL) {
        size_t plen = (size_t)(q - target);
        if (plen >= path_cap) plen = path_cap - 1;
        memcpy(path_out, target, plen);
        path_out[plen] = '\0';
        snprintf(query_out, query_cap, "%s", q + 1);
    } else {
        snprintf(path_out, path_cap, "%s", target);
    }
}

static int check_http_version(const char *ver, char *out, size_t out_cap) {
    if (strcmp(ver, "HTTP/1.1") == 0 || strcmp(ver, "HTTP/1.0") == 0) {
        snprintf(out, out_cap, "%s", ver);
        return 0;
    }
    // HTTP/<major>.<minor> 형태면 지원하지 않는 버전 -> 505
    if (strncmp(ver, "HTTP/", 5) == 0) {
        const char *p = ver + 5;
        int maj_digits = 0, min_digits = 0;
        while (*p && isdigit((unsigned char)*p)) { maj_digits++; p++; }
        if (*p == '.') {
            p++;
            while (*p && isdigit((unsigned char)*p)) { min_digits++; p++; }
            if (*p == '\0' && maj_digits > 0 && min_digits > 0) {
                return 505;
            }
        }
    }
    return 400;
}

int parse_request_line(const char *line, http_request *req) {
    if (line == NULL || req == NULL) return 400;

    char buf[MAX_LINE];
    size_t raw_len = strlen(line);
    if (raw_len == 0 || raw_len >= sizeof(buf)) return 400;
    memcpy(buf, line, raw_len + 1);
    strip_crlf(buf);
    if (buf[0] == '\0') return 400;

    // 공백(' ') 기준으로 정확히 3개 파트 분리 (연속 공백/선후행 공백 불허)
    char *sp1 = strchr(buf, ' ');
    if (sp1 == NULL || sp1 == buf) return 400;
    *sp1 = '\0';

    char *uri_start = sp1 + 1;
    if (*uri_start == ' ' || *uri_start == '\0') return 400;

    char *sp2 = strchr(uri_start, ' ');
    if (sp2 == NULL || sp2 == uri_start) return 400;
    *sp2 = '\0';

    char *ver_start = sp2 + 1;
    if (*ver_start == '\0' || strchr(ver_start, ' ') != NULL) return 400;

    size_t method_len = strlen(buf);
    if (!is_valid_token(buf, method_len)) return 400;
    if (method_len >= sizeof(req->method)) return 501;
    if (strcmp(buf, "GET") != 0 && strcmp(buf, "POST") != 0 && strcmp(buf, "HEAD") != 0) {
        return 501;
    }

    size_t uri_len = strlen(uri_start);
    if (uri_len >= MAX_URI_LEN) return 414;
    for (size_t i = 0; i < uri_len; i++) {
        if (!is_valid_uri_char((unsigned char)uri_start[i])) return 400;
    }
    if (!has_valid_abs_http_host(uri_start)) return 400;

    int ver_status = check_http_version(ver_start, req->version, sizeof(req->version));
    if (ver_status != 0) return ver_status;

    memcpy(req->method, buf, method_len);
    req->method[method_len] = '\0';

    snprintf(req->uri, sizeof(req->uri), "%s", uri_start);
    split_target_path_query(req->uri,
                            req->path, sizeof(req->path),
                            req->query, sizeof(req->query));

    req->keep_alive = (strcmp(req->version, "HTTP/1.1") == 0) ? 1 : 0;
    return 0;
}

static int parse_content_length_val(const char *val, size_t *out_len) {
    if (val == NULL || *val == '\0') return 400;
    for (const char *p = val; *p != '\0'; p++) {
        if (!isdigit((unsigned char)*p)) return 400;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(val, &end, 10);
    if (errno == ERANGE || end == val || *end != '\0') return 400;
    if (parsed > (unsigned long long)MAX_BODY_LEN) return 413;
    *out_len = (size_t)parsed;
    return 0;
}

// 콤마로 구분된 값 목록(val)에 token이 있는지 (앞뒤 공백 무시, 대소문자 무시)
static int has_token(const char *val, const char *token) {
    char tmp[MAX_HEADER_VALUE];
    snprintf(tmp, sizeof(tmp), "%s", val);

    char *tok = tmp;
    while (tok != NULL && *tok != '\0') {
        char *comma = strchr(tok, ',');
        if (comma != NULL) *comma = '\0';

        while (*tok == ' ' || *tok == '\t') tok++;
        char *end = tok + strlen(tok);
        while (end > tok && (end[-1] == ' ' || end[-1] == '\t')) end--;
        *end = '\0';

        if (strcasecmp(tok, token) == 0) return 1;
        tok = (comma != NULL) ? (comma + 1) : NULL;
    }
    return 0;
}

// Connection 헤더 반영. close가 어디서든(같은 줄·앞선 줄) 한 번이라도 나오면 닫는다 —
// 뒤에 온 keep-alive가 close를 덮어쓰면 클라이언트는 닫으려는데 서버는 기다리는 상태가 된다
static void apply_connection_header(http_request *req, const char *val) {
    if (has_token(val, "close")) {
        req->keep_alive = 0;
        return;
    }
    if (!has_token(val, "keep-alive")) return;
    for (int i = 0; i < req->header_count; i++) {          // 이미 저장된 Connection 헤더에 close가 있었는지
        if (strcasecmp(req->headers[i].name, "Connection") == 0 && has_token(req->headers[i].value, "close"))
            return;
    }
    req->keep_alive = 1;
}

int parse_header_line(const char *line, http_request *req) {
    if (line == NULL || req == NULL) return 400;

    char buf[MAX_LINE];
    size_t raw_len = strlen(line);
    if (raw_len == 0 || raw_len >= sizeof(buf)) return 431;
    memcpy(buf, line, raw_len + 1);
    strip_crlf(buf);
    if (buf[0] == '\0') return 0;

    // obs-fold(공백/탭으로 시작하는 줄) 또는 ':' 앞 공백은 불허
    if (buf[0] == ' ' || buf[0] == '\t') return 400;

    char *colon = strchr(buf, ':');
    if (colon == NULL) return 400;
    *colon = '\0';

    size_t name_len = strlen(buf);
    if (!is_valid_token(buf, name_len)) return 400;
    if (name_len >= MAX_HEADER_NAME) return 431;

    if (req->header_count >= MAX_HEADERS) return 431;

    const char *vstart = colon + 1;
    while (*vstart == ' ' || *vstart == '\t') vstart++;

    size_t vlen = strlen(vstart);
    while (vlen > 0 && (vstart[vlen - 1] == ' ' || vstart[vlen - 1] == '\t')) {
        vlen--;
    }
    if (vlen >= MAX_HEADER_VALUE) return 431;

    // Host가 둘 이상이면 어느 호스트로 보낸 요청인지 모호 → 400 (RFC 7230 5.4). Content-Length 중복도 아래에서 400
    if (strcasecmp(buf, "Host") == 0 && get_header(req, "Host") != NULL) return 400;

    const char *prev_cl = get_header(req, "Content-Length");

    size_t parsed_cl = 0;
    int is_cl = (strcasecmp(buf, "Content-Length") == 0);
    if (is_cl) {
        if (prev_cl != NULL) return 400;
        // h->value에 들어갈 공백 제거 문자열 기준으로 검증
        char val_tmp[MAX_HEADER_VALUE];
        memcpy(val_tmp, vstart, vlen);
        val_tmp[vlen] = '\0';
        int rc = parse_content_length_val(val_tmp, &parsed_cl);
        if (rc != 0) return rc;
    }

    http_header *h = &req->headers[req->header_count];
    memcpy(h->name, buf, name_len);
    h->name[name_len] = '\0';
    memcpy(h->value, vstart, vlen);
    h->value[vlen] = '\0';
    req->header_count++;

    if (is_cl) {
        req->content_length = parsed_cl;
    } else if (strcasecmp(h->name, "Connection") == 0) {
        apply_connection_header(req, h->value);
    }

    return 0;
}

// 요청 하나 읽기 (실제 파싱). 시간 제한 설정·해제와 408 변환은 아래 read_request가 맡는다
static int read_request_inner(rio_t *rp, http_request *req) {
    memset(req, 0, sizeof(*req));

    char line[MAX_LINE];
    ssize_t n = rio_readlineb(rp, line, sizeof(line));
    if (n < 0) return -1;
    if (n == 0) return -1; // 정상 EOF(요청 시작 전 연결 종료)
    // 시간 제한은 요청 라인의 첫 줄을 받은 뒤부터 잰다 (그 전의 대기는 keep-alive 유휴 시간 → 수신 타임아웃이 담당)
    rp->deadline = rio_now() + REQUEST_TIMEOUT_SEC;
    // 요청 라인이 버퍼보다 길어 잘림('\n' 없이 가득 참) → 잘린 줄은 버전이 빠져 400으로 오판되므로 414로 거부
    if (line[n - 1] != '\n' && (size_t)n == sizeof(line) - 1) return 414;

    int rc = parse_request_line(line, req);
    if (rc != 0) {
        free_request(req);
        return rc;
    }

    while (1) {
        n = rio_readlineb(rp, line, sizeof(line));
        if (n < 0) {
            free_request(req);
            return -1;
        }
        if (n == 0) {
            free_request(req);
            return 400;
        }
        if (strcmp(line, "\r\n") == 0 || strcmp(line, "\n") == 0) {
            break;
        }
        if (line[n - 1] != '\n' && (size_t)n == sizeof(line) - 1) {
            free_request(req);
            return 431;
        }

        rc = parse_header_line(line, req);
        if (rc != 0) {
            free_request(req);
            return rc;
        }
    }

    if (strcmp(req->version, "HTTP/1.1") == 0 && get_header(req, "Host") == NULL) {
        free_request(req);
        return 400;
    }

    // Transfer-Encoding은 종류(chunked, gzip ...)와 상관없이 지원하지 않는다.
    // chunked만 막고 나머지를 통과시키면 Content-Length와 함께 왔을 때 본문 경계를
    // 프록시·백엔드가 서로 다르게 해석할 수 있다(request smuggling) → 헤더가 있으면 전부 501
    if (get_header(req, "Transfer-Encoding") != NULL) {
        free_request(req);
        return 501;
    }

    if (strcmp(req->method, "POST") == 0 && get_header(req, "Content-Length") == NULL) {
        free_request(req);
        return 411;
    }

    // content_length == 0인 경우 별도 할당 없이 req->body == NULL 상태 유지
    if (req->content_length > 0) {
        if (req->content_length > MAX_BODY_LEN) {
            free_request(req);
            return 413;
        }
        req->body = malloc(req->content_length + 1);
        if (req->body == NULL) {
            free_request(req);
            return 500;
        }

        ssize_t got = rio_readnb(rp, req->body, req->content_length);
        if (got < 0) {
            free_request(req);
            return -1;
        }
        if ((size_t)got < req->content_length) {   // 약속한 길이보다 일찍 EOF
            free_request(req);
            return 400;
        }
        req->body[req->content_length] = '\0';
    }

    return 0;
}

int read_request(rio_t *rp, http_request *req) {
    if (rp == NULL || req == NULL) return -1;
    rp->deadline = 0;
    int rc = read_request_inner(rp, req);
    // 요청을 받는 도중(deadline이 설정된 뒤)의 시간 초과 → 응답 없이 끊지 않고 408을 알려 준다
    //   ETIMEDOUT: 요청 전체 제한(10초) 초과 / EAGAIN: read 한 번이 수신 타임아웃(5초)에 걸림 (요청 라인만 보내고 멈춤)
    // 요청 시작 전(deadline 0) keep-alive 대기의 시간 초과는 정상 유휴라 그대로 -1 → 조용히 닫는다
    if (rc == -1 && rp->deadline > 0 &&
        (errno == ETIMEDOUT || errno == EAGAIN || errno == EWOULDBLOCK)) rc = 408;
    rp->deadline = 0;                      // keep-alive 다음 요청을 기다리는 동안엔 제한 없음
    return rc;
}

const char *get_header(const http_request *req, const char *name) {
    if (req == NULL || name == NULL) return NULL;
    for (int i = 0; i < req->header_count; i++) {
        if (strcasecmp(req->headers[i].name, name) == 0) {
            return req->headers[i].value;
        }
    }
    return NULL;
}

void free_request(http_request *req) {
    if (req == NULL) return;
    // read_request는 읽기 실패(-1) 뒤 이 함수를 부르고, 바깥에서 errno(ETIMEDOUT/EAGAIN)를 보고 408을 정한다.
    // free()가 errno를 보존한다는 보장은 최신 POSIX·glibc에만 있으므로 직접 보존한다
    int saved_errno = errno;
    free(req->body);
    errno = saved_errno;
    req->body = NULL;
    req->content_length = 0;
    req->header_count = 0;
}