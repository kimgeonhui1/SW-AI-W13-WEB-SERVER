// [test_rio.c] Robust I/O 단위 테스트 — socketpair(서로 연결된 소켓 2개)로 네트워크 없이 검증
#include "test.h"
#include "rio.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>

static void make_pair(int sv[2]) {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        perror("socketpair");
        exit(1);
    }
}

// 한 번에 쓴 두 줄 → 한 줄씩 나눠 읽기, 마지막엔 EOF(0)
static void test_readline_splits_lines(void) {
    int sv[2];
    make_pair(sv);
    const char *msg = "GET / HTTP/1.1\r\nHost: a\r\n";
    rio_writen(sv[0], msg, strlen(msg));
    close(sv[0]);

    rio_t rio;
    char line[64];
    rio_readinitb(&rio, sv[1]);
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 16);
    CHECK_STR(line, "GET / HTTP/1.1\r\n");
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 9);
    CHECK_STR(line, "Host: a\r\n");
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 0);   // EOF
    close(sv[1]);
}

// 한 줄이 여러 번에 나뉘어 도착해도(short count) 한 줄로 합쳐 읽기
static void test_readline_joins_fragments(void) {
    int sv[2];
    make_pair(sv);
    rio_writen(sv[0], "GE", 2);
    rio_writen(sv[0], "T /a HT", 7);
    rio_writen(sv[0], "TP/1.0\r\n", 8);
    close(sv[0]);

    rio_t rio;
    char line[64];
    rio_readinitb(&rio, sv[1]);
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 17);
    CHECK_STR(line, "GET /a HTTP/1.0\r\n");
    close(sv[1]);
}

// maxlen보다 긴 줄 → maxlen-1바이트로 잘리고 나머지는 다음 호출에서, 마지막 줄은 '\n' 없이 EOF
static void test_readline_long_line_and_no_newline(void) {
    int sv[2];
    make_pair(sv);
    rio_writen(sv[0], "abcdefghij\nxyz", 14);
    close(sv[0]);

    rio_t rio;
    char line[5];
    rio_readinitb(&rio, sv[1]);
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 4);
    CHECK_STR(line, "abcd");                                // '\n'이 없으면 줄이 잘린 것
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 4);
    CHECK_STR(line, "efgh");
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 3);
    CHECK_STR(line, "ij\n");
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 3);
    CHECK_STR(line, "xyz");                                 // '\n' 없이 EOF로 끝난 마지막 줄
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 0);
    CHECK(rio_readlineb(&rio, line, 0) == -1);              // 버퍼 크기 0
    CHECK(rio_readlineb(&rio, line, 1) == -1);              // '\0' 자리뿐 — EOF(0)와 헷갈리지 않게 -1
    close(sv[1]);
}

// 헤더를 readlineb로 읽은 뒤 body를 readnb로 이어 읽기 — 버퍼에 먼저 들어온 body가 빠지면 안 됨
static void test_readnb_after_readline(void) {
    int sv[2];
    make_pair(sv);
    const char *msg = "POST / HTTP/1.1\r\n\r\nname=kim&age=20";
    rio_writen(sv[0], msg, strlen(msg));
    close(sv[0]);

    rio_t rio;
    char line[64], body[32] = {0};
    rio_readinitb(&rio, sv[1]);
    rio_readlineb(&rio, line, sizeof(line));
    rio_readlineb(&rio, line, sizeof(line));
    CHECK_STR(line, "\r\n");
    CHECK(rio_readnb(&rio, body, 15) == 15);
    CHECK_STR(body, "name=kim&age=20");
    CHECK(rio_readnb(&rio, body, 10) == 0);                 // 더 없음 → EOF
    close(sv[1]);
}

// readnb: 약속한 길이보다 일찍 EOF면 읽은 만큼만 반환
static void test_readnb_short_on_eof(void) {
    int sv[2];
    make_pair(sv);
    rio_writen(sv[0], "abc", 3);
    close(sv[0]);

    rio_t rio;
    char buf[16];
    rio_readinitb(&rio, sv[1]);
    CHECK(rio_readnb(&rio, buf, 10) == 3);
    close(sv[1]);
}

// 소켓 버퍼보다 큰 데이터를 rio_writen으로 보내면 write가 여러 번 나뉘어도 전부 도착해야 함
#define BIG (512 * 1024)
static char big_out[BIG], big_in[BIG];

static void *reader_thread(void *arg) {
    rio_t rio;
    rio_readinitb(&rio, *(int *)arg);
    ssize_t n = rio_readnb(&rio, big_in, BIG);
    return (void *)n;
}

