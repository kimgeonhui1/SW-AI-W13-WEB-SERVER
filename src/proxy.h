// [proxy.h] 프록시 공통 중계 + Reverse Proxy
// 프록시는 연결 2개를 잇는다: 클라이언트 ─(clientfd)→ 프록시 ─(upstreamfd)→ 백엔드 / 원 서버
// 반환 규약: 0 = 응답 보냄, 연결 유지 가능 / >0 = 아직 아무것도 안 보냄, 호출한 쪽이 보낼 오류 코드(502, 504)
//            -1 = 응답을 보냈지만(또는 보내다 끊겨) 클라이언트 연결을 닫아야 함
#ifndef PROXY_H
#define PROXY_H

#include "http_request.h"
#include "router.h"

#define PROXY_TIMEOUT_SEC 5          // 백엔드 응답 헤더를 기다리는 전체 시간, 본문 read 한 번의 최대 대기. 넘으면 504
#define PROXY_MAX_RESP_HEADER 65536  // 백엔드 응답 헤더 전체 상한. 넘으면 502

// 공통 (Reverse·Forward 모두 사용): 이미 연결된 upstreamfd로 요청을 보내고 응답을 clientfd로 중계
//  - target = 요청 라인에 넣을 경로 ("/a.html?x=1")
//  - 요청: hop-by-hop 헤더(Connection, Keep-Alive, Proxy-Connection, TE, Trailer, Upgrade)를 빼고 Connection: close
//  - 응답: 상태 줄을 검사하고 hop-by-hop 헤더를 바꿔 Connection을 다시 붙인다
//    본문 길이를 알면(Content-Length, HEAD, 204, 304) 그만큼만 보내고 클라이언트 keep-alive 유지,
//    모르면(Transfer-Encoding, 길이 없음) 백엔드가 닫을 때까지 보내고 -1 (클라이언트도 닫아야 끝을 알 수 있음)
int proxy_relay(int clientfd, int upstreamfd, const http_request *req, const char *target);

// Reverse Proxy: r의 백엔드를 Round-Robin으로 골라 전달. 연결이 안 되면 다음 백엔드, 모두 실패하면 502
int reverse_proxy_request(int clientfd, const http_request *req, route *r);

#endif
