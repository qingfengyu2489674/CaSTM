#include "EBRManager/ThreadSlotManager.hpp"
#include <utility>
#include <new>      // for std::nothrow
#include <mutex>

ThreadSlotManager::ThreadSlotManager()
    : capacity_(0) {}

ThreadSlotManager::~ThreadSlotManager() {}


ThreadSlot* ThreadSlotManager::getLocalSlot() {

    thread_local LocalSlotProxy g_local_slot_proxy;

    if(!g_local_slot_proxy.hasSlot()) {
        ThreadSlot* new_slot = acquireSlot_();
        if(!new_slot) {
            return nullptr;
        }
        g_local_slot_proxy.acquire(this, new_slot);
    }

    return g_local_slot_proxy.get();
}

void ThreadSlotManager::releaseSlot_(ThreadSlot* slot) noexcept{
    free_slots_.push(slot);
}

ThreadSlot* ThreadSlotManager::acquireSlot_() {

    ThreadSlot* slot = free_slots_.pop();
    if(slot) {
        return slot;
    }

    return expandAndAcquire();
}

ThreadSlot* ThreadSlotManager::expandAndAcquire() {
    // 使用 std::mutex 对应的锁保护
    std::lock_guard<std::mutex> lock(resize_lock_);

    // Double Check: 再次检查空闲链表
    ThreadSlot* slot = free_slots_.pop();
    if(slot) {
        return slot;
    }

    const size_t current_capacity = capacity_.load(std::memory_order_relaxed);
    const size_t new_slots_to_add = (current_capacity == 0) ? kInitialCapacity : current_capacity;

    // 1. 使用系统堆分配内存 (System Heap Allocation)
    // 使用 std::nothrow 以便在失败时返回 nullptr，而不是抛出异常
    ThreadSlot* new_slots_array = new (std::nothrow) ThreadSlot[new_slots_to_add];
    
    if (!new_slots_array) {
        return nullptr;
    }

    // 2. 初始化 Segment 结构体
    Segment new_segment;
    // 将原生指针的所有权转移给 unique_ptr，析构时会自动调用 delete[]
    new_segment.slots.reset(new_slots_array); 
    // 记录该段的大小
    new_segment.count = new_slots_to_add;

    // 3. 将前 N-1 个槽位放入空闲链表
    // 注意：new[] 已经自动调用了 ThreadSlot 的默认构造函数，不需要再 placement new
    for(size_t i = 0; i < new_slots_to_add - 1; ++i) {
        free_slots_.push(&new_slots_array[i]);
    }

    // 4. 将新段移入容器管理
    segments_.push_back(std::move(new_segment));
    
    capacity_.fetch_add(new_slots_to_add, std::memory_order_relaxed);

    // 返回最后一个槽位给当前请求者
    return &new_slots_array[new_slots_to_add - 1];
}

ThreadSlotManager::LocalSlotProxy::~LocalSlotProxy() {
    if(manager_ && slot_) {
        manager_->releaseSlot_(slot_);
    }
}