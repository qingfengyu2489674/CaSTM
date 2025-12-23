#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <cstring>
#include <algorithm>

// 包含你的 ThreadHeap 头文件
#include "gc_malloc/ThreadHeap/ThreadHeap.hpp"

// -----------------------------------------------------------------------
// 辅助函数：计算实际分配的指针 (跳过 BlockHeader)
// -----------------------------------------------------------------------
// 注意：这个测试假设 ThreadHeap::allocate 返回的是用户可用指针
// 内部会有 sizeof(BlockHeader) 的偏移。

class ThreadHeapTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 每次测试开始前，最好做一次 GC 保证干净（虽然是 TLS，影响不大）
        ThreadHeap::garbageCollect();
    }
};

// 1. 小对象分配测试 (Small Allocation)
TEST_F(ThreadHeapTest, SmallAllocation) {
    size_t size = 64; // 64字节
    void* ptr = ThreadHeap::allocate(size);

    ASSERT_NE(ptr, nullptr) << "ThreadHeap::allocate returned null for small size";

    // 测试读写
    std::memset(ptr, 0xAA, size);
    unsigned char* bytes = static_cast<unsigned char*>(ptr);
    EXPECT_EQ(bytes[0], 0xAA);
    EXPECT_EQ(bytes[size - 1], 0xAA);

    ThreadHeap::deallocate(ptr);
}

// 2. 连续分配测试 (Verify SizeClass logic)
TEST_F(ThreadHeapTest, MultipleSmallAllocations) {
    std::vector<void*> ptrs;
    size_t size = 128;
    
    // 分配 100 个
    for (int i = 0; i < 100; ++i) {
        void* p = ThreadHeap::allocate(size);
        ASSERT_NE(p, nullptr);
        std::memset(p, i % 255, size); // 写入不同模式
        ptrs.push_back(p);
    }

    // 释放
    for (void* p : ptrs) {
        ThreadHeap::deallocate(p);
    }
    
    // 触发回收
    size_t count = ThreadHeap::garbageCollect();
    // 具体的 count 取决于实现细节，但应该大于 0
    EXPECT_GT(count, 0) << "Garbage collect should reclaim blocks";
}

// 3. 大对象测试 (Large Object > MaxSmallAlloc)
// 假设 kMaxSmallAlloc 约 8KB 或 32KB，这里分配 1MB
TEST_F(ThreadHeapTest, LargeAllocation) {
    size_t size = 1024 * 1024; // 1MB
    void* ptr = ThreadHeap::allocate(size);

    ASSERT_NE(ptr, nullptr);

    // 测试读写
    std::memset(ptr, 0xBB, size);
    unsigned char* bytes = static_cast<unsigned char*>(ptr);
    EXPECT_EQ(bytes[0], 0xBB);
    EXPECT_EQ(bytes[size - 1], 0xBB);

    ThreadHeap::deallocate(ptr);
    // 大对象可能不经过 ManagedList 延迟释放，取决于你的实现
    // 如果是直接还给 CentralHeap，GC 可能不会统计到它
    ThreadHeap::garbageCollect();
}

// 4. 多线程隔离性测试 (TLS Check)
// 线程 A 分配的内存，在线程 A 释放；线程 B 分配的在 B 释放。互不干扰。
TEST_F(ThreadHeapTest, MultiThreadIsolation) {
    auto thread_func = [](int id) {
        std::vector<void*> ptrs;
        for (int i = 0; i < 50; ++i) {
            void* p = ThreadHeap::allocate(256);
            if (p) {
                std::memset(p, id, 256); // 用线程ID填充
                ptrs.push_back(p);
            }
        }
        
        // 模拟一些工作
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        for (void* p : ptrs) {
            // 校验数据没被其他线程改写
            unsigned char* bytes = static_cast<unsigned char*>(p);
            ASSERT_EQ(bytes[0], id) << "Memory corruption detected in thread " << id;
            ThreadHeap::deallocate(p);
        }
        
        ThreadHeap::garbageCollect();
    };

    std::thread t1(thread_func, 1);
    std::thread t2(thread_func, 2);

    t1.join();
    t2.join();
}

// 5. 跨线程释放测试 (Cross-Thread Deallocation)
// ThreadHeap 的一个难点：A 分配，B 释放。
// 你的代码中 storeFree() 只是改状态，真正回收靠 GC。
// 需要测试 B 释放后，A 调用 GC 能否回收。
TEST_F(ThreadHeapTest, CrossThreadDeallocation) {
    void* ptr = nullptr;
    
    // 线程 A 分配
    std::thread t1([&]() {
        ptr = ThreadHeap::allocate(512);
        // 模拟传递给 B
    });
    t1.join();

    ASSERT_NE(ptr, nullptr);

    // 线程 B 释放
    std::thread t2([&]() {
        // B 只能调用 deallocate，修改 BlockHeader 状态
        ThreadHeap::deallocate(ptr);
    });
    t2.join();

    // 此时 ptr 指向的 BlockHeader 应该是 Free 状态
    // 如果我们在主线程（或者再次在 A 线程）调用 GC，理论上应该能回收？
    // 注意：ManagedList 是 TLS 的。
    // A 分配的块挂在 A 的 ManagedList 上。
    // B 调用 deallocate 只是把状态改成 Free。
    // A 必须再次运行 GC 才能回收它。
    
    // 让 A 再次运行来进行回收
    std::thread t3([&]() {
        // 回收，看看会不会 crash
        ThreadHeap::garbageCollect(); 
    });
    t3.join();
}
