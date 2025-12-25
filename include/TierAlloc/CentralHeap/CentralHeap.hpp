#pragma once 

#include "CentralHeap/ChunkFreelist.hpp"
#include "CentralHeap/SystemChunkAllocator.hpp"
#include <cstddef>


class CentralHeap {
public:
    static CentralHeap& GetInstance();

    void* fetchChunk();
    void returnChunk(void* ptr);
    size_t getFreeChunkCount();

private:
    CentralHeap() = default;
    ~CentralHeap() = default;

    // 严禁拷贝/移动
    CentralHeap(const CentralHeap&) = delete;
    CentralHeap& operator=(const CentralHeap&) = delete;
    CentralHeap(CentralHeap&&) = delete;
    CentralHeap& operator=(CentralHeap&&) = delete;


private:
    SystemChunkAllocator system_allocator_;
    ChunkFreelist free_list_;
};