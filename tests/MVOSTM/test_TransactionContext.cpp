#include <gtest/gtest.h>
#include "MVOSTM/TransactionContext.hpp" // 包含你的头文件

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
    // 转换为具体类型并删除
    delete static_cast<MockNode*>(node);
    g_delete_call_count++;
}

// 模拟的提交器 (在这个单元测试中不需要实际逻辑)
void mockCommitter(void* tmvar, void* node, uint64_t wts) {
    // no-op
}

class TransactionContextTest : public ::testing::Test {
protected:
    void SetUp() override {
        g_delete_call_count = 0;
    }

    void TearDown() override {
        // 确保没有内存泄漏干扰其他测试
    }
};

// ============================================================================
// 测试用例
// ============================================================================

// 测试 1: 基础属性存取
TEST_F(TransactionContextTest, BasicProperties) {
    TransactionDescriptor desc;

    // 初始状态
    EXPECT_EQ(desc.getReadVersion(), 0);
    EXPECT_TRUE(desc.readSet().empty());
    EXPECT_TRUE(desc.writeSet().empty());

    // 设置版本
    desc.setReadVersion(100);
    EXPECT_EQ(desc.getReadVersion(), 100);

    // Reset 应重置版本
    desc.reset();
    EXPECT_EQ(desc.getReadVersion(), 0);
}

// 测试 2: 读集 (Read Set) 操作
TEST_F(TransactionContextTest, ReadSetOperations) {
    TransactionDescriptor desc;

    int dummy_var1 = 1;
    int dummy_var2 = 2;

    desc.addToReadSet(&dummy_var1);
    desc.addToReadSet(&dummy_var2);

    ASSERT_EQ(desc.readSet().size(), 2);
    EXPECT_EQ(desc.readSet()[0], &dummy_var1);
    EXPECT_EQ(desc.readSet()[1], &dummy_var2);

    // Reset 应清空读集
    desc.reset();
    EXPECT_TRUE(desc.readSet().empty());
}

// 测试 3: 写集清理逻辑 (Reset 场景) -> 模拟 Abort
TEST_F(TransactionContextTest, WriteSetCleanupOnReset) {
    TransactionDescriptor desc;

    // 模拟分配内存
    MockNode* node1 = new MockNode{10};
    MockNode* node2 = new MockNode{20};

    // 添加到写集
    // 这里的地址参数 (void* addr) 可以是任意值，测试不关心
    desc.addToWriteSet(nullptr, node1, mockCommitter, mockDeleter);
    desc.addToWriteSet(nullptr, node2, mockCommitter, mockDeleter);

    ASSERT_EQ(desc.writeSet().size(), 2);
    
    // 执行 Reset (模拟事务 Abort)
    // 预期：deleter 应该被调用 2 次，node1 和 node2 被 delete
    desc.reset();

    EXPECT_EQ(g_delete_call_count, 2);
    EXPECT_TRUE(desc.writeSet().empty());
}

// 测试 4: 析构函数清理逻辑 -> 模拟线程退出
TEST_F(TransactionContextTest, DestructorCleanup) {
    {
        TransactionDescriptor desc;
        MockNode* node = new MockNode{99};
        desc.addToWriteSet(nullptr, node, mockCommitter, mockDeleter);
        
        // desc 在这里生命周期结束
    }
    
    // 预期：析构函数调用了 cleaner，计数器 +1
    EXPECT_EQ(g_delete_call_count, 1);
}

// 测试 5: 模拟提交成功场景 (Ownership Transfer)
TEST_F(TransactionContextTest, CommitScenario) {
    TransactionDescriptor desc;

    MockNode* node = new MockNode{100};
    desc.addToWriteSet(nullptr, node, mockCommitter, mockDeleter);

    // --- 模拟 Commit 过程 ---
    // 1. 遍历 writeSet，挂载节点 (略)
    // 2. 提交成功后，所有权转移给 TMVar
    // 3. 因此，必须清空 WriteSet，防止 reset/destruct 时重复释放
    desc.writeSet().clear(); 

    // --- 模拟 Commit 后重置 ---
    desc.reset();

    // 预期：deleter 没有被调用，因为 vector 已经被手动清空了
    // 如果被调用了，说明发生了 Double Free (因为我们在上面没真的挂载，这里就会 delete)
    // 或者说明逻辑错误。
    EXPECT_EQ(g_delete_call_count, 0);

    // 清理手动分配的内存 (仅测试用，因为上面模拟提交没真的接管内存)
    delete node; 
}