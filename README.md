# Enhanced User-Level Thread Library

A comprehensive **user-level thread library** implementing the **M:N threading model** with advanced scheduling, synchronization primitives, and performance monitoring.  

---

## Features

### 🧵 Thread Management
- **M:N Threading Model**: Multiple user threads mapped to kernel threads.  
- **Priority Scheduling**: Dynamic priority-based scheduling with aging.  
- **Round-Robin Scheduling**: Fair time-sliced scheduling policy.  
- **Preemptive Scheduling**: Timer-based thread preemption.  
- **Multicore Support**: Thread affinity and parallel execution.  

### 🔒 Synchronization Primitives
- **Mutexes**: Fast user-level mutexes with blocking.  
- **Condition Variables**: Full support for condition variables.  
- **Producer-Consumer**: Bounded buffer with proper synchronization.  
- **Deadlock Detection**: Automatic deadlock detection and recovery.  

### 📊 Performance Monitoring
- **Comprehensive Statistics**: Context switches, yields, preemptions.  
- **Real-time Metrics**: Active thread count, synchronization events.  
- **Configurable Policies**: Runtime scheduling policy changes.  

---

## API Reference

### Thread Management
```c
int uthread_create(void (*fn)(void *), void *arg);
int uthread_create_priority(void (*fn)(void *), void *arg, int priority);
void uthread_yield(void);
void uthread_exit(void *retval);
int uthread_join(int tid, void **retval);
int uthread_self(void);
Scheduling Control
void uthread_set_scheduling(int policy);
void uthread_enable_preempt(void);
void uthread_disable_preempt(void);
void uthread_enable_multicore(void);
void uthread_set_affinity(int tid, int core);

Synchronization
Mutex Operations
int uthread_mutex_init(uthread_mutex_t **mutex);
int uthread_mutex_lock(uthread_mutex_t *mutex);
int uthread_mutex_trylock(uthread_mutex_t *mutex);
int uthread_mutex_unlock(uthread_mutex_t *mutex);
int uthread_mutex_destroy(uthread_mutex_t *mutex);

Condition Variables
int uthread_cond_init(uthread_cond_t **cond);
int uthread_cond_wait(uthread_cond_t *cond, uthread_mutex_t *mutex);
int uthread_cond_signal(uthread_cond_t *cond);
int uthread_cond_broadcast(uthread_cond_t *cond);
int uthread_cond_destroy(uthread_cond_t *cond);

Statistics
void uthread_print_stats(void);
void uthread_reset_stats(void);
int uthread_get_created_count(void);
int uthread_get_terminated_count(void);
int uthread_get_active_count(void);

Architecture
Thread States

THREAD_READY: Ready for execution.

THREAD_RUNNING: Currently executing.

THREAD_BLOCKED: Waiting for synchronization.

THREAD_TERMINATED: Finished execution.

THREAD_WAITING: Waiting on condition variable.

Key Components

Thread Control Block (TCB): Stores context (ucontext_t), stack, priority, scheduling data, and synchronization state.

Scheduler: Policy-based thread selection, preemption, and multicore load balancing.

Synchronization: User-level mutexes, condition variables, and deadlock detection.

Building and Running
Prerequisites

POSIX-compliant system (Linux/BSD)

pthread library

C11 compiler

Compilation
gcc -std=c11 -D_XOPEN_SOURCE=700 -o uthread main.c -lpthread

Running Tests
./uthread


The test suite includes:

Week 1: Basic cooperative threading

Week 2: Preemption and mutex synchronization

Week 3: Priority scheduling

Week 4: Condition variables and producer-consumer

Configuration
Scheduling Policies

0: Round-Robin (default)

1: Priority-based with aging

System Limits

Maximum threads: 256

Maximum kernel threads: 4

Stack size: 128KB per thread

Priority levels: 0-10

Performance Characteristics

Low Overhead: User-level context switches.

Scalable: M:N threading model.

Fair: Priority aging prevents starvation.

Robust: Deadlock detection and recovery.

Example Usage
#include "uthread.h"

void worker(void *arg) {
    int id = *(int*)arg;
    printf("Thread %d working\n", id);
    uthread_yield();
    printf("Thread %d completed\n", id);
}

int main() {
    int ids[] = {1, 2, 3};
    
    for (int i = 0; i < 3; i++) {
        uthread_create(worker, &ids[i]);
    }
    
    uthread_start_scheduler();
    uthread_print_stats();
    return 0;
}

Safety Features

Stack Protection: Automatic stack cleanup.

Memory Management: Proper allocation/deallocation.

Error Handling: Comprehensive error checking.

Thread Safety: Atomic operations for shared data.
