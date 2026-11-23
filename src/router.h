// [router.h] Reverse Proxy 라우팅 — 경로 prefix로 백엔드 그룹을 고르고, 그룹 안에서 Round-Robin
//
// 설정 파일 형식 (conf/routes.conf): 한 줄에 규칙 하나, '#'은 주석
//   <경로 prefix> <host:port> [host:port ...]
//   /api/ 127.0.0.1:8001 127.0.0.1:8002
#ifndef ROUTER_H
#define ROUTER_H

#define MAX_ROUTES   16
#define MAX_BACKENDS 8

typedef struct {
    char host[256];
    char port[8];
} backend;

typedef struct {
    char prefix[256];                    // 이 문자열로 시작하는 경로가 대상
    backend backends[MAX_BACKENDS];
    int backend_count;
    unsigned int next;                   // Round-Robin 카운터. 여러 워커 스레드가 동시에 증가시키므로 atomic 사용
} route;

typedef struct {
    route routes[MAX_ROUTES];
    int count;
} route_table;

// 설정 파일 한 줄 파싱. 규칙이면 1, 빈 줄·주석이면 0, 형식 오류면 -1
int parse_route_line(const char *line, route *r);

// 설정 파일 전체를 읽어 table 채우기. 성공 0, 실패 -1
int load_routes(const char *path, route_table *table);

// path에 맞는 규칙 찾기. 여러 개 맞으면 prefix가 가장 긴 것. 없으면 NULL
route *match_route(route_table *table, const char *path);

// Round-Robin으로 다음 백엔드 선택 (스레드 안전)
const backend *next_backend(route *r);

#endif
