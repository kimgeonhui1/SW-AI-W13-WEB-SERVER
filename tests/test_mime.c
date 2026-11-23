// [test_mime.c] MIME 타입 매핑 단위 테스트
#include "test.h"
#include "mime.h"

// html, css, js, png, jpg, jpeg 등 기본 매핑
static void test_known_types(void) {
    CHECK_STR(mime_type_from_path("/index.html"), "text/html; charset=utf-8");
    CHECK_STR(mime_type_from_path("/a.htm"), "text/html; charset=utf-8");
    CHECK_STR(mime_type_from_path("/style.css"), "text/css; charset=utf-8");
    CHECK_STR(mime_type_from_path("/app.js"), "application/javascript; charset=utf-8");
    CHECK_STR(mime_type_from_path("/data.json"), "application/json; charset=utf-8");
    CHECK_STR(mime_type_from_path("/note.txt"), "text/plain; charset=utf-8");
    CHECK_STR(mime_type_from_path("/img.png"), "image/png");
    CHECK_STR(mime_type_from_path("/photo.jpg"), "image/jpeg");
    CHECK_STR(mime_type_from_path("/photo.jpeg"), "image/jpeg");
    CHECK_STR(mime_type_from_path("/anim.gif"), "image/gif");
    CHECK_STR(mime_type_from_path("/logo.svg"), "image/svg+xml");
    CHECK_STR(mime_type_from_path("index.html"), "text/html; charset=utf-8");   // '/' 없는 파일명
}

// 대문자 확장자도 같은 결과
static void test_case_insensitive(void) {
    CHECK_STR(mime_type_from_path("/INDEX.HTML"), "text/html; charset=utf-8");
    CHECK_STR(mime_type_from_path("/IMG.PNG"), "image/png");
    CHECK_STR(mime_type_from_path("/Photo.JpEg"), "image/jpeg");
}

// 쿼리스트링은 확장자 판정에서 제외
static void test_query_string_ignored(void) {
    CHECK_STR(mime_type_from_path("/app.js?v=42"), "application/javascript; charset=utf-8");
    CHECK_STR(mime_type_from_path("/page?x=a.png"), "application/octet-stream");  // '?' 뒤의 ".png"에 속지 않음
}

// 확장자 없음, 모르는 확장자, 디렉터리 이름의 '.', 숨김 파일, 마지막 '.'만 확장자로 판정
static void test_unknown_types(void) {
    CHECK_STR(mime_type_from_path("/README"), "application/octet-stream");
    CHECK_STR(mime_type_from_path("/archive.unknownext"), "application/octet-stream");
    CHECK_STR(mime_type_from_path("/dir.v1/noext"), "application/octet-stream");
    CHECK_STR(mime_type_from_path("/.env"), "application/octet-stream");
    CHECK_STR(mime_type_from_path("/file."), "application/octet-stream");
    CHECK_STR(mime_type_from_path("/x.html.bak"), "application/octet-stream");
    CHECK_STR(mime_type_from_path("/x.bak.html"), "text/html; charset=utf-8");
    CHECK_STR(mime_type_from_path("/.html"), "application/octet-stream");   // 숨김 파일 이름 ".html"
    CHECK_STR(mime_type_from_path("/htmlhtml"), "application/octet-stream"); // 접미사 비교가 아니라 '.' 기준
    CHECK_STR(mime_type_from_path("/"), "application/octet-stream");
    CHECK_STR(mime_type_from_path(""), "application/octet-stream");
    CHECK_STR(mime_type_from_path(NULL), "application/octet-stream");
}

int main(void) {
    test_known_types();
    test_case_insensitive();
    test_query_string_ignored();
    test_unknown_types();
    return TEST_RESULT();
}
