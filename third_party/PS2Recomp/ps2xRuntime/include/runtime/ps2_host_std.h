// [gta-recomp] Host clock, timed waits and a recursive lock that cost the same with every Windows C++ library
// (docs/PERFORMANCE.md 29). llvm-mingw's libc++ makes steady_clock::now an out-of-line call with two 64-bit divisions
// (MSVC's STL: inline, a multiply for the usual 10 MHz QueryPerformanceCounter); its timed condition-variable waits
// truncate the timeout to whole milliseconds through system_clock, so a wait with less than 1 ms left returns at once
// and a wait_for(1 ms) loop or the last millisecond of a wait_until spins; its recursive_mutex is a CRITICAL_SECTION
// (MSVC's: an SRW lock). hostNow() / waitUntil() / waitFor() / RecursiveMutex are steady_clock::now,
// condition_variable's waits and recursive_mutex everywhere else; on Windows with libc++ they go to
// QueryPerformanceCounter, SleepConditionVariableSRW with the timeout rounded up (as MSVC's STL does) and a std::mutex
// (an SRW lock) with an owner and a depth.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <type_traits>

#if defined(_WIN32) && defined(_LIBCPP_VERSION)
#define PS2X_HOST_TIME_WIN32 1
#endif

#include <cfenv>
#include <cstdint>
#include <functional>
#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#elif !defined(__aarch64__) && !defined(_M_ARM64)
#error "ps2_host_std.h: x86-64 or arm64 only"
#endif
#if !defined(_WIN32)
#include <pthread.h>
#endif

namespace ps2x
{
    // [gta-recomp] The host's floating-point control word: MXCSR on x86-64, FPCR on arm64 (macOS, docs/MACOS.md).
    // The VU and the native draws run toward zero; the word is passed between threads (the render worker runs work in
    // the game thread's mode). fegetround / fesetround would also reprogram the x87 unit on x64 (PERFORMANCE.md 12),
    // and sse2neon's _mm_getcsr only emulates the rounding bits - so the register itself, on both.
    using FpControl = uint32_t;
#if defined(__x86_64__) || defined(_M_X64)
    inline FpControl fpControl() noexcept { return _mm_getcsr(); }
    inline void setFpControl(FpControl c) noexcept { _mm_setcsr(c); }
    // MXCSR.RC (bits 13-14): 0 nearest, 1 down, 2 up, 3 toward zero
    constexpr FpControl kFpRoundMask = 0x6000u;
    constexpr FpControl roundBits(int mode) noexcept
    {
        return (mode == FE_DOWNWARD ? 1u : mode == FE_UPWARD ? 2u : mode == FE_TOWARDZERO ? 3u : 0u) << 13;
    }
    constexpr int roundMode(FpControl c) noexcept
    {
        const unsigned rc = (c >> 13) & 3u;
        return rc == 0u ? FE_TONEAREST : rc == 1u ? FE_DOWNWARD : rc == 2u ? FE_UPWARD : FE_TOWARDZERO;
    }
    inline void cpuRelax() noexcept { _mm_pause(); }
#else
    inline FpControl fpControl() noexcept
    {
        uint64_t v;
        __asm__ __volatile__("mrs %0, fpcr" : "=r"(v));
        return static_cast<FpControl>(v);
    }
    inline void setFpControl(FpControl c) noexcept
    {
        const uint64_t v = c;
        __asm__ __volatile__("msr fpcr, %0" : : "r"(v));
    }
    // FPCR.RMode (bits 22-23): 0 nearest, 1 toward +inf, 2 toward -inf, 3 toward zero (scalar and NEON alike)
    constexpr FpControl kFpRoundMask = 3u << 22;
    constexpr FpControl roundBits(int mode) noexcept
    {
        return (mode == FE_UPWARD ? 1u : mode == FE_DOWNWARD ? 2u : mode == FE_TOWARDZERO ? 3u : 0u) << 22;
    }
    constexpr int roundMode(FpControl c) noexcept
    {
        const unsigned rm = (c >> 22) & 3u;
        return rm == 0u ? FE_TONEAREST : rm == 1u ? FE_UPWARD : rm == 2u ? FE_DOWNWARD : FE_TOWARDZERO;
    }
    inline void cpuRelax() noexcept { __asm__ __volatile__("yield"); }
#endif
    // the rounding mode alone, as FE_* values
    inline int fpRounding() noexcept { return roundMode(fpControl()); }
    inline void setFpRounding(int mode) noexcept { setFpControl((fpControl() & ~kFpRoundMask) | roundBits(mode)); }
    // `c` with round toward zero (the VU's mode)
    constexpr FpControl towardZero(FpControl c) noexcept { return (c & ~kFpRoundMask) | roundBits(FE_TOWARDZERO); }

