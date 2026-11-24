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
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define STATIC_BUF_SIZE 65536

// 작은 파일 메모리 캐시 (스레드별, 락 없음)
// 이유: WSL의 /mnt/c(9P)에서는 realpath·open이 각각 ~1.6ms라 요청마다 파일 시스템을 거치면 초당 ~3,000건에서 막힌다.
//       같은 파일을 짧은 시간 안에 다시 요청하면 시스템 콜 없이 메모리에서 바로 보낸다
// shortcut: 파일이 바뀌거나 지워져도 최대 CACHE_TTL_SEC 동안 이전 내용을 보낸다. 즉시 반영이 필요하면 TTL을 0으로(캐시 끔)
#define CACHE_SLOTS    64              // 스레드당 칸 수 (경로 해시로 바로 찾아가고, 충돌하면 덮어씀)
#define CACHE_MAX_FILE 65536           // 이보다 큰 파일은 캐시하지 않고 매번 읽어 보낸다 (메모리 상한: 워커 수 × 64칸 × 64KB)
#define CACHE_TTL_SEC  1.0

typedef struct {
    char key[PATH_MAX + 1];            // 루트 + '\n' + 요청 경로 (루트가 다른 서버·테스트끼리 섞이지 않게)
    char *data;                        // 파일 내용 (NULL이면 빈 칸)
    size_t len;
    const char *mime;                  // mime_type_from_path가 돌려준 정적 문자열
    double expires;                    // rio_now() 기준 만료 시각
} cache_entry;

static __thread cache_entry t_cache[CACHE_SLOTS];

static unsigned int cache_hash(const char *s) {
    unsigned int h = 2166136261u;      // FNV-1a
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h % CACHE_SLOTS;
}

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

// 워커 스레드가 끝날 때 그 스레드의 캐시 메모리를 해제 (__thread 변수는 스레드가 끝나면 포인터만 사라져 누수가 됨)
static pthread_key_t g_cache_key;
static pthread_once_t g_cache_once = PTHREAD_ONCE_INIT;
static void cache_free_thread(void *p) {
    cache_entry *c = p;
    for (int i = 0; i < CACHE_SLOTS; i++) {
        free(c[i].data);
        c[i].data = NULL;
    }
}
static void cache_key_init(void) { pthread_key_create(&g_cache_key, cache_free_thread); }

static cache_entry *cache_find(const char *key) {
    cache_entry *e = &t_cache[cache_hash(key)];
    if (e->data != NULL && rio_now() < e->expires && strcmp(e->key, key) == 0) return e;
    return NULL;
}

// data의 소유권을 캐시로 넘긴다 (같은 칸의 이전 내용은 해제)
static void cache_put(const char *key, char *data, size_t len, const char *mime) {
    pthread_once(&g_cache_once, cache_key_init);
    pthread_setspecific(g_cache_key, t_cache);   // 이 스레드가 끝날 때 cache_free_thread가 불리도록
    cache_entry *e = &t_cache[cache_hash(key)];
    free(e->data);
    snprintf(e->key, sizeof(e->key), "%s", key);
    e->data = data;
    e->len = len;
    e->mime = mime;
    e->expires = rio_now() + CACHE_TTL_SEC;
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

    // 캐시 확인: 최근(CACHE_TTL_SEC 안)에 이 스레드가 보낸 작은 파일이면 파일 시스템을 거치지 않는다
    // (경로 탈출·권한 검사는 캐시에 넣기 전에 이미 통과한 것만 들어 있다)
    char key[PATH_MAX + 1];
    int klen = snprintf(key, sizeof(key), "%s\n%s", root, req_path);
    int keyed = (klen > 0 && (size_t)klen < sizeof(key));
    cache_entry *hit = keyed ? cache_find(key) : NULL;
    if (hit != NULL) {
        if (send_headers(fd, 200, hit->mime, hit->len, req->keep_alive) != 0) return -1;
        if (is_head || hit->len == 0) return 0;
        return rio_writen(fd, hit->data, hit->len) == (ssize_t)hit->len ? 0 : -1;
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
    const char *mime = mime_type_from_path(resolved_target);

    // 작은 파일: 통째로 읽어 캐시에 넣고 보낸다. 다 못 읽으면(읽는 중에 파일이 줄어듦 등) 캐시하지 않고 500
    if (keyed && file_size <= CACHE_MAX_FILE) {
        char *data = malloc(file_size > 0 ? file_size : 1);
        size_t got = 0;
        while (data != NULL && got < file_size) {
            ssize_t r = read(file_fd, data + got, file_size - got);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            got += (size_t)r;
        }
        close(file_fd);
        if (data == NULL || got != file_size) {
            free(data);
            return 500;
        }
        cache_put(key, data, file_size, mime);
        if (send_headers(fd, 200, mime, file_size, req->keep_alive) != 0) return -1;
        if (is_head || file_size == 0) return 0;
        return rio_writen(fd, data, file_size) == (ssize_t)file_size ? 0 : -1;
    }

    if (send_headers(fd, 200, mime, file_size, req->keep_alive) != 0) {
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
