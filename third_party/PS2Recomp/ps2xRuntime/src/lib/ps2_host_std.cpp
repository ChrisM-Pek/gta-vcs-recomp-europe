// [gta-recomp] See runtime/ps2_host_std.h. The clock / waits: Windows with libc++ only (llvm-mingw); elsewhere the
// header is inline. ps2x::Thread: every platform.
#include "runtime/ps2_host_std.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

namespace ps2x
{
#if defined(_WIN32)
    Thread::Thread(size_t, bool, std::function<void()> fn) : m_thread(std::move(fn)) {}
    Thread::~Thread()
    {
        if (m_thread.joinable())
            std::terminate();
    }
    bool Thread::joinable() const noexcept { return m_thread.joinable(); }
    void Thread::join() { m_thread.join(); }
    void Thread::detach() { m_thread.detach(); }
    void Thread::swap(Thread &o) noexcept { m_thread.swap(o.m_thread); }
#else
    namespace
    {
        struct Start
        {
            std::function<void()> fn;
            bool interactive;
        };

        void *threadMain(void *arg)
        {
            std::unique_ptr<Start> start(static_cast<Start *>(arg));
#if defined(__APPLE__)
            if (start->interactive)
                pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
            start->fn(); // an escaping exception ends the program, as with std::thread
            return nullptr;
        }
    }

    Thread::Thread(size_t stackBytes, bool interactive, std::function<void()> fn)
    {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        if (stackBytes)
            pthread_attr_setstacksize(&attr, stackBytes);
        auto *start = new Start{std::move(fn), interactive};
        const int err = pthread_create(&m_thread, &attr, threadMain, start);
        pthread_attr_destroy(&attr);
        if (err != 0)
        {
            delete start;
            std::fprintf(stderr, "[thread] pthread_create failed (%d)\n", err);
            std::abort();
        }
        m_running = true;
    }
    Thread::~Thread()
    {
        if (m_running)
            std::terminate();
    }
    bool Thread::joinable() const noexcept { return m_running; }
    void Thread::join()
    {
        if (m_running)
            pthread_join(m_thread, nullptr);
        m_running = false;
    }
    void Thread::detach()
    {
        if (m_running)
            pthread_detach(m_thread);
        m_running = false;
    }
    void Thread::swap(Thread &o) noexcept
    {
        std::swap(m_thread, o.m_thread);
        std::swap(m_running, o.m_running);
    }
#endif
}

#if PS2X_HOST_TIME_WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace ps2x
{
    namespace
    {
        int64_t qpcFrequency() noexcept
        {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            return f.QuadPart;
        }
    }

    std::chrono::steady_clock::time_point hostNow() noexcept
    {
        // fixed at boot (Windows' documentation); a local static, as other files' globals read the clock while they
        // are initialised
        static const int64_t f = qpcFrequency();
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        // libc++'s own formula (whole seconds, then the fraction), so the values equal steady_clock::now's; 10 MHz,
        // what Windows 10 and later report, is the same as a multiply
        const int64_t ns = f == 10000000 ? c.QuadPart * 100
                                         : c.QuadPart / f * 1000000000 + c.QuadPart % f * 1000000000 / f;
        return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(ns));
    }

    bool waitMs(std::condition_variable &cv, std::unique_lock<std::mutex> &lock, unsigned long ms) noexcept
    {
        // libc++'s condition_variable and mutex are a CONDITION_VARIABLE and an SRWLOCK on Windows (what its own
        // __libcpp_condvar_timedwait passes); the lock stays held across the call as the wait expects
        return SleepConditionVariableSRW(reinterpret_cast<PCONDITION_VARIABLE>(cv.native_handle()),
                                         reinterpret_cast<PSRWLOCK>(lock.mutex()->native_handle()), ms, 0) != 0;
    }
}
#endif
