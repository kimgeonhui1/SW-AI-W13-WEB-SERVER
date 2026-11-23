// [main.c] 진입점 — 인자 파싱 → 시그널 설정 → 서버 실행
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "server.h"

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [-p port] [-r root] [-g cgi_dir] [-c routes.conf] [-f] [-t threads] [-q queue_size]\n",
            prog);
}

int main(int argc, char **argv) {
    server_config cfg = {
        .port = "8080",
        .root = "./www",
        .cgi_dir = "./cgi-bin",
        .routes_path = NULL,
        .forward_enabled = 0,
        .threads = 8,
        .queue_size = 64,
    };

    int opt;
    while ((opt = getopt(argc, argv, "p:r:g:c:ft:q:")) != -1) {
        switch (opt) {
        case 'p': cfg.port = optarg; break;
        case 'r': cfg.root = optarg; break;
        case 'g': cfg.cgi_dir = optarg; break;
        case 'c': cfg.routes_path = optarg; break;
        case 'f': cfg.forward_enabled = 1; break;
        case 't': cfg.threads = atoi(optarg); break;
        case 'q': cfg.queue_size = atoi(optarg); break;
        default: usage(argv[0]); return 1;
        }
    }

    // TODO: 인자 검사 — 포트 1~65535 (htons는 큰 값을 조용히 잘라 버림), threads·queue_size 양수
    // TODO: 시그널 설정
    //   - SIGPIPE 무시: 끊긴 소켓에 write하면 프로세스가 죽는 것을 막고 write가 -1(EPIPE)을 반환하게 함
    //   - SIGINT/SIGTERM: sigaction으로 request_stop 등록. SA_RESTART를 빼야 accept가 EINTR로 깨어남

    if (run_server(&cfg) != 0) {
        fprintf(stderr, "server failed to start (not implemented yet)\n");
        return 1;
    }
    return 0;
}
