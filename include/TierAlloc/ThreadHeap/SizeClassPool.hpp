#pragma once

#include "ThreadHeap/Slab.hpp"
#include "ThreadHeap/SlabList.hpp"

#include <cassert>
#include <cstdint>


class ThreadChunkCache;

class SizeClassPool {

public:
    [[nodiscard]] void* allocate();
    void deallocate(Slab* slab, void* ptr);

    [[nodiscard]] uint32_t getBlockSize() const { return block_size_; }

public:
    SizeClassPool() = default; 

    void Init(size_t block_size, ThreadChunkCache* cache) {
        block_size_ = block_size;
        thread_chunk_cache_ = cache;
    }

    ~SizeClassPool();

    SizeClassPool(const SizeClassPool&) = delete;
    SizeClassPool& operator=(const SizeClassPool&) = delete;

private:
    void* allocFromPartial_();
    void* allocFromNew_();
    [[nodiscard]] void* allocFromRescue_();

private:
    uint32_t block_size_ = 0;

    Slab* current_slab_ = nullptr;
    SlabList partial_list_;
    SlabList full_list_;

    ThreadChunkCache* thread_chunk_cache_;
};