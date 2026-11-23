// [test_net.c] open_listenfd / open_clientfd 단위 테스트 — 실제 TCP 소켓으로 로컬(127.0.0.1)에서 검증
#include "test.h"
#include "net.h"
#include "rio.h"

#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

// 이 프로세스가 지금 연 fd 개수 (/proc/self/fd 항목 수) — fd 누수 검사용
static int count_open_fds(void) {
    DIR *d = opendir("/proc/self/fd");
    if (!d) return -1;
    int n = 0;
    while (readdir(d)) n++;
    closedir(d);
    return n;
}

// 듣기 소켓의 실제 포트 번호를 문자열로 (포트 "0"으로 열면 커널이 빈 포트를 골라 준다)
static void port_of(int fd, char *out, size_t len) {
    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    getsockname(fd, (struct sockaddr *)&ss, &sl);
    unsigned port = ss.ss_family == AF_INET6 ? ntohs(((struct sockaddr_in6 *)&ss)->sin6_port)
                                             : ntohs(((struct sockaddr_in *)&ss)->sin_port);
    snprintf(out, len, "%u", port);
}

// 듣기 소켓 열기 → 클라이언트 연결 → accept → 데이터 왕복
static void test_listen_connect_roundtrip(void) {
    char port[8];
    int lfd = open_listenfd("0");
    CHECK(lfd >= 0);
    port_of(lfd, port, sizeof(port));

    int cfd = open_clientfd("127.0.0.1", port);
    CHECK(cfd >= 0);
    int afd = accept(lfd, NULL, NULL);                     // handshake는 connect 때 커널이 이미 끝냄
    CHECK(afd >= 0);

    // TCP는 read 한 번에 4바이트가 다 온다는 보장이 없으므로 rio로 정확히 4바이트 읽기
    char buf[8] = {0};
    rio_t rio;
    rio_readinitb(&rio, afd);
    CHECK(rio_writen(cfd, "ping", 4) == 4);
    CHECK(rio_readnb(&rio, buf, 4) == 4);
    CHECK_STR(buf, "ping");

    // CGI 자식이 물려받지 않도록 FD_CLOEXEC가 켜져 있어야 함
    CHECK((fcntl(lfd, F_GETFD) & FD_CLOEXEC) != 0);
    CHECK((fcntl(cfd, F_GETFD) & FD_CLOEXEC) != 0);

    close(afd);
    close(cfd);
    close(lfd);
}

// "localhost" 이름으로도 연결 (getaddrinfo가 이름 → 주소 변환)
static void test_connect_by_name(void) {
    char port[8];
    int lfd = open_listenfd("0");
    port_of(lfd, port, sizeof(port));
    int cfd = open_clientfd("localhost", port);
    CHECK(cfd >= 0);
    if (cfd >= 0) close(cfd);
    close(lfd);
}

// 이미 다른 소켓이 listen 중인 포트 → 실패 (SO_REUSEADDR는 TIME_WAIT만 허용, 사용 중인 포트는 아님)
static void test_listen_port_in_use(void) {
    char port[8];
    int lfd = open_listenfd("0");
    port_of(lfd, port, sizeof(port));
    fprintf(stderr, "  (아래 bind 에러 메시지는 예상된 것)\n");
    CHECK(open_listenfd(port) == -1);
    close(lfd);
}

// 서버가 없는 포트 / 없는 호스트 / 잘못된 포트 문자열 → -1 (프로세스는 계속 산다)
static void test_connect_failures(void) {
    char port[8];
    int lfd = open_listenfd("0");
    port_of(lfd, port, sizeof(port));
    close(lfd);                                             // 이제 아무도 듣지 않는 포트

    CHECK(open_clientfd("127.0.0.1", port) == -1);          // ECONNREFUSED
    CHECK(open_clientfd("no-such-host.invalid", "80") == -1); // DNS 실패 (.invalid는 존재할 수 없는 도메인)
    CHECK(open_clientfd("127.0.0.1", "http") == -1);        // AI_NUMERICSERV: 서비스 이름 금지
    fprintf(stderr, "  (아래 getaddrinfo 에러 메시지는 예상된 것)\n");
    CHECK(open_listenfd("abc") == -1);
}

// ───────────── 추가 케이스 ─────────────

