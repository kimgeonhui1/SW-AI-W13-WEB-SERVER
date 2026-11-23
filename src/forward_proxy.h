// [forward_proxy.h] Forward Proxy — 클라이언트를 대신해 요청 URL의 원 서버에 접속하고 응답을 중계
//
// 클라이언트가 프록시를 설정하면 요청 라인에 전체 URL(절대 URI)이 온다
//   GET http://example.com:8000/a.html?x=1 HTTP/1.1
// 프록시는 원 서버에 경로만 담아 보낸다
//   GET /a.html?x=1 HTTP/1.1
// 반환 규약은 static_file.h와 같음
#ifndef FORWARD_PROXY_H
#define FORWARD_PROXY_H

#include <stddef.h>
#include "http_request.h"

// URI가 "http://"로 시작(대소문자 무시)하면 1 — Forward Proxy 요청
int is_absolute_uri(const char *uri);

// "http://host[:port]/path?query" 분해 → host, port(없으면 "80"), path(없으면 "/", 쿼리 포함)
// 성공 0, 형식 오류(호스트 없음, 포트 범위 밖, 버퍼 부족, http:// 아님) -1
int parse_absolute_uri(const char *uri, char *host, size_t hostlen,
                       char *port, size_t portlen, char *path, size_t pathlen);

// 원 서버에 연결해 req를 전달하고 응답을 clientfd로 중계. 응답 후 클라이언트 연결은 닫는다
int forward_proxy_request(int clientfd, const http_request *req);

#endif
