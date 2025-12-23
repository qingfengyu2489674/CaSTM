#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <cstdint>
#include <cstring>

// 引入你的头文件
#include "gc_malloc/CentralHeap/CentralHeap.hpp"

// 定义一个测试类，虽然 CentralHeap 是单例，但用 Fixture 可以方便扩展
class CentralHeapTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 每个测试开始前的准备工作 (如果有)
    }

    void TearDown() override {
        // 每个测试结束后的清理工作
    }

    // 获取单例引用
    CentralHeap& GetHeap() {
        return CentralHeap::GetInstance();
    }

    const size_t kChunkSize = CentralHeap::kChunkSize;
};

// 1. 测试基本的分配功能
TEST_F(CentralHeapTest, BasicAllocation) {
    CentralHeap& heap = GetHeap();
    
    void* ptr = heap.acquireChunk(kChunkSize);
    
    // 断言：指针不为空
    ASSERT_NE(ptr, nullptr) << "Failed to allocate chunk from CentralHeap";

    // 断言：内存可写 (防止拿到只读内存或无效地址)
    // 写入一些数据测试
    std::memset(ptr, 0xAB, 1024); 
    unsigned char* byte_ptr = static_cast<unsigned char*>(ptr);
    EXPECT_EQ(byte_ptr[0], 0xAB);
    EXPECT_EQ(byte_ptr[1023], 0xAB);

    // 释放内存
    heap.releaseChunk(ptr, kChunkSize);
}

// 2. 测试内存对齐 (AlignedChunkAllocatorByMmap 的核心功能)
TEST_F(CentralHeapTest, AlignmentCheck) {
    CentralHeap& heap = GetHeap();
    
    void* ptr = heap.acquireChunk(kChunkSize);
    ASSERT_NE(ptr, nullptr);

    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    size_t alignment = 2 * 1024 * 1024; // 2MB

    // 检查地址能否被 2MB 整除
    EXPECT_EQ(addr % alignment, 0) 
        << "Pointer " << std::hex << addr << " is not aligned to 2MB";

    heap.releaseChunk(ptr, kChunkSize);
}

// 3. 测试缓存复用机制 (LIFO - 后进先出)
// 假如我申请了 A，释放 A，再申请 B，B 应该等于 A (因为 A 在缓存链表头)
TEST_F(CentralHeapTest, CacheReuse) {
    CentralHeap& heap = GetHeap();

    // 1. 申请第一个块
    void* ptr1 = heap.acquireChunk(kChunkSize);
    ASSERT_NE(ptr1, nullptr);

    // 2. 释放它
    heap.releaseChunk(ptr1, kChunkSize);

    // 3. 再次申请
    void* ptr2 = heap.acquireChunk(kChunkSize);
    
    // 断言：应该拿到同一块内存 (验证 FreeChunkListCache 是否工作)
    EXPECT_EQ(ptr1, ptr2) 
        << "CentralHeap did not reuse the recently released chunk";

    heap.releaseChunk(ptr2, kChunkSize);
}

// 4. 测试多个块的分配
TEST_F(CentralHeapTest, MultipleAllocations) {
    CentralHeap& heap = GetHeap();
    
    void* ptr1 = heap.acquireChunk(kChunkSize);
    void* ptr2 = heap.acquireChunk(kChunkSize);

    ASSERT_NE(ptr1, nullptr);
    ASSERT_NE(ptr2, nullptr);
    EXPECT_NE(ptr1, ptr2) << "Two allocations returned the same address without release";

    heap.releaseChunk(ptr1, kChunkSize);
    heap.releaseChunk(ptr2, kChunkSize);
}

// 5. 并发压力测试 (验证线程安全)
TEST_F(CentralHeapTest, ConcurrencyTest) {
    CentralHeap& heap = GetHeap();
    const int thread_count = 8;     // 8个线程
    const int iterations = 100;     // 每个线程跑100次分配释放

    auto worker_task = [&heap, this]() {
        for (int i = 0; i < iterations; ++i) {
            void* ptr = heap.acquireChunk(kChunkSize);
            
            // 简单校验
            if (ptr != nullptr) {
                // 模拟写入操作，增加持有时间，更容易暴露竞态条件
                static_cast<char*>(ptr)[0] = 'X'; 
                
                // 立即释放
                heap.releaseChunk(ptr, kChunkSize);
            } else {
                // 如果 mmap 失败 (OOM)，在测试中视为错误
                ADD_FAILURE() << "Allocation failed during concurrency test";
            }
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < thread_count; ++i) {
        threads.emplace_back(worker_task);
    }

    for (auto& t : threads) {
        t.join();
    }

    // 如果运行到这里没有 Crash，且没有 Sanitizer 报错，说明基本线程安全
}
