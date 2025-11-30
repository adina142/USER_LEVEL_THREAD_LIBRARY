#define _XOPEN_SOURCE 700
#include <ucontext.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/time.h>
#include <pthread.h>
#include <unistd.h>
#include <sched.h>

#define STACK_SZ (1024*128)
#define MAX_THREADS 256
#define MAX_KERNEL_THREADS 4
#define MAX_PRIORITY 10

// Thread states
typedef enum {
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_TERMINATED,
    THREAD_WAITING
} thread_state_t;

// Forward declarations
typedef struct uthread_mutex uthread_mutex_t;
typedef struct uthread_cond uthread_cond_t;

// Thread structure
typedef struct uthread {
    ucontext_t ctx;
    void *stack;
    int id;
    thread_state_t state;
    int priority;
    int age;
    long exit_value;  // Changed to store value directly
    int joined_by;
    struct uthread *next;
    struct uthread *prev;
    
    // For condition variables
    int cond_waiting;
    uthread_cond_t *waiting_cond;
    uthread_mutex_t *waiting_mutex;
} uthread_t;

// Mutex structure
struct uthread_mutex {
    atomic_int locked;
    uthread_t *waiting;
    pthread_mutex_t kernel_mutex;
};

// Condition variable structure
struct uthread_cond {
    uthread_t *waiting;
    pthread_cond_t kernel_cond;
};

// Kernel thread wrapper for M:N
typedef struct {
    int id;
    pthread_t thread;
    uthread_t *current_thread;
    atomic_int running;
    uthread_mutex_t *queue_mutex;
    uthread_cond_t *queue_cond;
} kernel_thread_t;

// Global variables
static atomic_int threads_created = 0;
static atomic_int threads_terminated = 0;
static atomic_int context_switches = 0;
static atomic_int voluntary_yields = 0;
static atomic_int preemptions = 0;
static atomic_int mutex_blocks = 0;

static uthread_t *ready_queue = NULL;
static uthread_t *all_threads[MAX_THREADS];
static int next_thread_id = 1;

static kernel_thread_t kernel_threads[MAX_KERNEL_THREADS];
static int kernel_thread_count = 0;
static atomic_int scheduler_running = 0;
static pthread_mutex_t ready_queue_lock = PTHREAD_MUTEX_INITIALIZER;

static ucontext_t scheduler_ctx;
static int scheduler_done = 0;
static int scheduling_policy = 0;
static volatile sig_atomic_t preemption_enabled = 0;
static struct itimerval timer;
static int multi_core_enabled = 0;

static void add_to_ready_queue(uthread_t *thread) {
    if (!thread) return;

    thread->next = NULL;
    
   
    if (!ready_queue) {
        ready_queue = thread;
        thread->prev = NULL;
    } else {
        uthread_t *tail = ready_queue;
        while (tail->next) tail = tail->next;
        tail->next = thread;
        thread->prev = tail;
    }
   
}
uthread_t* pop_ready_queue(void) {

    if (!ready_queue) return NULL;

    if (scheduling_policy == 0) { // Round-Robin
       
        uthread_t *t = ready_queue;
        ready_queue = t->next;
        if (ready_queue) ready_queue->prev = NULL;
        t->next = t->prev = NULL;
        return t;
    } else { // Priority + aging
        uthread_t *t = ready_queue;
        uthread_t *highest = ready_queue;

        // Find thread with highest (priority + age)
        for (; t; t = t->next) {
            if ((t->priority + t->age) > (highest->priority + highest->age)) {
                highest = t;
            }
        }

        // Remove highest from queue
        if (highest->prev) highest->prev->next = highest->next;
        else ready_queue = highest->next;

        if (highest->next) highest->next->prev = highest->prev;

        highest->next = highest->prev = NULL;

        // Increment age of remaining threads
        for (t = ready_queue; t; t = t->next) {
            t->age++;
        }
        highest->age = 0;

        return highest;
    }
}
static uthread_t* pop_ready_queue_priority(void) {
    if (!ready_queue) return NULL;

    uthread_t *best = ready_queue;
    uthread_t *iter = ready_queue->next;

    // Find thread with max (priority + age)
    while (iter) {
        if ((iter->priority + iter->age) > (best->priority + best->age)) {
            best = iter;
        }
        iter = iter->next;
    }

    // Remove 'best' from ready queue
    if (best->prev) best->prev->next = best->next;
    else ready_queue = best->next;

    if (best->next) best->next->prev = best->prev;

    best->next = best->prev = NULL;

    // Increment age for all other threads
    for (iter = ready_queue; iter; iter = iter->next) {
        iter->age++;
    }

    // Reset age of selected thread
    best->age = 0;

    return best;
}



