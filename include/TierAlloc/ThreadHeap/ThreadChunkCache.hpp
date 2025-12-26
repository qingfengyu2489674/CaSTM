#pragma once
#include "CentralHeap/CentralHeap.hpp"
#include "common/GlobalConfig.hpp"
#include <cstddef>

class ThreadChunkCache {
public:
    ThreadChunkCache() = default;
    
    // 析构时，把缓存里剩下的都还给中心堆
    ~ThreadChunkCache() {
        while (free_list_head_) {
            void* next = *reinterpret_cast<void**>(free_list_head_);
            CentralHeap::GetInstance().returnChunk(free_list_head_);
            free_list_head_ = next;
        }
    }

    // 获取 Chunk
    [[nodiscard]] void* fetchChunk() {
        if (free_list_head_) {
            void* chunk = free_list_head_;
            free_list_head_ = *reinterpret_cast<void**>(chunk);
            count_--;
            return chunk;
        }
        return CentralHeap::GetInstance().fetchChunk();
    }

    // 归还 Chunk
    void returnChunk(void* chunk) {
        // 1. 如果缓存满了，直接还给中心堆
        if (count_ >= kMaxCacheSize) {
            CentralHeap::GetInstance().returnChunk(chunk);
            return;
        }

        // 2. 放入缓存 (头插法)
        *reinterpret_cast<void**>(chunk) = free_list_head_;
        free_list_head_ = chunk;
        count_++;
    }

private:
    void* free_list_head_ = nullptr;
    
    size_t count_ = 0;
    
    static constexpr size_t kMaxCacheSize = kMaxThreadCacheSize; 
};