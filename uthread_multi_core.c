#define _GNU_SOURCE
#include <ucontext.h>
#include <sched.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/syscall.h>

#define MAX_USER_THREADS 1024
#define KERNEL_STACK_SZ (64*1024)
#define USER_STACK_SZ (128*1024)

typedef enum {
    UTHREAD_READY,
    UTHREAD_RUNNING,
    UTHREAD_TERMINATED
} uthread_state_t;

typedef struct uthread {
    ucontext_t ctx;
    void *stack;
    int id;
    atomic_int state;
    atomic_int owned;  // Track which worker owns this thread
    struct uthread *next;
} uthread_t;

typedef struct kworker {
    int id;
    char *kstack;
    uthread_t *current;
    atomic_int running;
    ucontext_t sched_ctx;

    uthread_t *ready_queue;
    atomic_int queue_lock;

    struct kworker *next_worker;
} kworker_t;

/* ---------------- globals ---------------- */
static kworker_t *workers = NULL;
static int worker_count = 0;
static atomic_int active_threads = 0;
static atomic_int workers_should_run = 1;
__thread kworker_t *self_worker = NULL;

static uthread_t *all_threads[MAX_USER_THREADS];
static atomic_int next_tid = 1;

/* ---------------- atomic queue operations ---------------- */
static void enqueue_thread(kworker_t *worker, uthread_t *t) {
    if (!worker || !t) return;
    if (atomic_load(&t->state) != UTHREAD_READY) return;

    // Release ownership for threads that are READY
    atomic_store(&t->owned, 0);

    while (atomic_exchange(&worker->queue_lock, 1)) sched_yield();

    t->next = NULL;
    if (!worker->ready_queue) {
        worker->ready_queue = t;
    } else {
        uthread_t *last = worker->ready_queue;
        while (last->next) last = last->next;
        last->next = t;
    }

    atomic_store(&worker->queue_lock, 0);

    // LOG
    printf("[Worker %d] Enqueued Thread %d (owned=%d)\n",
        worker->id, t->id, atomic_load(&t->owned));
}

static uthread_t* dequeue_thread(kworker_t *worker) {
    if (!worker) return NULL;

    while (atomic_exchange(&worker->queue_lock, 1)) sched_yield();

    uthread_t *t = worker->ready_queue;
    uthread_t *prev = NULL;
    uthread_t *found = NULL;

    while (t) {
        uthread_state_t st = atomic_load(&t->state);
        if (st == UTHREAD_READY) {
            int expected_owned = 0;
            if (atomic_compare_exchange_strong(&t->owned, &expected_owned, 1)) {
                found = t;
                if (prev) prev->next = t->next;
                else worker->ready_queue = t->next;
                found->next = NULL;

                // LOG
                printf("[Worker %d] Dequeued Thread %d (owned=%d)\n",
                    worker->id, found->id, atomic_load(&found->owned));

                break;
            }
        }
        prev = t;
        t = t->next;
    }

    atomic_store(&worker->queue_lock, 0);
    return found;
}


/* ---------------- atomic work stealing ---------------- */
static uthread_t* steal_work(kworker_t *stealer) {
    if (!stealer || !stealer->next_worker) return NULL;

    kworker_t *target = stealer->next_worker;
    kworker_t *start = target;

    do {
        uthread_t *stolen = dequeue_thread(target);
        if (stolen) {
            printf("[Worker %d] Stole Thread %d from Worker %d\n",
                   stealer->id, stolen->id, target->id);
            return stolen;
        }
        target = target->next_worker;
    } while (target != start);

    return NULL;
}


/* ---------------- user thread APIs ---------------- */
int uthread_create(void (*fn)(void *), void *arg) {
    int tid = atomic_fetch_add(&next_tid, 1);
    if (tid >= MAX_USER_THREADS) return -1;

    uthread_t *t = malloc(sizeof(*t));
    if (!t) return -1;
    memset(t, 0, sizeof(*t));

    if (getcontext(&t->ctx) == -1) { free(t); return -1; }

    t->stack = malloc(USER_STACK_SZ);
    if (!t->stack) { free(t); return -1; }

    t->ctx.uc_stack.ss_sp = t->stack;
    t->ctx.uc_stack.ss_size = USER_STACK_SZ;
    t->ctx.uc_link = NULL;
    makecontext(&t->ctx, (void(*)(void))fn, 1, arg);

    t->id = tid;
    atomic_store(&t->state, UTHREAD_READY);
    atomic_store(&t->owned, 0);

    all_threads[tid] = t;
    atomic_fetch_add(&active_threads, 1);

    static atomic_int next_worker = 0;
    int worker_idx = atomic_fetch_add(&next_worker, 1) % worker_count;
    enqueue_thread(&workers[worker_idx], t);

    printf("Created user thread %d -> Worker %d\n", tid, worker_idx);
    return tid;
}

void uthread_yield(void) {
    kworker_t *worker = self_worker;
    uthread_t *cur = worker ? worker->current : NULL;
    if (!cur || atomic_load(&cur->state) != UTHREAD_RUNNING) return;

    atomic_store(&cur->state, UTHREAD_READY);
    enqueue_thread(worker, cur);

    worker->current = NULL;
    swapcontext(&cur->ctx, &worker->sched_ctx);
}

void uthread_exit(void) {
    kworker_t *worker = self_worker;
    uthread_t *cur = worker ? worker->current : NULL;
    if (!cur) return;

    printf("[user %d] exiting on worker %d\n", cur->id, worker->id);

    // Mark terminated **first** and keep ownership
    atomic_store(&cur->state, UTHREAD_TERMINATED);

    atomic_fetch_sub(&active_threads, 1);
    worker->current = NULL;

    setcontext(&worker->sched_ctx);
}