static void test_writen_large(void) {
    int sv[2];
    make_pair(sv);
    for (int i = 0; i < BIG; i++) big_out[i] = (char)(i * 31);

    pthread_t t;
    // 상대가 읽어 줘야 송신 버퍼가 비워진다 — 스레드 생성에 실패하면 writen이 영원히 막히므로 중단
    if (pthread_create(&t, NULL, reader_thread, &sv[1]) != 0) {
        CHECK(!"pthread_create failed");
        close(sv[0]);
        close(sv[1]);
        return;
    }
    CHECK(rio_writen(sv[0], big_out, BIG) == BIG);
    void *ret;
    CHECK(pthread_join(t, &ret) == 0);
    CHECK((ssize_t)ret == BIG);
    CHECK(memcmp(big_out, big_in, BIG) == 0);
    close(sv[0]);
    close(sv[1]);
}

// 개행이 정확히 maxlen-1번째 칸에 오는 경계: "abc\n" + maxlen 5 → 잘리지 않고 한 줄 전체
static void test_readline_newline_at_boundary(void) {
    int sv[2];
    make_pair(sv);
    rio_writen(sv[0], "abc\nd", 5);
    close(sv[0]);

    rio_t rio;
    char line[5];
    rio_readinitb(&rio, sv[1]);
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 4);
    CHECK_STR(line, "abc\n");
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 1);
    CHECK_STR(line, "d");
    close(sv[1]);
}

// ───────────── EINTR: 실제 시그널로 결정적으로 재현 ─────────────
// sleep으로 "잠들었겠지" 추측하지 않고, /proc/self/task/<tid>/syscall로 대상 스레드가
// 실제로 read/write 시스템 콜 안에서 막혀 있는 것을 확인한 뒤 시그널을 보낸다.
// 핸들러 실행 횟수를 세어, 시그널이 실제로 read/write를 끊었는지도 확인한다 (헛통과 방지)
static volatile sig_atomic_t g_usr1_count = 0;
static void on_usr1(int sig) { (void)sig; g_usr1_count++; }

static void install_usr1_no_restart(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                        // SA_RESTART 없음 → 막혀 있던 read/write가 EINTR로 돌아옴
    sigaction(SIGUSR1, &sa, NULL);
}

static pid_t my_tid(void) { return (pid_t)syscall(SYS_gettid); }

// tid 스레드가 시스템 콜 nr 안에서 잠들 때까지 기다린다 (최대 3초). 성공 1
static int wait_in_syscall(pid_t tid, long nr) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/task/%d/syscall", (int)tid);
    for (int i = 0; i < 3000; i++) {
        FILE *f = fopen(path, "r");
        if (f) {
            long cur = -2;
            if (fscanf(f, "%ld", &cur) != 1) cur = -2;
            fclose(f);
            if (cur == nr) return 1;
        }
        usleep(1000);
    }
    return 0;
}

// 핸들러가 count를 넘을 때까지 기다린다 (최대 3초)
static int wait_usr1_count(int target) {
    for (int i = 0; i < 3000 && g_usr1_count < target; i++) usleep(1000);
    return g_usr1_count >= target;
}

enum { EINTR_ROUNDS = 5 };

struct eintr_arg {
    int fd;
    int mode;                               // 0 = readlineb, 1 = readnb
    volatile pid_t tid;
    ssize_t n;
    char buf[64];
};

static void *eintr_reader(void *p) {
    struct eintr_arg *a = p;
    a->tid = my_tid();
    rio_t rio;
    rio_readinitb(&rio, a->fd);
    if (a->mode == 0) a->n = rio_readlineb(&rio, a->buf, sizeof(a->buf));
    else              a->n = rio_readnb(&rio, a->buf, 13);
    return NULL;
}

// 읽기 스레드가 read에서 막힌 상태에서 시그널을 EINTR_ROUNDS번 → 그 뒤 데이터 도착 → 데이터 온전
static void run_read_eintr_case(int mode) {
    int sv[2];
    make_pair(sv);
    install_usr1_no_restart();
    g_usr1_count = 0;

    struct eintr_arg a = { .fd = sv[1], .mode = mode, .tid = 0 };
    pthread_t t;
    if (pthread_create(&t, NULL, eintr_reader, &a) != 0) {
        CHECK(!"pthread_create failed");
        close(sv[0]);
        close(sv[1]);
        return;
    }
    while (a.tid == 0) usleep(100);

    int blocked = 1;
    for (int r = 1; r <= EINTR_ROUNDS; r++) {
        if (!wait_in_syscall(a.tid, SYS_read)) { blocked = 0; break; }   // 다시 read에 들어갔는지
        pthread_kill(t, SIGUSR1);
        if (!wait_usr1_count(r)) { blocked = 0; break; }
    }
    CHECK(blocked);                                         // 매번 실제로 read 안에서 막혀 있었음
    CHECK(g_usr1_count == EINTR_ROUNDS);                    // 시그널이 실제로 EINTR_ROUNDS번 끊음
    CHECK(wait_in_syscall(a.tid, SYS_read));                // EINTR 뒤에도 포기하지 않고 다시 read 중
    rio_writen(sv[0], "after-signal\n", 13);
    CHECK(pthread_join(t, NULL) == 0);

    CHECK(a.n == 13);
    CHECK(memcmp(a.buf, "after-signal\n", 13) == 0);
    close(sv[0]);
    close(sv[1]);
}

