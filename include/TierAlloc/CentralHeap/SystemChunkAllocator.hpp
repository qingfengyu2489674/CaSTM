#pragma once
#include <cstddef>

class SystemChunkAllocator {
public:
    void* allocate(size_t size);
    void deallocate(void* ptr, size_t size);

    SystemChunkAllocator() = default;
    ~SystemChunkAllocator() = default;

    // 禁用拷贝和移动
    SystemChunkAllocator(const SystemChunkAllocator&) = delete;
    SystemChunkAllocator& operator=(const SystemChunkAllocator&) = delete;
    SystemChunkAllocator(SystemChunkAllocator&&) = delete;
    SystemChunkAllocator& operator=(SystemChunkAllocator&&) = delete;
};