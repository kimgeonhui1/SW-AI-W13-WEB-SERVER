// [rio.c] Robust I/O 구현 — 네트워크 read/write는 쪼개져서 오가므로 원하는 만큼 될 때까지 반복한다
#include "rio.h"

ssize_t rio_writen(int fd, const void *buf, size_t n) {
    (void)fd; (void)buf; (void)n;
    return -1; // TODO: write를 반복해 n바이트를 다 보낸다. EINTR이면 재시도
}

void rio_readinitb(rio_t *rp, int fd) {
    (void)rp; (void)fd;
    // TODO: rp->fd = fd, cnt = 0, bufp = buf
}

ssize_t rio_readlineb(rio_t *rp, void *buf, size_t maxlen) {
    (void)rp; (void)buf; (void)maxlen;
    return -1; // TODO: 내부 버퍼에서 1바이트씩 꺼내며 '\n'을 만날 때까지 복사. 버퍼가 비면 read로 다시 채운다
}

ssize_t rio_readnb(rio_t *rp, void *buf, size_t n) {
    (void)rp; (void)buf; (void)n;
    return -1; // TODO: 내부 버퍼에 남은 것부터 꺼내고, 부족하면 read로 채우며 n바이트까지 복사
}
