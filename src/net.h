// [net.h] 소켓 연결 도우미 — getaddrinfo 후보 목록을 돌며 듣기 소켓 / 클라이언트 소켓을 연다 (CSAPP 11.4.8)
#ifndef NET_H
#define NET_H

// 서버용 듣기 소켓: AI_PASSIVE로 0.0.0.0:port 주소를 받아 socket → SO_REUSEADDR → bind → listen. 성공 fd, 실패 -1
int open_listenfd(const char *port);

// host:port 에 연결된 클라이언트 소켓 (프록시 → 백엔드용). 성공 fd, 실패 -1
// 실패해도 프로세스를 종료하지 않는다 — 호출한 쪽이 502로 응답해야 하므로
int open_clientfd(const char *host, const char *port);

// fd에 FD_CLOEXEC 설정 — CGI(fork + execve) 자식이 이 fd를 물려받지 않게 한다 (accept로 받은 fd에도 사용)
void set_cloexec(int fd);

#endif
