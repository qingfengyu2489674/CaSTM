#pragma once
#include "TierAlloc/ThreadHeap/ThreadHeap.hpp"

// 使用 detail 命名空间，告诉用户：这里的代码不要乱动
namespace detail {

template<typename T>
struct VersionNode {
    uint64_t write_ts;
    VersionNode* prev;
    T payload;

    template<typename... Args>
    VersionNode(uint64_t v, VersionNode* p, Args&&... args) 
        : payload(std::forward<Args>(args)...)
        , write_ts(v)
        , prev(p)
        {}

    static void* operator new(size_t size) { return ThreadHeap::allocate(size); }
    static void operator delete(void *p) { ThreadHeap::deallocate(p); }
};

}