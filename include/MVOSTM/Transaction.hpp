#pragma once

#include "TransactionDescriptor.hpp"
#include "StripedLockTable.hpp"
#include "GlobalClock.hpp"
#include "TMVar.hpp"
#include <algorithm>
#include <cstdint>
#include <vector>

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


// template <typename T>
// T Transaction::load(TMVar<T>& var) {
//     using Node = typename TMVar<T>::Node;

//     auto& wset = desc_->writeSet();
//     for(auto it = wset.rbegin(); it != wset.rend(); ++it) {
//         if(it->tmvar_addr == &var) {
//             return static_cast<Node*>(it->new_node)->payload;
//         }
//     }

//     auto* curr = var.loadHead();
//     desc_->addToReadSet(&var, TMVar<T>::validate);

//     while (curr != nullptr) {
//         if(curr->write_ts <= desc_->getReadVersion()) {
//             return curr->payload;
//         }
//         curr = curr->prev;
//     }
//     throw RetryException(); 
// }

// include/Transaction.hpp

template<typename T>
T Transaction::load(TMVar<T>& var) {
    using Node = typename TMVar<T>::Node;
    
    // 1. Read-Your-Own-Writes (查写集)
    auto& wset = desc_->writeSet();
    for(auto it = wset.rbegin(); it != wset.rend(); ++it) {
        if(it->tmvar_addr == &var) return static_cast<Node*>(it->new_node)->payload;
    }
    
    // 2. Direct Read (读取内存)
    auto* curr = var.loadHead();

    // 3. 【核心补丁】Load-Time Lock Check
    // TL2 要求：读取时必须确保变量未被锁定。
    // 既然它不在我的 WriteSet 里，如果它被锁了，那一定是别人锁的。
    // 读一个被锁的变量是危险的，必须重试。
    if (StripedLockTable::instance().is_locked(&var)) {
        throw RetryException();
    }

    // 4. Register Validation (加入读集)
    desc_->addToReadSet(&var, TMVar<T>::validate);

    // 5. Strict Version Check (禁止读未来版本)
    if (curr == nullptr || curr->write_ts > desc_->getReadVersion()) {
        throw RetryException(); 
    }
    
    // (可选) 为了极其严格的正确性，可以在读完 Version 后再查一次锁 (Post-Read Lock Check)
    // 但在大多数实现中，上面的 Pre-Read Lock Check 加上 Commit 时的验证已经足够达到 100% 正确率。
    if (StripedLockTable::instance().is_locked(&var)) {
         throw RetryException();
    }

    return curr->payload;
}


template <typename T>
void Transaction::store(TMVar<T>& var, const T& val) {
    using Node = typename TMVar<T>::Node;
    Node* node = new Node(0, nullptr, val);

    desc_->addToWriteSet(&var, node, TMVar<T>::committer, TMVar<T>::deleter);
}

inline bool Transaction::commit() {
    auto& wset = desc_->writeSet();
    auto& rset = desc_->readSet();

    // 只读事务
    if (desc_->writeSet().empty()) {
        desc_->reset();
        return true;
    }

    // 读写事务
    if(!validateReadSet()) {
        return false;
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

    desc_->reset();
    return true;
}
// include/Transaction.hpp

// 必须包含头文件
#include <atomic> 

// ...

inline bool Transaction::validateReadSet() {
    uint64_t rv = desc_->getReadVersion();
    auto& lock_table = StripedLockTable::instance();
    auto& locks = desc_->lockSet(); // 我持有的锁

    for(const auto& entry : desc_->readSet()) {
        // ==========================================================
        // STEP 1: 前置锁检查 (Pre-Check)
        // 确保读之前没人锁
        // ==========================================================
        if(lock_table.is_locked(entry.tmvar_addr)){
            // 如果被锁了，除非是我自己锁的，否则 Abort
            bool locked_by_me = std::binary_search(locks.begin(), locks.end(), entry.tmvar_addr);
            if(!locked_by_me) return false;
        }

        // ==========================================================
        // STEP 2: 版本/数据校验 (Value Check)
        // 读取内存中的 VersionNode
        // ==========================================================
        if(!entry.validator(entry.tmvar_addr, rv)) {
            return false;
        }

        // ==========================================================
        // STEP 3: 【关键修复】内存屏障 (Memory Fence)
        // 强制 CPU 和编译器：必须先完成上面的 STEP 2 (读数据)，
        // 才能执行下面的 STEP 4 (读锁状态)。
        // 防止指令重排导致的 "ABA" 漏判。
        // ==========================================================
        std::atomic_thread_fence(std::memory_order_seq_cst);

        // ==========================================================
        // STEP 4: 后置锁检查 (Post-Check / Double-Check)
        // 确保读的过程中，没有别的线程偷偷加了锁
        // ==========================================================
        if(lock_table.is_locked(entry.tmvar_addr)){
            bool locked_by_me = std::binary_search(locks.begin(), locks.end(), entry.tmvar_addr);
            if(!locked_by_me) return false;
        }
    }
    return true;
}


inline void Transaction::lockWriteSet() {
    auto& wset = desc_->writeSet();
    auto& locks = desc_->lockSet();
    locks.clear();
    for(auto& entry : wset) locks.push_back(entry.tmvar_addr);

    // 排序去重
    std::sort(locks.begin(), locks.end());
    auto last = std::unique(locks.begin(), locks.end());
    locks.erase(last, locks.end());

    auto& lock_table = StripedLockTable::instance();
    for(void* addr : locks) {
        lock_table.lock(addr);
    }

}


inline void Transaction::unlockWriteSet() {
    auto& locks = desc_->lockSet();
    auto& lock_table = StripedLockTable::instance();
    
    for (void* addr : locks) {
        lock_table.unlock(addr);
    }
}