    // [gta-recomp] A thread with a stack of its own size: macOS gives a std::thread 512 KB (Linux 8 MB, the
    // rlimit; Windows 1 MB), too little for deep guest call chains with the generated code's large frames. Windows
    // keeps std::thread (its stack follows the exe's reserve). `interactive`: macOS QoS user-interactive, so the
    // scheduler keeps it on the performance cores. Join or detach it before it is destroyed, as a std::thread.
    constexpr size_t kGuestThreadStack = 16u << 20;
    class Thread
    {
    public:
        Thread() = default;
        Thread(size_t stackBytes, bool interactive, std::function<void()> fn);
        Thread(Thread &&o) noexcept { swap(o); }
        Thread &operator=(Thread &&o) noexcept
        {
            Thread(std::move(o)).swap(*this);
            return *this;
        }
        Thread(const Thread &) = delete;
        Thread &operator=(const Thread &) = delete;
        ~Thread();
        bool joinable() const noexcept;
        void join();
        void detach();

    private:
        void swap(Thread &o) noexcept;
#if defined(_WIN32)
        std::thread m_thread;
#else
        pthread_t m_thread{};
        bool m_running = false;
#endif
    };

#if PS2X_HOST_TIME_WIN32
    std::chrono::steady_clock::time_point hostNow() noexcept; // the same values as steady_clock::now
    // false: timed out. Waits at most `ms` (the caller rounds up); wakes may be spurious.
    bool waitMs(std::condition_variable &cv, std::unique_lock<std::mutex> &lock, unsigned long ms) noexcept;
#else
    inline std::chrono::steady_clock::time_point hostNow() noexcept { return std::chrono::steady_clock::now(); }
#endif

    template <class Clock, class Duration>
    std::cv_status waitUntil(std::condition_variable &cv, std::unique_lock<std::mutex> &lock,
                             const std::chrono::time_point<Clock, Duration> &deadline)
    {
#if PS2X_HOST_TIME_WIN32
        static_assert(std::is_same_v<Clock, std::chrono::steady_clock>, "steady_clock deadlines only");
        const auto now = hostNow();
        if (now >= deadline)
            return std::cv_status::timeout;
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
        waitMs(cv, lock, left >= 0x7fffffff ? 0x7fffffffUL : static_cast<unsigned long>(left));
        return hostNow() >= deadline ? std::cv_status::timeout : std::cv_status::no_timeout;
#else
        return cv.wait_until(lock, deadline);
#endif
    }

    template <class Clock, class Duration, class Predicate>
    bool waitUntil(std::condition_variable &cv, std::unique_lock<std::mutex> &lock,
                   const std::chrono::time_point<Clock, Duration> &deadline, Predicate pred)
    {
#if PS2X_HOST_TIME_WIN32
        while (!pred())
            if (waitUntil(cv, lock, deadline) == std::cv_status::timeout)
                return pred();
        return true;
#else
        return cv.wait_until(lock, deadline, std::move(pred));
#endif
    }

    template <class Rep, class Period>
    std::cv_status waitFor(std::condition_variable &cv, std::unique_lock<std::mutex> &lock,
                           const std::chrono::duration<Rep, Period> &d)
    {
#if PS2X_HOST_TIME_WIN32
        return waitUntil(cv, lock, hostNow() + std::chrono::ceil<std::chrono::steady_clock::duration>(d));
#else
        return cv.wait_for(lock, d);
#endif
    }

    template <class Rep, class Period, class Predicate>
    bool waitFor(std::condition_variable &cv, std::unique_lock<std::mutex> &lock,
                 const std::chrono::duration<Rep, Period> &d, Predicate pred)
    {
#if PS2X_HOST_TIME_WIN32
        return waitUntil(cv, lock, hostNow() + std::chrono::ceil<std::chrono::steady_clock::duration>(d), std::move(pred));
#else
        return cv.wait_for(lock, d, std::move(pred));
#endif
    }

#if PS2X_HOST_TIME_WIN32
    class RecursiveMutex
    {
    public:
        void lock()
        {
            const std::thread::id me = std::this_thread::get_id();
            if (m_owner.load(std::memory_order_relaxed) == me) // only this thread ever stores its own id
            {
                ++m_depth;
                return;
            }
            m_mutex.lock();
            m_owner.store(me, std::memory_order_relaxed);
            m_depth = 1;
        }
        bool try_lock()
        {
            const std::thread::id me = std::this_thread::get_id();
            if (m_owner.load(std::memory_order_relaxed) == me)
            {
                ++m_depth;
                return true;
            }
            if (!m_mutex.try_lock())
                return false;
            m_owner.store(me, std::memory_order_relaxed);
            m_depth = 1;
            return true;
        }
        void unlock()
        {
            if (--m_depth)
                return;
            m_owner.store(std::thread::id(), std::memory_order_relaxed);
            m_mutex.unlock();
        }

    private:
        std::mutex m_mutex;
        std::atomic<std::thread::id> m_owner{};
        unsigned m_depth = 0;
    };
#else
    using RecursiveMutex = std::recursive_mutex;
#endif
}