static void test_read_retries_on_eintr(void) {
    run_read_eintr_case(0);                                 // rio_readlineb
    run_read_eintr_case(1);                                 // rio_readnb
}

// 이미 읽어 둔 줄 일부("sec")가 내부 버퍼에 있는 상태에서 EINTR → 그 데이터가 사라지거나 중복되지 않음
struct second_line_arg {
    rio_t *rio;
    volatile pid_t tid;
    ssize_t n;
    char buf[64];
};

static void *eintr_second_line(void *p) {
    struct second_line_arg *s = p;
    s->tid = my_tid();
    s->n = rio_readlineb(s->rio, s->buf, sizeof(s->buf));
    return NULL;
}

static void test_eintr_keeps_buffered_data(void) {
    int sv[2];
    make_pair(sv);
    install_usr1_no_restart();
    g_usr1_count = 0;
    rio_writen(sv[0], "first\nsec", 9);                     // 두 번째 줄은 일부만 도착

    rio_t rio;
    char line[64];
    rio_readinitb(&rio, sv[1]);
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 6);    // 이때 "sec"까지 한 번에 버퍼로 들어옴
    CHECK_STR(line, "first\n");

    struct second_line_arg s = { .rio = &rio, .tid = 0 };
    pthread_t t;
    if (pthread_create(&t, NULL, eintr_second_line, &s) != 0) {
        CHECK(!"pthread_create failed");
        close(sv[0]);
        close(sv[1]);
        return;
    }
    while (s.tid == 0) usleep(100);
    CHECK(wait_in_syscall(s.tid, SYS_read));                // "sec"을 꺼낸 뒤 나머지를 기다리며 read에서 막힘
    pthread_kill(t, SIGUSR1);
    CHECK(wait_usr1_count(1));
    rio_writen(sv[0], "ond\n", 4);
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(s.n == 7);
    CHECK_STR(s.buf, "second\n");
    close(sv[0]);
    close(sv[1]);
}

// 상대가 닫은 소켓에 쓰면 -1 (EPIPE). SIGPIPE로 테스트가 죽지 않도록 무시
static void test_writen_closed_peer(void) {
    int sv[2];
    make_pair(sv);
    close(sv[1]);
    signal(SIGPIPE, SIG_IGN);
    CHECK(rio_writen(sv[0], "x", 1) == -1);
    close(sv[0]);
}

// ───────────── 추가 케이스 ─────────────

// 내용 없이 "\n"만 있는 줄, "\r\n"만 있는 줄(HTTP 헤더 끝) → 각각 1, 2바이트
static void test_readline_empty_lines(void) {
    int sv[2];
    make_pair(sv);
    rio_writen(sv[0], "\n\r\n", 3);
    close(sv[0]);

    rio_t rio;
    char line[16];
    rio_readinitb(&rio, sv[1]);
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 1);
    CHECK_STR(line, "\n");
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 2);
    CHECK_STR(line, "\r\n");
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 0);
    close(sv[1]);
}

// maxlen=2 → 한 번에 1글자씩만 담김
static void test_readline_maxlen_two(void) {
    int sv[2];
    make_pair(sv);
    rio_writen(sv[0], "ab\n", 3);
    close(sv[0]);

    rio_t rio;
    char line[2];
    rio_readinitb(&rio, sv[1]);
    CHECK(rio_readlineb(&rio, line, 2) == 1);
    CHECK_STR(line, "a");
    CHECK(rio_readlineb(&rio, line, 2) == 1);
    CHECK_STR(line, "b");
    CHECK(rio_readlineb(&rio, line, 2) == 1);
    CHECK_STR(line, "\n");
    CHECK(rio_readlineb(&rio, line, 2) == 0);
    close(sv[1]);
}