/* ---------------- kernel worker ---------------- */
static void worker_loop(kworker_t *kw) {
    printf("Worker %d starting\n", kw->id);

    if (getcontext(&kw->sched_ctx) == -1) {
        printf("Worker %d: getcontext failed\n", kw->id);
        return;
    }
    kw->sched_ctx.uc_stack.ss_sp = kw->kstack;
    kw->sched_ctx.uc_stack.ss_size = KERNEL_STACK_SZ;
    kw->sched_ctx.uc_link = NULL;

    while (atomic_load(&workers_should_run)) {
        // Try to get a local thread
        uthread_t *t = dequeue_thread(kw);

        // Work stealing if no local thread
        if (!t) t = steal_work(kw);

        if (!t) {
            // No threads available
            if (atomic_load(&active_threads) == 0 && !atomic_load(&workers_should_run))
                break;
            usleep(1000);
            continue;
        }

        // Mark as running
        atomic_store(&t->state, UTHREAD_RUNNING);
        kw->current = t;

        // LOG
        printf("Worker %d => Thread %d (owned=%d)\n",
               kw->id, t->id, atomic_load(&t->owned));

        // Switch to user thread
        if (swapcontext(&kw->sched_ctx, &t->ctx) == -1) {
            printf("Worker %d: swapcontext failed for Thread %d\n", kw->id, t->id);
            atomic_store(&t->state, UTHREAD_TERMINATED);
            atomic_store(&t->owned, 0);
            atomic_fetch_sub(&active_threads, 1);
            free(t->stack);
            free(t);
            kw->current = NULL;
            continue;
        }

        // Back from user thread
        printf("Worker %d <= Thread %d (state=%d, owned=%d)\n",
               kw->id, t->id, atomic_load(&t->state), atomic_load(&t->owned));

        int state = atomic_load(&t->state);
        if (state == UTHREAD_TERMINATED) {
            int expected_owned = 1;
            if (atomic_compare_exchange_strong(&t->owned, &expected_owned, 0)) {
                printf("[Worker %d] Cleaning up Thread %d\n", kw->id, t->id);
                free(t->stack);
                all_threads[t->id] = NULL;
                free(t);
            }
        } else if (state == UTHREAD_READY) {
            // Thread yielded
            atomic_store(&t->owned, 0);
            printf("[Worker %d] Thread %d yielded (owned released)\n", kw->id, t->id);
        } else {
            // Unexpected running state
            atomic_store(&t->state, UTHREAD_READY);
            atomic_store(&t->owned, 0);
            enqueue_thread(kw, t);
            printf("[Worker %d] Thread %d re-enqueued (state reset)\n", kw->id, t->id);
        }

        kw->current = NULL;
    }

    printf("Worker %d stopping\n", kw->id);
}


static int worker_trampoline(void *arg) {
    self_worker = (kworker_t*)arg;
    worker_loop(self_worker);
    return 0;
}

int start_kernel_workers(int n) {
    worker_count = n;
    workers = calloc(n, sizeof(kworker_t));
    if (!workers) return -1;

    for (int i = 0; i < n; ++i) {
        workers[i].id = i;
        workers[i].kstack = malloc(KERNEL_STACK_SZ);
        if (!workers[i].kstack) return -1;
        workers[i].current = NULL;
        workers[i].ready_queue = NULL;
        atomic_store(&workers[i].queue_lock, 0);
        workers[i].next_worker = &workers[(i+1)%n];
    }

    for (int i = 0; i < n; ++i) {
        int flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                    CLONE_THREAD | CLONE_SYSVSEM;
        void *stack_top = workers[i].kstack + KERNEL_STACK_SZ;
        if (clone(worker_trampoline, stack_top, flags, &workers[i]) == -1) {
            perror("clone");
            return -1;
        }
    }

    sleep(1);
    return 0;
}

void stop_kernel_workers(void) {
    atomic_store(&workers_should_run, 0);
}

void user_fn(void *arg) {
    int id = *(int*)arg;
    kworker_t *worker = self_worker;
    int worker_id = worker ? worker->id : -1;

    printf("[user %d] starting on worker %d\n", id, worker_id);

    for (int i = 0; i < 2; ++i) {
        printf("[user %d] iteration %d on worker %d\n", id, i, worker_id);
        for (volatile long j = 0; j < 300000; ++j);
        uthread_yield();
    }

    printf("[user %d] finished\n", id);
    uthread_exit();
}

int main(void) {
    printf("M:N Threading - Atomic Ownership + Work Stealing\n");
    printf("================================================\n");

    int N = 4;
    if (start_kernel_workers(N) == -1) return 1;

    int M = 8;
    int ids[] = {1,2,3,4,5,6,7,8};
    printf("Creating %d user threads distributed across %d workers...\n", M, N);
    for (int i = 0; i < M; i++) {
        if (uthread_create(user_fn, &ids[i]) == -1) {
            fprintf(stderr, "Failed to create user thread %d\n", i);
            return 1;
        }
    }

    printf("Waiting for completion...\n");
    for (int i = 0; i < 15; i++) {
        int remaining = atomic_load(&active_threads);
        printf("Main: %d threads remaining\n", remaining);
        if (remaining == 0) {
            printf("All threads completed!\n");
            break;
        }
        sleep(1);
    }

    printf("Stopping workers...\n");
    stop_kernel_workers();
    sleep(2);

    for (int i = 0; i < worker_count; i++) free(workers[i].kstack);
    free(workers);

    printf("Demo completed!\n");
    return 0;
}
