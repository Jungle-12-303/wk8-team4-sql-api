#ifndef PLATFORM_H
#define PLATFORM_H

#include <pthread.h>

typedef void *(*PlatformThreadRoutine)(void *);

typedef pthread_mutex_t PlatformMutex;
typedef pthread_cond_t PlatformCond;
typedef pthread_rwlock_t PlatformRWLock;
typedef pthread_t PlatformThread;

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
