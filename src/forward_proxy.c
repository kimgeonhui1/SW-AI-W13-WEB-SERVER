// [forward_proxy.c] Forward Proxy 구현
#include "forward_proxy.h"

#include <strings.h>

int is_absolute_uri(const char *uri) {
    return uri != NULL && strncasecmp(uri, "http://", 7) == 0;   // 스킴은 대소문자 무시 (RFC 3986)
}

int parse_absolute_uri(const char *uri, char *host, size_t hostlen,
                       char *port, size_t portlen, char *path, size_t pathlen) {
    (void)uri; (void)host; (void)hostlen; (void)port; (void)portlen; (void)path; (void)pathlen;
    // TODO:
    //  1. "http://" 뒤부터 첫 '/' 전까지 = authority ("host" 또는 "host:port")
    //  2. ':'가 있으면 host / port 분리, 없으면 port = "80". 호스트가 비었거나 포트가 1~65535 밖이면 -1
    //  3. 나머지 = path (없으면 "/"). 각 버퍼 크기를 넘으면 -1
    return -1;
}

int forward_proxy_request(int clientfd, const http_request *req) {
    (void)clientfd; (void)req;
    // TODO:
    //  1. parse_absolute_uri 실패 → 400
    //  2. open_clientfd(host, port) 실패 → 502 (DNS 조회 실패 포함)
    //  3. proxy_relay(clientfd, serverfd, req, path)   ← 요청 라인은 경로만, Host 헤더는 그대로
    //  4. close(serverfd)
    return 502;
}
