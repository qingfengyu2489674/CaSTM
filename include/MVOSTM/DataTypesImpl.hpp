#pragma  once

#include "DataTypes.hpp"
#include "TierAlloc/ThreadHeap/ThreadHeap.hpp"
#include <atomic>
#include <cstdint>

template<typename T>
struct VersionNode {
    T payload;
    uint64_t commit_ts;
    VersionNode* prev;

    template<typename... Args>
    VersionNode(uint64_t v, VersionNode* p, Args&&... args) 
        : payload(std::forward<Args>(args)...)
        , commit_ts(v)
        , prev(p)
        {}

    static void* operator new(size_t size) { return ThreadHeap::allocate(size); }
    static void operator delete(void *p) { ThreadHeap::deallocate(p); }
};

template<typename T>
template<typename... Args>
TMVar<T>::TMVar(Args&&... args) {
    Node* init_node = new Node(0, nullptr, std::forward<Args>(args)...);
    head_.store(init_node, std::memory_order_release);
}

template<typename T>
TMVar<T>::~TMVar() {
    Node* curr = head_.load(std::memory_order_acquire);
    while (curr) {
        Node* prev = curr->prev;
        delete curr;
        curr = prev;
    }
}