// Function declarations
int uthread_create(void (*fn)(void *), void *arg);
int uthread_create_priority(void (*fn)(void *), void *arg, int priority);
void uthread_yield(void);
void uthread_exit(void *retval);
int uthread_join(int tid, void **retval);
int uthread_self(void);

void uthread_set_scheduling(int policy);
void uthread_enable_preempt(void);
void uthread_disable_preempt(void);
void uthread_enable_multicore(void);
void uthread_set_affinity(int tid, int core);

int uthread_mutex_init(uthread_mutex_t **mutex);
int uthread_mutex_lock(uthread_mutex_t *mutex);
int uthread_mutex_trylock(uthread_mutex_t *mutex);
int uthread_mutex_unlock(uthread_mutex_t *mutex);
int uthread_mutex_destroy(uthread_mutex_t *mutex);

void uthread_print_stats(void);
void uthread_reset_stats(void);
int uthread_get_created_count(void);
int uthread_get_terminated_count(void);
int uthread_get_active_count(void);

static void schedule(void);
static void *kernel_thread_wrapper(void *arg);
static void timer_handler(int sig);
static void cleanup_thread(uthread_t *thread);

// Current thread pointer - simplified for single-core
static uthread_t *current_thread = NULL;
// ==================== TERMINATED THREAD QUEUE ====================

typedef struct terminated_node {
    uthread_t *thread;
    struct terminated_node *next;
} terminated_node_t;

static terminated_node_t *terminated_head = NULL;
static terminated_node_t *terminated_tail = NULL;

// Add a terminated thread to the list (defer cleanup)
void add_to_terminated_list(uthread_t *t) {
    terminated_node_t *node = malloc(sizeof(terminated_node_t));
    node->thread = t;
    node->next = NULL;

    if (!terminated_head) {
        terminated_head = terminated_tail = node;
    } else {
        terminated_tail->next = node;
        terminated_tail = node;
    }
}

// Pop all terminated threads for cleanup
uthread_t *pop_terminated_list(void) {
    if (!terminated_head) return NULL;

    terminated_node_t *node = terminated_head;
    uthread_t *t = node->thread;

    terminated_head = node->next;
    if (!terminated_head) terminated_tail = NULL;

    free(node);
    return t;
}


// ==================== CORE THREAD FUNCTIONS ====================

int uthread_create(void (*fn)(void *), void *arg) {
    return uthread_create_priority(fn, arg, 5);
}

int uthread_create_priority(void (*fn)(void *), void *arg, int priority) {
    if (next_thread_id >= MAX_THREADS) return -1;
    
    uthread_t *t = malloc(sizeof(*t));
    if (!t) return -1;
    
    memset(t, 0, sizeof(*t));
    
    if (getcontext(&t->ctx) == -1) {
        free(t);
        return -1;
    }
    
    t->stack = malloc(STACK_SZ);
    if (!t->stack) {
        free(t);
        return -1;
    }
    
    t->ctx.uc_stack.ss_sp = t->stack;
    t->ctx.uc_stack.ss_size = STACK_SZ;
    t->ctx.uc_link = &scheduler_ctx;
    
    makecontext(&t->ctx, (void(*)(void))fn, 1, arg);
    
    t->id = next_thread_id;
    t->state = THREAD_READY;
    t->priority = priority;
    t->age = 0;
    t->joined_by = -1;
    t->next = NULL;
    t->prev = NULL;
    t->cond_waiting = 0;
    t->waiting_cond = NULL;
    t->waiting_mutex = NULL;
    t->exit_value = 0;
    
    all_threads[next_thread_id] = t;
    
    add_to_ready_queue(t);
    
    atomic_fetch_add(&threads_created, 1);
    return next_thread_id++;
}

void uthread_yield(void) {
    atomic_fetch_add(&context_switches, 1);
    atomic_fetch_add(&voluntary_yields, 1);
    
    if (current_thread && current_thread->state == THREAD_RUNNING) {
        current_thread->state = THREAD_READY;
        add_to_ready_queue(current_thread);
    }
    
    schedule();
}

