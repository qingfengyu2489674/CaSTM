#include "ThreadHeap/ThreadHeap.hpp"
#include "ThreadHeap/Slab.hpp"
#include "ThreadHeap/Span.hpp"
#include "CentralHeap/CentralHeap.hpp"
#include "ThreadHeap/ChunkHeader.hpp"
#include "common/SizeClassConfig.hpp"
#include <cstdint>


[[nodiscard]] void* ThreadHeap::allocate(size_t nbytes) noexcept{
    // 1.大对象路径
    if(nbytes > SizeClassConfig::kMaxAlloc) [[unlikely]] {
        size_t total_req_size = nbytes + sizeof(Span);
        void* chunk_start = CentralHeap::GetInstance().allocateLarge(total_req_size);
        if (!chunk_start) return nullptr;

        Span* span = Span::CreateAt(chunk_start, total_req_size);
        return span->payload();
    }

    // 2.小对象路径
    ThreadHeap& heap = local_();
    uint32_t class_idx = SizeClassConfig::SizeToClass(nbytes);
    return heap.pools_[class_idx].allocate();
}

void ThreadHeap::deallocate(void *ptr) noexcept{

    if(!ptr) [[unlikely]] return;

    ChunkHeader* header = ChunkHeader::Get(ptr);

    // 1.小对象释放
    if(header->type == ChunkHeader::Type::SMALL) [[likely]] {
        Slab* slab = static_cast<Slab*>(header);
        ThreadHeap& heap = local_();
        
        if(heap.isOwnSlab_(slab)) {
            slab->owner()->deallocate(slab, ptr);
            return;
        }
        else {
            slab->freeRemote(ptr);
            return;
        }
    }
    // 2.大对象释放
    else {
        Span* span = static_cast<Span*>(header);
        CentralHeap::GetInstance().freeLarge(span, span->size());
    } 
}


ThreadHeap& ThreadHeap::local_() noexcept {
    static thread_local ThreadHeap heap;
    return heap;
}


ThreadHeap::ThreadHeap() noexcept {
    SizeClassConfig::Init();
   
    for (uint32_t i = 0; i < SizeClassConfig::kClassCount; ++i) {
        uint32_t block_size = SizeClassConfig::ClassToSize(i);
        pools_[i].Init(block_size, &chunk_cache_);
    }
}

[[nodiscard]] bool ThreadHeap::isOwnSlab_(const Slab* slab) const noexcept{
    uint32_t class_idx = SizeClassConfig::SizeToClass(slab->block_size());
    
    const SizeClassPool* expected_pool = &pools_[class_idx];
    return expected_pool == slab->owner();
}

