// [proxy.c] 프록시 공통 중계 + Reverse Proxy 구현
#include "proxy.h"

int proxy_relay(int clientfd, int upstreamfd, const http_request *req, const char *target) {
    (void)clientfd; (void)upstreamfd; (void)req; (void)target;
    // TODO:
    //  1. upstreamfd에 SO_RCVTIMEO(PROXY_TIMEOUT_SEC) 설정
    //  2. 요청 재구성해서 송신: "메서드 target 버전" + 헤더(Connection은 close로 교체) + 빈 줄 + body
    //  3. upstream 응답을 read → rio_writen(clientfd) 반복 (EOF까지)
    //     첫 응답 전에 타임아웃(EAGAIN)이면 504, 중계 중 클라이언트가 끊기면 중단(-1)
    return 502;
}

int reverse_proxy_request(int clientfd, const http_request *req, route *r) {
    (void)clientfd; (void)req; (void)r;
    // TODO:
    //  1. next_backend → open_clientfd. 실패하면 다음 백엔드 시도, 모두 실패하면 502
    //  2. proxy_relay(clientfd, backendfd, req, req->uri)
    //  3. close(backendfd)
    return 502;
}