void uthread_exit(void *retval) {
    if (!current_thread) return;

    current_thread->exit_value = retval ? (long)retval : 0;
    current_thread->state = THREAD_TERMINATED;
    atomic_fetch_add(&threads_terminated, 1);

    // Wake up joiner
    if (current_thread->joined_by != -1 && all_threads[current_thread->joined_by]) {
        uthread_t *joiner = all_threads[current_thread->joined_by];
        if (joiner->state == THREAD_BLOCKED) {
            joiner->state = THREAD_READY;
            add_to_ready_queue(joiner);
        }
    }

    // Add to terminated list for later cleanup (scheduler/reaper will free)
    add_to_terminated_list(current_thread);

    // Remove current from running and switch to scheduler — never free here
    uthread_t *exiting = current_thread;
    current_thread = NULL;

    // Do not free 'exiting' here.
    schedule(); // scheduler will pick next thread and eventually cleanup 'exiting'
    // should never return
}


int uthread_join(int tid, void **retval) {
    if (tid < 1 || tid >= MAX_THREADS || !all_threads[tid]) return -1;
    uthread_t *target = all_threads[tid];

    if (target->state != THREAD_TERMINATED) {
        target->joined_by = current_thread ? current_thread->id : -1;
        current_thread->state = THREAD_BLOCKED;
        uthread_yield();
    }

    if (retval) *retval = (void*)target->exit_value;

    // Do NOT free here if cleanup is done elsewhere. Instead mark/let scheduler free:
    // e.g. remove from all_threads and let reaper free.
    // If you want join to do cleanup, ensure exclusive ownership and no other cleanup path.
    return 0;
}


int uthread_self(void) {
    return current_thread ? current_thread->id : 0;
}

// ==================== M:N SCHEDULING ====================

void uthread_enable_multicore(void) {
    if (multi_core_enabled || scheduler_running) return;
    
    int cores = sysconf(_SC_NPROCESSORS_ONLN);
    kernel_thread_count = (cores > MAX_KERNEL_THREADS) ? MAX_KERNEL_THREADS : cores;
    if (kernel_thread_count < 1) kernel_thread_count = 1;
    
    printf("[M:N Scheduling] Creating %d kernel threads for %d cores\n", kernel_thread_count, cores);
    
    for (int i = 0; i < kernel_thread_count; i++) {
        kernel_threads[i].id = i;
        kernel_threads[i].current_thread = NULL;
        kernel_threads[i].running = 1;
        kernel_threads[i].queue_mutex = NULL;
        kernel_threads[i].queue_cond = NULL;
        
        pthread_create(&kernel_threads[i].thread, NULL, kernel_thread_wrapper, &kernel_threads[i].id);
    }
    
    multi_core_enabled = 1;
    scheduler_running = 1;
}

void uthread_set_affinity(int tid, int core) {
    if (!multi_core_enabled || core < 0 || core >= kernel_thread_count) return;
    if (tid < 1 || tid >= MAX_THREADS || !all_threads[tid]) return;
    printf("Thread %d affinity set to core %d\n", tid, core);
}

static void *kernel_thread_wrapper(void *arg) {
    int kernel_tid = *(int*)arg;
    printf("Kernel thread %d started\n", kernel_tid);

    while (atomic_load(&kernel_threads[kernel_tid].running)) {
        uthread_t *next = pop_ready_queue();
        if (!next) {
            // No ready threads, sleep briefly
            struct timespec ts = {0, 10 * 1000 * 1000}; // 10 ms
            nanosleep(&ts, NULL);
            continue;
        }

        kernel_threads[kernel_tid].current_thread = next;
        next->state = THREAD_RUNNING;
        current_thread = next;

        atomic_fetch_add(&context_switches, 1);

        // Swap context with scheduler, returns when thread yields or exits
        swapcontext(&scheduler_ctx, &next->ctx);

        kernel_threads[kernel_tid].current_thread = NULL;
        current_thread = NULL;
    }

    printf("Kernel thread %d stopped\n", kernel_tid);
    return NULL;
}


void stop_kernel_threads(void) {
    for (int i = 0; i < kernel_thread_count; i++) {
        atomic_store(&kernel_threads[i].running, 0);
        pthread_join(kernel_threads[i].thread, NULL);
    }
    multi_core_enabled = 0;
    scheduler_running = 0;
}


// ==================== SCHEDULING CONTROL ====================

void uthread_set_scheduling(int policy) {
    scheduling_policy = policy;
    printf("Scheduling policy set to: %s\n", 
           policy == 0 ? "Round-Robin" : "Priority");
}

