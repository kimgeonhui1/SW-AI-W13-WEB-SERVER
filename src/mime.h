// [mime.h] MIME 타입 매핑 — 파일 확장자로 Content-Type 값을 정한다
#ifndef MIME_H
#define MIME_H

// 경로의 확장자 → MIME 타입 (".html" → "text/html"). 모르는 확장자·확장자 없음 → "application/octet-stream"
const char *mime_type(const char *path);

#endif
