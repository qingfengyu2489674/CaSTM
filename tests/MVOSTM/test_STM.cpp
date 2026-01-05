#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <stdexcept>
#include "MVOSTM/STM.hpp"

// 1. 基础功能测试：验证 load/store 和返回值逻辑
TEST(STMTest, BasicReadWrite) {
    STM::Var<int> account(100);

    // 测试 void 类型的 atomically
    STM::atomically([&](Transaction& tx) {
        int val = tx.load(account);
        tx.store(account, val + 50);
    });

    // 测试带返回值的 atomically
    int current_balance = STM::atomically([&](Transaction& tx) {
        return tx.load(account);
    });

    EXPECT_EQ(current_balance, 150);
}

// 2. 异常回滚测试：验证抛出异常后，修改不会生效（原子性）
TEST(STMTest, ExceptionRollback) {
    STM::Var<std::string> status("Clean");

    // 预期会抛出 runtime_error
    EXPECT_THROW({
        STM::atomically([&](Transaction& tx) {
            tx.store(status, std::string("Dirty")); // 正确：显式转换为 std::string
            throw std::runtime_error("Boom!"); // 抛出异常
        });
    }, std::runtime_error);

    // 验证状态是否保持原样
    std::string result = STM::atomically([&](Transaction& tx) {
        return tx.load(status);
    });

    EXPECT_EQ(result, "Clean");
}

// 3. 并发测试：多线程累加器（验证冲突检测与重试机制）
TEST(STMTest, ConcurrentCounter) {
    STM::Var<int> counter(0);
    
    const int NUM_THREADS = 16;
    const int INC_PER_THREAD = 2000;

    std::vector<std::thread> workers;
    
    // 启动线程
    for (int i = 0; i < NUM_THREADS; ++i) {
        workers.emplace_back([&]() {
            for (int j = 0; j < INC_PER_THREAD; ++j) {
                STM::atomically([&](Transaction& tx) {
                    // 经典的 Read-Modify-Write 竞争场景
                    int val = tx.load(counter);
                    tx.store(counter, val + 1);
                });
            }
        });
    }

    // 等待结束
    for (auto& t : workers) {
        t.join();
    }

    // 验证最终结果
    int final_val = STM::atomically([&](Transaction& tx) {
        return tx.load(counter);
    });

    // 如果没有 MVCC 冲突检测，这个值会小于 8000
    EXPECT_EQ(final_val, NUM_THREADS * INC_PER_THREAD);
}

// 定义链表节点结构
// 注意：节点内的 next 指针必须被 STM::Var 包裹，这样才能被事务管理
struct ListNode {
    int val;
    STM::Var<ListNode*> next;

    ListNode(int v) : val(v), next(nullptr) {}
};

TEST(STMTest, ConcurrentOrderedList) {
    // 链表头指针，也是一个受 STM 管理的变量
    STM::Var<ListNode*> head(nullptr);

    // 配置：4个线程，每个线程插入50个节点（总量200个）
    // 数量不宜过大，因为有序链表插入是 O(N^2) 操作，冲突率极高，重试会很频繁
    const int NUM_THREADS = 4;
    const int ITEMS_PER_THREAD = 50;

    std::vector<std::thread> workers;

    // 启动线程
    for (int i = 0; i < NUM_THREADS; ++i) {
        workers.emplace_back([&, i]() {
            // 每个线程负责插入一系列特定的数字
            // Thread 0: 0, 4, 8...
            // Thread 1: 1, 5, 9...
            // 这样可以保证插入的值唯一，且分布在链表不同位置
            for (int j = 0; j < ITEMS_PER_THREAD; ++j) {
                int val_to_insert = j * NUM_THREADS + i;

                STM::atomically([&](Transaction& tx) {
                    // 1. 创建新节点 (本地堆内存，尚未共享)
                    ListNode* new_node = new ListNode(val_to_insert);

                    // 2. 寻找插入位置 (prev -> new_node -> curr)
                    ListNode* prev = nullptr;
                    ListNode* curr = tx.load(head);

                    while (curr != nullptr) {
                        // 如果当前节点值大于待插入值，说明找到位置了
                        if (curr->val > val_to_insert) {
                            break;
                        }
                        prev = curr;
                        // 【关键】读取下一个节点，这也需要通过 tx.load
                        curr = tx.load(curr->next);
                    }

                    // 3. 执行链接操作
                    // 设置新节点的 next 指向 curr
                    // 注意：这里 new_node 是私有的，可以直接初始化 next，
                    // 但为了严谨，我们用 store 将其 next 指向 curr
                    tx.store(new_node->next, curr);

                    if (prev == nullptr) {
                        // 插在头部
                        tx.store(head, new_node);
                    } else {
                        // 插在中间/尾部
                        tx.store(prev->next, new_node);
                    }
                });
            }
        });
    }

    // 等待所有插入完成
    for (auto& t : workers) {
        t.join();
    }

    // ==========================================
    // 验证阶段 (Verify)
    // ==========================================
    
    // 使用一个只读事务来遍历和验证整个链表
    STM::atomically([&](Transaction& tx) {
        ListNode* curr = tx.load(head);
        int count = 0;
        int last_val = -1;

        while (curr != nullptr) {
            // 1. 验证有序性 (Strictly Increasing)
            if (curr->val <= last_val) {
                // 如果发现乱序，说明之前的事务在插入时没“看清”前驱/后继的关系
                ADD_FAILURE() << "List is NOT sorted! Found " << curr->val << " after " << last_val;
            }
            last_val = curr->val;
            
            // 2. 计数
            count++;
            
            // 继续遍历
            curr = tx.load(curr->next);
        }

        // 3. 验证完整性 (Lost Update Check)
        // 如果少于预期，说明有插入事务被“覆盖”了
        EXPECT_EQ(count, NUM_THREADS * ITEMS_PER_THREAD) 
            << "List size mismatch! Possible Lost Insert.";
    });

    // 清理内存 (可选，防止 Valgrind/ASAN 报错)
    // 实际生产环境通常依靠 EBR 或智能指针，这里手动清理简化演示
    /*
    STM::atomically([&](Transaction& tx) {
        ListNode* curr = tx.load(head);
        while (curr) {
            ListNode* next = tx.load(curr->next);
            delete curr;
            curr = next;
        }
        tx.store(head, (ListNode*)nullptr);
    });
    */
}

