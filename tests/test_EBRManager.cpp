#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <new> // for placement new

#include "EBRManager/EBRManager.hpp"
#include "TierAlloc/ThreadHeap/ThreadHeap.hpp" // 包含你的内存分配器

// ==========================================
// 辅助类：用于跟踪对象构造和析构
// ==========================================
struct TrackedObject {
    static std::atomic<int> alive_count;
    int value;

    TrackedObject(int v) : value(v) {
        alive_count.fetch_add(1);
    }

    ~TrackedObject() {
        alive_count.fetch_sub(1);
    }
    
    // 辅助函数：使用 ThreadHeap 分配并构造对象
    // 因为 EBRManager::retire 内部会调用 ThreadHeap::deallocate
    static TrackedObject* create(int v) {
        void* mem = ThreadHeap::allocate(sizeof(TrackedObject));
        return new(mem) TrackedObject(v);
    }
};

// 初始化静态成员
std::atomic<int> TrackedObject::alive_count{0};

// ==========================================
// 测试套件
// ==========================================
class EBRManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 每个测试开始前重置计数器
        TrackedObject::alive_count = 0;
    }

    void TearDown() override {
        // 确保没有内存泄漏（注意：如果测试失败，这里可能不为0，具体取决于EBR是否清理干净）
    }
    
    // 辅助函数：手动推进 Epoch
    // EBR 通常需要经历 3 个阶段 (Current -> Previous -> Reclamation) 才能回收
    // 这里的循环次数取决于 kNumEpochLists = 3
    void cycleEpochs(EBRManager& mgr, int cycles = 3) {
        for (int i = 0; i < cycles; ++i) {
            mgr.enter();
            mgr.leave(); // leave 通常会尝试 tryAdvanceEpoch_
        }
    }
};

// ==========================================
// 测试用例
// ==========================================

// 1. 测试基本的构造和析构
TEST_F(EBRManagerTest, BasicConstruction) {
    EBRManager mgr;
    // 只是确保创建和销毁不崩溃
    SUCCEED();
}

// 2. 测试 Retire 基本逻辑
// 验证 retire 后，对象最终会被回收
TEST_F(EBRManagerTest, RetireRecyclesEventually) {
    EBRManager mgr;

    {
        mgr.enter();
        
        // 创建对象
        TrackedObject* obj = TrackedObject::create(100);
        EXPECT_EQ(TrackedObject::alive_count.load(), 1);
        
        // 标记回收
        mgr.retire(obj);
        
        // 在 enter 期间，EBR 应该保证对象还活着（或者至少在当前 epoch 结束前）
        // 注意：具体的立即存活保证取决于实现，但在单线程且未 leave 前通常是安全的
        // 这里主要验证它不会崩溃
        
        mgr.leave();
    }

    // 此时对象可能还没被回收，因为 global_epoch 可能还没推进足够的次数。
    // 我们手动模拟多次 enter/leave 来推进 epoch。
    // kNumEpochLists 是 3，所以我们需要循环几次让 global epoch 绕回来清理垃圾列表。
    
    // 循环多次以确保触发垃圾回收
    // 0 -> 1 -> 2 -> 0 (此时清理旧的 0)
    for(int i = 0; i < 10; ++i) {
        mgr.enter();
        mgr.leave();
        if (TrackedObject::alive_count.load() == 0) break;
    }

    EXPECT_EQ(TrackedObject::alive_count.load(), 0) << "Object should be destructed after epoch advancement";
}

// 3. 测试保护机制 (Protection)
// 验证当一个线程卡在旧 Epoch 时，对象不会被回收
TEST_F(EBRManagerTest, ActiveThreadPreventsReclamation) {
    EBRManager mgr;
    std::atomic<bool> thread_ready{false};
    std::atomic<bool> can_finish{false};

    // 启动一个“慢”线程，它进入后不离开
    std::thread slow_thread([&]() {
        mgr.enter(); // 锁定当前 Epoch
        thread_ready = true;
        while (!can_finish.load()) {
            std::this_thread::yield();
        }
        mgr.leave();
    });

    while (!thread_ready.load()) std::this_thread::yield();

    // 主线程创建并 retire 对象
    mgr.enter();
    TrackedObject* obj = TrackedObject::create(200);
    mgr.retire(obj);
    mgr.leave();

    // 尝试推进 Epoch
    // 因为 slow_thread 还在 enter() 状态，它会阻止 Global Epoch 推进超过它的 safe 范围
    // 因此 obj 不应该被回收
    for(int i = 0; i < 5; ++i) {
        mgr.enter();
        mgr.leave();
    }

    // 断言：由于有个线程卡住了 Epoch，对象应该还存活
    // 注意：这取决于 EBRManager 具体的 tryAdvanceEpoch_ 实现。
    // 如果是标准的 EBR，只要有一个线程没更新 epoch，全局 epoch 就无法推进两轮以上。
    EXPECT_EQ(TrackedObject::alive_count.load(), 1) << "Object should NOT be deleted while a thread is active in old epoch";

    // 让慢线程结束
    can_finish = true;
    slow_thread.join();

    // 现在慢线程离开了，再次推进 Epoch，对象应该被回收
    for(int i = 0; i < 5; ++i) {
        mgr.enter();
        mgr.leave();
    }
    
    EXPECT_EQ(TrackedObject::alive_count.load(), 0) << "Object SHOULD be deleted after blocking thread leaves";
}

// 4. 多线程压力测试
// 验证多线程并发 alloc/retire 不会崩溃或泄漏
TEST_F(EBRManagerTest, MultiThreadStress) {
    EBRManager mgr;
    const int thread_count = 8;
    const int iterations_per_thread = 1000;

    std::vector<std::thread> threads;
    
    for (int i = 0; i < thread_count; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < iterations_per_thread; ++j) {
                mgr.enter();
                TrackedObject* obj = TrackedObject::create(j);
                // 模拟一些工作
                obj->value++; 
                mgr.retire(obj);
                mgr.leave();
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // 此时 alive_count 可能不为 0，因为最后几个 Epoch 的垃圾还在列表里
    // 需要主线程帮忙推一下 Epoch
    for(int i = 0; i < 20; ++i) {
        mgr.enter();
        mgr.leave();
    }

    EXPECT_EQ(TrackedObject::alive_count.load(), 0) << "All objects should be reclaimed after stress test";
}