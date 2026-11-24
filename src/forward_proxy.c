// [forward_proxy.c] Forward Proxy 구현
#include "forward_proxy.h"
#include "net.h"
#include "proxy.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

int is_absolute_uri(const char *uri) {
    return uri != NULL && strncasecmp(uri, "http://", 7) == 0;   // 스킴은 대소문자 무시 (RFC 3986)
}

// 문자열 [s, s+n)을 크기 cap인 버퍼에 복사. 넘치면 -1
static int copy_n(char *dst, size_t cap, const char *s, size_t n) {
    if (n >= cap) return -1;
    memcpy(dst, s, n);
    dst[n] = '\0';
    return 0;
}

int parse_absolute_uri(const char *uri, char *host, size_t hostlen,
                       char *port, size_t portlen, char *path, size_t pathlen) {
    if (!is_absolute_uri(uri) || host == NULL || port == NULL || path == NULL) return -1;
    const char *auth = uri + 7;
    size_t alen = strcspn(auth, "/?#");                     // authority = "http://" 뒤 첫 '/', '?', '#' 전까지
    const char *rest = auth + alen;
    // userinfo("user:pass@host")는 거부: "http://good.com@evil.com/"처럼 실제 접속 호스트를 속이는 데 쓰인다
    if (memchr(auth, '@', alen) != NULL) return -1;

    const char *h = auth, *colon;
    size_t hlen;
    if (alen > 0 && auth[0] == '[') {                       // IPv6: "[::1]:8080"
        const char *rb = memchr(auth, ']', alen);
        if (rb == NULL) return -1;
        h = auth + 1;
        hlen = (size_t)(rb - h);
        colon = (rb + 1 < auth + alen) ? rb + 1 : NULL;
        if (colon != NULL && *colon != ':') return -1;      // "]" 뒤에는 ":포트"만
    } else {
        colon = memchr(auth, ':', alen);
        hlen = colon ? (size_t)(colon - auth) : alen;
        if (colon != NULL && memchr(colon + 1, ':', (size_t)(auth + alen - colon - 1)) != NULL) return -1;
    }
    if (hlen == 0 || copy_n(host, hostlen, h, hlen) != 0) return -1;

    // 포트: 숫자만 1~65535, 없거나 비었으면 80 (RFC 3986 3.2.3)
    const char *p = colon ? colon + 1 : "";
    size_t plen = colon ? (size_t)(auth + alen - p) : 0;
    if (plen == 0) {
        if (copy_n(port, portlen, "80", 2) != 0) return -1;
    } else {
        long v = 0;
        if (plen > 5) return -1;
        for (size_t i = 0; i < plen; i++) {
            if (p[i] < '0' || p[i] > '9') return -1;
            v = v * 10 + (p[i] - '0');
        }
        if (v < 1 || v > 65535 || copy_n(port, portlen, p, plen) != 0) return -1;
    }

    // 경로(+쿼리): 없으면 "/". "?a=1"만 있으면 "/?a=1". '#' 뒤(조각)는 서버로 보내지 않는다
    size_t rlen = strcspn(rest, "#");
    if (rest[0] == '/') return copy_n(path, pathlen, rest, rlen);
    if (pathlen < 2 + rlen) return -1;
    path[0] = '/';
    memcpy(path + 1, rest, rlen);
    path[1 + rlen] = '\0';
    return 0;
}

int forward_proxy_request(int clientfd, const http_request *req) {
    if (clientfd < 0 || req == NULL) return 400;
    char host[256], port[8], path[MAX_URI_LEN];
    if (parse_absolute_uri(req->uri, host, sizeof(host), port, sizeof(port), path, sizeof(path)) != 0) return 400;

    // Host 헤더 값 = URI에 적힌 authority 그대로 ("example.com", "127.0.0.1:8001", "[::1]:8080")
    char authority[MAX_URI_LEN];
    const char *auth = req->uri + 7;
    if (copy_n(authority, sizeof(authority), auth, strcspn(auth, "/?#")) != 0) return 400;

    // shortcut: 접속 대상 제한이 없다 (localhost·사설망 포함). -f로 켤 때만 동작하므로 신뢰하는 환경에서만 쓸 것.
    //           외부에 열어야 하면 허용 호스트 목록이나 사설 주소 차단을 추가
    int upfd = open_clientfd(host, port);
    if (upfd < 0) return errno == ETIMEDOUT ? 504 : 502;    // 응답 없는 호스트 504, DNS 실패·연결 거부 502
    int rc = proxy_relay(clientfd, upfd, req, path, authority);
    close(upfd);
    return rc;
}
