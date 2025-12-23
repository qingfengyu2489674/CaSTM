#pragma once

#include <cstddef>
#include <mutex>

// 引入你之前确认不需要修改的那两个头文件
#include "AlignedChunkAllocatorByMmap.hpp"
#include "FreeChunkListCache.hpp"

class CentralHeap {
public:
    // 1. 单例获取方式修改：
    // 不再需要 shm_base 和 size，因为是向操作系统动态申请，不是固定区域
    static CentralHeap& GetInstance();

    // 2. 核心接口
    void* acquireChunk(size_t size);
    void releaseChunk(void* chunk, size_t size);

    // 禁用拷贝和移动
    CentralHeap(const CentralHeap&) = delete;
    CentralHeap& operator=(const CentralHeap&) = delete;
    CentralHeap(CentralHeap&&) = delete;
    CentralHeap& operator=(CentralHeap&&) = delete;

public:
    static constexpr size_t kChunkSize = 2 * 1024 * 1024; // 2MB

private:
    CentralHeap() = default;
    ~CentralHeap() = default;

    // 3. 成员变量替换：
    // 负责向 OS 申请大块内存
    AlignedChunkAllocatorByMmap allocator_;
    
    // 负责管理回收回来的空闲块 (Thread-Safe)
    FreeChunkListCache free_list_;
};