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

// 루트 경로의 realpath를 스레드마다 한 번만 계산해 둔다.
// 루트는 실행 중 바뀌지 않는데 요청마다 realpath를 부르면 디렉터리마다 stat이 일어난다
// (WSL의 /mnt/c처럼 파일 시스템이 느리면 특히 비쌈). 스레드별 저장소라 락이 필요 없다
// shortcut: 실행 중 루트 폴더를 지우거나 심볼릭 링크를 바꾸면 이전 해석을 계속 쓴다. 그런 운영이 필요하면 캐시 제거
static const char *resolve_root_cached(const char *root) {
    static __thread char cached_in[PATH_MAX];
    static __thread char cached_out[PATH_MAX];
    static __thread int cached_ok;
    if (cached_ok && strcmp(cached_in, root) == 0) return cached_out;
    if (realpath(root, cached_out) == NULL) {
        cached_ok = 0;
        return NULL;
    }
    snprintf(cached_in, sizeof(cached_in), "%s", root);
    cached_ok = 1;
    return cached_out;
}

// open/realpath 실패 errno → HTTP 상태 코드
static int status_from_errno(int e) {
    if (e == ENOENT || e == ENOTDIR) return 404;
    if (e == EACCES || e == EPERM) return 403;
    if (e == EMFILE || e == ENFILE) return 503;            // fd 한도: 일시적 과부하
    return 500;
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

    const char *resolved_root = resolve_root_cached(root);
    if (resolved_root == NULL) {
        return 500;
    }

    char candidate[PATH_MAX];
    char resolved_target[PATH_MAX];
    size_t plen = strlen(req_path);
    int ends_with_slash = (req_path[plen - 1] == '/');
    int written = snprintf(candidate, sizeof(candidate), ends_with_slash ? "%s%sindex.html" : "%s%s",
                           resolved_root, req_path);
    if (written < 0 || (size_t)written >= sizeof(candidate)) {
        return 414;
    }

    // 심볼릭 링크·".."를 모두 풀어 실제 경로를 구한 뒤 루트 안인지 확인 (경로 탈출 차단)
    if (realpath(candidate, resolved_target) == NULL) {
        return status_from_errno(errno);
    }
    if (!is_within_root(resolved_root, resolved_target)) {
        return 403;
    }

    // 먼저 열고(open) 그 fd로 확인(fstat)한다:
    //  - 200 헤더를 보내기 전에 열기 실패(권한, fd 한도 등)를 알 수 있어 오류 응답을 깔끔하게 보낼 수 있다
    //  - 확인과 사용이 같은 fd라 그 사이 파일이 바뀌는 경쟁(TOCTOU)이 없고, 따로 access()를 부를 필요도 없다
    //  - O_NONBLOCK: FIFO를 열 때 쓰는 쪽을 기다리며 워커가 멈추지 않게 (일반 파일 읽기에는 영향 없음)
    //  - O_CLOEXEC: CGI(fork + execve) 자식이 이 fd를 물려받지 않게
    int file_fd = open(resolved_target, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (file_fd < 0) {
        return status_from_errno(errno);
    }

    struct stat st;
    if (fstat(file_fd, &st) != 0) {
        close(file_fd);
        return 500;
    }
    if (!S_ISREG(st.st_mode)) {                            // 디렉터리, FIFO, 장치 파일 등
        close(file_fd);
        return 403;
    }

    size_t file_size = (size_t)st.st_size;
    if (send_headers(fd, 200, mime_type_from_path(resolved_target), file_size, req->keep_alive) != 0) {
        close(file_fd);
        return -1;
    }
    if (is_head || file_size == 0) {
        close(file_fd);
        return 0;
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

        // 헤더에 약속한 크기보다 파일이 줄어듦(nread 0) 또는 읽기 오류 → 이미 200을 보냈으므로 연결을 끊는다
        if (nread <= 0) {
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
    return io_failed ? -1 : 0;
}