void uthread_enable_preempt(void) {
    if (preemption_enabled) return;
    
    struct sigaction sa;
    sa.sa_handler = timer_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGVTALRM, &sa, NULL);
    
    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = 10000;
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = 10000;
    setitimer(ITIMER_VIRTUAL, &timer, NULL);
    
    preemption_enabled = 1;
    printf("[Preemption ENABLED]\n");
}

void uthread_disable_preempt(void) {
    if (!preemption_enabled) return;
    
    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = 0;
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = 0;
    setitimer(ITIMER_VIRTUAL, &timer, NULL);
    
    preemption_enabled = 0;
    printf("[Preemption DISABLED]\n");
}

// ==================== SYNCHRONIZATION PRIMITIVES ====================

int uthread_mutex_init(uthread_mutex_t **mutex) {
    *mutex = malloc(sizeof(uthread_mutex_t));
    if (!*mutex) return -1;
    
    atomic_store(&(*mutex)->locked, 0);
    (*mutex)->waiting = NULL;
    return 0;
}

int uthread_mutex_lock(uthread_mutex_t *mutex) {
    int expected = 0;
    while (!atomic_compare_exchange_weak(&mutex->locked, &expected, 1)) {
        atomic_fetch_add(&mutex_blocks, 1);
        
        // Add current thread to mutex wait queue
        current_thread->state = THREAD_BLOCKED;
        current_thread->next = mutex->waiting;
        mutex->waiting = current_thread;
        
        uthread_yield();
        expected = 0;
    }
    return 0;
}

int uthread_mutex_trylock(uthread_mutex_t *mutex) {
    int expected = 0;
    if (atomic_compare_exchange_weak(&mutex->locked, &expected, 1)) {
        return 0;
    }
    return -1;
}

int uthread_mutex_unlock(uthread_mutex_t *mutex) {
    atomic_store(&mutex->locked, 0);
    
    // Wake up one waiting thread
    if (mutex->waiting) {
        uthread_t *waiter = mutex->waiting;
        mutex->waiting = waiter->next;
        
        waiter->state = THREAD_READY;
        add_to_ready_queue(waiter);
    }
    return 0;
}

int uthread_mutex_destroy(uthread_mutex_t *mutex) {
    free(mutex);
    return 0;
}

// ==================== INTERNAL FUNCTIONS ====================

static void schedule(void) {
    if (multi_core_enabled) {
        // In M:N mode, kernel threads handle scheduling
        return;
    }

    // Single-core scheduler
    if (current_thread && current_thread->state == THREAD_RUNNING) {
        swapcontext(&current_thread->ctx, &scheduler_ctx);
        return;
    }

    uthread_t *next = NULL;
    if (scheduling_policy == 1) {
        next = pop_ready_queue_priority(); // priority + aging
    } else {
        next = pop_ready_queue();          // Round-Robin
    }

    if (!next) {
        if (atomic_load(&threads_created) == atomic_load(&threads_terminated)) {
            scheduler_done = 1;
        }
        return;
    }

    uthread_t *prev_thread = current_thread;
    current_thread = next;
    current_thread->state = THREAD_RUNNING;

    atomic_fetch_add(&context_switches, 1);

    if (prev_thread) {
        swapcontext(&prev_thread->ctx, &current_thread->ctx);
    } else {
        setcontext(&current_thread->ctx);
    }
}


static void timer_handler(int sig) {
    if (!preemption_enabled || sig != SIGVTALRM || !current_thread)
        return;

    // Only count preemption
    atomic_fetch_add(&preemptions, 1);

    // Optional: print message only once per switch
    static int last_thread_id = -1;
    if (current_thread->id != last_thread_id) {
        printf("[PREEMPTION!] Thread %d\n", current_thread->id);
        last_thread_id = current_thread->id;
    }

    uthread_yield();
}


static void cleanup_thread(uthread_t *thread) {
    if (!thread) return;
    all_threads[thread->id] = NULL;
}

// ==================== SCHEDULER START ====================

void uthread_start_scheduler(void) {
    getcontext(&scheduler_ctx);
    
    if (!scheduler_done) {
        if (multi_core_enabled) {
            // M:N mode - kernel threads handle scheduling
            // Wait for all threads to complete
            while (atomic_load(&threads_created) != atomic_load(&threads_terminated)) {
                sleep(100000); // 100ms
            }
        } else {
            // Single-core scheduler
            while (ready_queue || atomic_load(&threads_created) != atomic_load(&threads_terminated)) {
                schedule();
            }
        }
        scheduler_done = 1;
    }
}

