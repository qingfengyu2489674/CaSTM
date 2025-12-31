#pragma once 

#include <atomic>
#include <cstddef>
#include <functional>
#include <thread>
#include <new> // for std::hardware_destructive_interference_size

class StripedLockTable {
public:
    // 1M 的锁表在堆上分配是可以的
    static constexpr size_t kTableSize = 1 << 20; 
    static constexpr size_t kTableMask = kTableSize - 1; 

    static StripedLockTable& instance() noexcept {
        static StripedLockTable table;
        return table;
    } 

    StripedLockTable(const StripedLockTable&) = delete;
    StripedLockTable& operator=(const StripedLockTable&) = delete;

public:
    void lock(const void* addr) noexcept {
        LockEntry& entry = getEntry_(addr);
        // 自旋等待
        while (entry.is_locked.exchange(true, std::memory_order_acquire)) {
            // 简单的退避策略
            std::this_thread::yield(); 
        }
    }

    bool try_lock(const void* addr) noexcept {
        LockEntry& entry = getEntry_(addr);
        return !entry.is_locked.exchange(true, std::memory_order_acquire);
    }

    void unlock(const void* addr) noexcept {
        LockEntry& entry = getEntry_(addr);
        entry.is_locked.store(false, std::memory_order_release);
    }

    bool is_locked(const void* addr) const noexcept {
        const LockEntry& entry = getEntry_(addr);
        return entry.is_locked.load(std::memory_order_acquire);
    }

private:
    // 构造函数：在堆上分配内存
    StripedLockTable() {
        locks_ = new LockEntry[kTableSize];
        // new[] 出来的 atomic 默认是未初始化的 (trivial default constructor)
        // 我们需要手动初始化它们为 false
        for (size_t i = 0; i < kTableSize; ++i) {
            locks_[i].is_locked.store(false, std::memory_order_relaxed);
        }
    }

    // 析构函数：释放内存 (虽然单例通常不析构，但为了严谨)
    ~StripedLockTable() {
        delete[] locks_;
    }

    // 缓存行对齐，防止 False Sharing
    struct alignas(64) LockEntry {
        std::atomic<bool> is_locked;
    };

    LockEntry& getEntry_ (const void* addr) noexcept {
        size_t idx = std::hash<const void*>{} (addr) & kTableMask;
        return locks_[idx];
    }

    const LockEntry& getEntry_ (const void* addr) const noexcept {
        size_t idx = std::hash<const void*>{} (addr) & kTableMask;
        return locks_[idx];
    }

private:
    // 改为指针，不再是巨大的 std::array
    LockEntry* locks_ = nullptr;
};