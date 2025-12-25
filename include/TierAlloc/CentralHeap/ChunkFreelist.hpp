#pragma once

#include "CentralHeap/sys/SpinLock.hpp"

class ChunkFreelist {
public:
    ChunkFreelist() = default;
    ~ChunkFreelist() = default;

    ChunkFreelist(const ChunkFreelist&) = delete;
    ChunkFreelist& operator=(const ChunkFreelist&) = delete;

    void* try_pop();
    void push(void* ptr);
    size_t size() const;


private:
    struct FreeNode {
        FreeNode* next;
    };
    
    FreeNode* head_ = nullptr;
    std::atomic<size_t> count_{0}; 
    
    mutable SpinLock lock_;
};