// 한 줄이 내부 버퍼(RIO_BUFSIZE) 경계에 걸쳐 있어도 이어 붙여 읽기 — 버퍼 재충전 경로 검증
static void test_readline_across_buffer_boundary(void) {
    int sv[2];
    make_pair(sv);
    static char data[RIO_BUFSIZE + 100];
    memset(data, 'a', RIO_BUFSIZE - 10);                 // 첫 줄: 버퍼 끝 직전까지
    data[RIO_BUFSIZE - 10] = '\n';
    memset(data + RIO_BUFSIZE - 9, 'b', 50);             // 둘째 줄: 버퍼 경계를 가로지름
    data[RIO_BUFSIZE + 41] = '\n';
    size_t total = RIO_BUFSIZE + 42;
    CHECK(rio_writen(sv[0], data, total) == (ssize_t)total);
    close(sv[0]);

    rio_t rio;
    static char line[RIO_BUFSIZE + 100];
    rio_readinitb(&rio, sv[1]);
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == RIO_BUFSIZE - 9);
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 51);
    CHECK(line[0] == 'b' && line[49] == 'b' && line[50] == '\n');
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 0);
    close(sv[1]);
}

// readnb가 내부 버퍼보다 큰 양(3배)을 요청 → 버퍼를 여러 번 채우며 정확히 다 읽기
static void test_readnb_larger_than_buffer(void) {
    int sv[2];
    make_pair(sv);
    static char out[RIO_BUFSIZE * 3], in[RIO_BUFSIZE * 3];
    for (size_t i = 0; i < sizeof(out); i++) out[i] = (char)(i % 251);
    CHECK(rio_writen(sv[0], out, sizeof(out)) == (ssize_t)sizeof(out));
    close(sv[0]);

    rio_t rio;
    rio_readinitb(&rio, sv[1]);
    CHECK(rio_readnb(&rio, in, sizeof(in)) == (ssize_t)sizeof(in));
    CHECK(memcmp(out, in, sizeof(out)) == 0);
    close(sv[1]);
}

// '\0'이 섞인 바이너리 body도 readnb는 길이만큼 그대로 (문자열 함수처럼 '\0'에서 멈추지 않음)
static void test_readnb_binary_with_nul(void) {
    int sv[2];
    make_pair(sv);
    const char bin[6] = { 'A', '\0', 'B', '\0', '\0', 'C' };
    rio_writen(sv[0], bin, sizeof(bin));
    close(sv[0]);

    rio_t rio;
    char in[6];
    rio_readinitb(&rio, sv[1]);
    CHECK(rio_readnb(&rio, in, 6) == 6);
    CHECK(memcmp(in, bin, 6) == 0);
    close(sv[1]);
}

// keep-alive처럼 요청 두 개가 붙어서 도착: 줄 → body → 줄 → body 순서가 섞이지 않아야 함
static void test_pipelined_requests(void) {
    int sv[2];
    make_pair(sv);
    const char *msg = "POST /a HTTP/1.1\r\n\r\nAAAAPOST /b HTTP/1.1\r\n\r\nBB";
    rio_writen(sv[0], msg, strlen(msg));
    close(sv[0]);

    rio_t rio;
    char line[64], body[8];
    rio_readinitb(&rio, sv[1]);
    rio_readlineb(&rio, line, sizeof(line));
    CHECK_STR(line, "POST /a HTTP/1.1\r\n");
    rio_readlineb(&rio, line, sizeof(line));
    memset(body, 0, sizeof(body));
    CHECK(rio_readnb(&rio, body, 4) == 4);
    CHECK_STR(body, "AAAA");
    rio_readlineb(&rio, line, sizeof(line));
    CHECK_STR(line, "POST /b HTTP/1.1\r\n");
    rio_readlineb(&rio, line, sizeof(line));
    memset(body, 0, sizeof(body));
    CHECK(rio_readnb(&rio, body, 2) == 2);
    CHECK_STR(body, "BB");
    close(sv[1]);
}

// 데이터 없이 EOF → readnb 0, readlineb 0
static void test_read_on_immediate_eof(void) {
    int sv[2];
    make_pair(sv);
    close(sv[0]);

    rio_t rio;
    char buf[8];
    rio_readinitb(&rio, sv[1]);
    CHECK(rio_readnb(&rio, buf, 8) == 0);
    CHECK(rio_readlineb(&rio, buf, sizeof(buf)) == 0);
    close(sv[1]);
}

// 잘못된 fd → read 실패(EBADF) → -1
static void test_read_bad_fd(void) {
    rio_t rio;
    char buf[8];
    rio_readinitb(&rio, -1);
    errno = 0;
    CHECK(rio_readlineb(&rio, buf, sizeof(buf)) == -1);
    CHECK(errno == EBADF);
    CHECK(rio_readnb(&rio, buf, 4) == -1);
    CHECK(rio_writen(-1, "x", 1) == -1);
}

// 0바이트 쓰기 → write를 부르지 않고 0
static void test_writen_zero(void) {
    CHECK(rio_writen(-1, "", 0) == 0);
}

