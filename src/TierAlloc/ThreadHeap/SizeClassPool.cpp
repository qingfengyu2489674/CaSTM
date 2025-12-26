#include "ThreadHeap/SizeClassPool.hpp"
#include "ThreadHeap/ThreadChunkCache.hpp"
#include "CentralHeap/CentralHeap.hpp"
#include "ThreadHeap/Slab.hpp"
#include "common/GlobalConfig.hpp"

SizeClassPool::~SizeClassPool() {
    auto& central = CentralHeap::GetInstance();

    if(current_slab_ != nullptr) {
        central.returnChunk(reinterpret_cast<void*>(current_slab_));
    }

    while(!partial_list_.empty()) {
        Slab* slab = partial_list_.pop_front();
        central.returnChunk(reinterpret_cast<void*>(slab));
    }

    while(!full_list_.empty()) {
        Slab* slab = full_list_.pop_front();
        central.returnChunk(reinterpret_cast<void*>(slab));
    }
}

void* SizeClassPool::allocate() {
    if(current_slab_) {
        void* ptr = current_slab_->allocate();
        if(ptr) return ptr;

        full_list_.push_back(current_slab_);
        current_slab_ = nullptr;
    }

    if(!partial_list_.empty()) {
        return allocFromPartial_();
    }

    if(!full_list_.empty()) {
        void* ptr = allocFromRescue_();
        if(ptr) return ptr;
    }

    return allocFromNew_();
}


void SizeClassPool::deallocate(Slab* slab, void* ptr) {
    bool was_full = slab->isFull();

    bool is_local_empty = slab->freeLocal(ptr);

    if(is_local_empty) {
        if(slab->reclaimRemoteMemory() > 0) {
            if(was_full) {
                full_list_.remove(slab);
                partial_list_.push_front(slab);
            }
        }
        else {
            if (current_slab_ == slab) {
                current_slab_ = nullptr;
            } 
            else if (was_full) {
                full_list_.remove(slab);
            } 
            else {
                partial_list_.remove(slab);
            }
            slab->DestroyForReuse();
            thread_chunk_cache_->returnChunk(reinterpret_cast<void*>(slab)); 
        }
    }
    else if (was_full) {
        full_list_.remove(slab);
        partial_list_.push_front(slab);
    }
}

void* SizeClassPool::allocFromPartial_() {
    Slab* slab = partial_list_.pop_front();
    current_slab_ = slab;
    return current_slab_->allocate();
}

[[nodiscard]] void* SizeClassPool::allocFromRescue_() {
    int checks = 0;

    while (!full_list_.empty() && checks < kMaxPoolRescueChecks) {
        Slab* victim = full_list_.front();

        if(victim->reclaimRemoteMemory() > 0) {
            full_list_.remove(victim); 
            current_slab_ = victim;
            return current_slab_->allocate();
        } 
        else {
            full_list_.move_head_to_tail();
        }
        checks++;
    }
    return nullptr;
}

void* SizeClassPool::allocFromNew_() {
    void* chunk = thread_chunk_cache_->fetchChunk(); 
    if(chunk == nullptr)
        return nullptr;

    Slab* new_slab = Slab::CreateAt(chunk, this, block_size_);

    current_slab_ = new_slab;
    return current_slab_->allocate();
}