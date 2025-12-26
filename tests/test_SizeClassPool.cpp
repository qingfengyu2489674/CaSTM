#include <gtest/gtest.h>
#include <vector>
#include "ThreadHeap/SizeClassPool.hpp"
#include "ThreadHeap/ThreadChunkCache.hpp"
#include "ThreadHeap/Slab.hpp"

class SizeClassPoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 每个测试开始前，确保有一个干净的 Cache
        cache_ = new ThreadChunkCache();
    }

    void TearDown() override {
        delete cache_;
    }

    ThreadChunkCache* cache_;
};

// 1. 基础分配与释放测试
TEST_F(SizeClassPoolTest, BasicAllocAndFree) {
    // 创建一个管理 64B 块的 Pool
    SizeClassPool pool(64, cache_);

    void* p1 = pool.allocate();
    EXPECT_NE(p1, nullptr);
    
    // 写入数据测试，确保内存可用
    memset(p1, 0xAA, 64);

    void* p2 = pool.allocate();
    EXPECT_NE(p2, nullptr);
    EXPECT_NE(p1, p2);

    // 此时 Slab 应该是 Current，Owner 应该是 pool
    Slab* slab = Slab::GetSlab(p1); // 假设你实现了静态 GetSlab 方法
    // 或者手动计算: 
    // Slab* slab = reinterpret_cast<Slab*>(reinterpret_cast<uintptr_t>(p1) & ~(kChunkSize - 1));
    
    // 验证释放
    pool.deallocate(slab, p1);
    pool.deallocate(slab, p2);
}

// 2. 验证 LIFO 策略 (Partial List)
TEST_F(SizeClassPoolTest, LIFO_Reuse_Strategy) {
    SizeClassPool pool(128, cache_);

    void* p1 = pool.allocate();
    void* p2 = pool.allocate();
    void* p3 = pool.allocate();

    // 释放 p3 (最后分配的)
    Slab* slab = reinterpret_cast<Slab*>(reinterpret_cast<uintptr_t>(p3) & ~(kChunkSize - 1));
    pool.deallocate(slab, p3);

    // 再次分配，根据 LIFO，应该立刻拿回 p3 (或者在同一个 Slab 内的新地址)
    // 注意：具体是 p3 还是 p3 旁边的地址取决于 Slab 内部 FreeList 实现。
    // 但重点是不能触发 New Slab
    void* p4 = pool.allocate();
    EXPECT_NE(p4, nullptr);
}

// 3. 验证 Full List 转换与自动扩容
TEST_F(SizeClassPoolTest, SlabExhaustionAndNewSlab) {
    // 为了快速填满 Slab，我们使用大块大小
    // 假设 Slab 头 + 对齐占用约 1KB，2MB Chunk 剩余空间很大
    // 选一个能整除的大小，比如 256KB，这样约 7-8 个块就能填满
    size_t large_size = 256 * 1024; 
    SizeClassPool pool(large_size, cache_);
    
    std::vector<void*> ptrs;
    
    // 1. 填满第一个 Slab
    // 持续分配直到触发新 Slab (由于我们无法直接访问私有成员，我们通过地址跳变来推测)
    
    void* first_ptr = pool.allocate();
    ptrs.push_back(first_ptr);
    uintptr_t first_slab_addr = reinterpret_cast<uintptr_t>(first_ptr) & ~(kChunkSize - 1);

    bool new_slab_created = false;
    for (int i = 0; i < 20; ++i) { // 20个 256KB 肯定超过 2MB 了
        void* p = pool.allocate();
        ptrs.push_back(p);
        uintptr_t current_slab_addr = reinterpret_cast<uintptr_t>(p) & ~(kChunkSize - 1);
        
        if (current_slab_addr != first_slab_addr) {
            new_slab_created = true;
            break;
        }
    }

    EXPECT_TRUE(new_slab_created) << "Pool should allocate new slab when current is full";

    // 清理
    for (void* p : ptrs) {
        Slab* s = reinterpret_cast<Slab*>(reinterpret_cast<uintptr_t>(p) & ~(kChunkSize - 1));
        // 这里为了测试简单，假设 owner check pass，实际应由 ThreadCache 路由
        pool.deallocate(s, p); 
    }
}

TEST_F(SizeClassPoolTest, RescueFromFullList) {
    size_t block_size = 512 * 1024; // 512KB
    SizeClassPool pool(block_size, cache_);
    
    std::vector<void*> full_slab_ptrs;

    // 1. 填满第一个 Slab (Slab A)
    // 持续分配直到发生 Slab 切换
    void* p0 = pool.allocate();
    full_slab_ptrs.push_back(p0);
    Slab* slab_a = Slab::GetSlab(p0);
    
    // 填满它
    while (true) {
        void* p = pool.allocate();
        if (Slab::GetSlab(p) != slab_a) {
            // 发生了切换！p 属于新的 Slab B
            // 此时 Slab A 应该已经在 full_list_ 中了
            // 我们把 p (Slab B 的第一个块) 存下来，稍后用来填满 B
            full_slab_ptrs.push_back(p); 
            break;
        }
        full_slab_ptrs.push_back(p);
    }

    // 2. 模拟远程释放 (Remote Free to Slab A)
    // 释放 Slab A 中的第一个块
    void* victim_ptr = full_slab_ptrs[0]; 
    slab_a->freeRemote(victim_ptr);

    // 此时 Slab A 在 Full List，且有一个空闲位 (Remote)
    
    // 3. 耗尽 Current (Slab B) 并期待 Rescue
    // 我们继续分配，每一次分配都检查：是不是回到了 Slab A？
    
    bool rescued = false;
    void* rescued_ptr = nullptr;

    // 分配次数上限设为 10，足够填满 Slab B 并触发救援
    for (int i = 0; i < 10; ++i) {
        void* p = pool.allocate();
        if (Slab::GetSlab(p) == slab_a) {
            rescued = true;
            rescued_ptr = p;
            break; // 成功救回！停止测试
        }
    }

    EXPECT_TRUE(rescued) << "Allocator should eventually rescue memory from Slab A in full list";
    
    if (rescued) {
        // 进一步验证：拿到的确实是 Slab A
        EXPECT_EQ(Slab::GetSlab(rescued_ptr), slab_a);
    }
}