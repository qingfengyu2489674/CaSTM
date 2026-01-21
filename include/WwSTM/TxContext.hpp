#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <thread>

#include <algorithm>
#include "GlobalClock.hpp"
#include "TxDescriptor.hpp"
#include "TxStatus.hpp"
#include "TMVar.hpp"
#include "EBRManager/EBRManager.hpp"


namespace STM {
namespace Ww {

class TxContext {
private:
    struct ReadLogEntry {
        TMVarBase* var;
        uint64_t read_ts;
    };

    struct WriteLogEntry {
        TMVarBase* var;
        void* record_ptr;
    };

    TxDescriptor* my_desc_ = nullptr;
    uint64_t start_ts_ = 0;
    bool is_active_ = false;      // 事务描述符是否有效
    bool in_epoch_ = false;     // 事务是否进入纪元，被EBRManager所管理

    std::vector<ReadLogEntry> read_set_;
    std::vector<WriteLogEntry> write_set_;

public:
    TxContext(const TxContext&) = delete;
    TxContext& operator=(const TxContext&) = delete;

    TxContext() {
        startNewTransaction();
    }

    ~TxContext() {
        // 1. 先检查描述符是否存在
        if (my_desc_) {
            // 如果已经提交，只做资源清理；否则执行 Abort
            if (TxStatusHelper::is_committed(my_desc_->status)) {
                cleanupResources();
            } else {
                abortTransaction();
            }
        } 
        else {
            leaveEpoch();
        }
    }


    void begin() {
        if (my_desc_) {
            abortTransaction();
        }

        startNewTransaction();
    }

    bool commit() {
        if (!ensureActive()) return false;

        if (!validateReadSet()) {
            abortTransaction();
            return false;
        }

        if (write_set_.empty()) {
            cleanupResources(); // 退休 Descriptor，退出 Epoch
            return true;
        }

        // 2. 尝试 CAS 修改状态为 COMMITTED
        if (!TxStatusHelper::tryCommit(my_desc_->status)) {
            // 失败说明被更高优先级的事务 Wound (Kill) 了
            abortTransaction();
            return false;
        }

        // 3. 获取提交时间戳
        uint64_t commit_ts = GlobalClock::tick();

        // 4. 提交更改 (释放锁)
        for (auto& entry : write_set_) {
            entry.var->commitReleaseRecord(commit_ts);
        }

        // 5. 清理资源
        cleanupResources();
        return true;
    }

    template<typename T>
    const T& read(TMVar<T>& var) {
        if(!ensureActive()) {
            static T dummy{};
            return dummy;
        }

        const T& val = var.readProxy(my_desc_);

        bool found = false;
        for(auto& entry : read_set_) { 
            if(entry.var == &var) { 
                found = true; break; 
            } 
        }

        if (!found) {
            read_set_.push_back({&var, var.getDataVersion()});
        }
        return val;
    }

    template<typename T> 
    void write(TMVar<T>& var, const T& val) {
        if (!ensureActive()) return;

        // 写前验证
        for (const auto& r_entry : read_set_) {
            if (r_entry.var == &var) {
                // 如果当前版本 不等于 我读时的版本
                if (var.getDataVersion() != r_entry.read_ts) {
                    abortTransaction();
                    return;
                }
                break; // 找到了就不用继续找了
            }
        }

        while (true) {
            TxDescriptor* conflict_tx = nullptr;

            // 尝试获取锁 (或者如果是重入，这里会更新 Record 的值)
            void* record = var.tryWriteAndGetRecord(my_desc_, &val, conflict_tx);
            
            if (record) {
                // 将锁记录在案 (这里面去做去重，防止重复添加)
                trackWrite(&var, record);
                return;
            }

            // 遭遇冲突
            resolveConflict(conflict_tx);

            if (!ensureActive()) return;
            std::this_thread::yield();
        }
    }


private:
    void startNewTransaction() {
        enterEpoch(); // 确保进入 EBR 保护区
        
        write_set_.clear();
        start_ts_ = GlobalClock::now();
        
        // 分配新的描述符
        my_desc_ = new TxDescriptor(start_ts_);

        auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id()) % 1000;
        std::printf("[T%zu] ALLOC TxDesc %p\n", tid, my_desc_);

