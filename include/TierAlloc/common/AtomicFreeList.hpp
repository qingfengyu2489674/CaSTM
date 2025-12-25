#pragma once
 
#include <atomic>
class AtomicFreeList {
public:
    struct Node {
        Node* next;
    };

    AtomicFreeList() noexcept = default;
    ~AtomicFreeList() = default;

    // 禁用拷贝和移动 (std::atomic 不可拷贝)
    AtomicFreeList(const AtomicFreeList&) = delete;
    AtomicFreeList& operator=(const AtomicFreeList&) = delete;
    AtomicFreeList(AtomicFreeList&&) = delete;
    AtomicFreeList& operator=(AtomicFreeList&&) = delete;

    void push(void* ptr) noexcept {
        if(ptr == nullptr)
            return;

        Node* new_node = static_cast<Node*>(ptr);

        Node* old_node = head_.load(std::memory_order_relaxed);

        do {
            new_node->next = old_node;
        } while (!head_.compare_exchange_weak(
            old_node,
            new_node, 
            std::memory_order_release, 
            std::memory_order_relaxed));  
    }

    [[nodiscard]] void* steal_all() noexcept {
        return head_.exchange(nullptr, std::memory_order_acq_rel);
    }
private:
    std::atomic<Node*> head_{nullptr};
};