// open_listenfd가 돌려준 소켓은 실제로 LISTEN 상태이고 SO_REUSEADDR가 켜져 있어야 함
static void test_listen_socket_options(void) {
    int lfd = open_listenfd("0");
    CHECK(lfd >= 0);
    int val = 0;
    socklen_t len = sizeof(val);
    CHECK(getsockopt(lfd, SOL_SOCKET, SO_ACCEPTCONN, &val, &len) == 0 && val == 1);
    val = 0;
    len = sizeof(val);
    CHECK(getsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &val, &len) == 0 && val != 0);
    val = 0;
    len = sizeof(val);
    CHECK(getsockopt(lfd, SOL_SOCKET, SO_TYPE, &val, &len) == 0 && val == SOCK_STREAM);   // TCP
    close(lfd);
}

// 와일드카드 주소(0.0.0.0 또는 ::)에 bind되어야 모든 IP로 오는 연결을 받음
static void test_listen_on_wildcard(void) {
    int lfd = open_listenfd("0");
    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    getsockname(lfd, (struct sockaddr *)&ss, &sl);
    if (ss.ss_family == AF_INET)
        CHECK(((struct sockaddr_in *)&ss)->sin_addr.s_addr == htonl(INADDR_ANY));
    else
        CHECK(memcmp(&((struct sockaddr_in6 *)&ss)->sin6_addr, &in6addr_any, sizeof(in6addr_any)) == 0);
    close(lfd);
}

// accept 전에 여러 클라이언트가 동시에 connect → handshake는 커널이 끝내 대기 큐에 쌓이고, 나중에 전부 accept 가능
static void test_backlog_queue(void) {
    enum { N = 50 };
    char port[8];
    int lfd = open_listenfd("0");
    port_of(lfd, port, sizeof(port));

    int cfds[N], ok = 0;
    for (int i = 0; i < N; i++) {
        cfds[i] = open_clientfd("127.0.0.1", port);       // 아직 아무도 accept 안 함
        if (cfds[i] >= 0) ok++;
    }
    CHECK(ok == N);

    int accepted = 0;
    for (int i = 0; i < N; i++) {
        int afd = accept(lfd, NULL, NULL);
        if (afd >= 0) {
            accepted++;
            close(afd);
        }
    }
    CHECK(accepted == N);
    for (int i = 0; i < N; i++) if (cfds[i] >= 0) close(cfds[i]);
    close(lfd);
}

// 성공·실패 경로를 100번씩 반복해도 fd 개수가 늘지 않아야 함 (실패한 후보 소켓을 닫는지)
static void test_no_fd_leak(void) {
    char port[8], dead_port[8];
    int lfd = open_listenfd("0");
    port_of(lfd, port, sizeof(port));
    int tmp = open_listenfd("0");
    port_of(tmp, dead_port, sizeof(dead_port));
    close(tmp);                                             // dead_port: 아무도 안 듣는 포트

    int before = count_open_fds();
    fprintf(stderr, "  (아래 bind 에러 메시지 100줄 생략 위해 stderr 잠시 닫음)\n");
    int saved_err = dup(2);
    int devnull = open("/dev/null", O_WRONLY);
    dup2(devnull, 2);
    for (int i = 0; i < 100; i++) {
        int c = open_clientfd("127.0.0.1", port);           // 성공 → accept → 둘 다 닫기
        int a = accept(lfd, NULL, NULL);
        close(c);
        close(a);
        open_clientfd("127.0.0.1", dead_port);              // 실패 경로 (ECONNREFUSED)
        open_clientfd("no-such-host.invalid", "80");        // 실패 경로 (DNS)
        open_listenfd(port);                                // 실패 경로 (EADDRINUSE)
    }
    dup2(saved_err, 2);
    close(saved_err);
    close(devnull);
    CHECK(count_open_fds() == before);
    close(lfd);
}

// 상대가 close하면 이쪽 read는 0(EOF), 이후 write는 -1(EPIPE) — 서버의 "클라이언트 끊김" 판단 근거
static void test_peer_close_eof_and_epipe(void) {
    char port[8];
    int lfd = open_listenfd("0");
    port_of(lfd, port, sizeof(port));
    int cfd = open_clientfd("127.0.0.1", port);
    int afd = accept(lfd, NULL, NULL);
    close(cfd);

    rio_t rio;
    char buf[8];
    rio_readinitb(&rio, afd);
    CHECK(rio_readlineb(&rio, buf, sizeof(buf)) == 0);    // FIN → EOF

    signal(SIGPIPE, SIG_IGN);
    int got_err = 0;
    for (int i = 0; i < 5 && !got_err; i++) {              // 첫 write는 RST를 받기 전이라 성공할 수 있음
        if (rio_writen(afd, "x", 1) < 0) got_err = 1;
        usleep(20 * 1000);
    }
    CHECK(got_err);
    close(afd);
    close(lfd);
}