        my_desc_->status.store(TxStatus::ACTIVE, std::memory_order_release);
        
        is_active_ = true;
    }

    void abortTransaction() {
        if (!my_desc_) return;

        // 1. 标记状态为 ABORTED (通知其他试图 Steal 的人)
        TxStatusHelper::tryAbort(my_desc_->status);
        is_active_ = false;

        // 2. 回滚数据
        for (auto it = write_set_.rbegin(); it != write_set_.rend(); ++it) {
            it->var->abortRestoreData(it->record_ptr);
        }

        // 3. 清理资源
        cleanupResources();
    }

    
    void cleanupResources() {
        write_set_.clear();
        is_active_ = false;

        if (my_desc_) {
            auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id()) % 1000;
            std::printf("[T%zu] RETIRE TxDesc %p\n", tid, my_desc_);
            EBRManager::instance()->retire(my_desc_);
            my_desc_ = nullptr;
        }

        leaveEpoch();
    }

    bool ensureActive() {
        if (!is_active_) return false;
        if (!my_desc_) return false;

        // 惰性检查：如果被别人标记为 Aborted，更新本地状态
        if (my_desc_->status.load(std::memory_order_relaxed) == TxStatus::ABORTED) {
            is_active_ = false;
        }
        return is_active_;
    }

    void trackWrite(TMVarBase* var, void* record) {
        // 线性扫描去重
        for (const auto& entry : write_set_) {
            if (entry.var == var) {
                // 已经存在，说明是重入写入。
                return;
            }
        }
        write_set_.push_back({var, record});
    }



    bool validateReadSet() {
        for (const auto& entry : read_set_) {
            // 1. 如果这个变量也在我的写集里，说明我已持有锁，且拥有最新版本。
            bool locked_by_me = false;
            for (const auto& w_entry : write_set_) {
                if (w_entry.var == entry.var) {
                    locked_by_me = true;
                    break;
                }
            }
            
            if (locked_by_me) continue;

            // 2. 如果没锁，必须验证内存中的稳定版本是否依然等于我读时的版本
            if (entry.var->getDataVersion() != entry.read_ts) {
                return false;
            }
        }
        return true;
    }


    // EBR 状态管理
    void enterEpoch() {
        if (!in_epoch_) {
            EBRManager::instance()->enter();
            in_epoch_ = true;
        }
    }

    void leaveEpoch() {
        if (in_epoch_) {
            EBRManager::instance()->leave();
            in_epoch_ = false;
        }
    }

    // 冲突解决策略 (Wound-Wait)
    void resolveConflict(TxDescriptor* conflict_tx) {
        if (!conflict_tx) return;

        // 再次检查对方状态，如果对方已经完事了，直接返回重试
        TxStatus s = conflict_tx->status.load(std::memory_order_acquire);
        if (s == TxStatus::ABORTED) {
            return;
        }

        if (s == TxStatus::COMMITTED) {
            while (conflict_tx->status.load(std::memory_order_acquire) == TxStatus::COMMITTED) {
                std::this_thread::yield();
            }
            return;
        }

        uint64_t my_ts = start_ts_;
        uint64_t enemy_ts = conflict_tx->start_ts;

        // 判断优先级：时间戳越小(越早)优先级越高
        bool i_am_older = (my_ts < enemy_ts);
        
        // 如果时间戳相同，用地址决胜负，保证全局一致的顺序，防止活锁
        if (my_ts == enemy_ts) {
            i_am_older = (my_desc_ < conflict_tx);
        }

        if (i_am_older) {
            // === Wound (杀掉年轻的) ===
            // 尝试将对方状态改为 ABORTED
            if (TxStatusHelper::tryAbort(conflict_tx->status)) {
                // 成功杀敌，返回继续抢锁
                return;
            } else {
                // 杀敌失败，说明对方状态变了
                s = conflict_tx->status.load(std::memory_order_acquire);
                if (s == TxStatus::COMMITTED) {
                    // 对方刚好提交了，我们等一下锁释放
                    std::this_thread::yield();
                }
                // 如果对方 Aborted，我们下轮循环会自动 Steal
            }
        } else {
            // === Wait / Die (年轻的自杀) ===
            abortTransaction();
        }
    }


};


}
}