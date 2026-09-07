#pragma once

#include <cstddef>

#include "TierAlloc/ThreadHeap/ThreadHeap.hpp"

// The default keeps each STM backend's historical allocator choice:
// OccSTM uses TierAlloc and WwSTM uses the system allocator.  Benchmark
// builds override this with STM_ALLOCATOR_MODE=0 (system) or 1 (tier).
#define STM_ALLOCATOR_MODE_NATIVE (-1)
#define STM_ALLOCATOR_MODE_SYSTEM 0
#define STM_ALLOCATOR_MODE_TIER 1

#ifndef STM_ALLOCATOR_MODE
#define STM_ALLOCATOR_MODE STM_ALLOCATOR_MODE_NATIVE
#endif

#if STM_ALLOCATOR_MODE < STM_ALLOCATOR_MODE_NATIVE || \
    STM_ALLOCATOR_MODE > STM_ALLOCATOR_MODE_TIER
#error "STM_ALLOCATOR_MODE must be -1 (native), 0 (system), or 1 (tier)"
#endif

namespace STM {
namespace Memory {

inline void* systemAllocate(std::size_t size) {
    return ::operator new(size);
}

inline void systemDeallocate(void* ptr) noexcept {
    ::operator delete(ptr);
}

inline void* occAllocate(std::size_t size) {
#if STM_ALLOCATOR_MODE == STM_ALLOCATOR_MODE_SYSTEM
    return systemAllocate(size);
#else
    return ThreadHeap::allocate(size);
#endif
}

inline void occDeallocate(void* ptr) noexcept {
#if STM_ALLOCATOR_MODE == STM_ALLOCATOR_MODE_SYSTEM
    systemDeallocate(ptr);
#else
    ThreadHeap::deallocate(ptr);
#endif
}

inline void* wwAllocate(std::size_t size) {
#if STM_ALLOCATOR_MODE == STM_ALLOCATOR_MODE_TIER
    return ThreadHeap::allocate(size);
#else
    return systemAllocate(size);
#endif
}

inline void wwDeallocate(void* ptr) noexcept {
#if STM_ALLOCATOR_MODE == STM_ALLOCATOR_MODE_TIER
    ThreadHeap::deallocate(ptr);
#else
    systemDeallocate(ptr);
#endif
}

constexpr const char* configuredMode() noexcept {
#if STM_ALLOCATOR_MODE == STM_ALLOCATOR_MODE_SYSTEM
    return "system";
#elif STM_ALLOCATOR_MODE == STM_ALLOCATOR_MODE_TIER
    return "tier";
#else
    return "native";
#endif
}

constexpr const char* occMode() noexcept {
#if STM_ALLOCATOR_MODE == STM_ALLOCATOR_MODE_SYSTEM
    return "system";
#else
    return "tier";
#endif
}

constexpr const char* wwMode() noexcept {
#if STM_ALLOCATOR_MODE == STM_ALLOCATOR_MODE_TIER
    return "tier";
#else
    return "system";
#endif
}

} // namespace Memory
} // namespace STM
