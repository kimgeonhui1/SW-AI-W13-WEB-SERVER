// [proxy.c] 프록시 공통 중계 + Reverse Proxy 구현
#include "proxy.h"
#include "net.h"
#include "rio.h"

#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

// 한 구간(홉)에만 의미가 있는 헤더: 프록시는 이것을 다음 구간으로 넘기지 않고 자기 값으로 바꾼다 (RFC 9110 7.6.1)
static int is_hop_by_hop(const char *name) {
    static const char *const hop[] = { "Connection", "Keep-Alive", "Proxy-Connection", "TE", "Trailer", "Upgrade" };
    for (size_t i = 0; i < sizeof(hop) / sizeof(hop[0]); i++)
        if (strcasecmp(name, hop[i]) == 0) return 1;
    return 0;
}

// 늘어나는 버퍼에 이어 붙이기. 실패(상한 초과·메모리 부족) -1
typedef struct { char *p; size_t len, cap, max; } sbuf;
static int sb_append(sbuf *b, const char *s, size_t n) {
    if (b->len + n > b->max) return -1;
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap : 4096;
        while (nc < b->len + n) nc *= 2;
        char *np = realloc(b->p, nc);
        if (np == NULL) return -1;
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    return 0;
}
static int sb_puts(sbuf *b, const char *s) { return sb_append(b, s, strlen(s)); }

// 요청을 백엔드로: "메서드 target 버전" + 헤더(hop-by-hop 제외) + Connection: close + 빈 줄 + 본문
static int send_upstream_request(int upfd, const http_request *req, const char *target) {
    sbuf b = { NULL, 0, 0, 1024 * 1024 };
    int bad = 0;
    bad |= sb_puts(&b, req->method) | sb_puts(&b, " ") | sb_puts(&b, target) | sb_puts(&b, " ")
         | sb_puts(&b, req->version) | sb_puts(&b, "\r\n");
    for (int i = 0; i < req->header_count; i++) {
        if (is_hop_by_hop(req->headers[i].name)) continue;
        bad |= sb_puts(&b, req->headers[i].name) | sb_puts(&b, ": ") | sb_puts(&b, req->headers[i].value)
             | sb_puts(&b, "\r\n");
    }
    // 백엔드 연결은 요청 하나에만 쓴다: 백엔드가 응답 후 닫아 주므로 길이를 모르는 응답도 끝을 알 수 있다
    bad |= sb_puts(&b, "Connection: close\r\n\r\n");
    int rc = -1;
    if (!bad && rio_writen(upfd, b.p, b.len) == (ssize_t)b.len) {
        rc = 0;
        if (req->body != NULL && req->content_length > 0 &&
            rio_writen(upfd, req->body, req->content_length) != (ssize_t)req->content_length)
            rc = -1;
    }
    free(b.p);
    return rc;
}

// "Content-Length" 값: 숫자만. 실패 -1
static long long parse_length(const char *v) {
    if (*v == '\0') return -1;
    long long n = 0;
    for (; *v; v++) {
        if (*v < '0' || *v > '9') return -1;
        if (n > (long long)1e15) return -1;
        n = n * 10 + (*v - '0');
    }
    return n;
}

static int is_timeout(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT; }

