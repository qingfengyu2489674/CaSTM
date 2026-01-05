#pragma once

#include <atomic>
#include <thread>
#include <functional>
#include <immintrin.h> // for _mm_pause

class StripedLockTable {
public:
    // 2^20 = 1048576，足够大以减少哈希冲突
    static constexpr size_t kTableSize = 1 << 20; 
    static constexpr size_t kTableMask = kTableSize - 1; 

    static StripedLockTable& instance() noexcept {
        static StripedLockTable table;
        return table;
    } 

    // ============================================================
    // 核心接口：支持通过索引操作锁（用于解决死锁问题的关键）
    // ============================================================

    // 计算地址对应的锁索引
    size_t getStripeIndex(const void* addr) const noexcept {
        // 使用 std::hash 将地址映射到 0 ~ kTableMask
        return std::hash<const void*>{}(addr) & kTableMask;
    }

    // 通过索引加锁 (自旋锁实现)
    // 注意：这不是递归锁！同一个线程不能对同一个 index 连续调用两次 lockByIndex
    // 必须在调用前进行去重。
    void lockByIndex(size_t index) noexcept {
        LockEntry& entry = locks_[index];
        
        while (true) {
            // 1. TTAS 优化: Test (Read Only)
            // 先只读检查，如果已经被锁，则自旋等待，避免频繁写入导致缓存颠簸
            bool is_locked = entry.flag.load(std::memory_order_relaxed);
            if (is_locked) {
                _mm_pause(); // 提示 CPU 这是一个自旋循环
                continue;
            }

            // 2. Test-And-Set (Atomic Write)
            // 尝试原子地将 flag 从 false 置为 true
            // exchange 返回之前的值，如果之前是 false，说明我们要么抢到了锁
            if (!entry.flag.exchange(true, std::memory_order_acquire)) {
                return; // 成功拿到锁
            }
            
            // 3. 竞争激烈时的避让策略
            std::this_thread::yield(); 
        }
    }

    // 通过索引解锁
    void unlockByIndex(size_t index) noexcept {
        // memory_order_release 保证在此之前的写入对其他线程可见
        locks_[index].flag.store(false, std::memory_order_release);
    }

    // ============================================================
    // 兼容接口：直接通过地址操作 (内部转发给索引接口)
    // ============================================================

    void lock(const void* addr) noexcept {
        lockByIndex(getStripeIndex(addr));
    }

    void unlock(const void* addr) noexcept {
        unlockByIndex(getStripeIndex(addr));
    }

    // 检查是否被锁 (用于调试或验证)
    bool is_locked(const void* addr) const noexcept {
        size_t idx = getStripeIndex(addr);
        return locks_[idx].flag.load(std::memory_order_acquire);
    }

private:
    // 64字节对齐，防止 False Sharing (伪共享)
    // 确保每个锁独占一个 CPU 缓存行
    struct alignas(64) LockEntry {
        std::atomic<bool> flag{false};
    };

    StripedLockTable() {
        locks_ = new LockEntry[kTableSize];
        // 显式初始化（虽然 atomic 默认构造通常是 false，但为了安全）
        for (size_t i = 0; i < kTableSize; ++i) {
            locks_[i].flag.store(false, std::memory_order_relaxed);
        }
    }

    ~StripedLockTable() { 
        delete[] locks_; 
    }

    LockEntry* locks_ = nullptr;
};