// SO_RCVTIMEO 시간 초과 → readlineb가 -1, errno는 EAGAIN/EWOULDBLOCK (keep-alive 유휴 타임아웃의 기반)
static void test_read_timeout(void) {
    int sv[2];
    make_pair(sv);
    struct timeval tv = { .tv_sec = 0, .tv_usec = 200 * 1000 };
    setsockopt(sv[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    rio_t rio;
    char buf[8];
    rio_readinitb(&rio, sv[1]);
    errno = 0;
    CHECK(rio_readlineb(&rio, buf, sizeof(buf)) == -1);
    CHECK(errno == EAGAIN || errno == EWOULDBLOCK);
    close(sv[0]);
    close(sv[1]);
}

// 타임아웃 전에 받은 부분 줄이 있어도 시간 초과면 -1 (잘린 줄을 정상 줄로 착각하지 않음)
static void test_read_timeout_after_partial_line(void) {
    int sv[2];
    make_pair(sv);
    struct timeval tv = { .tv_sec = 0, .tv_usec = 200 * 1000 };
    setsockopt(sv[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    rio_writen(sv[0], "GET / HT", 8);

    rio_t rio;
    char buf[64];
    rio_readinitb(&rio, sv[1]);
    CHECK(rio_readlineb(&rio, buf, sizeof(buf)) == -1);
    close(sv[0]);
    close(sv[1]);
}

// 서로 다른 rio_t 두 개는 버퍼를 공유하지 않음 (연결마다 rio_t 하나)
static void test_independent_rio_buffers(void) {
    int a[2], b[2];
    make_pair(a);
    make_pair(b);
    rio_writen(a[0], "from-a\n", 7);
    rio_writen(b[0], "from-b\n", 7);
    close(a[0]);
    close(b[0]);

    rio_t ra, rb;
    char la[16], lb[16];
    rio_readinitb(&ra, a[1]);
    rio_readinitb(&rb, b[1]);
    rio_readlineb(&rb, lb, sizeof(lb));
    rio_readlineb(&ra, la, sizeof(la));
    CHECK_STR(la, "from-a\n");
    CHECK_STR(lb, "from-b\n");
    close(a[1]);
    close(b[1]);
}

// 송신 버퍼가 가득 차 write가 막힌 상태에서 시그널을 여러 번(EINTR) → rio_writen이 남은 데이터를 끝까지 보냄
// 순서 확정: [메인] writen이 write에서 막힘 → [poker] 막힌 것을 확인하고 시그널 N번 → 그 뒤에야 [reader]가 비우기 시작
static char wbig_out[BIG], wbig_in[BIG];

struct writer_eintr_ctx {
    int rfd;
    pid_t writer_tid;
    pthread_t writer;
    volatile int go_drain;                  // poker가 시그널을 다 보낸 뒤 1
    volatile int blocked_ok;
    ssize_t n;
};

static void *poke_writer(void *p) {
    struct writer_eintr_ctx *c = p;
    c->blocked_ok = 1;
    for (int r = 1; r <= EINTR_ROUNDS; r++) {
        if (!wait_in_syscall(c->writer_tid, SYS_write)) { c->blocked_ok = 0; break; }
        pthread_kill(c->writer, SIGUSR1);
        if (!wait_usr1_count(r)) { c->blocked_ok = 0; break; }
    }
    c->go_drain = 1;
    return NULL;
}

static void *drain_after_signals(void *p) {
    struct writer_eintr_ctx *c = p;
    for (int i = 0; i < 3000 && !c->go_drain; i++) usleep(1000);
    rio_t rio;
    rio_readinitb(&rio, c->rfd);
    c->n = rio_readnb(&rio, wbig_in, BIG);
    return NULL;
}

static void test_writen_retries_on_eintr(void) {
    int sv[2];
    make_pair(sv);
    for (int i = 0; i < BIG; i++) wbig_out[i] = (char)(i * 7);
    memset(wbig_in, 0, BIG);
    install_usr1_no_restart();
    g_usr1_count = 0;

    struct writer_eintr_ctx c = { .rfd = sv[1], .writer_tid = my_tid(), .writer = pthread_self() };
    pthread_t poker, reader;
    if (pthread_create(&poker, NULL, poke_writer, &c) != 0) {
        CHECK(!"pthread_create failed");
        close(sv[0]);
        close(sv[1]);
        return;
    }
    if (pthread_create(&reader, NULL, drain_after_signals, &c) != 0) {
        CHECK(!"pthread_create failed");
        c.go_drain = 1;
        pthread_join(poker, NULL);
        close(sv[0]);
        close(sv[1]);
        return;
    }
    CHECK(rio_writen(sv[0], wbig_out, BIG) == BIG);         // 512KB > 소켓 버퍼 → 반드시 write에서 막힘
    CHECK(pthread_join(poker, NULL) == 0);
    CHECK(pthread_join(reader, NULL) == 0);
    CHECK(c.blocked_ok);                                    // 매번 실제로 write 안에서 막혀 있었음
    CHECK(g_usr1_count == EINTR_ROUNDS);
    CHECK(c.n == BIG);
    CHECK(memcmp(wbig_out, wbig_in, BIG) == 0);             // 전체 길이 비교: 누락·중복 없음
    close(sv[0]);
    close(sv[1]);
}

// maxlen=1은 -1을 반환하고 입력을 소비하지 않음 (다음 정상 호출에서 그대로 읽힘)
static void test_readline_maxlen_one_does_not_consume(void) {
    int sv[2];
    make_pair(sv);
    rio_writen(sv[0], "hello\n", 6);
    close(sv[0]);

    rio_t rio;
    char line[16];
    rio_readinitb(&rio, sv[1]);
    CHECK(rio_readlineb(&rio, line, 1) == -1);
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 6);
    CHECK_STR(line, "hello\n");
    close(sv[1]);
}

// readnb로 body를 읽은 뒤 같은 rio_t로 readlineb (body 뒤 다음 요청 라인)
static void test_readline_after_readnb(void) {
    int sv[2];
    make_pair(sv);
    rio_writen(sv[0], "BODYnext-line\nX", 15);
    close(sv[0]);

    rio_t rio;
    char body[8] = {0}, line[32];
    rio_readinitb(&rio, sv[1]);
    CHECK(rio_readnb(&rio, body, 4) == 4);
    CHECK_STR(body, "BODY");
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 10);
    CHECK_STR(line, "next-line\n");
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 1);
    CHECK_STR(line, "X");
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 0);
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 0);    // EOF 뒤 반복 호출도 0
    close(sv[1]);
}

