// [proxy.h] 프록시 공통 중계 + Reverse Proxy
// 프록시는 연결 2개를 잇는다: 클라이언트 ─(clientfd)→ 프록시 ─(upstreamfd)→ 백엔드 / 원 서버
// 반환 규약은 static_file.h와 같음
#ifndef PROXY_H
#define PROXY_H

#include "http_request.h"
#include "router.h"

#define PROXY_TIMEOUT_SEC 5   // 백엔드·원 서버 응답을 기다리는 최대 시간. 넘으면 504

// 공통 (Reverse·Forward 모두 사용): 이미 연결된 upstreamfd로 요청을 보내고 응답을 clientfd로 그대로 중계
// target = 요청 라인에 넣을 경로 ("/a.html?x=1"). 헤더는 Connection: close로 바꿔 보낸다
int proxy_relay(int clientfd, int upstreamfd, const http_request *req, const char *target);

// Reverse Proxy: r의 백엔드 중 하나(Round-Robin)로 전달. 응답 후 클라이언트 연결은 닫는다
int reverse_proxy_request(int clientfd, const http_request *req, route *r);

#endif
