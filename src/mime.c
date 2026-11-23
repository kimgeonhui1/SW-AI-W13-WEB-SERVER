// [mime.c] MIME 타입 매핑 구현
#include "mime.h"

const char *mime_type(const char *path) {
    (void)path;
    // TODO: 마지막 '.' 뒤 확장자를 대소문자 무시하고 표에서 찾기
    //   html/htm → text/html, css → text/css, js → application/javascript,
    //   png → image/png, jpg/jpeg → image/jpeg, gif → image/gif, txt → text/plain, json → application/json
    //   주의: "/dir.v1/file"처럼 디렉터리 이름의 '.'은 확장자가 아님
    return "application/octet-stream";
}
