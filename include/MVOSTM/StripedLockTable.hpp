#pragma once 

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <functional>
#include <thread>


class StripedLockTable {
public:
    static constexpr size_t kTableSize = 1 << 20;
    static constexpr size_t kTableMask = kTableSize - 1; 

    static StripedLockTable& instance() noexcept{
        static StripedLockTable table;
        return table;
    } 

    StripedLockTable(const StripedLockTable&) = delete;
    StripedLockTable& operator=(const StripedLockTable&) = delete;

public:
    void lock(const void* addr) noexcept {
        LockEntry& entry = getEntry_(addr);
        while (entry.is_locked.exchange(true, std::memory_order_acquire)) {
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
        return;
    }

    bool is_locked(const void* addr) const noexcept {
        const LockEntry& entry = getEntry_(addr);
        return entry.is_locked.load(std::memory_order_acquire);
    }

private:
    StripedLockTable() = default;
    ~StripedLockTable() = default;

    struct alignas(64) LockEntry {
        std::atomic<bool> is_locked{false};
    };

    LockEntry& getEntry_ (const void* addr) noexcept{
        size_t idx = std::hash<const void*>{} (addr) & kTableMask;
        return locks_[idx];
    }

    const LockEntry& getEntry_ (const void* addr) const noexcept{
        size_t idx = std::hash<const void*>{} (addr) & kTableMask;
        return locks_[idx];
    }

private:
    std::array<LockEntry, kTableSize> locks_{};
};