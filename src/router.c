// [router.c] Reverse Proxy 라우팅 구현
#include "router.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

// 포트 문자열: 숫자만, 1~65535
static int valid_port(const char *s) {
    if (*s == '\0' || strlen(s) > 5) return 0;
    long v = 0;
    for (; *s; s++) {
        if (!isdigit((unsigned char)*s)) return 0;
        v = v * 10 + (*s - '0');
    }
    return v >= 1 && v <= 65535;
}

// "host:port" 또는 "[::1]:port" → backend. 성공 0, 형식 오류 -1
static int parse_backend(const char *tok, backend *b) {
    const char *colon = strrchr(tok, ':');                // 마지막 ':' 기준 (IPv6 주소 안의 ':'와 구분)
    if (colon == NULL || colon == tok) return -1;
    const char *h = tok;
    size_t hlen = (size_t)(colon - tok);
    if (h[0] == '[') {                                     // [IPv6]:port → 괄호 제거
        if (hlen < 3 || h[hlen - 1] != ']') return -1;
        h++;
        hlen -= 2;
    }
    if (hlen == 0 || hlen >= sizeof(b->host)) return -1;
    if (strlen(colon + 1) >= sizeof(b->port) || !valid_port(colon + 1)) return -1;
    memcpy(b->host, h, hlen);
    b->host[hlen] = '\0';
    snprintf(b->port, sizeof(b->port), "%s", colon + 1);
    return 0;
}

int parse_route_line(const char *line, route *r) {
    if (line == NULL || r == NULL) return -1;
    char buf[1024];
    if (strlen(line) >= sizeof(buf)) return -1;
    snprintf(buf, sizeof(buf), "%s", line);

    memset(r, 0, sizeof(*r));
    int ntok = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, " \t\r\n", &save); tok != NULL; tok = strtok_r(NULL, " \t\r\n", &save)) {
        if (tok[0] == '#') break;                          // 이후는 주석
        // "/api/#x"처럼 토큰에 붙은 '#': URL 경로에는 '#'가 오지 않아 그 규칙은 영원히 안 맞으므로 설정 실수로 거부
        if (strchr(tok, '#') != NULL) return -1;
        if (ntok == 0) {
            // prefix는 '/'로 시작해야 요청 경로와 비교할 수 있다
            if (tok[0] != '/' || strlen(tok) >= sizeof(r->prefix)) return -1;
            snprintf(r->prefix, sizeof(r->prefix), "%s", tok);
        } else {
            if (r->backend_count >= MAX_BACKENDS) return -1;
            if (parse_backend(tok, &r->backends[r->backend_count]) != 0) return -1;
            r->backend_count++;
        }
        ntok++;
    }
    if (ntok == 0) return 0;                               // 빈 줄·주석
    return r->backend_count > 0 ? 1 : -1;                  // prefix만 있고 백엔드 없음
}

int load_routes(const char *path, route_table *table) {
    if (path == NULL || table == NULL) return -1;
    memset(table, 0, sizeof(*table));
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        perror(path);
        return -1;
    }
    char line[1024];
    int lineno = 0, rc = 0;
    while (fgets(line, sizeof(line), fp) != NULL) {
        lineno++;
        if (strchr(line, '\n') == NULL && !feof(fp)) {     // 한 줄이 버퍼보다 김
            fprintf(stderr, "%s:%d: line too long\n", path, lineno);
            rc = -1;
            break;
        }
        route r;
        int pr = parse_route_line(line, &r);
        if (pr == 0) continue;
        if (pr < 0) {
            fprintf(stderr, "%s:%d: invalid route (format: <prefix> <host:port> [host:port ...])\n", path, lineno);
            rc = -1;
            break;
        }
        // 같은 prefix가 두 번이면 뒤 규칙은 영원히 쓰이지 않는다 → 설정 실수로 보고 거부
        int dup = 0;
        for (int i = 0; i < table->count; i++) dup |= (strcmp(table->routes[i].prefix, r.prefix) == 0);
        if (dup) {
            fprintf(stderr, "%s:%d: duplicate prefix %s\n", path, lineno, r.prefix);
            rc = -1;
            break;
        }
        if (table->count >= MAX_ROUTES) {
            fprintf(stderr, "%s:%d: too many routes (max %d)\n", path, lineno, MAX_ROUTES);
            rc = -1;
            break;
        }
        table->routes[table->count++] = r;
    }
    fclose(fp);
    return rc;
}

route *match_route(route_table *table, const char *path) {
    if (table == NULL || path == NULL) return NULL;
    route *best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < table->count; i++) {
        size_t len = strlen(table->routes[i].prefix);
        // 가장 긴 prefix 우선: "/api/"와 "/api/v2/"가 둘 다 맞으면 더 구체적인 "/api/v2/"
        if (len > best_len && strncmp(path, table->routes[i].prefix, len) == 0) {
            best = &table->routes[i];
            best_len = len;
        }
    }
    return best;
}

const backend *next_backend(route *r) {
    if (r == NULL || r->backend_count <= 0) return NULL;
    return &r->backends[next_backend_index(r)];
}

unsigned int next_backend_index(route *r) {
    if (r == NULL || r->backend_count <= 0) return 0;
    // 여러 워커가 동시에 부르므로 원자적 증가 (락 없이 카운터만 공유). 순서 보장만 필요해 RELAXED로 충분
    unsigned int n = __atomic_fetch_add(&r->next, 1, __ATOMIC_RELAXED);
    return n % (unsigned int)r->backend_count;
}
