// [mime.c] MIME 타입 매핑 구현
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "mime.h"

#include <stddef.h>
#include <string.h>
#include <strings.h>

typedef struct {
    const char *ext;
    const char *mime;
} mime_entry;

static const mime_entry k_mime_table[] = {
    { ".html",  "text/html; charset=utf-8" },
    { ".htm",   "text/html; charset=utf-8" },
    { ".css",   "text/css; charset=utf-8" },
    { ".js",    "application/javascript; charset=utf-8" },
    { ".json",  "application/json; charset=utf-8" },
    { ".xml",   "application/xml; charset=utf-8" },
    { ".txt",   "text/plain; charset=utf-8" },
    { ".csv",   "text/csv; charset=utf-8" },
    { ".svg",   "image/svg+xml" },
    { ".png",   "image/png" },
    { ".jpg",   "image/jpeg" },
    { ".jpeg",  "image/jpeg" },
    { ".gif",   "image/gif" },
    { ".webp",  "image/webp" },
    { ".ico",   "image/x-icon" },
    { ".woff",  "font/woff" },
    { ".woff2", "font/woff2" },
    { ".pdf",   "application/pdf" },
    { ".zip",   "application/zip" },
    { ".wasm",  "application/wasm" },
};

const char *mime_type_from_path(const char *path) {
    if (path == NULL || *path == '\0') {
        return "application/octet-stream";
    }

    // 쿼리스트링이 섞여 들어온 경우 '?' 이전까지만 확장자 판별에 사용
    const char *q = strchr(path, '?');
    size_t end_len = (q != NULL) ? (size_t)(q - path) : strlen(path);
    if (end_len == 0) {
        return "application/octet-stream";
    }

    // 마지막 '/' 이후 파일명 구간만 확인 (디렉토리 이름에 '.'이 있는 경우 오판 방지)
    const char *last_slash = NULL;
    for (size_t i = end_len; i > 0; i--) {
        if (path[i - 1] == '/') {
            last_slash = path + (i - 1);
            break;
        }
    }
    const char *filename = (last_slash != NULL) ? (last_slash + 1) : path;
    size_t fname_len = (size_t)((path + end_len) - filename);
    if (fname_len == 0) {
        return "application/octet-stream";
    }

    // 파일명 내 마지막 '.' 탐색 (숨김파일 ".env"처럼 맨 앞만 '.'인 경우는 확장자 없음으로 간주)
    const char *dot = NULL;
    for (size_t i = fname_len; i > 1; i--) {
        if (filename[i - 1] == '.') {
            dot = filename + (i - 1);
            break;
        }
    }
    if (dot == NULL) {
        return "application/octet-stream";
    }

    size_t ext_len = (size_t)((path + end_len) - dot);
    size_t table_len = sizeof(k_mime_table) / sizeof(k_mime_table[0]);

    for (size_t i = 0; i < table_len; i++) {
        if (strlen(k_mime_table[i].ext) == ext_len &&
            strncasecmp(dot, k_mime_table[i].ext, ext_len) == 0) {
            return k_mime_table[i].mime;
        }
    }

    return "application/octet-stream";
}