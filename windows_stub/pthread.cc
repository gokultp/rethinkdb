
#include <pthread.h>

#include <tuple>

#include "errors.hpp"
#include "logger.hpp"

int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine) (void *), void *arg) {
    typedef std::tuple<void *(*)(void*), void*> data_t;
    data_t* data = new data_t(start_routine, arg);
    static const auto go = [](void* rawdata) {
        data_t* data = static_cast<data_t*>(rawdata);
        auto f = std::get<0>(*data);
        auto args = std::get<1>(*data);
        delete data;
        void* res = f(args);
        return reinterpret_cast<DWORD>(res); // TODO ATN: void* doesn't fit in DWORD
    };
    HANDLE handle = CreateThread(nullptr, 0, go, static_cast<void*>(data), 0, nullptr);
    if (handle == NULL) {
        logERR("CreateThread failed: %s", winerr_string(GetLastError()));
        return EINVAL; // TODO: check GetLastError()
    } else {
        *thread = handle;
        return 0;
    }
}

int pthread_join(pthread_t other, void** retval) {
    DWORD res = WaitForSingleObject(other, INFINITE);
    if (res != WAIT_OBJECT_0) {
        return EINVAL; // ATN TODO: decode res, see docs for WaitForSingleObject
    } else {
        if (retval != nullptr) {
            DWORD exit_code;
            guarantee_winerr(GetExitCodeThread(other, &exit_code));
            *retval = reinterpret_cast<void*>(exit_code); // TODO ATN: void* doesn't fit in DWORD
        }
        return 0;
    }
}

int pthread_mutex_init(pthread_mutex_t *mutex, void *opts) {
    rassert(opts == NULL, "this implementation of pthread_mutex_init does not support attributes");
    InitializeCriticalSection(mutex); // TODO ATN: spin count?
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t* mutex) {
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t* mutex) {
    EnterCriticalSection(mutex);
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t* mutex) {
    LeaveCriticalSection(mutex);
    return 0;
}

int pthread_attr_init(pthread_attr_t*) { return 0;  }
int pthread_attr_setstacksize(pthread_attr_t*, size_t) { return 0; }
int pthread_attr_destroy(pthread_attr_t*) { return 0; }

int pthread_cond_init(pthread_cond_t* cond, void*) {
    InitializeConditionVariable(cond);
    return 0;
}

int pthread_cond_destroy(pthread_cond_t* cond) {
    return 0;
}

int pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
    BOOL res = SleepConditionVariableCS(cond, mutex, INFINITE);
    if (res) {
        return 0;
    } else {
        return EINVAL; // TODO;
    }
}

int pthread_cond_signal(pthread_cond_t* cond) {
    WakeConditionVariable(cond);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t* cond) {
    WakeAllConditionVariable(cond);
    return 0;
}

int pthread_once(bool *complete, void(*init)(void)) {
    if (*complete == PTHREAD_ONCE_INIT) {
        *complete = PTHREAD_ONCE_COMPLETED;
        init();
    }
    return 0;
}