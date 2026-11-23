// [cgi.c] CGI 실행 구현
// 흐름: 프로그램 확인 → 환경변수 준비(fork 전에) → 파이프 2개 → fork → 자식: stdin/stdout 연결 후 execve
//       → 부모: 본문 쓰기와 출력 읽기를 poll로 동시에(교착 방지) → 시간·크기 제한 → 자식 회수 → 출력 검사 → 응답
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   // memmem
#endif

#include "cgi.h"
#include "http_response.h"
#include "net.h"
#include "rio.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// /cgi-bin/ 뒤의 이름: 비어 있지 않고 '/'가 없어야 한다 ("..", 하위 폴더, cgi_dir 밖으로의 탈출 차단)
static int valid_script_name(const char *name) {
    size_t len = strlen(name);
    if (len == 0 || len > 255) return 0;
    if (strchr(name, '/') != NULL) return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
    return 1;
}

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

// 자식이 끝나기를 deadline까지 기다리고, 넘으면 강제 종료한 뒤 회수한다(좀비 방지). 시간 초과였으면 1
// (stdout만 닫고 계속 실행 중인 CGI도 있으므로 무작정 waitpid로 막히면 안 된다)
static int reap_child(pid_t pid, double deadline, int *wstatus) {
    for (;;) {
        pid_t r = waitpid(pid, wstatus, WNOHANG);
        if (r == pid) return 0;
        if (r < 0 && errno != EINTR) return 0;           // 이미 회수됨(예외적) — 더 기다릴 것 없음
        if (rio_now() > deadline) {
            kill(-pid, SIGKILL);                         // 프로세스 그룹 전체 (CGI가 띄운 자식까지)
            while (waitpid(pid, wstatus, 0) < 0 && errno == EINTR) {}
            return 1;
        }
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 2 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
}

// CGI 출력의 헤더 부분을 해석한다. 성공하면 0과 함께 상태·Content-Type·본문 위치를 채운다. 형식 오류면 -1
static int parse_cgi_output(char *out, size_t len, int *status, char *ctype, size_t ctype_cap,
                            const char **body, size_t *body_len) {
    // 헤더 끝: "\r\n\r\n" 또는 "\n\n" 중 먼저 나오는 것 (CGI 프로그램은 LF만 쓰기도 한다)
    char *crlf = memmem(out, len, "\r\n\r\n", 4);
    char *lf = memmem(out, len, "\n\n", 2);
    char *end = NULL;
    size_t sep = 0;
    if (crlf != NULL && (lf == NULL || crlf < lf)) { end = crlf; sep = 4; }
    else if (lf != NULL) { end = lf; sep = 2; }
    if (end == NULL) return -1;

    *status = 200;
    ctype[0] = '\0';
    *end = '\0';                                            // 헤더 부분만 문자열로
    char *save = NULL;
    for (char *line = strtok_r(out, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save)) {
        size_t l = strlen(line);
        if (l > 0 && line[l - 1] == '\r') line[--l] = '\0';
        char *colon = strchr(line, ':');
        if (colon == NULL) return -1;                       // "이름: 값" 형식이 아닌 줄
        *colon = '\0';
        char *val = colon + 1;
        while (*val == ' ' || *val == '\t') val++;
        if (strcasecmp(line, "Content-Type") == 0) {
            if (strlen(val) >= ctype_cap || val[0] == '\0') return -1;
            snprintf(ctype, ctype_cap, "%s", val);
        } else if (strcasecmp(line, "Status") == 0) {
            char *e = NULL;
            long code = strtol(val, &e, 10);
            if (e == val || code < 200 || code > 599 || (*e != '\0' && *e != ' ')) return -1;
            *status = (int)code;
        }
        // shortcut: Set-Cookie·Location 등 다른 CGI 헤더는 버린다. 필요해지면 응답에 그대로 붙이는 기능 추가
    }
    if (ctype[0] == '\0') return -1;                        // CGI 명세상 Content-Type은 필수
    *body = end + sep;
    *body_len = len - (size_t)((end + sep) - out);
    return 0;
}

int serve_cgi(int fd, const char *cgi_dir, const http_request *req) {
    if (fd < 0 || cgi_dir == NULL || req == NULL) return 500;
    int is_head = (strcmp(req->method, "HEAD") == 0);
    if (!is_head && strcmp(req->method, "GET") != 0 && strcmp(req->method, "POST") != 0) return 405;

    // 1. 실행할 프로그램 확인
    const char *name = req->path + strlen(CGI_PREFIX);
    if (!valid_script_name(name)) return 404;
    char prog[PATH_MAX];
    int w = snprintf(prog, sizeof(prog), "%s/%s", cgi_dir, name);
    if (w < 0 || (size_t)w >= sizeof(prog)) return 414;
    struct stat st;
    if (stat(prog, &st) != 0) return (errno == ENOENT || errno == ENOTDIR) ? 404 : (errno == EACCES ? 403 : 500);
    if (!S_ISREG(st.st_mode)) return 403;
    if (access(prog, X_OK) != 0) return 403;

    // 2. 환경변수와 인자는 fork 전에 만든다 — 멀티스레드 프로세스에서 fork한 자식은 exec 전까지
    //    malloc·setenv·printf 같은 함수를 쓰면 안 된다(다른 스레드가 잡고 있던 lock이 잠긴 채 복제될 수 있음)
    char e_method[32], e_query[MAX_URI_LEN + 16], e_len[64], e_ctype[MAX_HEADER_VALUE + 16];
    char e_script[MAX_URI_LEN + 16], e_proto[32];
    snprintf(e_method, sizeof(e_method), "REQUEST_METHOD=%s", req->method);
    snprintf(e_query, sizeof(e_query), "QUERY_STRING=%s", req->query);
    snprintf(e_len, sizeof(e_len), "CONTENT_LENGTH=%zu", req->body != NULL ? req->content_length : (size_t)0);
    snprintf(e_script, sizeof(e_script), "SCRIPT_NAME=%s", req->path);
    snprintf(e_proto, sizeof(e_proto), "SERVER_PROTOCOL=%s", req->version);
    const char *ct = get_header(req, "Content-Type");
    snprintf(e_ctype, sizeof(e_ctype), "CONTENT_TYPE=%s", ct != NULL ? ct : "");
    char *envp[] = { "GATEWAY_INTERFACE=CGI/1.1", e_proto, e_method, e_query, e_len, e_ctype, e_script,
                     "PATH=/usr/local/bin:/usr/bin:/bin", NULL };
    char *argv[] = { prog, NULL };

    // 3. 파이프: in = 서버 → CGI 표준 입력(POST 본문), out = CGI 표준 출력 → 서버
    //    CLOEXEC: 다른 워커가 동시에 fork한 CGI에 이 파이프가 새면 EOF가 오지 않는다
    //    shortcut: pipe와 set_cloexec 사이 틈은 남는다(macOS에 pipe2 없음). Linux 전용이면 pipe2(O_CLOEXEC)
    int in[2], out[2];
    if (pipe(in) != 0) return 500;
    if (pipe(out) != 0) {
        close(in[0]);
        close(in[1]);
        return 500;
    }
    set_cloexec(in[0]); set_cloexec(in[1]); set_cloexec(out[0]); set_cloexec(out[1]);

    pid_t pid = fork();
    if (pid < 0) {
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        return 503;                                         // 프로세스 한도 등 일시적 자원 부족
    }
    if (pid == 0) {
        // ── 자식: async-signal-safe 함수만 사용 ──
        // 워커에서 물려받은 시그널 설정을 원래대로: SIGINT/SIGTERM 막힘 해제, SIGPIPE 무시 해제
        // (막힘·무시 설정은 exec 뒤에도 남아 CGI 프로그램이 Ctrl+C·끊긴 파이프에 반응하지 않게 된다)
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        struct sigaction dfl;
        memset(&dfl, 0, sizeof(dfl));
        dfl.sa_handler = SIG_DFL;
        sigaction(SIGPIPE, &dfl, NULL);
        // 자기만의 프로세스 그룹: 시간 초과 때 kill(-pid)로 CGI가 띄운 자식(셸 스크립트의 sleep 등)까지 한 번에 종료
        setpgid(0, 0);
        // dup2로 만든 0·1번 fd는 CLOEXEC가 꺼져 exec 뒤에도 남고, 원래 파이프 fd들은 exec 때 닫힌다
        if (dup2(in[0], STDIN_FILENO) < 0 || dup2(out[1], STDOUT_FILENO) < 0) _exit(127);
        execve(prog, argv, envp);
        _exit(127);                                         // exec 실패 → 부모가 500으로 판단
    }

    // ── 부모 ──
    setpgid(pid, pid);                                      // 자식과 같은 설정: 아래 kill(-pid)가 자식의 setpgid보다 먼저 와도 그룹이 존재
    close(in[0]);
    close(out[1]);
    set_nonblock(in[1]);
    set_nonblock(out[0]);

    // 4. 본문 쓰기와 출력 읽기를 동시에: 하나씩 하면 CGI가 출력을 많이 내며 파이프가 차는 동안
    //    서버는 입력을 쓰려고 기다리는 교착이 생길 수 있다
    const char *body_in = req->body;
    size_t in_left = (req->body != NULL) ? req->content_length : 0;
    int in_fd = in[1];
    if (in_left == 0) { close(in_fd); in_fd = -1; }         // 본문 없음 → 바로 EOF를 알림

    size_t cap = 8192, len = 0;
    char *outbuf = malloc(cap + 1);
    int timed_out = 0, too_big = 0, alloc_fail = (outbuf == NULL);
    double deadline = rio_now() + CGI_TIMEOUT_SEC;

    while (!alloc_fail) {
        struct pollfd pfd[2];
        int n = 0;
        pfd[n].fd = out[0]; pfd[n].events = POLLIN; pfd[n].revents = 0; n++;
        if (in_fd >= 0) { pfd[n].fd = in_fd; pfd[n].events = POLLOUT; pfd[n].revents = 0; n++; }
        int wait_ms = (int)((deadline - rio_now()) * 1000);
        if (wait_ms <= 0) { timed_out = 1; break; }
        int pr = poll(pfd, (nfds_t)n, wait_ms);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) { timed_out = 1; break; }

        if (in_fd >= 0 && (pfd[1].revents & (POLLOUT | POLLERR | POLLHUP))) {
            ssize_t wr = write(in_fd, body_in, in_left);
            if (wr > 0) { body_in += wr; in_left -= (size_t)wr; }
            // 다 썼거나, CGI가 입력을 안 읽고 닫음(EPIPE, SIGPIPE는 무시 상태) → 입력 쪽 닫기
            if (in_left == 0 || (wr < 0 && errno != EAGAIN && errno != EINTR)) { close(in_fd); in_fd = -1; }
        }
        if (pfd[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            // 버퍼 상한을 1MB + 1로 둬서 "정확히 1MB(허용)"와 "1MB 초과"를 구별한다
            if (len == cap) {
                if (cap > CGI_MAX_OUTPUT) { too_big = 1; break; }
                size_t ncap = cap * 2 > CGI_MAX_OUTPUT + 1 ? CGI_MAX_OUTPUT + 1 : cap * 2;
                char *nb = realloc(outbuf, ncap + 1);
                if (nb == NULL) { alloc_fail = 1; break; }
                outbuf = nb;
                cap = ncap;
            }
            ssize_t rd = read(out[0], outbuf + len, cap - len);
            if (rd > 0) len += (size_t)rd;
            else if (rd == 0) break;                        // CGI가 표준 출력을 닫음 → 출력 끝
            else if (errno != EAGAIN && errno != EINTR) break;
        }
    }
    if (in_fd >= 0) close(in_fd);
    close(out[0]);
    if (len > CGI_MAX_OUTPUT) too_big = 1;                  // 1MB + 1바이트째에서 EOF가 난 경우

    // 5. 자식 회수 (시간 초과·출력 과다면 강제 종료)
    int wstatus = 0;
    if (timed_out || too_big || alloc_fail) {
        kill(-pid, SIGKILL);
        while (waitpid(pid, &wstatus, 0) < 0 && errno == EINTR) {}
    } else if (reap_child(pid, deadline, &wstatus)) {
        timed_out = 1;                                      // 출력은 닫았는데 계속 실행 중이던 CGI
    }

    int rc;
    if (alloc_fail) rc = 500;
    else if (timed_out) rc = 504;
    else if (too_big) rc = 502;
    else if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 127) rc = 500;   // exec 실패
    else if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) rc = 502;    // 비정상 종료
    else {
        // 6. 출력 검사 후 응답 (Content-Length는 서버가 계산)
        int status;
        char ctype[256];
        const char *body;
        size_t body_len;
        if (parse_cgi_output(outbuf, len, &status, ctype, sizeof(ctype), &body, &body_len) != 0) {
            rc = 502;
        } else if (is_head) {
            rc = send_headers(fd, status, ctype, body_len, req->keep_alive) == 0 ? 0 : -1;
        } else {
            rc = send_response(fd, status, ctype, body, body_len, req->keep_alive) == 0 ? 0 : -1;
        }
    }
    free(outbuf);
    return rc;
}
