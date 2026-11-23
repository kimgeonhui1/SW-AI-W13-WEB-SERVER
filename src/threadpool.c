// [threadpool.c] 스레드 풀 구현
// 큐는 여러 스레드가 동시에 건드리므로 mutex로 보호하고, 비었을 때·찼을 때는 조건 변수로 잠든다
#include "threadpool.h"
#include <pthread.h>
#include <stddef.h>

struct threadpool {
    pthread_mutex_t lock;
    pthread_cond_t not_empty;   // 큐에 작업이 들어오면 워커를 깨움
    int *queue;                 // 원형 큐 (connfd 배열)
    int capacity, head, tail, count;
    pthread_t *threads;
    int nthreads;
    int stopping;               // 1이면 새 작업 거부, 큐가 비면 워커 종료
    task_fn fn;
};

threadpool *threadpool_create(int nthreads, int queue_size, task_fn fn) {
    (void)nthreads; (void)queue_size; (void)fn;
    // TODO: malloc → mutex/cond 초기화 → pthread_create로 워커 생성 (워커: 큐에서 꺼내 fn(connfd) 반복)
    return NULL;
}

int threadpool_submit(threadpool *pool, int connfd) {
    (void)pool; (void)connfd;
    return -1; // TODO: lock → 가득 찼으면 -1 → tail에 넣기 → signal(not_empty) → unlock
}

void threadpool_destroy(threadpool *pool) {
    (void)pool;
    // TODO: lock → stopping = 1 → broadcast → unlock → 모든 워커 join → free
}
