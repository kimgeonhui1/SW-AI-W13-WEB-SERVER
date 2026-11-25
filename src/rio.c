// [rio.c] Robust I/O 구현 — 네트워크 read/write는 쪼개져서 오가므로 원하는 만큼 될 때까지 반복한다
#include "rio.h"

#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

double rio_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

ssize_t rio_writen(int fd, const void *buf, size_t n) {
    const char *p = buf;
    size_t left = n;

    // write는 소켓 송신 버퍼에 들어간 만큼만 반환할 수 있다(short count) → 남은 양이 0이 될 때까지 반복
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;   // 시그널 때문에 중간에 끊김 → 에러가 아니므로 다시 시도
            return -1;                      // EPIPE(상대가 끊음) 등 진짜 에러
        }
        if (w == 0) {                       // 진행이 없으면 같은 호출을 무한 반복하게 되므로 에러로 끝낸다
            errno = EIO;
            return -1;
        }
        left -= (size_t)w;
        p += w;
    }
    return (ssize_t)n;
}

void rio_readinitb(rio_t *rp, int fd) {
    rp->fd = fd;
    rp->cnt = 0;
    rp->bufp = rp->buf;
    rp->deadline = 0;
}

// 내부 버퍼에서 최대 n바이트를 꺼낸다. 버퍼가 비어 있을 때만 read를 한 번 호출해 크게 채운다.
// 한 줄을 읽을 때 바이트마다 read(시스템 콜)를 부르지 않게 하는 것이 목적
static ssize_t rio_read(rio_t *rp, char *usrbuf, size_t n) {
    while (rp->cnt <= 0) {
        // 마감 시각 확인은 실제로 소켓에서 더 받아야 할 때만 — 버퍼에 남은 데이터는 시간과 상관없이 꺼내 준다.
        // read마다 검사하므로 1바이트씩 천천히 보내는 클라이언트(slowloris)도 줄 중간에서 끊긴다
        if (rp->deadline > 0 && rio_now() > rp->deadline) {
            errno = ETIMEDOUT;
            return -1;
        }
        rp->cnt = read(rp->fd, rp->buf, sizeof(rp->buf));
        if (rp->cnt < 0) {
            if (errno == EINTR) continue;   // 시그널로 끊김 → 다시 read
            return -1;                      // 에러 또는 SO_RCVTIMEO 시간 초과(EAGAIN)
        }
        if (rp->cnt == 0) return 0;         // EOF: 상대가 연결을 닫음(FIN)
        rp->bufp = rp->buf;
    }

    size_t cnt = n < (size_t)rp->cnt ? n : (size_t)rp->cnt;
    memcpy(usrbuf, rp->bufp, cnt);
    rp->bufp += cnt;
    rp->cnt -= (ssize_t)cnt;
    return (ssize_t)cnt;
}

int rio_waitb(rio_t *rp) {
    char c;
    if (rio_read(rp, &c, 0) < 0) return -1;   // n = 0: 버퍼가 비었으면 채우기만 하고 아무것도 꺼내지 않음
    return rp->cnt > 0 ? 1 : 0;
}

ssize_t rio_readlineb(rio_t *rp, void *buf, size_t maxlen) {
    char *p = buf;
    size_t n;

    if (maxlen <= 1) return -1;             // 글자를 하나도 담을 수 없음 (0을 반환하면 EOF와 구분 안 됨)

    // 최대 maxlen-1바이트까지 복사. 줄이 더 길면 잘린 채로 반환되고 나머지는 다음 호출에서 나온다
    // → 호출한 쪽은 "끝이 '\n'이 아니고 길이가 maxlen-1"이면 줄이 너무 긴 것으로 판단할 수 있다
    for (n = 1; n < maxlen; n++) {
        char c;
        ssize_t r = rio_read(rp, &c, 1);
        if (r == 1) {
            *p++ = c;
            if (c == '\n') {
                n++;
                break;
            }
        } else if (r == 0) {
            if (n == 1) return 0;           // 한 글자도 못 읽고 EOF
            break;                          // 마지막 줄에 '\n' 없이 EOF
        } else {
            return -1;
        }
    }
    *p = '\0';
    return (ssize_t)(n - 1);
}

ssize_t rio_readnb(rio_t *rp, void *buf, size_t n) {
    char *p = buf;
    size_t left = n;

    // rio_readlineb와 같은 내부 버퍼를 이어서 쓴다 → 헤더를 읽을 때 미리 받아 둔 body 앞부분도 빠지지 않음
    while (left > 0) {
        ssize_t r = rio_read(rp, p, left);
        if (r < 0) return -1;
        if (r == 0) break;                  // EOF: 약속한 길이보다 일찍 끝남
        left -= (size_t)r;
        p += r;
    }
    return (ssize_t)(n - left);
}
