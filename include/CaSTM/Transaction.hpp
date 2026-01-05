#pragma once

#include "TransactionDescriptor.hpp"
#include "StripedLockTable.hpp"
#include "GlobalClock.hpp"
#include "TMVar.hpp"
#include <algorithm>
#include <cstdint>
#include <vector>
#include <atomic> 

struct RetryException : public std::exception {};


class Transaction {
public:
    explicit Transaction(TransactionDescriptor* desc) : desc_(desc) {}

    void begin();
    bool commit();

    template<typename T>
    T load(TMVar<T>& var);

    template<typename T>
    void store(TMVar<T>& var, const T& val);

    template<typename T, typename... Args>
    T* alloc(Args&&... args);

    template<typename T>
    void free(T* ptr);

private:
    bool validateReadSet();
    void lockWriteSet();
    void unlockWriteSet();

private:
    TransactionDescriptor* desc_;
};


inline void Transaction::begin() {
    desc_->reset();
    desc_->setReadVersion(GlobalClock::now());
}


template<typename T>
T Transaction::load(TMVar<T>& var) {
    using Node = typename TMVar<T>::Node;

    // 1. Read-Your-Own-Writes (总是先查自己有没有写过)
    // 即使是只读模式，如果用户写了代码先 store 再 load，也得能读到（虽然这种变态逻辑很少见）
    auto& wset = desc_->writeSet();
    if (!wset.empty()) {
        for(auto it = wset.rbegin(); it != wset.rend(); ++it) {
            if(it->tmvar_addr == &var) return static_cast<Node*>(it->new_node)->payload;
        }
    }

    auto* curr = var.loadHead();

    desc_->addToReadSet(&var, curr, TMVar<T>::validate);
    
    uint64_t rv = desc_->getReadVersion();

    // 遍历链表找 <= RV 的版本
    while (curr != nullptr) {
        if (curr->write_ts <= rv) {
            return curr->payload;
        }
        curr = curr->prev;
    }
    
    throw RetryException();    
}

template <typename T>
void Transaction::store(TMVar<T>& var, const T& val) {
    using Node = typename TMVar<T>::Node;
    Node* node = new Node(0, nullptr, val);

    desc_->addToWriteSet(&var, node, TMVar<T>::committer, TMVar<T>::deleter);
}


template<typename T, typename... Args>
T* Transaction::alloc(Args&&... args) {
    // 1. 从 ThreadHeap 申请裸内存
    void* raw_mem = ThreadHeap::allocate(sizeof(T));

    // 2. 记录到 Descriptor，以此保证：如果不幸 Abort，这块内存会被归还
    desc_->recordAllocation(raw_mem);

    // 3. 原地构造 (Placement New)
    return new(raw_mem) T(std::forward<Args>(args)...);
}


template<typename T>
void Transaction::free(T* ptr) {
    if (!ptr) return;

    ptr->~T();
    ThreadHeap::deallocate(ptr);
}

inline bool Transaction::commit() {
    auto& wset = desc_->writeSet();
    auto& rset = desc_->readSet();

    // 只读事务
    if (desc_->writeSet().empty()) {
        desc_->reset();
        return true;
    }

    lockWriteSet();
    uint64_t wv = GlobalClock::tick();

    if(!validateReadSet()) {
        unlockWriteSet();
        return false;
    }

    for (auto& entry : wset) {
        entry.committer(entry.tmvar_addr, entry.new_node, wv);
        entry.new_node = nullptr;
    }

    unlockWriteSet();

    desc_->commitAllocations(); 
    desc_->reset();
    return true;
}

inline bool Transaction::validateReadSet() {
    uint64_t rv = desc_->getReadVersion();
    auto& lock_table = StripedLockTable::instance();
    auto& locks = desc_->lockSet(); // 这里面存的是 (void*)index

    for(const auto& entry : desc_->readSet()) {
        // [Step 1] 前置锁检查
        if(lock_table.is_locked(entry.tmvar_addr)){
            // 【修复关键点】必须先计算出地址对应的索引
            size_t idx = lock_table.getStripeIndex(entry.tmvar_addr);
            void* idx_ptr = reinterpret_cast<void*>(idx);

            // 然后在 locks 列表中查找这个索引
            bool locked_by_me = std::binary_search(locks.begin(), locks.end(), idx_ptr);
            
            // 如果被锁了且不是我锁的 -> 冲突
            if(!locked_by_me) return false;
        }

        // [Step 2] 身份 + 时间验证
        if(!entry.validator(entry.tmvar_addr, entry.expected_head, rv)) {
            return false;
        }

        // [Step 3] 内存屏障 & 后置锁检查
        std::atomic_thread_fence(std::memory_order_seq_cst);

        if(lock_table.is_locked(entry.tmvar_addr)){
            // 【修复关键点】同样的逻辑
            size_t idx = lock_table.getStripeIndex(entry.tmvar_addr);
            void* idx_ptr = reinterpret_cast<void*>(idx);

            bool locked_by_me = std::binary_search(locks.begin(), locks.end(), idx_ptr);
            if(!locked_by_me) return false;
        }
    }
    return true;
}


// Transaction.cpp 中:

inline void Transaction::lockWriteSet() {
    auto& wset = desc_->writeSet();
    auto& locks = desc_->lockSet(); 
    
    locks.clear(); // 这里现在存的是 (void*)index
    
    auto& lock_table = StripedLockTable::instance();
    
    // 1. 获取所有索引
    std::vector<size_t> indices;
    indices.reserve(wset.size());
    for(auto& entry : wset) {
        indices.push_back(lock_table.getStripeIndex(entry.tmvar_addr));
    }

    // 2. 排序去重 (防止死锁的核心步骤)
    std::sort(indices.begin(), indices.end());
    auto last = std::unique(indices.begin(), indices.end());
    indices.erase(last, indices.end());

    // 3. 加锁
    for(size_t idx : indices) {
        lock_table.lockByIndex(idx);
        // 保存索引以便解锁
        locks.push_back(reinterpret_cast<void*>(idx));
    }
}

inline void Transaction::unlockWriteSet() {
    auto& locks = desc_->lockSet();
    auto& lock_table = StripedLockTable::instance();
    
    for (auto it = locks.rbegin(); it != locks.rend(); ++it) {
        size_t idx = reinterpret_cast<size_t>(*it);
        lock_table.unlockByIndex(idx);
    }
    locks.clear();
}

