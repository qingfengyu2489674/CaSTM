#include "ThreadHeap/ChunkMetadata.hpp"
#include "common/GlobalConfig.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>


ChunkMetadata* ChunkMetadata::CreateAt(void *chunk_start, SizeClassPool *pool, uint32_t block_size) {
    assert(chunk_start != nullptr);
    assert(block_size >= sizeof(void*));

    ChunkMetadata* meta = new (chunk_start) ChunkMetadata();

    meta->owner_ = pool;
    meta->block_size_ = block_size;

    uintptr_t base = reinterpret_cast<uintptr_t>(chunk_start); 
    size_t meta_size = sizeof(ChunkMetadata);
    size_t head_size = (meta_size + kCacheLineSize - 1) & ~(kCacheLineSize - 1);
    
    meta->bump_ptr_ = reinterpret_cast<char*>(base + head_size);
    meta->end_ptr_ = reinterpret_cast<char*>(base + kChunkSize);
    
    size_t avail_bytes = kChunkSize - head_size;
    meta->max_block_count_ = static_cast<uint32_t>(avail_bytes / block_size);

    return meta;
}

[[nodiscard]] void* ChunkMetadata::allocate() {
    if(local_free_list_ != nullptr) {
        void* ptr = local_free_list_;
        local_free_list_ = *reinterpret_cast<void**>(ptr);
        
        allocated_count_++;
        return ptr;
    }

    if(bump_ptr_ + block_size_ <= end_ptr_) {
        void* ptr = static_cast<void*>(bump_ptr_);
        bump_ptr_ += block_size_;

        allocated_count_++;
        return ptr;
    }

    if(reclaim_remote_memory() > 0) {
        return allocate();
    }

    return nullptr;
}


bool ChunkMetadata::freeLocal(void* ptr) {
    *reinterpret_cast<void**>(ptr) = local_free_list_;
    local_free_list_ = ptr;

    allocated_count_--;

    return allocated_count_ == 0;
}


void ChunkMetadata::freeRemote(void* ptr) {
    remote_free_list_.push(ptr);
}

uint32_t ChunkMetadata::reclaim_remote_memory() {
    void* head = remote_free_list_.steal_all();
    if(head == nullptr)
        return 0;

    uint32_t count = 0;
    void* curr = head;
    void* tail = nullptr;

    while (curr) {
        tail = curr;
        count++;
        curr = *reinterpret_cast<void**>(curr);
    }

    *reinterpret_cast<void**>(tail) = local_free_list_;
    local_free_list_ = head;
    allocated_count_ -= count;

    return count;
}


uint32_t ChunkMetadata::block_size() const {
    return block_size_;
}

uint32_t ChunkMetadata::max_block_count() const {
    return max_block_count_;
}

uint32_t ChunkMetadata::allocated_count() const {
    return allocated_count_;
}

SizeClassPool* ChunkMetadata::owner() const {
    return owner_;
}

bool ChunkMetadata::isFull() const {
    return allocated_count_ == max_block_count_;
}

bool ChunkMetadata::isEmpty() const {
    return allocated_count_ == 0;
}

