#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <random>
#include <chrono>
#include <iostream>
#include <memory>

// 引入你的核心头文件
#include "WwSTM/TxContext.hpp"
#include "WwSTM/TMVar.hpp"

using namespace STM::Ww;

// =========================================================
// 测试夹具 (保持不变)
// =========================================================
class DebugStressTest : public ::testing::Test {
protected:
    std::vector<TMVar<int>*> accounts;
    const int INITIAL_BALANCE = 1000;
    const int NUM_ACCOUNTS = 2; // 只用2个账户，制造极端冲突

    void SetUp() override {
        for (int i = 0; i < NUM_ACCOUNTS; ++i) {
            accounts.push_back(new TMVar<int>(INITIAL_BALANCE));
        }
    }

    void TearDown() override {
        for (auto acc : accounts) {
            delete acc;
        }
        accounts.clear();
    }
};

// =========================================================
// 低压力诊断测试
// =========================================================

TEST_F(DebugStressTest, TwoThreadsConflictDiagnosis) {
    const int NUM_THREADS = 2;       // 只有 T0 和 T1
    const int ITERATIONS = 50;       // 每个线程只跑 50 次，方便看日志
    
    std::atomic<long long> total_commits{0};
    std::atomic<long long> total_aborts{0};

    std::cout << "[ DIAGNOSE ] Starting Low-Pressure Test (2 Threads, 2 Accounts)..." << std::endl;

    auto worker = [&](int thread_id) {
        TxContext tx;
        std::mt19937 rng(thread_id + 100); 

        for (int i = 0; i < ITERATIONS; ++i) {
            // 强制冲突：两个线程都只操作 Account[0] 和 Account[1]
            int from_idx = 0; 
            int to_idx = 1;
            
            // 偶尔反向转账，增加死锁/活锁检测的复杂度
            if (i % 2 == 1) std::swap(from_idx, to_idx);

            tx.begin();

            // 1. 读取
            int bal_from = tx.read(*accounts[from_idx]);
            
            // !!! 关键延迟 !!!
            // 在持有读锁/快照后，睡 1ms。
            // 这给了另一个线程进入并在 Wound-Wait 中杀死当前线程的机会。
            std::this_thread::sleep_for(std::chrono::milliseconds(1));

            int bal_to = tx.read(*accounts[to_idx]);

            // 2. 写入
            if (bal_from >= 10) {
                tx.write(*accounts[from_idx], bal_from - 10);
                
                // !!! 关键延迟 !!!
                // 在持有一个写锁后，睡 1ms。
                // 这极大概率会导致另一个线程试图抢占这个锁，触发 Steal 或 Abort 逻辑。
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                
                tx.write(*accounts[to_idx], bal_to + 10);

                if (tx.commit()) {
                    total_commits.fetch_add(1);
                } else {
                    total_aborts.fetch_add(1);
                    // 如果 Abort，稍微避让一下，防止日志刷屏太快
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            } else {
                tx.commit();
            }
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back(worker, i);
    }

    for (auto& t : threads) {
        t.join();
    }

    // 验证一致性
    long long actual_total = 0;
    long long expected_total = NUM_ACCOUNTS * INITIAL_BALANCE;
    {
        TxContext verify_tx;
        for (auto acc : accounts) {
            actual_total += verify_tx.read(*acc);
        }
        verify_tx.commit();
    }

    std::cout << "------------------------------------------------" << std::endl;
    std::cout << "Total Commits: " << total_commits << std::endl;
    std::cout << "Total Aborts:  " << total_aborts << std::endl;
    std::cout << "Expected: " << expected_total << " | Actual: " << actual_total << std::endl;
    std::cout << "------------------------------------------------" << std::endl;

    EXPECT_EQ(actual_total, expected_total);
}