// [cgi.h] 동적 요청 처리 — CGI 프로그램을 fork + execve로 실행하고 그 출력을 응답으로 보낸다
// 반환 규약은 static_file.h와 같음 (0 = 응답 보냄 / >0 = 호출한 쪽이 보낼 오류 코드 / -1 = 송신 중 끊김)
#ifndef CGI_H
#define CGI_H

#include "http_request.h"

#define CGI_PREFIX      "/cgi-bin/"        // 이 경로로 시작하는 요청을 CGI로 처리
#define CGI_TIMEOUT_SEC 5                  // CGI 실행 전체 제한. 넘으면 강제 종료하고 504
#define CGI_MAX_OUTPUT  (1024 * 1024)      // CGI 출력 상한. 넘으면 강제 종료하고 502

// cgi_dir 바로 아래의 프로그램(/cgi-bin/<이름>, 하위 폴더 불가)을 실행한다.
//  - 요청 정보는 환경변수(REQUEST_METHOD, QUERY_STRING, CONTENT_LENGTH ...)로, POST 본문은 표준 입력으로 넘긴다
//  - 표준 출력은 파이프로 받아 "헤더 + 빈 줄 + 본문"을 검사한 뒤, 서버가 Content-Length를 붙여 응답한다
//    (출력을 소켓에 바로 연결하지 않으므로 keep-alive를 유지할 수 있고, 잘못된 출력·멈춘 CGI를 걸러 낼 수 있다)
//  - CGI 헤더 중 Content-Type(필수)과 Status(선택, 예: "Status: 404 Not Found")만 반영한다
// 오류: 없는 프로그램 404, 실행 권한 없음 403, 실행 실패 500, 시간 초과 504, 출력 형식 오류·비정상 종료·출력 과다 502
int serve_cgi(int fd, const char *cgi_dir, const http_request *req);

#endif
