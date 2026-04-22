#ifndef PLATFORM_H
#define PLATFORM_H

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef WINVER
#define WINVER 0x0600
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

typedef void *(*PlatformThreadRoutine)(void *);

#ifdef _WIN32
typedef CRITICAL_SECTION PlatformMutex;
typedef CONDITION_VARIABLE PlatformCond;
typedef struct PlatformRWLock {
    PlatformMutex mutex;
    unsigned long readers;
    int writer;
} PlatformRWLock;
typedef HANDLE PlatformThread;
#else
typedef pthread_mutex_t PlatformMutex;
typedef pthread_cond_t PlatformCond;
typedef pthread_rwlock_t PlatformRWLock;
typedef pthread_t PlatformThread;
#endif

int platform_mutex_init(PlatformMutex *mutex);
void platform_mutex_destroy(PlatformMutex *mutex);
int platform_mutex_lock(PlatformMutex *mutex);
int platform_mutex_unlock(PlatformMutex *mutex);

int platform_cond_init(PlatformCond *cond);
void platform_cond_destroy(PlatformCond *cond);
int platform_cond_wait(PlatformCond *cond, PlatformMutex *mutex);
int platform_cond_signal(PlatformCond *cond);
int platform_cond_broadcast(PlatformCond *cond);

int platform_rwlock_init(PlatformRWLock *lock);
void platform_rwlock_destroy(PlatformRWLock *lock);
int platform_rwlock_try_read_lock(PlatformRWLock *lock);
int platform_rwlock_try_write_lock(PlatformRWLock *lock);
int platform_rwlock_read_unlock(PlatformRWLock *lock);
int platform_rwlock_write_unlock(PlatformRWLock *lock);

int platform_thread_create(PlatformThread *thread, PlatformThreadRoutine routine, void *arg);
int platform_thread_join(PlatformThread thread);

unsigned long long platform_now_millis(void);
void platform_sleep_ms(unsigned int milliseconds);

#endif
