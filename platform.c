#include "platform.h"

#include <stddef.h>
#include <stdlib.h>

#ifdef _WIN32
#include <process.h>
#else
#include <errno.h>
#include <time.h>
#endif

int platform_mutex_init(PlatformMutex *mutex) {
#ifdef _WIN32
    InitializeCriticalSection(mutex);
    return 1;
#else
    return pthread_mutex_init(mutex, NULL) == 0;
#endif
}

void platform_mutex_destroy(PlatformMutex *mutex) {
#ifdef _WIN32
    DeleteCriticalSection(mutex);
#else
    pthread_mutex_destroy(mutex);
#endif
}

int platform_mutex_lock(PlatformMutex *mutex) {
#ifdef _WIN32
    EnterCriticalSection(mutex);
    return 1;
#else
    return pthread_mutex_lock(mutex) == 0;
#endif
}

int platform_mutex_unlock(PlatformMutex *mutex) {
#ifdef _WIN32
    LeaveCriticalSection(mutex);
    return 1;
#else
    return pthread_mutex_unlock(mutex) == 0;
#endif
}

int platform_cond_init(PlatformCond *cond) {
#ifdef _WIN32
    InitializeConditionVariable(cond);
    return 1;
#else
    return pthread_cond_init(cond, NULL) == 0;
#endif
}

void platform_cond_destroy(PlatformCond *cond) {
#ifdef _WIN32
    (void)cond;
#else
    pthread_cond_destroy(cond);
#endif
}

int platform_cond_wait(PlatformCond *cond, PlatformMutex *mutex) {
#ifdef _WIN32
    return SleepConditionVariableCS(cond, mutex, INFINITE) != 0;
#else
    return pthread_cond_wait(cond, mutex) == 0;
#endif
}

int platform_cond_signal(PlatformCond *cond) {
#ifdef _WIN32
    WakeConditionVariable(cond);
    return 1;
#else
    return pthread_cond_signal(cond) == 0;
#endif
}

int platform_cond_broadcast(PlatformCond *cond) {
#ifdef _WIN32
    WakeAllConditionVariable(cond);
    return 1;
#else
    return pthread_cond_broadcast(cond) == 0;
#endif
}

int platform_rwlock_init(PlatformRWLock *lock) {
#ifdef _WIN32
    if (!platform_mutex_init(&lock->mutex)) {
        return 0;
    }
    lock->readers = 0;
    lock->writer = 0;
    return 1;
#else
    return pthread_rwlock_init(lock, NULL) == 0;
#endif
}

void platform_rwlock_destroy(PlatformRWLock *lock) {
#ifdef _WIN32
    platform_mutex_destroy(&lock->mutex);
#else
    pthread_rwlock_destroy(lock);
#endif
}

int platform_rwlock_try_read_lock(PlatformRWLock *lock) {
#ifdef _WIN32
    int acquired = 0;

    platform_mutex_lock(&lock->mutex);
    if (!lock->writer) {
        lock->readers++;
        acquired = 1;
    }
    platform_mutex_unlock(&lock->mutex);
    return acquired;
#else
    return pthread_rwlock_tryrdlock(lock) == 0;
#endif
}

int platform_rwlock_try_write_lock(PlatformRWLock *lock) {
#ifdef _WIN32
    int acquired = 0;

    platform_mutex_lock(&lock->mutex);
    if (!lock->writer && lock->readers == 0) {
        lock->writer = 1;
        acquired = 1;
    }
    platform_mutex_unlock(&lock->mutex);
    return acquired;
#else
    return pthread_rwlock_trywrlock(lock) == 0;
#endif
}

int platform_rwlock_read_unlock(PlatformRWLock *lock) {
#ifdef _WIN32
    platform_mutex_lock(&lock->mutex);
    if (lock->readers > 0) {
        lock->readers--;
    }
    platform_mutex_unlock(&lock->mutex);
    return 1;
#else
    return pthread_rwlock_unlock(lock) == 0;
#endif
}

int platform_rwlock_write_unlock(PlatformRWLock *lock) {
#ifdef _WIN32
    platform_mutex_lock(&lock->mutex);
    lock->writer = 0;
    platform_mutex_unlock(&lock->mutex);
    return 1;
#else
    return pthread_rwlock_unlock(lock) == 0;
#endif
}

#ifdef _WIN32
typedef struct PlatformThreadStart {
    PlatformThreadRoutine routine;
    void *arg;
} PlatformThreadStart;

static unsigned __stdcall platform_thread_entry(void *raw_start) {
    PlatformThreadStart *start = (PlatformThreadStart *)raw_start;
    PlatformThreadRoutine routine = start->routine;
    void *arg = start->arg;

    free(start);
    routine(arg);
    return 0;
}
#endif

int platform_thread_create(PlatformThread *thread, PlatformThreadRoutine routine, void *arg) {
#ifdef _WIN32
    PlatformThreadStart *start = (PlatformThreadStart *)malloc(sizeof(*start));

    if (start == NULL) {
        return 0;
    }

    start->routine = routine;
    start->arg = arg;

    *thread = (HANDLE)_beginthreadex(NULL, 0, platform_thread_entry, start, 0, NULL);
    if (*thread == NULL) {
        free(start);
        return 0;
    }

    return 1;
#else
    return pthread_create(thread, NULL, routine, arg) == 0;
#endif
}

int platform_thread_join(PlatformThread thread) {
#ifdef _WIN32
    DWORD wait_result = WaitForSingleObject(thread, INFINITE);

    CloseHandle(thread);
    return wait_result == WAIT_OBJECT_0;
#else
    return pthread_join(thread, NULL) == 0;
#endif
}

unsigned long long platform_now_millis(void) {
#ifdef _WIN32
    return (unsigned long long)GetTickCount();
#else
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (unsigned long long)now.tv_sec * 1000ULL + (unsigned long long)now.tv_nsec / 1000000ULL;
#endif
}

void platform_sleep_ms(unsigned int milliseconds) {
#ifdef _WIN32
    Sleep(milliseconds);
#else
    struct timespec duration;

    duration.tv_sec = (time_t)(milliseconds / 1000U);
    duration.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;

    while (nanosleep(&duration, &duration) != 0 && errno == EINTR) {
    }
#endif
}
