#pragma once

#include <atomic>

template<typename T>
struct VersionNode;

template<typename T>
class TMVar {
public:
    using Node = VersionNode<T>;

    template<typename... Args>
    explicit TMVar(Args&&... args);
    
    ~TMVar();

    std::atomic<Node*>& getHeadRef() {
        return head_;
    }

    Node* loadHead() const {
        return head_.load(std::memory_order_acquire);
    }

    TMVar(const TMVar&) = delete;    
    TMVar& operator= (const TMVar&) = delete;

private:
    std::atomic<Node*> head_{nullptr};
};