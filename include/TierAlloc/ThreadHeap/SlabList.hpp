#pragma once
#include "ThreadHeap/Slab.hpp"

class SlabList {
public:
    SlabList() = default;

    [[nodiscard]] bool empty() const { return head_ == nullptr; }


    // [LIFO] 头部插入
    void push_front(Slab* slab) {
        slab->prev = nullptr;
        slab->next = head_;
        if (head_) head_->prev = slab;
        else tail_ = slab;
        head_ = slab;
    }

    // [FIFO] 尾部插入
    void push_back(Slab* slab) {
        slab->next = nullptr;
        slab->prev = tail_;
        if (tail_) tail_->next = slab;
        else head_ = slab;
        tail_ = slab;
    }

    // [Generic] 任意位置移除
    void remove(Slab* slab) {
        // 处理前驱
        if (slab->prev) slab->prev->next = slab->next;
        else head_ = slab->next;

        // 处理后继
        if (slab->next) slab->next->prev = slab->prev;
        else tail_ = slab->prev;

        // 清理指针，防止悬挂
        slab->prev = nullptr;
        slab->next = nullptr;
    }

    // [Access] 弹出头部
    Slab* pop_front() {
        if (!head_) return nullptr;
        Slab* slab = head_;
        remove(slab);
        return slab;
    }

    // [Access] 获取头部 (只看不拿，用于 Full 救援探测)
    Slab* front() const { return head_; }

    
    // [Rotation] 将头部节点直接移到尾部
    void move_head_to_tail() {
    
        if (head_ == tail_) return;

        Slab* first = head_;
        Slab* last = tail_;

        // 1. 摘下头
        head_ = first->next;
        head_->prev = nullptr;

        // 2. 接到尾
        last->next = first;
        first->prev = last;
        first->next = nullptr;
        tail_ = first;
    }

private:
    Slab* head_ = nullptr;
    Slab* tail_ = nullptr;
};