// 읽기 전용 fd에 쓰기, 쓰기 전용 fd에서 읽기 → -1 (EBADF)
static void test_wrong_direction_fds(void) {
    int p[2];
    CHECK(pipe(p) == 0);                                    // p[0] 읽기 전용, p[1] 쓰기 전용
    errno = 0;
    CHECK(rio_writen(p[0], "x", 1) == -1 && errno == EBADF);
    rio_t rio;
    char buf[4];
    rio_readinitb(&rio, p[1]);
    errno = 0;
    CHECK(rio_readlineb(&rio, buf, sizeof(buf)) == -1 && errno == EBADF);
    close(p[0]);
    close(p[1]);
}

// 수신 타임아웃(EAGAIN) 뒤에도 같은 rio_t로 이후 데이터를 정상적으로 읽을 수 있음
static void test_read_after_timeout_recovers(void) {
    int sv[2];
    make_pair(sv);
    struct timeval tv = { .tv_sec = 0, .tv_usec = 100 * 1000 };
    setsockopt(sv[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    rio_t rio;
    char line[32];
    rio_readinitb(&rio, sv[1]);
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == -1);   // 아직 데이터 없음 → 시간 초과
    rio_writen(sv[0], "late\n", 5);
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 5);
    CHECK_STR(line, "late\n");
    close(sv[0]);
    close(sv[1]);
}

// 1MB 쓰기 → 다른 스레드가 받아 전체 비교
#define HUGE_N (1024 * 1024)
static char huge_out[HUGE_N], huge_in[HUGE_N];
static void *huge_reader(void *arg) {
    rio_t rio;
    rio_readinitb(&rio, *(int *)arg);
    return (void *)rio_readnb(&rio, huge_in, HUGE_N);
}

static void test_writen_1mb(void) {
    int sv[2];
    make_pair(sv);
    for (int i = 0; i < HUGE_N; i++) huge_out[i] = (char)((i * 131) ^ (i >> 8));
    memset(huge_in, 0, HUGE_N);
    pthread_t t;
    if (pthread_create(&t, NULL, huge_reader, &sv[1]) != 0) {
        CHECK(!"pthread_create failed");
        close(sv[0]);
        close(sv[1]);
        return;
    }
    CHECK(rio_writen(sv[0], huge_out, HUGE_N) == HUGE_N);
    void *ret;
    CHECK(pthread_join(t, &ret) == 0);
    CHECK((ssize_t)ret == HUGE_N);
    CHECK(memcmp(huge_out, huge_in, HUGE_N) == 0);
    close(sv[0]);
    close(sv[1]);
}

// ───────────── 교차 리뷰(Gemini) 제안 반영 ─────────────

// 줄 길이가 maxlen-1과 같은데 '\n'이 아님: maxlen=5, "abcd\n" → "abcd"(4) 다음 "\n"(1)
static void test_readline_exact_fill_without_newline(void) {
    int sv[2];
    make_pair(sv);
    rio_writen(sv[0], "abcd\n", 5);
    close(sv[0]);

    rio_t rio;
    char line[5];
    rio_readinitb(&rio, sv[1]);
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 4);
    CHECK_STR(line, "abcd");
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 1);
    CHECK_STR(line, "\n");
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 0);
    close(sv[1]);
}

