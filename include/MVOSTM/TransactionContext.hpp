#pragma once


#include <cstdint>
#include <vector>

struct WriteLogEntry {
    void* tmvar_addr;   // 锁对象地址（TMVar的指针）
    void* new_node;     // 擦除掉类型信息的新节点

    using Committer = void (*)(void* tmvar, void* node, uint64_t commit_ts);
    Committer committer;

    using Deleter = void (*)(void* node);
    Deleter deleter;
};

class TransactionDescriptor {
public:
    enum class State {
        Active,
        Committed,
        Aborted
    };

    TransactionDescriptor() {
        read_set_.reserve(64);
        write_set_.reserve(16);
    }

    ~TransactionDescriptor() {
        clearWriteSet_(); 
    }

    void reset() {
        state_ = State::Active;
        read_version_ = 0;
        read_set_.clear();
        clearWriteSet_();
    }

    void setReadVersion(uint64_t rv) { read_version_ = rv; }
    uint64_t getReadVersion() const { return read_version_; }

    void addToReadSet(const void* addr) {
        read_set_.push_back(addr);
    }

    void addToWriteSet(void* addr, void* new_node, WriteLogEntry::Committer c, WriteLogEntry::Deleter d) {
        write_set_.push_back({addr, new_node, c, d});
    }

    const std::vector<const void*>& readSet() const { return read_set_; }
    std::vector<WriteLogEntry>& writeSet() { return write_set_; }

private:
    void clearWriteSet_() {
        for(WriteLogEntry& entry : write_set_) {
            if (entry.deleter && entry.new_node) {
                entry.deleter(entry.new_node);
            }
        }
        write_set_.clear();
    }

private:
    State state_{State::Active};
    uint64_t read_version_{0};
    std::vector<const void*> read_set_;
    std::vector<WriteLogEntry> write_set_;
};