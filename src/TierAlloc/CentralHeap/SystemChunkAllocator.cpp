#include "TierAlloc/CentralHeap/SystemChunkAllocator.hpp"
#include "TierAlloc/common/GlobalConfig.hpp"
#include "TierAlloc/CentralHeap/sys/mman.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>

void* SystemChunkAllocator::allocate(size_t size) {
    assert(size > 0 && size % kChunkSize == 0 && "Allocation size must be a positive multiple of kChunkSize (GlobalConfig)");
    
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    int protection = PROT_READ | PROT_WRITE;

    const size_t alignment = kChunkAlignment;
    const size_t over_alloc_size = size + alignment;

    void* raw_ptr = mmap(nullptr, over_alloc_size, protection, flags, -1, 0);

    if(raw_ptr == MAP_FAILED)
        return nullptr;

    uintptr_t raw_addr = reinterpret_cast<uintptr_t>(raw_ptr);

    // 向上对齐算法: (addr + align - 1) & ~(align - 1)
    uintptr_t aligned_addr = (raw_addr + alignment - 1) & ~(alignment - 1);
    
    uintptr_t aligned_end_addr = aligned_addr + size;
    uintptr_t raw_end_addr = raw_addr + over_alloc_size;

    // 裁剪头部 (Trim Head)
    size_t head_trim_size = aligned_addr - raw_addr;
    if (head_trim_size > 0) {
        munmap(raw_ptr, head_trim_size);
    }

    // 裁剪尾部 (Trim Tail)
    size_t tail_trim_size = raw_end_addr - aligned_end_addr;
    if (tail_trim_size > 0) {
        void* start_of_tail = reinterpret_cast<void*>(aligned_end_addr);
        // 调用你封装的 munmap
        munmap(start_of_tail, tail_trim_size);
    }

    return reinterpret_cast<void*>(aligned_addr);
}

void SystemChunkAllocator::deallocate(void* ptr, size_t size) {
    assert(ptr != nullptr && "Cannot deallocate a null pointer.");
    assert(size > 0 && "Deallocation size must be positive.");

    // 调用你封装的 munmap
    if (munmap(ptr, size) != 0) {
        std::cerr << "[FATAL] SystemChunkAllocator::deallocate: munmap failed. "
                  << "System error: " << strerror(errno) << " (errno=" << errno << ")." 
                  << std::endl;
        assert(false && "munmap failed! Check if ptr and size are valid.");
    }
}