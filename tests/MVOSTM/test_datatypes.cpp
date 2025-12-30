#include <gtest/gtest.h>
#include <string>

// 1. 包含对外头文件 (用户视角)
#include "MVOSTM/DataTypes.hpp"

// 2. 包含实现头文件 (事务系统视角)
// 注意：必须包含这个才能实例化 TMVar，否则会报 incomplete type 错误。
// 这正是我们想要的设计：只有 Transaction.cpp 或测试文件才包含它。
#include "MVOSTM/DataTypesImpl.hpp" 

// 引入 ThreadHeap。如果是在单元测试环境中不想链接复杂的分配器，
// 可以临时 mock 一下，或者确保 CMakeLists 链接了 TierAlloc。
// 这里假设 ThreadHeap 已经可用。

namespace {

// 一个用于测试的自定义结构体
struct ComplexData {
    int id;
    std::string name;
    
    // 必须提供默认构造函数，因为你的 TMVar 目前只支持 T{}
    ComplexData() : id(0), name("default") {}
    ComplexData(int i, std::string n) : id(i), name(n) {}
};

}

class MVCCDataStructureTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 如果 ThreadHeap 需要初始化，在这里调用
        // ThreadHeap::init(); 
    }
    
    void TearDown() override {
    }
};

// 测试 1: 基础整数类型的初始化 (Genesis Node)
TEST_F(MVCCDataStructureTest, IntegerInitialization) {
    // 实例化 TMVar<int>
    // 这会调用 TMVar::TMVar() -> new VersionNode(0, nullptr, int{})
    TMVar<int> var;

    // 验证 head 指针不为空
    auto* head = var.loadHead();
    ASSERT_NE(head, nullptr) << "TMVar head should not be null after construction";

    // 验证创世版本信息
    // 注意：这里能访问 head->version 是因为我们 include 了 DataTypesImpl.hpp
    EXPECT_EQ(head->commit_ts, 0) << "Genesis version should be 0";
    EXPECT_EQ(head->prev, nullptr) << "Genesis node should have no previous version";
    EXPECT_EQ(head->payload, 0) << "Payload should be default constructed (0 for int)";
}

// 测试 2: 复杂对象的初始化
TEST_F(MVCCDataStructureTest, ComplexObjectInitialization) {
    TMVar<ComplexData> var;
    
    auto* head = var.loadHead();
    ASSERT_NE(head, nullptr);
    
    EXPECT_EQ(head->payload.id, 0);
    EXPECT_EQ(head->payload.name, "default");
}

// 测试 3: 模拟事务更新 (链表挂载)
TEST_F(MVCCDataStructureTest, VersionChaining) {
    TMVar<int> var; // head -> Node(v=0, val=0)
    
    // 1. 获取当前头节点
    auto* old_head = var.loadHead();
    
    // 2. 模拟事务：创建新节点 (Version 100, Payload 42)
    // 新节点的 prev 指向 old_head
    // 使用 VersionNode 的 operator new (即 ThreadHeap)
    using Node = VersionNode<int>;
    Node* new_node = new Node(100, old_head, 42);
    
    // 3. 模拟 CAS 更新 head
    // 实际事务中会锁住 StripedLock，这里直接 store
    var.getHeadRef().store(new_node);
    
    // 4. 验证链表结构
    auto* current_head = var.loadHead();
    EXPECT_EQ(current_head, new_node);
    EXPECT_EQ(current_head->payload, 42);
    EXPECT_EQ(current_head->commit_ts, 100);
    
    // 验证回溯
    ASSERT_NE(current_head->prev, nullptr);
    EXPECT_EQ(current_head->prev, old_head);
    EXPECT_EQ(current_head->prev->payload, 0); // 创世值
    EXPECT_EQ(current_head->prev->commit_ts, 0);
}

// 测试 4: 验证 ThreadHeap 是否介入
// 这是一个偏向行为的测试，如果不方便 Mock ThreadHeap，可以跳过。
// 但我们可以通过简单的指针地址检查来确保 new 出来的对象地址合理。
TEST_F(MVCCDataStructureTest, AllocationSanity) {
    TMVar<int> var;
    auto* n1 = var.loadHead();
    
    // 手动分配一个节点
    using Node = VersionNode<int>;
    Node* n2 = new Node(1, n1, 123);
    
    // 简单的检查：地址不应为空
    EXPECT_NE(n1, nullptr);
    EXPECT_NE(n2, nullptr);
    EXPECT_NE(n1, n2);

    // 清理手动分配的节点，防止泄漏干扰后续测试
    // TMVar 析构只会清理它 head_ 指向的链条。
    // 如果我们没把 n2 挂上去，需要手动 delete。
    delete n2; 
}