// NUL 바이트가 줄 중간에 있어도 readlineb는 '\n'까지 전부 복사 (반환 길이로 판단해야 함, strlen 금지)
static void test_readline_with_nul_byte(void) {
    int sv[2];
    make_pair(sv);
    rio_writen(sv[0], "a\0b\nxy", 6);
    close(sv[0]);

    rio_t rio;
    char line[16];
    rio_readinitb(&rio, sv[1]);
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 4);
    CHECK(memcmp(line, "a\0b\n", 4) == 0);
    CHECK(line[4] == '\0');
    char rest[2];
    CHECK(rio_readnb(&rio, rest, 2) == 2);                 // 그 뒤 바이트도 손실 없이 이어짐
    CHECK(memcmp(rest, "xy", 2) == 0);
    close(sv[1]);
}

// readnb(n=0) → 0, 내부 버퍼 상태는 그대로 (다음 읽기에 영향 없음)
static void test_readnb_zero_keeps_state(void) {
    int sv[2];
    make_pair(sv);
    rio_writen(sv[0], "line1\nREST", 10);
    close(sv[0]);

    rio_t rio;
    char line[16], buf[8] = {0};
    rio_readinitb(&rio, sv[1]);
    rio_readlineb(&rio, line, sizeof(line));               // 이제 버퍼에 "REST"가 남아 있음
    CHECK(rio_readnb(&rio, buf, 0) == 0);
    CHECK(rio_readnb(&rio, buf, 0) == 0);
    CHECK(rio_readnb(&rio, buf, 4) == 4);                  // 내부 상태 대신 실제로 이어 읽히는 데이터로 판정
    CHECK_STR(buf, "REST");
    CHECK(rio_readnb(&rio, buf, 4) == 0);
    close(sv[1]);
}

// 사용하던 rio_t를 다른 fd로 다시 init → 이전 연결의 남은 바이트가 새 연결로 새면 안 됨
static void test_reinit_discards_old_buffer(void) {
    int a[2], b[2];
    make_pair(a);
    make_pair(b);
    rio_writen(a[0], "old1\nOLD-LEFTOVER", 17);
    rio_writen(b[0], "new\n", 4);
    close(a[0]);
    close(b[0]);

    rio_t rio;
    char line[32];
    rio_readinitb(&rio, a[1]);
    rio_readlineb(&rio, line, sizeof(line));               // "old1\n", 버퍼엔 "OLD-LEFTOVER" 남음
    rio_readinitb(&rio, b[1]);
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 4);
    CHECK_STR(line, "new\n");
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 0);
    close(a[1]);
    close(b[1]);
}

// 내부 버퍼 크기(8192) 주변 길이의 줄: 8191+'\n', 8192+'\n', 8193+'\n' — 버퍼 재충전 경계 버그 확인
static void check_line_of_len(size_t len) {
    int sv[2];
    make_pair(sv);
    char *data = malloc(len + 1);
    memset(data, 'x', len);
    data[len] = '\n';

    // 소켓 버퍼에 다 들어가므로 먼저 쓰고 읽음
    CHECK(rio_writen(sv[0], data, len + 1) == (ssize_t)(len + 1));
    close(sv[0]);

    rio_t rio;
    char *line = malloc(len + 16);
    rio_readinitb(&rio, sv[1]);
    CHECK(rio_readlineb(&rio, line, len + 16) == (ssize_t)(len + 1));
    CHECK(line[0] == 'x' && line[len - 1] == 'x' && line[len] == '\n' && line[len + 1] == '\0');
    CHECK(rio_readlineb(&rio, line, len + 16) == 0);
    free(data);
    free(line);
    close(sv[1]);
}

static void test_lines_around_bufsize(void) {
    check_line_of_len(RIO_BUFSIZE - 1);
    check_line_of_len(RIO_BUFSIZE);
    check_line_of_len(RIO_BUFSIZE + 1);
}

// 버퍼 경계를 걸치는 readlineb 직후 readnb → 경계 바이트 누락·중복 없음
static void test_readnb_right_after_boundary_line(void) {
    int sv[2];
    make_pair(sv);
    static char data[RIO_BUFSIZE * 2];
    size_t first = RIO_BUFSIZE + 5;                        // 첫 줄이 첫 버퍼를 넘어감
    memset(data, 'h', first - 1);
    data[first - 1] = '\n';
    for (size_t i = 0; i < 300; i++) data[first + i] = (char)('A' + i % 26);
    CHECK(rio_writen(sv[0], data, first + 300) == (ssize_t)(first + 300));
    close(sv[0]);

    rio_t rio;
    static char line[RIO_BUFSIZE * 2];
    char body[300];
    rio_readinitb(&rio, sv[1]);
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == (ssize_t)first);
    CHECK(rio_readnb(&rio, body, 300) == 300);
    CHECK(memcmp(body, data + first, 300) == 0);
    close(sv[1]);
}

