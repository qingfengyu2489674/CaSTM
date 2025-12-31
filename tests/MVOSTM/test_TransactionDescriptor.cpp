#include <gtest/gtest.h>
#include "MVOSTM/TransactionDescriptor.hpp" 

// ============================================================================
// 测试辅助设施
// ============================================================================

// 全局计数器，用于追踪 deleter 被调用的次数
static int g_delete_call_count = 0;

// 模拟的数据节点
struct MockNode {
    int value;
};

// 模拟的删除器：不使用 ThreadHeap，仅用于验证逻辑是否被执行
void mockDeleter(void* node) {
    if (node) {
        delete static_cast<MockNode*>(node);
        g_delete_call_count++;
    }
}

// 模拟的提交器 (在这个单元测试中不需要实际逻辑)
void mockCommitter(void* tmvar, void* node, uint64_t wts) {
    // no-op
}

// 模拟的校验器
bool mockValidator(const void* tmvar_addr, uint64_t rv) {
    return true; 
}

class TransactionContextTest : public ::testing::Test {
protected:
    void SetUp() override {
        g_delete_call_count = 0;
    }

    void TearDown() override {
    }
};

// ============================================================================
// 测试用例
// ============================================================================

// 测试 1: 基础属性存取与 Reset 行为
TEST_F(TransactionContextTest, BasicPropertiesAndReuse) {
    TransactionDescriptor desc;

    // 初始状态
    EXPECT_EQ(desc.getReadVersion(), 0);
    EXPECT_TRUE(desc.readSet().empty());
    EXPECT_TRUE(desc.writeSet().empty());
    EXPECT_TRUE(desc.lockSet().empty());

    // 设置版本
    desc.setReadVersion(100);
    EXPECT_EQ(desc.getReadVersion(), 100);

    // Reset 应重置所有状态
    desc.reset();
    EXPECT_EQ(desc.getReadVersion(), 0);
    EXPECT_TRUE(desc.readSet().empty());
    EXPECT_TRUE(desc.writeSet().empty());
    EXPECT_TRUE(desc.lockSet().empty());
}

// 测试 2: 读集 (Read Set) 操作 [已更新]
TEST_F(TransactionContextTest, ReadSetOperations) {
    TransactionDescriptor desc;

    int dummy_var1 = 1;
    int dummy_var2 = 2;

    // 【修正】：现在的 API 只有2个参数 (addr, validator)，不再需要版本号
    desc.addToReadSet(&dummy_var1, mockValidator);
    desc.addToReadSet(&dummy_var2, mockValidator);

    const auto& rset = desc.readSet();
    ASSERT_EQ(rset.size(), 2);
    
    // 验证存储的内容是 ReadLogEntry 结构体
    EXPECT_EQ(rset[0].tmvar_addr, &dummy_var1);
    EXPECT_EQ(rset[0].validator, mockValidator);
    // 不再检查 .version_at_load，因为它已经被移除了
    
    EXPECT_EQ(rset[1].tmvar_addr, &dummy_var2);
    EXPECT_EQ(rset[1].validator, mockValidator);

    desc.reset();
    EXPECT_TRUE(desc.readSet().empty());
}

// 测试 3: 写集清理逻辑 (Reset 场景) -> 模拟 Abort
TEST_F(TransactionContextTest, WriteSetCleanupOnReset) {
    TransactionDescriptor desc;

    // 模拟分配内存 (未挂载到 TMVar)
    MockNode* node1 = new MockNode{10};
    MockNode* node2 = new MockNode{20};

    // 添加到写集
    desc.addToWriteSet(nullptr, node1, mockCommitter, mockDeleter);
    desc.addToWriteSet(nullptr, node2, mockCommitter, mockDeleter);

    ASSERT_EQ(desc.writeSet().size(), 2);
    
    // 执行 Reset (模拟事务 Abort)
    // clearWriteSet_ 会被调用
    desc.reset();

    // 预期：deleter 应该被调用 2 次，释放 node1 和 node2
    EXPECT_EQ(g_delete_call_count, 2);
    EXPECT_TRUE(desc.writeSet().empty());
}

// 测试 4: 析构函数清理逻辑 -> 模拟线程退出或对象销毁
TEST_F(TransactionContextTest, DestructorCleanup) {
    {
        TransactionDescriptor desc;
        MockNode* node = new MockNode{99};
        desc.addToWriteSet(nullptr, node, mockCommitter, mockDeleter);
        
        // desc 作用域结束
    }
    
    // 预期：析构函数调用了 deleter
    EXPECT_EQ(g_delete_call_count, 1);
}

// 测试 5: 模拟提交成功场景 (Ownership Transfer)
TEST_F(TransactionContextTest, CommitScenario) {
    TransactionDescriptor desc;

    MockNode* node = new MockNode{100};
    desc.addToWriteSet(nullptr, node, mockCommitter, mockDeleter);

    // --- 模拟 Transaction::commit 过程 ---
    auto& wset = desc.writeSet();
    
    for (auto& entry : wset) {
        // 假设这里调用了 committer 成功挂载...
        
        // 关键：将 new_node 置空，标志所有权已移交
        entry.new_node = nullptr; 
    }

    // --- 模拟 Commit 后的 Reset ---
    desc.reset();

    // 预期：deleter 没有被调用 (g_delete_call_count == 0)
    // 因为 entry.new_node 为 nullptr
    EXPECT_EQ(g_delete_call_count, 0);

    // 清理手动分配的内存 (仅测试用，因为上面我们实际上没把 node 挂到链表里让 TMVar 管理)
    delete node; 
}

// 测试 6: 锁集复用 (Lock Set Reuse)
TEST_F(TransactionContextTest, LockSetReuse) {
    TransactionDescriptor desc;
    
    // 模拟 lockWriteSet 填充过程
    auto& locks = desc.lockSet();
    locks.push_back((void*)0x1234);
    locks.push_back((void*)0x5678);
    
    // 确保 reset 后 size 为 0，但 capacity 保持 (复用)
    // 虽然 capacity 是实现细节，但 size 必须为 0
    desc.reset();
    
    EXPECT_TRUE(desc.lockSet().empty());
    
    // 再次 reset 应该安全
    desc.reset();
    EXPECT_TRUE(desc.lockSet().empty());
}