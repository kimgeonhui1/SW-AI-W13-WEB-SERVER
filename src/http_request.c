// [http_request.c] HTTP 요청 파서 구현
// TCP는 메시지 경계가 없으므로 HTTP 규칙으로 경계를 찾는다: 헤더 끝 = 빈 줄("\r\n"), 본문 끝 = Content-Length
#include "http_request.h"

int parse_request_line(const char *line, http_request *req) {
    (void)line; (void)req;
    // TODO:
    //  - "메서드 URI 버전" 3개로 나눔. 개수가 다르면 400
    //  - 메서드가 GET/POST/HEAD가 아니면 501
    //  - URI가 MAX_URI_LEN 이상이면 414
    //  - 버전이 HTTP/1.0, HTTP/1.1이 아니면 505
    //  - URI를 '?' 기준으로 path / query로 분리 (절대 URI "http://host/..."면 호스트 뒤 부분 기준)
    //  - keep_alive 기본값: HTTP/1.1이면 1, HTTP/1.0이면 0
    return -1;
}

int parse_header_line(const char *line, http_request *req) {
    (void)line; (void)req;
    // TODO:
    //  - ':'가 없으면 400
    //  - 헤더 개수가 MAX_HEADERS를 넘거나 이름·값이 너무 길면 431
    //  - Content-Length: 숫자가 아니면 400, MAX_BODY_LEN 초과면 413
    //  - Connection: close / keep-alive → keep_alive 갱신
    return -1;
}

int read_request(rio_t *rp, http_request *req) {
    (void)rp; (void)req;
    // TODO:
    //  1. rio_readlineb로 요청 라인 → parse_request_line (0바이트면 EOF → -1)
    //  2. "\r\n"만 있는 줄이 나올 때까지 parse_header_line
    //  3. HTTP/1.1인데 Host 헤더가 없으면 400
    //  4. POST인데 Content-Length가 없으면 411, Transfer-Encoding: chunked면 501 (chunked 미지원)
    //  5. content_length > 0 이면 malloc 후 rio_readnb로 정확히 그만큼 읽기
    return -1;
}

const char *get_header(const http_request *req, const char *name) {
    (void)req; (void)name;
    return NULL; // TODO: headers를 돌며 strcasecmp로 이름 비교
}

void free_request(http_request *req) {
    (void)req;
    // TODO: free(req->body), body = NULL
}