// 줄 중간에 상대가 shutdown(SHUT_WR)(쓰기만 닫음) → 남은 바이트 반환, 다음 호출은 0(EOF)
static void test_readline_peer_shutdown_midline(void) {
    int sv[2];
    make_pair(sv);
    rio_writen(sv[0], "partial", 7);
    shutdown(sv[0], SHUT_WR);

    rio_t rio;
    char line[32];
    rio_readinitb(&rio, sv[1]);
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 7);
    CHECK_STR(line, "partial");
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 0);
    close(sv[0]);
    close(sv[1]);
}

// readnb 대기 중 상대가 약속보다 적게 쓰고 닫음 → 읽은 만큼(< n) 반환, 다음 호출 0
struct short_writer_arg { int fd; };
static void *short_writer(void *p) {
    struct short_writer_arg *a = p;
    usleep(100 * 1000);                     // reader가 먼저 readnb에서 기다리게
    rio_writen(a->fd, "12345", 5);
    shutdown(a->fd, SHUT_WR);
    return NULL;
}

static void test_readnb_peer_closes_early(void) {
    int sv[2];
    make_pair(sv);
    struct short_writer_arg a = { .fd = sv[0] };
    pthread_t t;
    if (pthread_create(&t, NULL, short_writer, &a) != 0) {
        CHECK(!"pthread_create failed");
        close(sv[0]);
        close(sv[1]);
        return;
    }
    rio_t rio;
    char buf[16];
    rio_readinitb(&rio, sv[1]);
    CHECK(rio_readnb(&rio, buf, 10) == 5);                 // Content-Length 10이라 했는데 5만 옴
    CHECK(memcmp(buf, "12345", 5) == 0);
    CHECK(rio_readnb(&rio, buf, 10) == 0);
    pthread_join(t, NULL);
    close(sv[0]);
    close(sv[1]);
}

// 이미 close한 fd로 읽기·쓰기 → -1, 크래시 없음
static void test_closed_fd(void) {
    int sv[2];
    make_pair(sv);
    int dead = sv[1];
    close(sv[0]);
    close(sv[1]);

    rio_t rio;
    char buf[8];
    rio_readinitb(&rio, dead);
    CHECK(rio_readlineb(&rio, buf, sizeof(buf)) == -1);
    CHECK(rio_readnb(&rio, buf, 4) == -1);
    CHECK(rio_writen(dead, "x", 1) == -1);
}

// 상대가 닫은 뒤 여러 청크 연속 전송 → 모두 -1, 프로세스는 살아 있음 (SIGPIPE 무시)
static void test_writen_multiple_chunks_after_close(void) {
    signal(SIGPIPE, SIG_IGN);
    int sv[2];
    make_pair(sv);
    close(sv[1]);
    int fails = 0;
    for (int i = 0; i < 10; i++)
        if (rio_writen(sv[0], "chunk", 5) == -1) fails++;
    CHECK(fails == 10);
    close(sv[0]);
}

int main(void) {
    test_readline_splits_lines();
    test_readline_joins_fragments();
    test_readline_long_line_and_no_newline();
    test_readnb_after_readline();
    test_readnb_short_on_eof();
    test_readline_newline_at_boundary();
    test_read_retries_on_eintr();
    test_writen_large();
    test_writen_closed_peer();
    test_readline_empty_lines();
    test_readline_maxlen_two();
    test_readline_across_buffer_boundary();
    test_readnb_larger_than_buffer();
    test_readnb_binary_with_nul();
    test_pipelined_requests();
    test_read_on_immediate_eof();
    test_read_bad_fd();
    test_writen_zero();
    test_read_timeout();
    test_read_timeout_after_partial_line();
    test_independent_rio_buffers();
    test_writen_retries_on_eintr();
    test_readline_exact_fill_without_newline();
    test_readline_with_nul_byte();
    test_readnb_zero_keeps_state();
    test_reinit_discards_old_buffer();
    test_lines_around_bufsize();
    test_readnb_right_after_boundary_line();
    test_readline_peer_shutdown_midline();
    test_readnb_peer_closes_early();
    test_closed_fd();
    test_writen_multiple_chunks_after_close();
    test_eintr_keeps_buffered_data();
    test_readline_maxlen_one_does_not_consume();
    test_readline_after_readnb();
    test_wrong_direction_fds();
    test_read_after_timeout_recovers();
    test_writen_1mb();
    return TEST_RESULT();
}