// 定义树节点
struct TreeNode {
    int val;
    STM::Var<TreeNode*> left;
    STM::Var<TreeNode*> right;

    TreeNode(int v) : val(v), left(nullptr), right(nullptr) {}
};

// 辅助函数：中序遍历验证 BST 性质和节点数量
void inorder_traversal(Transaction& tx, TreeNode* node, std::vector<int>& result) {
    if (!node) return;
    
    // 递归读取左子树
    TreeNode* left_child = tx.load(node->left);
    inorder_traversal(tx, left_child, result);
    
    // 访问当前节点
    result.push_back(node->val);
    
    // 递归读取右子树
    TreeNode* right_child = tx.load(node->right);
    inorder_traversal(tx, right_child, result);
}

TEST(STMTest, ConcurrentBST) {
    STM::Var<TreeNode*> root(nullptr);

    // 配置：4线程，每线程插入 50 个节点
    const int NUM_THREADS = 4;
    const int ITEMS_PER_THREAD = 50; 
    
    std::vector<std::thread> workers;

    for (int i = 0; i < NUM_THREADS; ++i) {
        workers.emplace_back([&, i]() {
            // 构造一些随机但唯一的数据
            // 使用 (j * NUM_THREADS + i) 保证全局唯一
            // 为了让树稍微平衡一点，不是纯粹的单调插入，我们可以对 j 进行一些变换
            // 但为了简单起见，这里还是用简单的步长逻辑，STM 不关心树是否平衡
            for (int j = 0; j < ITEMS_PER_THREAD; ++j) {
                // 生成一个伪随机数，但保证唯一性
                // 比如：(j * 4 + i) * 13 % (总数 * 10) ... 只要唯一即可
                // 这里为了简单，依然使用间隔插入，这样不同线程会竞争不同的子树
                int val_to_insert = i + j * NUM_THREADS; 

                STM::atomically([&](Transaction& tx) {
                    TreeNode* new_node = new TreeNode(val_to_insert);
                    
                    // 1. 处理根节点为空的情况
                    TreeNode* curr = tx.load(root);
                    if (curr == nullptr) {
                        tx.store(root, new_node);
                        return;
                    }

                    // 2. 寻找插入位置
                    while (true) {
                        if (val_to_insert < curr->val) {
                            // 向左走
                            TreeNode* left = tx.load(curr->left);
                            if (left == nullptr) {
                                // 找到位置，挂载
                                tx.store(curr->left, new_node);
                                break;
                            }
                            curr = left;
                        } else {
                            // 向右走
                            TreeNode* right = tx.load(curr->right);
                            if (right == nullptr) {
                                // 找到位置，挂载
                                tx.store(curr->right, new_node);
                                break;
                            }
                            curr = right;
                        }
                    }
                });
            }
        });
    }

    for (auto& t : workers) {
        t.join();
    }

    // ==========================================
    // 验证阶段
    // ==========================================
    STM::atomically([&](Transaction& tx) {
        std::vector<int> sorted_vals;
        TreeNode* root_node = tx.load(root);
        
        // 收集所有节点值
        inorder_traversal(tx, root_node, sorted_vals);

        // 1. 验证数量 (是否有丢失更新)
        EXPECT_EQ(sorted_vals.size(), NUM_THREADS * ITEMS_PER_THREAD)
            << "Tree size mismatch! Lost updates detected.";

        // 2. 验证顺序 (是否符合 BST 左小右大性质)
        // 中序遍历 BST 得到的一定是严格递增序列
        bool is_sorted = std::is_sorted(sorted_vals.begin(), sorted_vals.end());
        EXPECT_TRUE(is_sorted) << "Tree does not maintain BST property!";
        
        // 额外检查是否有重复（STM 应该保证我们在逻辑中没插入重复值）
        auto last = std::unique(sorted_vals.begin(), sorted_vals.end());
        EXPECT_EQ(last, sorted_vals.end()) << "Duplicate values found in tree!";
    });

    // (可选) 内存清理
    /*
    STM::atomically([&](Transaction& tx) {
        // BFS 或 DFS 删除所有节点... 略
    });
    */
}

// 标准 GTest 入口
int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}


