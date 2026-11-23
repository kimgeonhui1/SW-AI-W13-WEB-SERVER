// [static_file.h] 정적 파일 서빙 — 루트 디렉터리에서 파일을 읽어 응답
//
// 핸들러 반환 규약 (static_file / cgi / proxy 공통)
//    0  = 응답을 보냄
//   >0  = 아직 응답 안 보냄. 호출한 쪽이 send_error로 이 상태 코드를 보낸다 (403, 404, 500 ...)
//   -1  = 송신 중 연결이 끊김 → close
#ifndef STATIC_FILE_H
#define STATIC_FILE_H

#include "http_request.h"

// root 아래 req->path 파일을 응답. HEAD면 헤더만 보낸다
int serve_static(int fd, const char *root, const http_request *req);

#endif