// ==================== PERFORMANCE MONITORING ====================

void uthread_print_stats(void) {
    printf("\n=== Thread Statistics ===\n");
    printf("Threads created: %d\n", atomic_load(&threads_created));
    printf("Threads terminated: %d\n", atomic_load(&threads_terminated));
    printf("Total context switches: %d\n", atomic_load(&context_switches));
    printf("  Voluntary yields: %d\n", atomic_load(&voluntary_yields));
    printf("  Preemptions: %d\n", atomic_load(&preemptions));
    printf("  Mutex blocks: %d\n", atomic_load(&mutex_blocks));
    printf("Active threads: %d\n", 
           atomic_load(&threads_created) - atomic_load(&threads_terminated));
    printf("Preemption enabled: %s\n", preemption_enabled ? "YES" : "NO");
    printf("Multi-core enabled: %s\n", multi_core_enabled ? "YES" : "NO");
    printf("Kernel threads: %d\n", kernel_thread_count);
    printf("Scheduling policy: %s\n", scheduling_policy == 0 ? "Round-Robin" : "Priority");
}

void uthread_reset_stats(void) {
    atomic_store(&threads_created, 0);
    atomic_store(&threads_terminated, 0);
    atomic_store(&context_switches, 0);
    atomic_store(&voluntary_yields, 0);
    atomic_store(&preemptions, 0);
    atomic_store(&mutex_blocks, 0);
}

int uthread_get_created_count(void) {
    return atomic_load(&threads_created);
}

int uthread_get_terminated_count(void) {
    return atomic_load(&threads_terminated);
}

int uthread_get_active_count(void) {
    return atomic_load(&threads_created) - atomic_load(&threads_terminated);
}

// ==================== TEST FUNCTIONS ====================

void worker1(void *arg) {
    int id = *(int*)arg;
    printf("Thread %d: starting\n", id);
    for (int i = 0; i < 3; i++) {
        printf("Thread %d: step %d\n", id, i);
        uthread_yield();
    }
    printf("Thread %d: exiting\n", id);
    uthread_exit((void*)(long)(id * 100));
}

uthread_mutex_t *test_mutex;
int shared_counter = 0;
void preemptive_worker(void *arg) {
    int id = *(int*)arg;
    printf("Thread %d: starting\n", id);

    for (int i = 0; i < 3; i++) {  // 3 iterations → counter ends at 6
        uthread_mutex_lock(test_mutex);

        printf("Thread %d: counter=%d\n", id, shared_counter);

        // simulate work inside critical section (long enough for timer to fire)
        for (volatile long j = 0; j < 2000000; j++);

        shared_counter++;

        uthread_mutex_unlock(test_mutex);

        // simulate work outside mutex to allow other threads to run
        for (volatile long j = 0; j < 2000000; j++);

        // force voluntary yield to increase context switches
        uthread_yield();
    }

    printf("Thread %d: exiting\n", id);
    uthread_exit((void*)(long)id);
}


void mn_worker(void *arg) {
    int id = *(int*)arg;
    printf("M:N Thread %d: running on kernel thread\n", id);

    for (int i = 0; i < 2; i++) {
        printf("M:N Thread %d: iteration %d\n", id, i);

        // 100 ms sleep
        struct timespec ts = {0, 100 * 1000 * 1000}; // 100 ms
        nanosleep(&ts, NULL);

        // Yield back to scheduler
        uthread_yield();
    }

    printf("M:N Thread %d: exiting\n", id);
    uthread_exit((void*)(long)(id * 1000));
}


void test_week1(void) {
    printf("\n=== Week 1: Cooperative Threading ===\n");
    
    uthread_reset_stats();
    ready_queue = NULL;
    memset(all_threads, 0, sizeof(all_threads));
    next_thread_id = 1;
    scheduler_done = 0;
    current_thread = NULL;
    
    int id1 = 1, id2 = 2, id3 = 3;
    int tid1 = uthread_create(worker1, &id1);
    int tid2 = uthread_create(worker1, &id2);
    int tid3 = uthread_create(worker1, &id3);
    
    uthread_start_scheduler();
    
    void *ret1, *ret2, *ret3;
    uthread_join(tid1, &ret1);
    uthread_join(tid2, &ret2);
    uthread_join(tid3, &ret3);
    
    printf("Join results: %ld, %ld, %ld\n", (long)ret1, (long)ret2, (long)ret3);
    uthread_print_stats();
}

