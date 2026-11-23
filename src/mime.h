// [mime.h] MIME 타입 매핑 — 파일 확장자로 Content-Type 값을 정한다
#ifndef MIME_H
#define MIME_H

// URI/경로 또는 파일명에서 Content-Type MIME 문자열을 반환한다.
// 쿼리스트링('?')은 무시하며, 알 수 없는 확장자는 "application/octet-stream"을 반환한다.
const char *mime_type_from_path(const char *path);

#endif
