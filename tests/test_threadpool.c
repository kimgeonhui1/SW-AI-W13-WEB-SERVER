// [test_threadpool.c] 스레드 풀 단위 테스트 — 작업 정확히 한 번 실행, 실제 병렬 실행, 큐 가득 참, 종료 시 남은 작업 처리
#include "test.h"
#include "threadpool.h"

#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

// ───────────── 공용: 작업 실행 기록 ─────────────
enum { MAX_IDS = 4096 };
static atomic_int g_runs[MAX_IDS];          // id별 실행 횟수 (정확히 1이어야 함)
static atomic_int g_total;

static void reset_counts(void) {
    for (int i = 0; i < MAX_IDS; i++) atomic_store(&g_runs[i], 0);
    atomic_store(&g_total, 0);
}

static void count_task(int id) {
    atomic_fetch_add(&g_runs[id], 1);
    atomic_fetch_add(&g_total, 1);
}

// 큐가 차면 잠깐 기다렸다 다시 넣기 (테스트용 — 서버는 503으로 거절)
static void submit_retry(threadpool *p, int id) {
    while (threadpool_submit(p, id) != 0) usleep(100);
}

// ───────────── 게이트: 작업을 일부러 붙잡아 두기 ─────────────
static pthread_mutex_t g_gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_gate_cond = PTHREAD_COND_INITIALIZER;
static int g_gate_open;
static atomic_int g_started;

static void gated_task(int id) {
    atomic_fetch_add(&g_started, 1);
    pthread_mutex_lock(&g_gate_lock);
    while (!g_gate_open) pthread_cond_wait(&g_gate_cond, &g_gate_lock);
    pthread_mutex_unlock(&g_gate_lock);
    count_task(id);
}

static void gate_set(int open) {
    pthread_mutex_lock(&g_gate_lock);
    g_gate_open = open;
    pthread_cond_broadcast(&g_gate_cond);
    pthread_mutex_unlock(&g_gate_lock);
}

static int wait_started(int n) {            // 최대 3초
    for (int i = 0; i < 3000 && atomic_load(&g_started) < n; i++) usleep(1000);
    return atomic_load(&g_started) >= n;
}

// ───────────── 테스트 ─────────────

// 잘못된 인자 → NULL, destroy(NULL)은 아무 일도 안 함
static void test_invalid_args(void) {
    CHECK(threadpool_create(0, 4, count_task) == NULL);
    CHECK(threadpool_create(4, 0, count_task) == NULL);
    CHECK(threadpool_create(-1, 4, count_task) == NULL);
    CHECK(threadpool_create(4, 4, NULL) == NULL);
    threadpool_destroy(NULL);
}

// 작업 3000개를 넣으면 각각 정확히 한 번씩 실행 (누락·중복 없음 — mutex 보호 확인)
static void test_every_task_runs_once(void) {
    reset_counts();
    threadpool *p = threadpool_create(8, 16, count_task);  // 큐가 작아 submit이 자주 가득 참
    CHECK(p != NULL);
    if (!p) return;
    for (int i = 0; i < 3000; i++) submit_retry(p, i);
    threadpool_destroy(p);                                  // 남은 작업까지 끝난 뒤 반환
    CHECK(atomic_load(&g_total) == 3000);
    int bad = 0;
    for (int i = 0; i < 3000; i++) if (atomic_load(&g_runs[i]) != 1) bad++;
    CHECK(bad == 0);
}

// 워커 4개가 실제로 동시에 실행됨: 4개 작업이 모두 한 지점에 모여야 통과 (순차 실행이면 시간 초과)
static pthread_mutex_t g_meet_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_meet_cond = PTHREAD_COND_INITIALIZER;
static int g_arrived;
static atomic_int g_met;

