#pragma once

// 如果是 ARM 架构 (如 Mac M1/M2 或服务器)，需要改为 __builtin_yield() 或 asm("yield")
#include <atomic>
#include <cstdlib>
#include <emmintrin.h>
#include <sys/cdefs.h>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #include <immintrin.h>
#else
    // 兼容非 x86 平台
    #define _mm_pause() std::this_thread::yield() 
#endif

class SpinLock {
public:
    SpinLock() = default;

    // 锁对象不可复制/移动
    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;

    __attribute__((always_inline)) void lock() noexcept {
        while (flag_.test_and_set(std::memory_order_acquire)) {
            _mm_pause();
        }
    }

    __attribute__((always_inline)) void unlock() noexcept {
        flag_.clear(std::memory_order_release);
    }

private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};


class SpinLockGuard {
public:
    explicit SpinLockGuard(SpinLock& lock): lock_(lock) {
        lock_.lock();
    }

    ~SpinLockGuard() {
        lock_.unlock();
    }

    // 禁用拷贝/移动
    SpinLockGuard(const SpinLockGuard&) = delete;
    SpinLockGuard& operator=(const SpinLockGuard&) = delete;

private:
    SpinLock& lock_;
};