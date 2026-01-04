#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <random>
#include <iostream>

// 引入你的核心组件
// 请确保路径正确，如果你的 include 目录不同请调整
#include "MVOSTM/StripedLockTable.hpp"
#include "TierAlloc/ThreadHeap/ThreadHeap.hpp"
#include "EBRManager/EBRManager.hpp"

// ---------------------------------------------------------------------------
// 模块 1: 锁的原子性测试 (The Locking Integrity Test)
// ---------------------------------------------------------------------------

struct TestObject {
    // 填充防止伪共享，只测试锁本身
    char padding[64]; 
    int value = 0;
};

TEST(DebugSuite, LockIntegrity) {
    std::cout << "[DEBUG] Testing StripedLockTable Integrity..." << std::endl;
    
    constexpr int NUM_THREADS = 16;
    constexpr int OPS_PER_THREAD = 100000;
    
    TestObject resource;
    resource.value = 0;

    std::vector<std::thread> threads;
    auto& lockTable = StripedLockTable::instance();

    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < OPS_PER_THREAD; ++j) {
                // 测试点：高并发下的互斥性
                lockTable.lock(&resource);
                resource.value++;
                lockTable.unlock(&resource);
            }
        });
    }

    for (auto& t : threads) t.join();

    int expected = NUM_THREADS * OPS_PER_THREAD;
    std::cout << "[DEBUG] Lock Result: " << resource.value << " / " << expected << std::endl;

    ASSERT_EQ(resource.value, expected) 
        << "CRITICAL: StripedLockTable failed! Spinlock is broken.";
}

// ---------------------------------------------------------------------------
// 模块 2: 内存分配器与EBR压力测试
// ---------------------------------------------------------------------------

struct Payload {
    uint64_t data[8]; 
    static void* operator new(size_t size) { return ThreadHeap::allocate(size); }
    static void operator delete(void* p) { ThreadHeap::deallocate(p); }
};

TEST(DebugSuite, AllocatorAndEBR) {
    std::cout << "[DEBUG] Testing ThreadHeap & EBR Stability..." << std::endl;

    constexpr int NUM_THREADS = 16;
    // 缩短时间以便快速验证，如果崩会立刻崩
    constexpr int DURATION_MS = 1000; 

    std::atomic<bool> running{true};
    std::atomic<long> alloc_count{0};
    std::vector<std::thread> threads;

    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back([&]() {
            // 移除显式 register，依赖 enter() 的内部实现
            // EBRManager::instance()->registerThread(); 
            
            while (running.load(std::memory_order_relaxed)) {
                EBRManager::instance()->enter();
                
                Payload* p = new Payload();
                p->data[0] = 0xDEADBEEF; // 写入防止被优化

                EBRManager::instance()->retire(p, [](void* ptr){
                    delete static_cast<Payload*>(ptr);
                });

                alloc_count.fetch_add(1, std::memory_order_relaxed);
                EBRManager::instance()->leave();
                
                // 模拟负载间隙
                if (alloc_count.load() % 1000 == 0) std::this_thread::yield();
            }
            
            // EBRManager::instance()->unregisterThread();
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(DURATION_MS));
    running = false;

    for (auto& t : threads) t.join();

    std::cout << "[DEBUG] Total Alloc/Free Cycles: " << alloc_count.load() << std::endl;
    SUCCEED();
}