static void meet_task(int id) {
    (void)id;
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 3;
    pthread_mutex_lock(&g_meet_lock);
    g_arrived++;
    pthread_cond_broadcast(&g_meet_cond);
    int rc = 0;
    while (g_arrived < 4 && rc == 0) rc = pthread_cond_timedwait(&g_meet_cond, &g_meet_lock, &until);
    if (g_arrived >= 4) atomic_fetch_add(&g_met, 1);
    pthread_mutex_unlock(&g_meet_lock);
}

static void test_runs_in_parallel(void) {
    g_arrived = 0;
    atomic_store(&g_met, 0);
    threadpool *p = threadpool_create(4, 8, meet_task);
    CHECK(p != NULL);
    if (!p) return;
    for (int i = 0; i < 4; i++) CHECK(threadpool_submit(p, i) == 0);
    threadpool_destroy(p);
    CHECK(atomic_load(&g_met) == 4);
}

// 워커 1개가 바쁘고 큐(2칸)가 차면 submit은 기다리지 않고 바로 -1 (서버는 이때 503)
static void test_full_queue_rejects(void) {
    reset_counts();
    atomic_store(&g_started, 0);
    gate_set(0);
    threadpool *p = threadpool_create(1, 2, gated_task);
    CHECK(p != NULL);
    if (!p) return;
    CHECK(threadpool_submit(p, 0) == 0);
    CHECK(wait_started(1));                                 // 워커가 0번을 꺼내 게이트에서 대기 중 → 큐는 비어 있음
    CHECK(threadpool_submit(p, 1) == 0);
    CHECK(threadpool_submit(p, 2) == 0);                    // 큐 2칸 가득
    CHECK(threadpool_submit(p, 3) == -1);                   // 즉시 거절 (막히지 않음)
    gate_set(1);
    threadpool_destroy(p);
    CHECK(atomic_load(&g_total) == 3);                      // 받아 준 0,1,2만 실행
    CHECK(atomic_load(&g_runs[3]) == 0);
}

// destroy 시점에 큐에 남아 있던 작업도 버리지 않고 모두 처리 (서버: 받아 둔 연결을 닫지 않고 응답)
static void test_destroy_drains_queue(void) {
    reset_counts();
    atomic_store(&g_started, 0);
    gate_set(0);
    threadpool *p = threadpool_create(1, 16, gated_task);
    CHECK(p != NULL);
    if (!p) return;
    for (int i = 0; i < 10; i++) CHECK(threadpool_submit(p, i) == 0);
    CHECK(wait_started(1));
    gate_set(1);
    threadpool_destroy(p);
    CHECK(atomic_load(&g_total) == 10);
}

// threadpool_queued: 워커가 아직 꺼내 가지 않은(큐에서 기다리는) 작업 수
static void test_queued_count(void) {
    reset_counts();
    atomic_store(&g_started, 0);
    gate_set(0);
    threadpool *p = threadpool_create(1, 8, gated_task);
    CHECK(p != NULL);
    if (!p) return;
    CHECK(threadpool_queued(p) == 0);
    CHECK(threadpool_submit(p, 0) == 0);
    CHECK(wait_started(1));                                 // 0번은 워커가 꺼내 감 → 큐에는 없음
    CHECK(threadpool_queued(p) == 0);
    CHECK(threadpool_submit(p, 1) == 0);
    CHECK(threadpool_submit(p, 2) == 0);
    CHECK(threadpool_queued(p) == 2);
    gate_set(1);
    threadpool_destroy(p);
    CHECK(atomic_load(&g_total) == 3);
}

// 만들고 바로 없애기를 반복해도 문제없음 (작업 없이 잠든 워커를 깨워 종료)
static void test_create_destroy_repeated(void) {
    for (int i = 0; i < 200; i++) {
        threadpool *p = threadpool_create(4, 4, count_task);
        CHECK(p != NULL);
        threadpool_destroy(p);
    }
}

int main(void) {
    test_invalid_args();
    test_every_task_runs_once();
    test_runs_in_parallel();
    test_full_queue_rejects();
    test_destroy_drains_queue();
    test_queued_count();
    test_create_destroy_repeated();
    return TEST_RESULT();
}
