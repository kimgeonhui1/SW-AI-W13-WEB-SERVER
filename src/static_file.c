// [static_file.c] 정적 파일 서빙 구현
#include "static_file.h"

int serve_static(int fd, const char *root, const http_request *req) {
    (void)fd; (void)root; (void)req;
    // TODO:
    //  1. 경로 만들기: root + path ("/"로 끝나면 index.html 붙임)
    //  2. realpath 결과가 root 밖이면 403 ("../" 디렉터리 탈출 차단)
    //  3. stat: 없으면 404, 일반 파일이 아니거나 읽기 권한 없으면 403
    //  4. send_headers(200, mime_type(path), 파일 크기, keep_alive)
    //  5. GET이면 파일을 open → read → rio_writen 반복 (큰 파일도 메모리에 다 올리지 않음) → close
    return 404;
}