void test_week2(void) {
    printf("\n=== Week 2: Preemption + Mutex ===\n");
    
    uthread_reset_stats();
    ready_queue = NULL;
    memset(all_threads, 0, sizeof(all_threads));
    next_thread_id = 1;
    scheduler_done = 0;
    current_thread = NULL;
    shared_counter = 0;
    
    uthread_mutex_init(&test_mutex);
    
    uthread_enable_preempt();
    
    int id1 = 1, id2 = 2;
    int tid1 = uthread_create(preemptive_worker, &id1);
    int tid2 = uthread_create(preemptive_worker, &id2);
    
    uthread_start_scheduler();
    
    uthread_disable_preempt();
    
    void *ret1, *ret2;
    uthread_join(tid1, &ret1);
    uthread_join(tid2, &ret2);
    
    printf("Final counter: %d (expected: 6)\n", shared_counter);
    uthread_print_stats();
    
    uthread_mutex_destroy(test_mutex);
}
void worker_priority(void *arg) {
    int id = *(int*)arg;

    for (int step = 0; step < 3; step++) {
        printf("Thread %d (priority %d): step %d\n", id, current_thread->priority, step);

        // simulate work
        for (volatile long j = 0; j < 100000; j++);

        // voluntary yield to demonstrate scheduler selection
        uthread_yield();
    }

    printf("Thread %d: exiting\n", id);
    uthread_exit((void*)((long)id * 100));
}

void test_week3(void) {
    printf("\n=== Week 3: Priority Test (No Preemption) ===\n");

    // Reset scheduler and thread stats
    uthread_reset_stats();
    ready_queue = NULL;
    memset(all_threads, 0, sizeof(all_threads));
    next_thread_id = 1;
    scheduler_done = 0;
    current_thread = NULL;

    // Priority scheduling WITHOUT preemption
    uthread_set_scheduling(1);   // 0 = Round-Robin, 1 = Priority
    uthread_disable_preempt();   // ensure preemption is off

    int id1 = 1, id2 = 2, id3 = 3;

    // Create threads with different priorities
    int tid1 = uthread_create_priority(worker_priority, &id1, 1); // low
    int tid2 = uthread_create_priority(worker_priority, &id2, 3); // high
    int tid3 = uthread_create_priority(worker_priority, &id3, 2); // medium

    // Start scheduler
    uthread_start_scheduler();

    // Join threads and collect return values
    void *ret1, *ret2, *ret3;
    uthread_join(tid1, &ret1);
    uthread_join(tid2, &ret2);
    uthread_join(tid3, &ret3);

    printf("Join results: %ld, %ld, %ld\n", (long)ret1, (long)ret2, (long)ret3);

    uthread_print_stats();
}

void test_mn_scheduling(void) {
    printf("\n=== Week 4: M:N Scheduling Test ===\n");

    uthread_reset_stats();
    ready_queue = NULL;
    memset(all_threads, 0, sizeof(all_threads));
    next_thread_id = 1;
    scheduler_done = 0;
    current_thread = NULL;

    printf("Testing M:N scheduling (user threads over kernel threads)\n");
    uthread_enable_multicore(); // starts 4 kernel threads

    int thread_count = 8;
    int ids[thread_count];
    int tids[thread_count];

    printf("Creating %d user threads to run on %d kernel threads\n", 
           thread_count, kernel_thread_count);

    for (int i = 0; i < thread_count; i++) {
        ids[i] = i + 1;
        tids[i] = uthread_create_priority(mn_worker, &ids[i], (i % MAX_PRIORITY) + 1);
        printf("Created user thread %d with priority %d\n", tids[i], (i % MAX_PRIORITY) + 1);
    }

    uthread_start_scheduler();

    // Join all threads
    for (int i = 0; i < thread_count; i++) {
        void *retval;
        if (uthread_join(tids[i], &retval) == 0) {
            printf("Joined thread %d with result: %ld\n", tids[i], (long)retval);
        }
    }

    stop_kernel_threads();
    uthread_print_stats();
}


// Main function
int main(void) {
    printf("Enhanced User-Level Thread Library\n");
    printf("==================================\n");
    
    test_week1();
    test_week2(); 
    test_week3();
    test_mn_scheduling();
    
    printf("\n🎉 ALL TESTS COMPLETED! 🎉\n");
    return 0;
}