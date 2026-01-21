#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>

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
        else if (status == TxStatus::ABORTED) {
            // 对方已 Abort 但未恢复：读旧值
            return record->old_node->payload;
        } 
        else {
            // 对方 ACTIVE：读旧值 (MVCC Snapshot)
            return record->old_node->payload;
        }
    }

    void* tryWriteAndGetRecord(TxDescriptor* tx, const void* val_ptr, TxDescriptor*& out_conflict) {
        // 循环 CAS 以处理抢占过程中的变化
        while (true) {
            RecordT* current = record_ptr_.load(std::memory_order_acquire);
            
            if(current != nullptr) {
                TxStatus status = current->owner->status.load(std::memory_order_acquire);

                if(status == TxStatus::ACTIVE || status == TxStatus::COMMITTED) {
                    out_conflict = current->owner;
                    return nullptr;
                }

                // 走到这里说明 status == ABORTED,可以直接执行CAS把他覆盖掉 (Steal/Overwrite)。
            }

            // 确定 old_node 
            NodeT* base_node = (current != nullptr) ? current->old_node : data_ptr_.load(std::memory_order_acquire);
            // 装配 new_node 与 new_record 
            NodeT* new_node = new NodeT(tx->start_ts, *static_cast<const T*>(val_ptr));
            RecordT* new_record = new RecordT(tx, base_node, new_node);

            // 3. CAS 尝试上位
            // 如果 current 是 null，我们抢空位。
            // 如果 current 是死人的 Record，我们直接覆盖它。
            RecordT* expected = current;
            if (record_ptr_.compare_exchange_strong(expected, new_record, std::memory_order_acq_rel)) {
                // 成功！
                // 如果我们覆盖了死人的 Record，死人(Tx B)在自己的 abort 流程里会发现锁变了，
                // 他会负责回收他那个被我们挤掉的 Record 内存。
                return new_record;
            } 

            else {
                // CAS 失败
                delete new_node;
                delete new_record;
                
                // 如果 expected 变了，说明情况变了，返回 null 让外部重试
                if (expected != nullptr) {
                    out_conflict = expected->owner;
                    return nullptr;
                }
                // 如果 expected 变成了 null，说明锁释放了，我们可以 continue 循环立即去抢
            }
        }
    }


    void installNewNode(void* record_ptr) {
        auto* record = static_cast<RecordT*>(record_ptr);
        data_ptr_.store(record->new_node, std::memory_order_release);
    }

    void commitReleaseRecord(const uint64_t commit_ts) override {
        RecordT* record = record_ptr_.exchange(nullptr, std::memory_order_release);

        if(record) {
            // 先修改TMVar的时间戳，再回收
            record->new_node->write_ts = commit_ts;
            EBRManager::instance()->retire(record->old_node);
            EBRManager::instance()->retire(record);
        }
    }

    void abortRestoreData(void* saved_record_ptr) override {
        auto* my_record = static_cast<RecordT*>(saved_record_ptr);

        RecordT* current = record_ptr_.load(std::memory_order_acquire);

        if (current == my_record) {
            data_ptr_.store(my_record->old_node, std::memory_order_release);
            record_ptr_.store(nullptr, std::memory_order_release);
        }

        EBRManager::instance()->retire(my_record->new_node);
        EBRManager::instance()->retire(my_record);

    }

};


}
}