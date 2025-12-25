#include "CentralHeap/ChunkFreelist.hpp"
#include "CentralHeap/sys/SpinLock.hpp"

#include <atomic>
#include <cassert>

void* ChunkFreelist::try_pop() {
    SpinLockGuard guard(lock_);

    if(head_ == nullptr)
    {
        assert(count_ == 0);
        return nullptr;
    }

    FreeNode* node = head_;
    head_ = head_->next;
    count_.fetch_sub(1, std::memory_order_relaxed);

    return static_cast<void*>(node);
}

void ChunkFreelist::push(void* ptr) {
    if(ptr == nullptr)
        return;

    FreeNode* node = static_cast<FreeNode*>(ptr);

    SpinLockGuard guard(lock_);

    node->next = head_;
    head_ = node;
    count_.fetch_add(1, std::memory_order_relaxed);
}

size_t ChunkFreelist::size() const {
    return count_;
}