// [rio.h] Robust I/O — TCP의 short count(요청한 양보다 적게 읽힘·쓰임)를 처리하는 read/write 래퍼 (CSAPP 10.5)
#ifndef RIO_H
#define RIO_H

#include <sys/types.h>

#define RIO_BUFSIZE 8192

// 버퍼 있는 읽기 상태. 연결 소켓 fd 하나당 하나씩 만든다
typedef struct {
    int fd;                  // 읽을 fd
    ssize_t cnt;             // 내부 버퍼에 아직 꺼내지 않은 바이트 수
    char *bufp;              // 내부 버퍼에서 다음에 꺼낼 위치
    char buf[RIO_BUFSIZE];   // 내부 버퍼 (read 한 번에 크게 받아 두고 조금씩 꺼낸다)
} rio_t;

// n바이트를 다 쓸 때까지 write 반복. 성공 n, 실패 -1
ssize_t rio_writen(int fd, const void *buf, size_t n);

// rp를 fd에 연결하고 내부 버퍼를 비운다
void rio_readinitb(rio_t *rp, int fd);

// '\n'까지 한 줄 읽기 (최대 maxlen-1바이트 + '\0'). 읽은 바이트 수, EOF면 0, 실패 -1
ssize_t rio_readlineb(rio_t *rp, void *buf, size_t maxlen);

// 정확히 n바이트 읽기 (헤더 뒤 body용, rio_readlineb와 같은 버퍼를 이어서 사용). 읽은 바이트 수(EOF면 n보다 작음), 실패 -1
ssize_t rio_readnb(rio_t *rp, void *buf, size_t n);

#endif
