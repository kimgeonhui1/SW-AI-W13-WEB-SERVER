// [net.c] 소켓 연결 도우미 구현
#include "net.h"

int open_listenfd(const char *port) {
    (void)port;
    // TODO:
    //  1. hints: SOCK_STREAM, AI_PASSIVE | AI_ADDRCONFIG | AI_NUMERICSERV
    //  2. getaddrinfo(NULL, port, ...) → 후보마다 socket → setsockopt(SO_REUSEADDR) → bind, 성공하면 멈춤
    //  3. freeaddrinfo, listen(fd, 1024). listen 실패 시 close 후 -1
    return -1;
}

int open_clientfd(const char *host, const char *port) {
    (void)host; (void)port;
    // TODO:
    //  1. hints: SOCK_STREAM, AI_ADDRCONFIG | AI_NUMERICSERV
    //  2. getaddrinfo(host, port, ...) → 후보마다 socket → connect, 성공하면 멈춤 (실패한 소켓은 close)
    //  3. freeaddrinfo
    return -1;
}
