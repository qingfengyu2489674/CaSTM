#include "gc_malloc/CentralHeap/CentralHeap.hpp"
#include <cassert>
#include <iostream>

// -----------------------------------------------------------------------------
// 1. 单例实现
// -----------------------------------------------------------------------------
CentralHeap& CentralHeap::GetInstance() {
    // C++11 保证静态局部变量的初始化是线程安全的
    // 不需要再手动写原子锁或 CAS 逻辑
    static CentralHeap instance;
    return instance;
}

// -----------------------------------------------------------------------------
// 2. 核心逻辑实现
// -----------------------------------------------------------------------------

void* CentralHeap::acquireChunk(size_t size) {
    // 校验大小
    assert(size == kChunkSize);

    // 步骤 A: 优先从空闲链表(缓存)中获取
    // FreeChunkListCache 内部已经有 mutex 锁，所以这里不需要再加锁
    void* chunk = free_list_.acquire();
    
    if (chunk != nullptr) { 
        return chunk;
    }

    // 步骤 B: 如果缓存里没有，直接向操作系统(内核)申请
    // AlignedChunkAllocatorByMmap 使用 mmap，也是线程安全的（除非含状态）
    // 通常 mmap 系统调用本身就是原子的
    chunk = allocator_.allocate(size);

    if (chunk == nullptr) {
        std::cerr << "[CentralHeap::acquireChunk] FATAL: mmap failed (OOM)." << std::endl;
    }

    return chunk;
}

void CentralHeap::releaseChunk(void* chunk, size_t size) {
    if (chunk == nullptr) return;
    
    assert(size == kChunkSize);

    // 将使用完的内存块归还到空闲链表
    // FreeChunkListCache 内部会自动加锁
    free_list_.deposit(chunk);

    // 注意：目前的策略是“只借不还给OS”，归还的内存会留在 free_list_ 中供下次使用。
    // 如果希望内存紧张时归还给 OS，可以在这里判断 free_list_.getCacheCount() 的大小，
    // 如果超过某个阈值，则调用 allocator_.deallocate(chunk, size) 并跳过 deposit。
}