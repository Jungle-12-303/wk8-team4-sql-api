#include "platform.h"

#include <errno.h>
#include <time.h>

int platform_mutex_init(PlatformMutex *mutex) {
    return pthread_mutex_init(mutex, NULL) == 0;
}

void platform_mutex_destroy(PlatformMutex *mutex) {
    pthread_mutex_destroy(mutex);
}

int platform_mutex_lock(PlatformMutex *mutex) {
    return pthread_mutex_lock(mutex) == 0;
}

int platform_mutex_unlock(PlatformMutex *mutex) {
    return pthread_mutex_unlock(mutex) == 0;
}

int platform_cond_init(PlatformCond *cond) {
    return pthread_cond_init(cond, NULL) == 0;
}

void platform_cond_destroy(PlatformCond *cond) {
    pthread_cond_destroy(cond);
}

int platform_cond_wait(PlatformCond *cond, PlatformMutex *mutex) {
    return pthread_cond_wait(cond, mutex) == 0;
}

int platform_cond_signal(PlatformCond *cond) {
    return pthread_cond_signal(cond) == 0;
}

int platform_cond_broadcast(PlatformCond *cond) {
    return pthread_cond_broadcast(cond) == 0;
}

int platform_rwlock_init(PlatformRWLock *lock) {
    return pthread_rwlock_init(lock, NULL) == 0;
}

void platform_rwlock_destroy(PlatformRWLock *lock) {
    pthread_rwlock_destroy(lock);
}

int platform_rwlock_try_read_lock(PlatformRWLock *lock) {
    return pthread_rwlock_tryrdlock(lock) == 0;
}

int platform_rwlock_try_write_lock(PlatformRWLock *lock) {
    return pthread_rwlock_trywrlock(lock) == 0;
}

int platform_rwlock_read_unlock(PlatformRWLock *lock) {
    return pthread_rwlock_unlock(lock) == 0;
}

int platform_rwlock_write_unlock(PlatformRWLock *lock) {
    return pthread_rwlock_unlock(lock) == 0;
}

int platform_thread_create(PlatformThread *thread, PlatformThreadRoutine routine, void *arg) {
    return pthread_create(thread, NULL, routine, arg) == 0;
}

int platform_thread_join(PlatformThread thread) {
    return pthread_join(thread, NULL) == 0;
}

unsigned long long platform_now_millis(void) {
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (unsigned long long)now.tv_sec * 1000ULL + (unsigned long long)now.tv_nsec / 1000000ULL;
}

void platform_sleep_ms(unsigned int milliseconds) {
    struct timespec duration;

    duration.tv_sec = (time_t)(milliseconds / 1000U);
    duration.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;

    while (nanosleep(&duration, &duration) != 0 && errno == EINTR) {
    }
}
