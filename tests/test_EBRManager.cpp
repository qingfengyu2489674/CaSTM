#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <new> 

#include "EBRManager/EBRManager.hpp"
#include "TierAlloc/ThreadHeap/ThreadHeap.hpp"

// ==========================================
// 辅助类保持不变
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
    
    static TrackedObject* create(int v) {
        void* mem = ThreadHeap::allocate(sizeof(TrackedObject));
        return new(mem) TrackedObject(v);
    }
};

std::atomic<int> TrackedObject::alive_count{0};

// ==========================================
// 测试套件
// ==========================================
class EBRManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        TrackedObject::alive_count = 0;
    }

    void TearDown() override {
        // 单例模式下，如果不提供 Reset 接口，很难完全清理 EBR 内部状态。
        // 但只要保证没有存活的对象计数即可。
    }
    
    // 辅助函数：手动推进 Epoch
    void cycleEpochs(EBRManager& mgr, int cycles = 3) {
        for (int i = 0; i < cycles; ++i) {
            mgr.enter();
            mgr.leave(); 
        }
    }
};

// ==========================================
// 测试用例
// ==========================================

// 1. 测试单例获取
TEST_F(EBRManagerTest, SingletonAccess) {
    // 只要能获取到实例且不为空即可
    EBRManager* mgr = EBRManager::instance();
    ASSERT_NE(mgr, nullptr);
    
    EBRManager* mgr2 = EBRManager::instance();
    EXPECT_EQ(mgr, mgr2) << "Instance should be unique";
}

// 2. 测试 Retire 基本逻辑
TEST_F(EBRManagerTest, RetireRecyclesEventually) {
    // 获取单例引用
    EBRManager& mgr = *EBRManager::instance();

    {
        mgr.enter();
        
        TrackedObject* obj = TrackedObject::create(100);
        EXPECT_EQ(TrackedObject::alive_count.load(), 1);
        
        mgr.retire(obj);
        
        mgr.leave();
    }

    // 循环多次以确保触发垃圾回收
    for(int i = 0; i < 20; ++i) {
        mgr.enter();
        mgr.leave();
        if (TrackedObject::alive_count.load() == 0) break;
    }

    EXPECT_EQ(TrackedObject::alive_count.load(), 0) << "Object should be destructed after epoch advancement";
}

// 3. 测试保护机制 (Protection)
TEST_F(EBRManagerTest, ActiveThreadPreventsReclamation) {
    EBRManager& mgr = *EBRManager::instance();
    
    std::atomic<bool> thread_ready{false};
    std::atomic<bool> can_finish{false};

    std::thread slow_thread([&]() {
        // 在子线程中获取单例也是同一个实例
        EBRManager::instance()->enter(); 
        thread_ready = true;
        while (!can_finish.load()) {
            std::this_thread::yield();
        }
        EBRManager::instance()->leave();
    });

    while (!thread_ready.load()) std::this_thread::yield();

    mgr.enter();
    TrackedObject* obj = TrackedObject::create(200);
    mgr.retire(obj);
    mgr.leave();

    // 尝试推进 Epoch
    for(int i = 0; i < 10; ++i) {
        mgr.enter();
        mgr.leave();
    }

    // 因为 slow_thread 卡住了 Epoch，对象应存活
    EXPECT_EQ(TrackedObject::alive_count.load(), 1) << "Object should NOT be deleted while a thread is active in old epoch";

    can_finish = true;
    slow_thread.join();

    // 慢线程离开后，再次推进
    for(int i = 0; i < 10; ++i) {
        mgr.enter();
        mgr.leave();
    }
    
    EXPECT_EQ(TrackedObject::alive_count.load(), 0) << "Object SHOULD be deleted after blocking thread leaves";
}

// 4. 多线程压力测试
TEST_F(EBRManagerTest, MultiThreadStress) {
    EBRManager& mgr = *EBRManager::instance();
    const int thread_count = 8;
    const int iterations_per_thread = 1000;

    std::vector<std::thread> threads;
    
    for (int i = 0; i < thread_count; ++i) {
        threads.emplace_back([&]() {
            // 每个线程获取单例引用（实际上是全局同一个）
            auto& local_mgr = *EBRManager::instance();
            for (int j = 0; j < iterations_per_thread; ++j) {
                local_mgr.enter();
                TrackedObject* obj = TrackedObject::create(j);
                obj->value++; 
                local_mgr.retire(obj);
                local_mgr.leave();
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // 主线程推进清理剩余垃圾
    for(int i = 0; i < 20; ++i) {
        mgr.enter();
        mgr.leave();
    }

    EXPECT_EQ(TrackedObject::alive_count.load(), 0) << "All objects should be reclaimed after stress test";
}