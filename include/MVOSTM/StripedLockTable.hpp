// include/StripedLockTable.hpp
#pragma once

#include <atomic>
#include <vector>
#include <thread>
#include <functional>
#include <immintrin.h> // for _mm_pause

class StripedLockTable {
public:
    static constexpr size_t kTableSize = 1 << 20; 
    static constexpr size_t kTableMask = kTableSize - 1; 

    static StripedLockTable& instance() noexcept {
        static StripedLockTable table;
        return table;
    } 

    void lock(const void* addr) noexcept {
        LockEntry& entry = getEntry_(addr);
        while (true) {
            // 1. Test (Read Only) - 减少总线争用
            bool is_locked = entry.is_locked.load(std::memory_order_relaxed);
            if (is_locked) {
                _mm_pause(); // CPU 指令级避让
                continue;
            }

            // 2. Test-And-Set (Atomic Write)
            if (!entry.is_locked.exchange(true, std::memory_order_acquire)) {
                return; // 成功拿锁
            }
            
            // 3. 竞争失败，让出时间片
            std::this_thread::yield(); 
        }
    }

    void unlock(const void* addr) noexcept {
        LockEntry& entry = getEntry_(addr);
        entry.is_locked.store(false, std::memory_order_release);
    }

    bool is_locked(const void* addr) const noexcept {
        const LockEntry& entry = getEntry_(addr);
        return entry.is_locked.load(std::memory_order_acquire);
    }

    // 以前的 try_lock (保留兼容性)
    bool try_lock(const void* addr) noexcept {
        LockEntry& entry = getEntry_(addr);
        bool is_locked = entry.is_locked.load(std::memory_order_relaxed);
        if (is_locked) return false;
        return !entry.is_locked.exchange(true, std::memory_order_acquire);
    }

private:
    struct alignas(64) LockEntry {
        std::atomic<bool> is_locked;
    };

    StripedLockTable() {
        locks_ = new LockEntry[kTableSize];
        for (size_t i = 0; i < kTableSize; ++i) {
            locks_[i].is_locked.store(false, std::memory_order_relaxed);
        }
    }

    ~StripedLockTable() { delete[] locks_; }

    LockEntry* locks_ = nullptr;

    LockEntry& getEntry_ (const void* addr) const noexcept {
        size_t idx = std::hash<const void*>{} (addr) & kTableMask;
        return locks_[idx];
    }
};