// 1023 이하 포트는 root가 아니면 bind 불가 (EACCES) — 권한 있는 환경이면 건너뜀
static void test_privileged_port(void) {
    FILE *f = fopen("/proc/sys/net/ipv4/ip_unprivileged_port_start", "r");
    int start = 1024;
    if (f) {
        if (fscanf(f, "%d", &start) != 1) start = 1024;
        fclose(f);
    }
    if (geteuid() == 0 || start <= 1) {
        fprintf(stderr, "  (root 또는 낮은 포트 허용 환경 — privileged port 테스트 건너뜀)\n");
        return;
    }
    fprintf(stderr, "  (아래 bind 에러 메시지는 예상된 것)\n");
    CHECK(open_listenfd("1") == -1);
}

// 포트 문자열 형식 오류 → -1 (AI_NUMERICSERV)
static void test_bad_port_strings(void) {
    int saved_err = dup(2);
    int devnull = open("/dev/null", O_WRONLY);
    dup2(devnull, 2);
    CHECK(open_listenfd("") == -1);
    CHECK(open_listenfd("8080abc") == -1);
    CHECK(open_clientfd("127.0.0.1", "") == -1);
    CHECK(open_clientfd("127.0.0.1", "-1") == -1);
    dup2(saved_err, 2);
    close(saved_err);
    close(devnull);
}

