// [static_file.c] 정적 파일 서빙 구현
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "static_file.h"
#include "http_response.h"
#include "mime.h"
#include "rio.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define STATIC_BUF_SIZE 65536

static int is_within_root(const char *resolved_root, const char *resolved_target) {
    size_t root_len = strlen(resolved_root);
    if (root_len == 0) return 0;

    // 루트 경로가 "/"인 경우와 일반 경로("/srv/www")인 경우 모두 처리
    if (strcmp(resolved_root, "/") == 0) {
        return resolved_target[0] == '/';
    }

    if (strncmp(resolved_target, resolved_root, root_len) != 0) {
        return 0;
    }

    // 정확히 루트 디렉터리이거나, 루트 바로 아래 '/' 구분자가 있어야 함
    return resolved_target[root_len] == '\0' || resolved_target[root_len] == '/';
}

int serve_static(int fd, const char *root, const http_request *req) {
    if (fd < 0 || root == NULL || req == NULL) {
        return 500;
    }

    int is_get = (strcmp(req->method, "GET") == 0);
    int is_head = (strcmp(req->method, "HEAD") == 0);
    if (!is_get && !is_head) {
        return 405;
    }

    const char *req_path = (req->path[0] != '\0') ? req->path : "/";
    if (req_path[0] != '/') {
        return 400;
    }

    char *resolved_root = malloc(PATH_MAX);
    char *candidate = malloc(PATH_MAX);
    char *resolved_target = malloc(PATH_MAX);
    if (resolved_root == NULL || candidate == NULL || resolved_target == NULL) {
        free(resolved_root);
        free(candidate);
        free(resolved_target);
        return 500;
    }

    int status = 500;

    if (realpath(root, resolved_root) == NULL) {
        status = 500;
        goto out;
    }

    size_t plen = strlen(req_path);
    int ends_with_slash = (req_path[plen - 1] == '/');

    int written;
    if (ends_with_slash) {
        written = snprintf(candidate, PATH_MAX, "%s%sindex.html", resolved_root, req_path);
    } else {
        written = snprintf(candidate, PATH_MAX, "%s%s", resolved_root, req_path);
    }
    if (written < 0 || (size_t)written >= PATH_MAX) {
        status = 414;
        goto out;
    }

    if (realpath(candidate, resolved_target) == NULL) {
        if (errno == ENOENT || errno == ENOTDIR) {
            status = 404;
        } else if (errno == EACCES) {
            status = 403;
        } else {
            status = 500;
        }
        goto out;
    }

    if (!is_within_root(resolved_root, resolved_target)) {
        status = 403;
        goto out;
    }

    struct stat st;
    if (stat(resolved_target, &st) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            status = 404;
        } else if (errno == EACCES) {
            status = 403;
        } else {
            status = 500;
        }
        goto out;
    }

    if (!S_ISREG(st.st_mode)) {
        status = 403;
        goto out;
    }

    if (access(resolved_target, R_OK) != 0) {
        status = (errno == EACCES) ? 403 : 500;
        goto out;
    }

    if (st.st_size < 0) {
        status = 500;
        goto out;
    }

    size_t file_size = (size_t)st.st_size;
    const char *ctype = mime_type_from_path(resolved_target);

    if (send_headers(fd, 200, ctype, file_size, req->keep_alive) != 0) {
        status = -1;
        goto out;
    }

    if (is_head || file_size == 0) {
        status = 0;
        goto out;
    }

    int file_fd = open(resolved_target, O_RDONLY);
    if (file_fd < 0) {
        // 이미 200 헤더를 송신한 뒤 열기 실패한 예외 상황이므로 연결 종료 시그널(-1) 반환
        status = -1;
        goto out;
    }

    char buf[STATIC_BUF_SIZE];
    size_t remaining = file_size;
    int io_failed = 0;

    while (remaining > 0) {
        size_t to_read = (remaining < sizeof(buf)) ? remaining : sizeof(buf);
        ssize_t nread;
        do {
            nread = read(file_fd, buf, to_read);
        } while (nread < 0 && errno == EINTR);

        if (nread < 0 || nread == 0) {
            io_failed = 1;
            break;
        }

        size_t chunk = (size_t)nread;
        if (rio_writen(fd, buf, chunk) != (ssize_t)chunk) {
            io_failed = 1;
            break;
        }

        remaining -= chunk;
    }

    close(file_fd);
    status = io_failed ? -1 : 0;

out:
    free(resolved_root);
    free(candidate);
    free(resolved_target);
    return status;
}