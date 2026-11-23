// [threadpool.c] 스레드 풀 구현 — 생산자(메인 스레드: accept → submit) / 소비자(워커 N개: 꺼내서 처리)
// 큐는 여러 스레드가 동시에 건드리므로 mutex로 보호하고, 비었을 때는 조건 변수로 잠든다
#include "threadpool.h"

#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>

struct threadpool {
    pthread_mutex_t lock;
    pthread_cond_t not_empty;   // 큐에 작업이 들어오거나 종료가 시작되면 워커를 깨움
    int *queue;                 // 원형 큐 (connfd 배열): head에서 꺼내고 tail에 넣는다
    int capacity, head, tail, count;
    pthread_t *threads;
    int nthreads;               // 실제로 만들어진 워커 수
    int stopping;               // 1이면 새 작업 거부, 큐가 비면 워커 종료
    task_fn fn;
};

static void *worker_main(void *arg) {
    threadpool *pool = arg;
    for (;;) {
        pthread_mutex_lock(&pool->lock);
        // while로 다시 확인: 깨어났는데 다른 워커가 먼저 가져갔을 수 있다(spurious wakeup 포함)
        while (pool->count == 0 && !pool->stopping)
            pthread_cond_wait(&pool->not_empty, &pool->lock);   // 잠드는 순간 lock을 풀고, 깨면 다시 잡는다
        if (pool->count == 0 && pool->stopping) {                // 종료 중이고 남은 일도 없음 → 끝
            pthread_mutex_unlock(&pool->lock);
            return NULL;
        }
        int connfd = pool->queue[pool->head];
        pool->head = (pool->head + 1) % pool->capacity;
        pool->count--;
        pthread_mutex_unlock(&pool->lock);

        pool->fn(connfd);       // 오래 걸리는 처리는 lock 밖에서 — 그래야 다른 워커·submit이 막히지 않는다
    }
}

// 만든 워커들을 멈추고 자원 해제 (create 실패 정리와 destroy가 공유)
static void stop_and_free(threadpool *pool) {
    pthread_mutex_lock(&pool->lock);
    pool->stopping = 1;
    pthread_cond_broadcast(&pool->not_empty);                   // 잠든 워커 전부 깨워 종료 조건을 보게 함
    pthread_mutex_unlock(&pool->lock);
    for (int i = 0; i < pool->nthreads; i++) pthread_join(pool->threads[i], NULL);

    pthread_cond_destroy(&pool->not_empty);
    pthread_mutex_destroy(&pool->lock);
    free(pool->threads);
    free(pool->queue);
    free(pool);
}

threadpool *threadpool_create(int nthreads, int queue_size, task_fn fn) {
    if (nthreads <= 0 || queue_size <= 0 || fn == NULL) return NULL;

    threadpool *pool = calloc(1, sizeof(*pool));
    if (pool == NULL) return NULL;
    pool->queue = malloc(sizeof(int) * (size_t)queue_size);
    pool->threads = malloc(sizeof(pthread_t) * (size_t)nthreads);
    if (pool->queue == NULL || pool->threads == NULL) {
        free(pool->queue);
        free(pool->threads);
        free(pool);
        return NULL;
    }
    pool->capacity = queue_size;
    pool->fn = fn;
    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->not_empty, NULL);

    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&pool->threads[i], NULL, worker_main, pool) != 0) {
            stop_and_free(pool);                                // 이미 만든 워커만 join하고 실패 반환
            return NULL;
        }
        pool->nthreads++;
    }
    return pool;
}

int threadpool_submit(threadpool *pool, int connfd) {
    pthread_mutex_lock(&pool->lock);
    // 큐가 가득 차면 기다리지 않고 바로 실패 → 메인 스레드가 503으로 거절하고 다음 accept를 계속한다
    // (여기서 기다리면 메인 스레드가 막혀 종료 시그널 처리도 늦어진다)
    if (pool->stopping || pool->count == pool->capacity) {
        pthread_mutex_unlock(&pool->lock);
        return -1;
    }
    pool->queue[pool->tail] = connfd;
    pool->tail = (pool->tail + 1) % pool->capacity;
    pool->count++;
    pthread_cond_signal(&pool->not_empty);                      // 잠든 워커 하나만 깨우면 충분
    pthread_mutex_unlock(&pool->lock);
    return 0;
}

int threadpool_queued(threadpool *pool) {
    pthread_mutex_lock(&pool->lock);
    int n = pool->count;
    pthread_mutex_unlock(&pool->lock);
    return n;
}

void threadpool_destroy(threadpool *pool) {
    if (pool == NULL) return;
    stop_and_free(pool);        // 큐에 남은 연결은 워커가 마저 처리한 뒤 종료 (fd를 닫지 않고 버리지 않음)
}
