// [cgi.c] CGI 실행 구현
#include "cgi.h"

int serve_cgi(int fd, const char *cgi_dir, const http_request *req) {
    (void)fd; (void)cgi_dir; (void)req;
    // TODO:
    //  1. 프로그램 경로 = cgi_dir + (path에서 CGI_PREFIX 뒤). 없으면 404, 실행 권한 없으면 403, "../" 차단
    //  2. 상태 라인 "HTTP/1.1 200 OK\r\n" + "Connection: close\r\n" 먼저 송신 (나머지 헤더는 CGI가 출력)
    //  3. POST면 pipe를 만들어 body를 CGI의 stdin으로 전달
    //  4. fork → 자식: 환경변수(REQUEST_METHOD, QUERY_STRING, CONTENT_LENGTH) 설정,
    //               dup2(fd, STDOUT_FILENO) → execve. 실패하면 _exit
    //  5. 부모: waitpid로 자식 회수 (좀비 방지). exec 실패면 500
    return 404;
}
