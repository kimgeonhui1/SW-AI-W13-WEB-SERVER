// [net.c] 소켓 연결 도우미 구현
#include "net.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define LISTEN_BACKLOG 1024   // handshake가 끝났지만 아직 accept되지 않은 연결을 커널이 쌓아 둘 개수

void set_cloexec(int fd) {
    // CGI가 fork + execve할 때 자식이 이 fd를 물려받지 않게 한다.
    // 물려받으면 다른 클라이언트의 연결이 CGI 프로그램이 끝날 때까지 닫히지 않는다
    int flags = fcntl(fd, F_GETFD);
    if (flags >= 0) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

// 포트 문자열 검사: 숫자로만 된 0~65535.
// getaddrinfo(AI_NUMERICSERV)는 "", "65536", " 80", "+80"도 에러 없이 받아 포트 0이나 잘린 값으로 바꿔 버리므로
// (예: open_listenfd("")가 아무 포트에나 열림) 직접 확인한다. allow_zero: 0 = "커널이 빈 포트를 골라 달라"
static int valid_port(const char *port, int allow_zero) {
    if (port == NULL || *port == '\0') return 0;
    long v = 0;
    for (const char *p = port; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') return 0;
        v = v * 10 + (*p - '0');
        if (v > 65535) return 0;
    }
    return allow_zero || v != 0;
}

int open_listenfd(const char *port) {
    struct addrinfo hints, *list, *p;
    int fd = -1, optval = 1, last_errno = 0;

    if (!valid_port(port, 1)) {
        fprintf(stderr, "open_listenfd: invalid port '%s'\n", port ? port : "(null)");
        return -1;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;                                  // TCP
    hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG | AI_NUMERICSERV;     // host=NULL → 와일드카드 주소(0.0.0.0 / ::)
    int rc = getaddrinfo(NULL, port, &hints, &list);
    if (rc != 0) {
        fprintf(stderr, "open_listenfd: getaddrinfo(%s): %s\n", port, gai_strerror(rc));
        return -1;
    }

    // 후보(IPv4, IPv6 ...)를 차례로 시도해 bind가 성공하는 첫 주소를 쓴다
    for (p = list; p != NULL; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) {
            last_errno = errno;
            continue;
        }
        set_cloexec(fd);                    // 만들자마자 설정 — 이후 어느 시점에 fork가 있어도 새지 않음
        // 서버를 껐다 바로 켤 때 이전 연결이 TIME_WAIT로 남아 있어도 같은 포트에 bind 허용
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
        if (bind(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        last_errno = errno;
        close(fd);                          // 실패한 소켓은 닫아야 fd 누수가 없다
        fd = -1;
    }
    freeaddrinfo(list);

    if (fd < 0) {
        fprintf(stderr, "open_listenfd: bind port %s: %s\n", port, strerror(last_errno));
        return -1;
    }
    if (listen(fd, LISTEN_BACKLOG) < 0) {   // 능동 소켓 → 듣기 소켓으로 전환
        fprintf(stderr, "open_listenfd: listen: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

int open_clientfd(const char *host, const char *port) {
    struct addrinfo hints, *list, *p;
    int fd = -1;

    // host가 NULL이면 getaddrinfo가 루프백(localhost)을 돌려줘 엉뚱한 곳에 접속하므로 막는다. 포트 0도 접속 대상이 아님
    if (host == NULL || *host == '\0' || !valid_port(port, 0)) return -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG | AI_NUMERICSERV;
    // 호스트 이름이면 여기서 DNS 조회가 일어난다
    int rc = getaddrinfo(host, port, &hints, &list);
    if (rc != 0) return -1;                 // DNS 실패 → 호출한 쪽(프록시)이 502로 처리

    // 블로킹 connect는 응답 없는 호스트(패킷을 버리는 방화벽 등)에서 커널 기본 시간(수십 초~2분)까지 워커를 붙잡는다
    // → non-blocking으로 connect를 시작하고 poll로 CONNECT_TIMEOUT_SEC까지만 기다린다 (후보 전체 합산)
    // shortcut: getaddrinfo(DNS 조회)는 시간 제한이 없다. 느린 DNS가 문제 되면 별도 스레드·비동기 DNS로 교체
    int last_errno = ECONNREFUSED;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (p = list; p != NULL; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) { last_errno = errno; continue; }
        set_cloexec(fd);
        int fl = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int ok = (connect(fd, p->ai_addr, p->ai_addrlen) == 0);
        if (!ok && errno == EINPROGRESS) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long left_ms = CONNECT_TIMEOUT_SEC * 1000L - ((now.tv_sec - t0.tv_sec) * 1000L + (now.tv_nsec - t0.tv_nsec) / 1000000L);
            struct pollfd pfd = { .fd = fd, .events = POLLOUT, .revents = 0 };
            int pr;
            do { pr = poll(&pfd, 1, left_ms > 0 ? (int)left_ms : 0); } while (pr < 0 && errno == EINTR);
            if (pr == 0) {
                errno = ETIMEDOUT;
            } else if (pr > 0) {
                int err = 0;
                socklen_t len = sizeof(err);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);   // 연결 결과 (0이면 성공)
                ok = (err == 0);
                errno = err;
            }
        }
        if (ok) {
            fcntl(fd, F_SETFL, fl);         // 이후 읽기·쓰기는 블로킹 + SO_RCVTIMEO/SO_SNDTIMEO로 제한
            break;
        }
        last_errno = errno;
        close(fd);                          // 이 후보는 실패 → 닫고 다음 후보
        fd = -1;
    }
    freeaddrinfo(list);
    if (fd < 0) errno = last_errno;         // 호출한 쪽이 시간 초과(504)와 거부(502)를 구분할 수 있게
    return fd;
}
