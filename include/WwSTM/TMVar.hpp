#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>
#include <thread>
#include <functional> 

#include "TaggedPtr.hpp"
#include "VersionNode.hpp"
#include "WriteRecord.hpp"
#include "EBRManager/EBRManager.hpp"
#include "WwSTM/TxDescriptor.hpp"
#include "WwSTM/TxStatus.hpp"

namespace STM {
namespace Ww {

// 为了让 TxContext::write_set_ 可以通过 std::vector<TMVarBase*> 统一存储不同类型的变量
struct TMVarBase {
    virtual ~TMVarBase() = default;

    virtual void commitReleaseRecord(const uint64_t commit_ts) = 0;
    virtual void abortRestoreData(void* saved_record_ptr) = 0;
    virtual uint64_t getDataVersion() const = 0;
};


template <typename T>
class TMVar : public TMVarBase{
public:
    using NodeT = detail::VersionNode<T>;
    using RecordT = detail::WriteRecord<T>;

private:
    std::atomic<NodeT*> data_ptr_;
    std::atomic<RecordT*> record_ptr_;

public:
    template<typename... Args>
    TMVar(Args&&...args) : record_ptr_(nullptr) {
        NodeT* init_node = new NodeT(0, std::forward<Args>(args)...);
        data_ptr_.store(init_node, std::memory_order_release);
    }

    ~TMVar() {
        EBRManager::instance()->retire(data_ptr_.load(std::memory_order_acquire));
        RecordT* rec = record_ptr_.load(std::memory_order_acquire);
        if (rec) EBRManager::instance()->retire(rec);
    }

    // 禁止拷贝和移动
    TMVar(const TMVar&) = delete;
    TMVar& operator=(const TMVar&) = delete;
    TMVar(TMVar&&) = delete;
    TMVar& operator=(TMVar&&) = delete;

    const T& readProxy(TxDescriptor* tx) {
        RecordT* record = record_ptr_.load(std::memory_order_acquire);

        // Case 1: 无锁 -> 直接读
        if(record == nullptr)
            return data_ptr_.load(std::memory_order_acquire)->payload;

        // Case 2: 有锁 -> 检查 Owner
        if(record->owner == tx)
            return record->new_node->payload;

        // Case 3: 冲突检测 (Wound-Wait 读策略)
        TxStatus status = record->owner->status.load(std::memory_order_acquire);

        if (status == TxStatus::COMMITTED) {
            // 对方已提交但未释放锁：读新值
            return record->new_node->payload;
        } 
        else {
            // 对方 ACTIVE：读旧值 (MVCC Snapshot)
            return record->old_node->payload;
        }
    }

  void* tryWriteAndGetRecord(TxDescriptor* tx, const void* val_ptr, TxDescriptor*& out_conflict) {
        auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id()) % 1000;
        
        // 预先创建新对象 (避免在 CAS 循环内重复 new)
        NodeT* my_new_node = new NodeT(tx->start_ts, *static_cast<const T*>(val_ptr));
        RecordT* my_record = new RecordT(tx, nullptr, my_new_node);

        while (true) {
            RecordT* current = record_ptr_.load(std::memory_order_acquire);
            NodeT* stable_node = data_ptr_.load(std::memory_order_acquire);
            
            // 无论此时 current 是 null 还是 Aborted 的尸体，data_ptr_ 都是干净的稳定版
            my_record->old_node = stable_node;

            if(current != nullptr) {
                // --- 重入 (Re-entrant) ---
                if (current->owner == tx) {
                    // 不需要用 my_record 了
                    delete my_record; 
                    // 这里可以直接更新 current->new_node，也可以替换它
                    NodeT* old_draft_node = current->new_node;
                    std::printf("[T%zu] RETIRE Node   %p (Reason: Re-entrant)\n", tid, old_draft_node);
                    
                    current->new_node = my_new_node; // 更新为最新值
                    EBRManager::instance()->retire(old_draft_node); 
                    return current;
                }

                TxStatus status = current->owner->status.load(std::memory_order_acquire);

                // --- 冲突 (Active) ---
                if(status == TxStatus::ACTIVE) {
                    out_conflict = current->owner;
                    // 失败返回前，清理刚才申请的内存
                    delete my_new_node;
                    delete my_record;
                    return nullptr;
                }
                
                // --- 冲突 (Committed but not cleaned) ---
                if (status == TxStatus::COMMITTED) {
                    // 对方已提交但还没释放锁。必须等他释放并更新 data_ptr_。
                    std::this_thread::yield();
                    continue; // 重试
                }

                // --- 抢占 (Steal Aborted) ---
                // status == ABORTED，可以直接覆盖
                // current 指向的是死人的 Record
            }

            // --- CAS 尝试上位 ---
            RecordT* expected = current;
            if (record_ptr_.compare_exchange_strong(expected, my_record, std::memory_order_acq_rel)) {
                // [成功]
                std::printf("[T%zu] ALLOC Record %p (Tx: %p)\n", tid, my_record, tx);

                if (current != nullptr) {
                    // [修改] 如果我们覆盖了死人 (Steal)，需要帮他收尸
                    // 他的 Record 和 他的脏数据 (new_node) 都是垃圾
                    EBRManager::instance()->retire(current->new_node);
                    EBRManager::instance()->retire(current);
                }
                
                return my_record;
            } 
            else {
                // CAS 失败，有人插队
                // 循环重试，不要 delete my_record，下次循环继续用
                if (expected != nullptr && expected->owner != tx) {
                     // 如果 expected 变了且不是我 (甚至可能变成 Active)，可以快速失败
                     // 但为了简单，这里选择 continue 继续抢
                }
            }
        }
    }

   void commitReleaseRecord(const uint64_t commit_ts) override {
        auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id()) % 1000;

        // 1. 获取 Record
        RecordT* record = record_ptr_.load(std::memory_order_acquire);
        
        // 安全检查 (理论上一定是我的)
        if (!record) return; 

        // 2. 更新时间戳
        record->new_node->write_ts = commit_ts;

        // 3. 将新节点正式发布到 data_ptr_
        data_ptr_.store(record->new_node, std::memory_order_release);

        // 4. 释放锁 (置空 record_ptr_)
        record_ptr_.store(nullptr, std::memory_order_release);

        // 5. 资源回收
        std::printf("[T%zu] RETIRE Record %p (Reason: Commit)\n", tid, record);
        EBRManager::instance()->retire(record->old_node);
        EBRManager::instance()->retire(record);
    }


    void abortRestoreData(void* saved_record_ptr) override {
        auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id()) % 1000;
        auto* my_record = static_cast<RecordT*>(saved_record_ptr);
        
        // 1. 尝试摘除我的 Record (Undo)
        RecordT* expected = my_record;
        if (record_ptr_.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel)) {
            // 没人抢占,回收我创建的脏节点 (new_node) 和 Record
            std::printf("[T%zu] RETIRE Record %p (Reason: Abort Cleanup)\n", tid, my_record);
            EBRManager::instance()->retire(my_record->new_node);
            EBRManager::instance()->retire(my_record);
        } 
        else {
            // 已经被别人 Steal (覆盖) 了,只需要回收自己手里的垃圾
            std::printf("[T%zu] RETIRE Record %p (Reason: Abort Stolen)\n", tid, my_record);
        }
    }

    uint64_t getDataVersion() const override {
        NodeT* node = data_ptr_.load(std::memory_order_acquire);
        return node->write_ts;
    }
};


}
}