int proxy_relay(int clientfd, int upstreamfd, const http_request *req, const char *target) {
    if (clientfd < 0 || upstreamfd < 0 || req == NULL || target == NULL) return 502;

    // 백엔드가 멈춰도 워커가 영원히 붙잡히지 않게: read·write 한 번의 대기 제한
    struct timeval tv = { .tv_sec = PROXY_TIMEOUT_SEC, .tv_usec = 0 };
    setsockopt(upstreamfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(upstreamfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int one = 1;   // 헤더와 본문을 따로 write하므로 Nagle을 끄지 않으면 지연 ACK와 겹쳐 ~40ms 멈춤 (server.c와 같은 이유)
    setsockopt(upstreamfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    if (send_upstream_request(upstreamfd, req, target) != 0) return is_timeout() ? 504 : 502;

    // ── 응답 헤더: 상태 줄 + 헤더를 모아 검사한 뒤 한 번에 보낸다 (아직 클라이언트에 보낸 게 없으므로 오류면 502/504 가능)
    rio_t rio;
    rio_readinitb(&rio, upstreamfd);
    rio.deadline = rio_now() + PROXY_TIMEOUT_SEC;          // 헤더를 1바이트씩 흘리는 백엔드도 전체 시간으로 제한
    char line[MAX_LINE];
    ssize_t n = rio_readlineb(&rio, line, sizeof(line));
    if (n < 0) return is_timeout() ? 504 : 502;
    if (n == 0) return 502;                                 // 응답 없이 닫음

    // 상태 줄 "HTTP/1.x NNN ..." 검사
    int status = 0;
    // "HTTP/1.x" + 공백 + 숫자 3개 + (공백 또는 줄 끝). "200abc"처럼 숫자 뒤에 글자가 붙으면 형식 오류
    if (n < 13 || strncmp(line, "HTTP/1.", 7) != 0 || (line[7] != '0' && line[7] != '1') ||
        line[n - 1] != '\n' || line[8] != ' ' ||
        line[9] < '0' || line[9] > '9' || line[10] < '0' || line[10] > '9' || line[11] < '0' || line[11] > '9' ||
        (line[12] != ' ' && line[12] != '\r' && line[12] != '\n'))
        return 502;
    status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0');
    if (status < 100 || status > 599) return 502;
    if (status < 200) return 502;                           // shortcut: 1xx 중간 응답(100 Continue 등) 미지원

    sbuf hdr = { NULL, 0, 0, PROXY_MAX_RESP_HEADER };
    int bad = sb_append(&hdr, line, (size_t)n);
    long long clen = -1;
    int chunked = 0;
    for (;;) {
        n = rio_readlineb(&rio, line, sizeof(line));
        if (n <= 0) { free(hdr.p); return (n < 0 && is_timeout()) ? 504 : 502; }
        if (line[n - 1] != '\n') { free(hdr.p); return 502; }   // 한 줄이 너무 김
        if (strcmp(line, "\r\n") == 0 || strcmp(line, "\n") == 0) break;
        char *colon = strchr(line, ':');
        if (colon == NULL) { free(hdr.p); return 502; }
        *colon = '\0';
        char *val = colon + 1;
        while (*val == ' ' || *val == '\t') val++;
        char *end = val + strlen(val);
        while (end > val && (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
        // 이름에 공백·제어 문자, 값에 제어 문자(줄 중간 CR 등)가 있으면 클라이언트 쪽 응답 헤더가 깨진다 → 502
        int ctl = (line[0] == '\0');
        for (const char *c = line; *c; c++) ctl |= ((unsigned char)*c <= 0x20 || *c == 0x7f);
        for (const char *c = val; *c; c++) ctl |= (((unsigned char)*c < 0x20 && *c != '\t') || *c == 0x7f);
        if (ctl) { free(hdr.p); return 502; }
        if (is_hop_by_hop(line)) continue;
        if (strcasecmp(line, "Content-Length") == 0) {
            long long v = parse_length(val);
            if (v < 0 || (clen >= 0 && v != clen)) { free(hdr.p); return 502; }   // 값이 다른 중복은 응답 경계를 믿을 수 없음
            if (clen >= 0) continue;                         // 같은 값 중복은 하나로 합쳐 전달 (RFC 9112 6.3)
            clen = v;
        } else if (strcasecmp(line, "Transfer-Encoding") == 0) {
            chunked = 1;
        }
        bad |= sb_puts(&hdr, line) | sb_puts(&hdr, ": ") | sb_puts(&hdr, val) | sb_puts(&hdr, "\r\n");
    }
    if (bad) { free(hdr.p); return 502; }                   // 헤더가 너무 큼
    // Transfer-Encoding과 Content-Length가 함께 오면 어느 쪽이 경계인지 모호 → 응답 분할 공격일 수 있어 거부 (RFC 9112 6.1)
    if (chunked && clen >= 0) { free(hdr.p); return 502; }

    // 본문 길이 결정 (RFC 9112 6.3): HEAD·204·304는 본문 없음, Transfer-Encoding이면 끝까지, 아니면 Content-Length
    int is_head = (strcmp(req->method, "HEAD") == 0);
    long long body_len;                                      // -1 = 백엔드가 닫을 때까지
    if (is_head || status == 204 || status == 304) body_len = 0;
    else if (chunked) body_len = -1;                         // chunked는 해석하지 않고 그대로 흘려보내고, 끝은 닫힘으로 알림
    else body_len = clen;
    int keep = req->keep_alive && body_len >= 0;
    if (sb_puts(&hdr, keep ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n") != 0) {
        free(hdr.p);
        return 502;
    }
    ssize_t w = rio_writen(clientfd, hdr.p, hdr.len);
    int sent_ok = (w == (ssize_t)hdr.len);
    free(hdr.p);
    if (!sent_ok) return -1;

    // ── 본문: 이제부터는 이미 200 등을 보냈으므로 실패하면 연결을 끊는 것밖에 할 수 없다 (-1)
    rio.deadline = 0;                                        // 큰 본문은 오래 걸릴 수 있으니 read 한 번 제한(SO_RCVTIMEO)만
    char buf[16384];
    long long left = body_len;
    while (left != 0) {
        size_t want = sizeof(buf);
        if (left > 0 && (long long)want > left) want = (size_t)left;
        ssize_t r = rio_readnb(&rio, buf, want);
        if (r < 0) return -1;                                // 백엔드 시간 초과·에러
        if (r == 0) break;                                   // 백엔드가 닫음
        if (rio_writen(clientfd, buf, (size_t)r) != r) return -1;
        if (left > 0) left -= r;
        if ((size_t)r < want) break;                         // rio_readnb가 덜 읽음 = EOF
    }
    if (body_len > 0 && left > 0) return -1;                 // 약속한 길이보다 일찍 끊김 → 클라이언트도 끊어 잘린 것을 알림
    return keep ? 0 : -1;
}

int reverse_proxy_request(int clientfd, const http_request *req, route *r) {
    if (clientfd < 0 || req == NULL || r == NULL || r->backend_count <= 0) return 502;
    // 다음 차례 백엔드부터 시도하고, 연결이 안 되면(꺼짐 등) 다른 백엔드로 넘어간다 — 각 백엔드 최대 한 번
    for (int tries = 0; tries < r->backend_count; tries++) {
        const backend *b = next_backend(r);
        int upfd = open_clientfd(b->host, b->port);
        if (upfd < 0) continue;
        // 경로는 바꾸지 않고 원래 URI(경로 + 쿼리)를 그대로 넘긴다 ("/api/x" → 백엔드의 "/api/x")
        int rc = proxy_relay(clientfd, upfd, req, req->uri);
        close(upfd);
        return rc;
    }
    return 502;
}
