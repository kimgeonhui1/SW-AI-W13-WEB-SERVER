// [router.c] Reverse Proxy 라우팅 구현
#include "router.h"
#include <stddef.h>

int parse_route_line(const char *line, route *r) {
    (void)line; (void)r;
    // TODO: 공백으로 나눠 첫 토큰 = prefix, 나머지 = "host:port" (':' 기준 분리)
    //       백엔드가 0개거나 MAX_BACKENDS 초과, ':' 없음, 포트가 1~65535 밖이면 -1
    return -1;
}

int load_routes(const char *path, route_table *table) {
    (void)path; (void)table;
    return -1; // TODO: fopen → fgets로 한 줄씩 parse_route_line. 형식 오류면 몇 번째 줄인지 출력하고 -1
}

route *match_route(route_table *table, const char *path) {
    (void)table; (void)path;
    return NULL; // TODO: strncmp로 prefix 비교, 가장 긴 prefix 반환
}

const backend *next_backend(route *r) {
    (void)r;
    return NULL; // TODO: __atomic_fetch_add(&r->next, 1, __ATOMIC_RELAXED) % backend_count
}
