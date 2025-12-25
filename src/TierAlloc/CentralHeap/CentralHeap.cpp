#include "CentralHeap/CentralHeap.hpp"
#include "common/GlobalConfig.hpp"
#include <cassert>
#include <cstddef>

CentralHeap& CentralHeap::GetInstance() {
    static CentralHeap instance;
    return instance;
}

void* CentralHeap::fetchChunk() {
    void* ptr = free_list_.try_pop();
    if (ptr != nullptr) {
        return ptr;
    }

    return system_allocator_.allocate(kChunkSize);
}


void CentralHeap::returnChunk(void* ptr) {
    if(ptr == nullptr) 
        return;

    assert((reinterpret_cast<uintptr_t>(ptr) & (kChunkAlignment - 1 )) == 0);

    if(free_list_.size() >= kMaxCentralCacheSize) {
        system_allocator_.deallocate(ptr, kChunkSize);
        return;
    }

    free_list_.push(ptr);
    return;
}


size_t CentralHeap::getFreeChunkCount() {
    return free_list_.size();
}