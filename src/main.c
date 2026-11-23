// [main.c] 진입점 — 인자 파싱 → 시그널 설정 → 서버 실행
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "server.h"

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [-p port] [-r root] [-g cgi_dir] [-c routes.conf] [-f] [-t threads] [-q queue_size]\n",
            prog);
}

// 문자열 전체가 min~max 범위의 정수면 1. atoi는 "abc"→0, "80x"→80처럼 틀린 입력을 조용히 받아들이므로 strtol 사용
// strtol은 앞 공백(" 8")과 부호("+8")도 허용하므로, 첫 글자가 숫자가 아니면 먼저 거부한다 (음수는 범위 검사로 걸림)
static int parse_int(const char *s, long min, long max, long *out) {
    char *end;
    if (s[0] < '0' || s[0] > '9') return 0;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < min || v > max) return 0;
    *out = v;
    return 1;
}

// 시그널 핸들러 안에서는 async-signal-safe한 일만 해야 하므로 플래그만 세운다
static void on_stop_signal(int sig) {
    (void)sig;
    request_stop();
}

int main(int argc, char **argv) {
    server_config cfg = {
        .port = "8080",
        .root = "./www",
        .cgi_dir = "./cgi-bin",
        .routes_path = NULL,
        .forward_enabled = 0,
        .threads = 8,
        .queue_size = 1024,
    };
    long v;

    int opt;
    while ((opt = getopt(argc, argv, "p:r:g:c:ft:q:")) != -1) {
        switch (opt) {
        case 'p': cfg.port = optarg; break;
        case 'r': cfg.root = optarg; break;
        case 'g': cfg.cgi_dir = optarg; break;
        case 'c': cfg.routes_path = optarg; break;
        case 'f': cfg.forward_enabled = 1; break;
        case 't':
            if (!parse_int(optarg, 1, 1024, &v)) { fprintf(stderr, "invalid -t: %s (1~1024)\n", optarg); return 1; }
            cfg.threads = (int)v;
            break;
        case 'q':
            if (!parse_int(optarg, 1, 65536, &v)) { fprintf(stderr, "invalid -q: %s (1~65536)\n", optarg); return 1; }
            cfg.queue_size = (int)v;
            break;
        default: usage(argv[0]); return 1;
        }
    }
    if (optind < argc) {                    // 옵션이 아닌 남는 인자 (예: "./server 8080") — 조용히 무시하면 실수를 못 알아챔
        fprintf(stderr, "unexpected argument: %s\n", argv[optind]);
        usage(argv[0]);
        return 1;
    }
    // 포트 범위 검사: 65536 이상은 16비트 포트 칸에 들어가지 않는다 (htons가 조용히 잘라 버림)
    if (!parse_int(cfg.port, 1, 65535, &v)) {
        fprintf(stderr, "invalid -p: %s (1~65535)\n", cfg.port);
        return 1;
    }

    // SIGPIPE 무시: 클라이언트가 끊긴 소켓에 write하면 기본 동작은 프로세스 종료.
    // 무시하면 write가 -1(EPIPE)을 반환하므로 그 연결만 정리하고 서버는 계속 돈다
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);

    // SIGINT(Ctrl+C) / SIGTERM(kill): 종료 플래그만 세운다.
    // SA_RESTART를 빼야 대기 중인 pselect가 자동 재시작되지 않고 EINTR로 깨어나 루프가 플래그를 확인한다
    sa.sa_handler = on_stop_signal;
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (run_server(&cfg) != 0) {
        fprintf(stderr, "server failed to start\n");
        return 1;
    }
    return 0;
}
