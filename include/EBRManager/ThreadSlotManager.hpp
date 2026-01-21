#pragma once

#include <vector>
#include <atomic>
#include <memory>
#include <mutex>

#include "EBRManager/LockFreeReuseStack.hpp"
#include "EBRManager/ThreadSlot.hpp"

// [修改]: 彻底移除了 ThreadHeap 相关头文件，防止循环依赖导致的 Segfault
// #include "EBRManager/ThreadHeapAllocator.hpp" 
// #include "TierAlloc/ThreadHeap/ThreadHeap.hpp"

class ThreadSlotManager {
public:
    ThreadSlotManager();
    ~ThreadSlotManager();

    ThreadSlot* getLocalSlot();

    template<typename Callable>
    void forEachSlot(Callable func) const;

    // 禁用拷贝和移动
    ThreadSlotManager(const ThreadSlotManager&) = delete;
    ThreadSlotManager& operator=(const ThreadSlotManager&) = delete;
    ThreadSlotManager(ThreadSlotManager&&) = delete;
    ThreadSlotManager& operator=(ThreadSlotManager&&) = delete;

private:
    class LocalSlotProxy {
    public:
        LocalSlotProxy() noexcept : manager_(nullptr), slot_(nullptr) {}
        ~LocalSlotProxy();

        LocalSlotProxy(const LocalSlotProxy&) = delete;
        LocalSlotProxy& operator=(const LocalSlotProxy&) = delete;

        ThreadSlot* get() const noexcept { return slot_; }
        bool hasSlot() const noexcept { return slot_ != nullptr; }

        void acquire(ThreadSlotManager* manager, ThreadSlot* slot) {
            manager_ = manager;
            slot_ = slot;
        }

    private:
        ThreadSlotManager* manager_;
        ThreadSlot* slot_;
    };

    ThreadSlot* acquireSlot_();
    void releaseSlot_(ThreadSlot* slot) noexcept;
    ThreadSlot* expandAndAcquire();
    
    static constexpr size_t kInitialCapacity = 32;

    // [修改]: 定义一个结构体来同时持有 指针 和 数量
    // 我们不再依赖自定义 Deleter 来存储 count，这样更清晰且符合标准库用法
    struct Segment {
        std::unique_ptr<ThreadSlot[]> slots; // 自动管理内存释放 (使用系统 delete[])
        size_t count;                        // 记录这个段有多少个槽位
    };

    // [修改]: 使用标准 vector，分配器使用默认的 std::allocator (系统堆)
    using SegmentVector = std::vector<Segment>;

    LockFreeReuseStack<ThreadSlot> free_slots_;
    SegmentVector segments_;
    std::atomic<size_t> capacity_;
    
    mutable std::mutex resize_lock_;
};


template<typename Callable>
void ThreadSlotManager::forEachSlot(Callable func) const {
    // 加锁以确保在遍历期间 segments_ 向量不会被其他线程修改（例如扩容）。
    std::lock_guard<std::mutex> lock(resize_lock_);

    // 遍历每一个内存段 (Segment)
    for (const auto& segment : segments_) {
        // [修改]: 适配新的结构体访问方式
        const ThreadSlot* slots_array = segment.slots.get();
        const size_t count = segment.count;
        
        // 遍历该内存段中的每一个槽位
        for (size_t i = 0; i < count; ++i) {
            // 因为 forEachSlot 是 const 成员函数，所以我们传递的是 const ThreadSlot&
            func(slots_array[i]);
        }
    }
}