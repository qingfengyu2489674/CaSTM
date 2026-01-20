#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>

#include "TaggedPtr.hpp"
#include "VersionNode.hpp"
#include "WriteRecord.hpp"
#include "EBRManager/EBRManager.hpp"
#include "WwSTM/TxDescriptor.hpp"

namespace STM {
namespace Ww {

// 为了让 TxContext::write_set_ 可以通过 std::vector<TMVarBase*> 统一存储不同类型的变量
struct TMVarBase {
    virtual ~TMVarBase() = default;

    virtual void commitReleaseRecord() = 0;
    virtual void abortRestoreData(TxDescriptor* tx) = 0;
};


template <typename T>
class TMVar : public TMVarBase{
public:
    using NodeT = detail::VersionNode<T>;
    using RecordT = detail::WriteRecord<T>;

private:
    std::atomic<NodeT*> data_ptr_;
    std::atomic<RecordT> record_ptr_;

public:
    template<typename... Args>
    TMVar(Args&&...args) {
        NodeT* init_node = new NodeT(0, std::forward<Args>(args)...);
        uintptr_t raw = TaggedPtrHelper::packNode(init_node);
        data_ptr_.store(raw, std::memory_order_release);
    }

    ~TMVar() {
        EBRManager::instance()->retire(data_ptr_.load());
        RecordT* rec = record_ptr_.load();
        if (rec) EBRManager::instance()->retire(rec);
    }

    // 禁止拷贝和移动
    TMVar(const TMVar&) = delete;
    TMVar& operator=(const TMVar&) = delete;
    TMVar(TMVar&&) = delete;
    TMVar& operator=(TMVar&&) = delete;

    const T& readProxy(TxDescriptor* tx) {
        RecordT* record = record_ptr_.load(std::memory_order_release);

        // Case 1: 无锁 -> 直接读
        if(record == nullptr)
            return data_ptr_.load(std::memory_order_release)->payload;

        // Case 2: 有锁 -> 检查 Owner
        if(record->owning_tx == tx)
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
        RecordT* current = record_ptr_.load(std::memory_order_acquire);
        if (current != nullptr) {
            out_conflict = current->owning_tx;
            return nullptr;
        }

        NodeT* old_node = data_ptr_.load(std::memory_order_release);
        NodeT* new_node = new NodeT(tx->start_ts, *static_cast<const T*>(val_ptr));
        RecordT* new_record = new RecordT(tx, old_node, new_node);

        RecordT* expected = nullptr;
        if (record_ptr_.compare_exchange_strong(expected, new_record, std::memory_order_acq_rel)) {
            return new_record;
        }
        else {
            delete new_node;
            delete new_record;
            out_conflict = expected->owning_tx;
            return nullptr;
        }
    }

    void installNewNode() {
        data_ptr_.store(record_ptr_->new_version, std::memory_order_release);
    }

    void commitReleaseRecord() {
        RecordT* record = record_ptr_.load(std::memory_order_release);

        if(record) {
            EBRManager::instance()->retire(record->old_node);
            EBRManager::instance()->retire(record);
        }
    }

    void abortRestoreData(TxDescriptor* tx) override {
        RecordT* record = record_ptr_.load(std::memory_order_release);
        if(record->owning_tx == tx) {
            data_ptr_.store(record->old_version, std::memory_order_release);
            record_ptr_.store(nullptr, std::memory_order_release);
        }
        EBRManager::instance()->retire(record->new_version);
        EBRManager::instance()->retire(record);
    }

};


}
}