// set_cloexec: 일반 파일 fd에도 FD_CLOEXEC가 켜짐
static void test_set_cloexec(void) {
    int fd = open("/dev/null", O_RDONLY);
    CHECK((fcntl(fd, F_GETFD) & FD_CLOEXEC) == 0);
    set_cloexec(fd);
    CHECK((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
    close(fd);
}

// ───────────── 교차 리뷰(Gemini) 제안 반영 ─────────────

// 듣기 소켓 여러 개를 "0"으로 열면 서로 다른 포트, 각각 독립적으로 accept
static void test_multiple_listeners(void) {
    enum { N = 5 };
    int lfds[N];
    char ports[N][8];
    for (int i = 0; i < N; i++) {
        lfds[i] = open_listenfd("0");
        CHECK(lfds[i] >= 0);
        port_of(lfds[i], ports[i], sizeof(ports[i]));
    }
    for (int i = 0; i < N; i++)
        for (int j = i + 1; j < N; j++) CHECK(strcmp(ports[i], ports[j]) != 0);

    for (int i = 0; i < N; i++) {
        int c = open_clientfd("127.0.0.1", ports[i]);
        int a = accept(lfds[i], NULL, NULL);
        CHECK(c >= 0 && a >= 0);
        char msg[8], got[8] = {0};
        snprintf(msg, sizeof(msg), "L%d", i);
        rio_t rio;
        rio_readinitb(&rio, a);
        rio_writen(c, msg, strlen(msg));
        CHECK(rio_readnb(&rio, got, strlen(msg)) == (ssize_t)strlen(msg));
        CHECK_STR(got, msg);                                // 다른 듣기 소켓으로 새지 않음
        close(c);
        close(a);
        close(lfds[i]);
    }
}

// 여러 클라이언트(IP / 이름 섞어서) 동시 연결 → 연결마다 양방향 데이터가 섞이지 않음
static void test_many_clients_no_mixing(void) {
    enum { N = 10 };
    char port[8];
    int lfd = open_listenfd("0");
    port_of(lfd, port, sizeof(port));

    int cfds[N], afds[N];
    for (int i = 0; i < N; i++) {
        cfds[i] = open_clientfd(i % 2 ? "localhost" : "127.0.0.1", port);
        afds[i] = accept(lfd, NULL, NULL);                  // accept 순서 = connect 순서
        CHECK(cfds[i] >= 0 && afds[i] >= 0);
    }
    for (int i = 0; i < N; i++) {                           // 클라이언트 → 서버
        char msg[16];
        snprintf(msg, sizeof(msg), "client-%02d\n", i);
        rio_writen(cfds[i], msg, strlen(msg));
    }
    for (int i = N - 1; i >= 0; i--) {                      // 역순으로 읽어도 각자 자기 것만
        char expect[16], got[16];
        snprintf(expect, sizeof(expect), "client-%02d\n", i);
        rio_t rio;
        rio_readinitb(&rio, afds[i]);
        rio_readlineb(&rio, got, sizeof(got));
        CHECK_STR(got, expect);
        snprintf(expect, sizeof(expect), "server-%02d\n", i); // 서버 → 클라이언트
        rio_writen(afds[i], expect, strlen(expect));
        rio_readinitb(&rio, cfds[i]);
        rio_readlineb(&rio, got, sizeof(got));
        CHECK_STR(got, expect);
    }
    for (int i = 0; i < N; i++) {
        close(cfds[i]);
        close(afds[i]);
    }
    close(lfd);
}

// 연결을 맺고(서버가 먼저 close → TIME_WAIT 생김) 듣기 소켓을 닫은 뒤, 같은 포트로 바로 다시 열기
static void test_rebind_after_close_with_time_wait(void) {
    char port[8];
    int lfd = open_listenfd("0");
    port_of(lfd, port, sizeof(port));
    for (int i = 0; i < 3; i++) {
        int c = open_clientfd("127.0.0.1", port);
        int a = accept(lfd, NULL, NULL);
        close(a);                                           // 서버 쪽이 먼저 닫음 → 서버 쪽 TIME_WAIT
        usleep(10 * 1000);
        close(c);
    }
    close(lfd);
    usleep(50 * 1000);
    int again = open_listenfd(port);                        // SO_REUSEADDR 없으면 EADDRINUSE
    CHECK(again >= 0);
    if (again >= 0) close(again);
}

// NULL·빈 문자열·범위 밖 포트·빈 호스트 → 크래시 없이 -1
static void test_invalid_arguments(void) {
    char port[8];
    int lfd = open_listenfd("0");
    port_of(lfd, port, sizeof(port));                       // 실제로 열려 있는 포트

    int saved_err = dup(2);
    int devnull = open("/dev/null", O_WRONLY);
    dup2(devnull, 2);
    CHECK(open_listenfd(NULL) == -1);
    CHECK(open_listenfd("") == -1);                         // getaddrinfo는 ""를 포트 0으로 받아들임 → 직접 거부
    CHECK(open_listenfd("65536") == -1);
    CHECK(open_listenfd(" 80") == -1);
    CHECK(open_listenfd(" 0") == -1);                       // 검사가 없으면 getaddrinfo가 포트 0으로 받아 "성공"해 버리는 입력
    CHECK(open_listenfd("+0") == -1);
    CHECK(open_listenfd("0x") == -1);
    CHECK(open_clientfd(NULL, port) == -1);                 // NULL이면 getaddrinfo가 localhost로 접속해 버림 → 직접 거부
    CHECK(open_clientfd("", port) == -1);
    CHECK(open_clientfd("127.0.0.1", NULL) == -1);
    CHECK(open_clientfd("127.0.0.1", "") == -1);
    CHECK(open_clientfd("127.0.0.1", "0") == -1);           // 포트 0은 접속 대상이 아님
    CHECK(open_clientfd("127.0.0.1", "-1") == -1);
    CHECK(open_clientfd("127.0.0.1", "65536") == -1);       // getaddrinfo는 잘라서 받아들임 → 직접 거부
    CHECK(open_clientfd("127.0.0.1", "70000") == -1);
    CHECK(open_clientfd("127.0.0.1", "abc") == -1);
    CHECK(open_clientfd("127.0.0.1", "+80") == -1);
    dup2(saved_err, 2);
    close(saved_err);
    close(devnull);
    close(lfd);
}

int main(void) {
    test_listen_connect_roundtrip();
    test_connect_by_name();
    test_listen_port_in_use();
    test_connect_failures();
    test_listen_socket_options();
    test_listen_on_wildcard();
    test_backlog_queue();
    test_no_fd_leak();
    test_peer_close_eof_and_epipe();
    test_privileged_port();
    test_bad_port_strings();
    test_set_cloexec();
    test_multiple_listeners();
    test_many_clients_no_mixing();
    test_rebind_after_close_with_time_wait();
    test_invalid_arguments();
    return TEST_RESULT();
}
