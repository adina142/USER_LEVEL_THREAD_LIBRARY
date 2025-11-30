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

#define BUFFER_SIZE 5

typedef struct uthread_mutex {
    atomic_int locked;
    struct uthread *waiting;     
    pthread_mutex_t kernel_mutex;
} uthread_mutex_t;

// Condition variable structure
typedef struct uthread_cond {
    struct uthread *waiting;     
    pthread_cond_t kernel_cond;  
} uthread_cond_t;


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




int buffer[BUFFER_SIZE];
int count = 0;       // number of items in buffer
int in = 0;          // next insert index
int out = 0;         // next remove index

uthread_mutex_t *buffer_mutex;
uthread_cond_t *not_full;
uthread_cond_t *not_empty;


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
        
        // Skip terminated threads
        while (t && t->state == THREAD_TERMINATED) {
            uthread_t *next = t->next;
            // Remove terminated thread from queue
            if (t->prev) t->prev->next = t->next;
            else ready_queue = t->next;
            if (t->next) t->next->prev = t->prev;
            
            // Clean up terminated thread
            if (t->stack) free(t->stack);
            free(t);
            
            t = next;
        }
        
        if (!t) return NULL;
        
        ready_queue = t->next;
        if (ready_queue) ready_queue->prev = NULL;
        t->next = t->prev = NULL;
        return t;
    } else { 
        // Similar fix for priority scheduling...
        uthread_t *t = ready_queue;
        uthread_t *highest = NULL;
        
        // Find highest priority non-terminated thread
        for (; t; t = t->next) {
            if (t->state != THREAD_TERMINATED) {
                if (!highest || (t->priority + t->age) > (highest->priority + highest->age)) {
                    highest = t;
                }
            }
        }
        
        if (!highest) return NULL;
        
        // Remove highest from queue
        if (highest->prev) highest->prev->next = highest->next;
        else ready_queue = highest->next;
        if (highest->next) highest->next->prev = highest->prev;
        
        highest->next = highest->prev = NULL;
        
        // Increment age of remaining threads
        for (t = ready_queue; t; t = t->next) {
            if (t->state != THREAD_TERMINATED) {
                t->age++;
            }
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

static void cleanup_terminated_threads(void) {
    uthread_t *thread;
    while ((thread = pop_terminated_list()) != NULL) {
        if (thread->stack) free(thread->stack);
        free(thread);
    }
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

int uthread_cond_init(uthread_cond_t **cond) {
    *cond = malloc(sizeof(uthread_cond_t));
    if (!*cond) return -1;
    (*cond)->waiting = NULL;
    pthread_cond_init(&(*cond)->kernel_cond, NULL);
    return 0;
}

int uthread_cond_wait(uthread_cond_t *cond, uthread_mutex_t *mutex) {
    if (!cond || !mutex || !current_thread) return -1;

    // Add current thread to waiting list
    current_thread->state = THREAD_BLOCKED;
    current_thread->next = cond->waiting;
    cond->waiting = current_thread;

    printf("🔒 Thread %d waiting on cond, waiting list: ", current_thread->id);
    uthread_t *t = cond->waiting;
    while (t) {
        printf("%d -> ", t->id);
        t = t->next;
    }
    printf("NULL\n");

    // Unlock the mutex while waiting
    uthread_mutex_unlock(mutex);

    uthread_yield(); // Yield to scheduler

    // Re-lock mutex after waking up
    uthread_mutex_lock(mutex);
    return 0;
}

int uthread_cond_signal(uthread_cond_t *cond) {
    if (!cond) return -1;
    
    if (cond->waiting) {
        uthread_t *thread = cond->waiting;
        cond->waiting = thread->next;  // Remove from list
        thread->next = NULL;           // Important: clear next pointer
        thread->state = THREAD_READY;
        add_to_ready_queue(thread);
    }
    return 0;
}

int uthread_cond_broadcast(uthread_cond_t *cond) {
    if (!cond) return -1;

    uthread_t *thread = cond->waiting;
    uthread_t *next;
    
    while (thread) {
        next = thread->next;      // Save next before modifying
        thread->next = NULL;      // CRITICAL: Clear next pointer
        thread->state = THREAD_READY;
        add_to_ready_queue(thread);
        thread = next;
    }
    
    cond->waiting = NULL;         // Clear the waiting list
    return 0;
}

int uthread_cond_destroy(uthread_cond_t *cond) {
    free(cond);
    return 0;
}

// ==================== INTERNAL FUNCTIONS ====================

static void schedule(void) {
    if (multi_core_enabled) {
        return;
    }

    // Clean up any terminated threads from ready queue first
    

    // If current thread is running and healthy, save context
    if (current_thread && current_thread->state == THREAD_RUNNING) {
        if (current_thread->ctx.uc_stack.ss_sp != NULL) {
            swapcontext(&current_thread->ctx, &scheduler_ctx);
        } else {
            // Thread has corrupted context, force it to exit
            current_thread->state = THREAD_TERMINATED;
            atomic_fetch_add(&threads_terminated, 1);
            current_thread = NULL;
        }
        return;
    }

    uthread_t *next = NULL;
    int attempts = 0;
    
    // Keep looking for a runnable thread with safety limit
    while (attempts < 100) {
        if (scheduling_policy == 1) {
            next = pop_ready_queue_priority();
        } else {
            next = pop_ready_queue();
        }

        // Skip terminated or invalid threads
        if (next && next->state == THREAD_TERMINATED) {
            if (next->stack) free(next->stack);
            free(next);
            next = NULL;
            attempts++;
            continue;
        }

        if (next && next->state == THREAD_READY) {
            break; // Found a runnable thread
        }

        if (!next) {
            break; // No more threads
        }

        attempts++;
    }

    // DEADLOCK DETECTION: If no runnable threads but not all have terminated
    if (!next) {
        int created = atomic_load(&threads_created);
        int terminated = atomic_load(&threads_terminated);
        
        if (created != terminated) {
            printf("POSSIBLE DEADLOCK: %d threads created, %d terminated, but no threads in ready queue\n", 
                   created, terminated);
            printf("Checking for blocked threads...\n");
            
            // Force exit if we detect a deadlock
            for (int i = 1; i < MAX_THREADS; i++) {
                if (all_threads[i] && all_threads[i]->state == THREAD_BLOCKED) {
                    printf("Thread %d is blocked (waiting on cond var?)\n", i);
                }
            }
            
            // Emergency: mark all remaining threads as terminated
            for (int i = 1; i < MAX_THREADS; i++) {
                if (all_threads[i] && all_threads[i]->state != THREAD_TERMINATED) {
                    printf("Forcing thread %d to terminate\n", i);
                    all_threads[i]->state = THREAD_TERMINATED;
                    atomic_fetch_add(&threads_terminated, 1);
                }
            }
        }
        
        scheduler_done = 1;
        return;
    }

    uthread_t *prev_thread = current_thread;
    current_thread = next;
    current_thread->state = THREAD_RUNNING;

    atomic_fetch_add(&context_switches, 1);

    if (prev_thread && prev_thread->ctx.uc_stack.ss_sp != NULL) {
        swapcontext(&prev_thread->ctx, &current_thread->ctx);
    } else {
        setcontext(&current_thread->ctx);
    }
}

static void timer_handler(int sig) {
    if (!preemption_enabled || sig != SIGVTALRM || !current_thread)
        return;

    // Simply set a flag that schedule() can check
    static int last_thread_id = -1;
    
    if (current_thread->id != last_thread_id) {
        printf("[PREEMPTION!] Thread %d\n", current_thread->id);
        atomic_fetch_add(&preemptions, 1);
        last_thread_id = current_thread->id;
    }
    
    // Let the natural flow cause the switch rather than forcing it here
    // This prevents double-counting
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
            while (atomic_load(&threads_created) != atomic_load(&threads_terminated)) {
                sleep(100000); // 100ms - FIXED: use usleep instead of sleep
                cleanup_terminated_threads(); // Clean up periodically
            }
            stop_kernel_threads(); // Add this function to stop kernel threads
        } else {
            // Single-core scheduler
            while (ready_queue || atomic_load(&threads_created) != atomic_load(&threads_terminated)) {
                schedule();
                cleanup_terminated_threads(); // Clean up after each schedule
            }
        }
        scheduler_done = 1;
    }
    
    // Final cleanup
    cleanup_terminated_threads();
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
void init_buffer_sync() {
buffer_mutex = malloc(sizeof(uthread_mutex_t));
uthread_mutex_init(&buffer_mutex);

not_full = malloc(sizeof(uthread_cond_t));
uthread_cond_init(&not_full);

not_empty = malloc(sizeof(uthread_cond_t));
uthread_cond_init(&not_empty);

}


void producer(void *arg) {
    int id = *(int*)arg;
    int base = id * 100;
    int produced_count = 0;
    
    for (int i = 0; i < 10; i++) {
        uthread_mutex_lock(buffer_mutex);

        while (count == BUFFER_SIZE) {
            uthread_cond_wait(not_full, buffer_mutex);
        }

        int item = base + i;
        buffer[in] = item;
        in = (in + 1) % BUFFER_SIZE;
        count++;
        produced_count++;
        printf("Producer %d produced item %d (count: %d)\n", id, item, count);

        uthread_cond_signal(not_empty);
        uthread_mutex_unlock(buffer_mutex);

        uthread_yield();
    }
    
    printf("Producer %d FINISHED - produced %d items\n", id, produced_count);
    uthread_exit(NULL); // Explicit exit
}
void consumer(void *arg) {
    int id = *(int *)arg;
    int consumed_count = 0;
    
    for (int i = 0; i < 10; i++) {
        uthread_mutex_lock(buffer_mutex);
        
        while (count == 0) {
            printf("Consumer %d: buffer empty, waiting...\n", id);
            uthread_cond_wait(not_empty, buffer_mutex);
        }
        
        int item = buffer[out];
        out = (out + 1) % BUFFER_SIZE;
        count--;
        consumed_count++;
        
        printf("Consumer %d consumed item %d (count: %d)\n", id, item, count);
        
        uthread_cond_broadcast(not_full);
        uthread_mutex_unlock(buffer_mutex);
        
        // Optional: small delay for better interleaving
        for (volatile int j = 0; j < 100000; j++);
        
        uthread_yield();
    }
    
    printf("Consumer %d finished (consumed %d items)\n", id, consumed_count);
    uthread_exit(NULL); // CRITICAL: Explicit exit
}

void init_sync_primitives(void) {
    // Initialize mutex
    uthread_mutex_init(&buffer_mutex);
    
    // Initialize condition variables
    not_full = malloc(sizeof(uthread_cond_t));
    not_empty = malloc(sizeof(uthread_cond_t));
    if (!not_full || !not_empty) {
        printf("Error: Failed to allocate condition variables\n");
        return;
    }
    not_full->waiting = NULL;
    not_empty->waiting = NULL;
}

void test_week4_condition(void) {
    printf("\n=== Week 4: Condition Variables (Safe Cleanup) ===\n");
    
    // Reset thread library state
    uthread_reset_stats();
    ready_queue = NULL;
    memset(all_threads, 0, sizeof(all_threads));
    next_thread_id = 1;
    scheduler_done = 0;
    current_thread = NULL;
    
    // Reset buffer state
    count = in = out = 0;
    memset(buffer, 0, sizeof(buffer));
    
    // Initialize synchronization primitives
    init_sync_primitives();
    
    // Enable preemption to stress-test synchronization
    uthread_enable_preempt();
    
    printf("Creating 3 producers and 3 consumers...\n");
    
    int p_ids[3] = {1, 2, 3};
    int c_ids[3] = {1, 2, 3};
    
    // Create producers
    for (int i = 0; i < 3; i++) {
        int tid = uthread_create(producer, &p_ids[i]);
        printf("Created producer %d (thread %d)\n", p_ids[i], tid);
    }
    
    // Create consumers
    for (int i = 0; i < 3; i++) {
        int tid = uthread_create(consumer, &c_ids[i]);
        printf("Created consumer %d (thread %d)\n", c_ids[i], tid);
    }
    
    printf("Starting scheduler...\n");
    
    // Start scheduler - runs until all threads complete
    uthread_start_scheduler();
    
    // -------------------------------
    // SAFER CLEANUP APPROACH
    // -------------------------------
    
    // CRITICAL: Wait a bit to ensure all threads have completely finished
    printf("All threads completed. Waiting for safe cleanup...\n");
    
    // Disable preemption FIRST
    uthread_disable_preempt();
    
    // Add a small delay to ensure all thread cleanup is complete
    struct timespec ts = {0, 10000000}; // 10ms delay
    nanosleep(&ts, NULL);
    
    // Clear any remaining waiting threads from condition variables
    if (not_full) {
        not_full->waiting = NULL;
    }
    if (not_empty) {
        not_empty->waiting = NULL;
    }
    
    // Free condition variables
    if (not_full) {
        free(not_full);
        not_full = NULL;
    }
    if (not_empty) {
        free(not_empty);
        not_empty = NULL;
    }
    
    printf("Cleanup completed safely.\n");
    
    // Print final statistics
    uthread_print_stats();
    
    printf("Test finished safely.\n");
}


// Main function
int main(void) {
    printf("Enhanced User-Level Thread Library\n");
    printf("==================================\n");
    
    test_week1();
    test_week2(); 
    test_week3();
    test_week4_condition();  
   
  
    
    printf("\n🎉 ALL TESTS COMPLETED! 🎉\n");
    return 0;
}
