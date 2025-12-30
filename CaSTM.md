# MVOSTM (Multi-Version Object STM) 构建实施文档

## 0. 项目前置准备

**目标**：确认基础设施可用，环境配置无误。

*   **依赖组件**：
    *   `ThreadHeap`：线程局部堆分配器（已实现）。
    *   `EBRManager`：Epoch-Based Reclamation 管理器（已实现）。
*   **编译标准**：C++17 (`-std=c++17`)。
*   **平台**：Linux/Windows/macOS (x64/arm64)。

---

## 阶段一：全局基础设施 (Infrastructure Layer)

**目标**：构建事务的“时空基准”。我们需要一个全局时钟来定序，以及一个哈希锁表来保护写提交。

### 1.1 全局时钟 (Global Clock)
*   **定义**：一个原子单调递增的 `uint64_t` 计数器。
*   **功能**：提供事务的 `ReadVersion` (RV) 和 `WriteVersion` (WV)。
*   **实现细节**：
    *   使用 `std::atomic<uint64_t>`。
    *   考虑 Cache Line 对齐（虽然只有一个，但防止与锁表发生伪共享）。

### 1.2 条带化锁表 (Striped Lock Table)
*   **定义**：一个固定大小（如 $2^{20}$）的自旋锁数组。
*   **功能**：在 MVCC 模式下，**仅在写事务提交阶段**用于互斥。
*   **策略**：
    *   利用 `TMVar` 对象的内存地址进行 Hash 映射。
    *   **关键优化**：每个锁必须对齐到 Cache Line (64字节) 以避免 False Sharing。
*   **接口**：
    *   `void lock(const void* addr)`
    *   `void unlock(const void* addr)`

### 🧪 阶段一测试计划
1.  **时钟测试**：多线程并发 `fetch_add`，验证原子性和性能。
2.  **锁表冲突测试**：
    *   线程 A 锁定地址 P1，线程 B 锁定地址 P2（P1, P2 映射到不同槽），验证无阻塞。
    *   线程 A 锁定地址 P1，线程 B 锁定地址 P1，验证 B 被阻塞直到 A 释放。

---

## 阶段二：MVCC 数据结构 (Data Structure Layer)

**目标**：定义数据的存储格式。利用 `ThreadHeap` 实现高效的版本节点分配。

### 2.1 版本节点 (VersionNode)
*   **结构**：
    ```cpp
    template<typename T>
    struct VersionNode {
        T payload;              // 实际数据
        uint64_t wts;          // Write Timestamp (写入时的版本号)
        VersionNode* prev;      // 指向更旧的版本
        
        // 必须重载 new/delete 以使用 ThreadHeap
        static void* operator new(size_t size) { return ThreadHeap::allocate(size); }
        static void operator delete(void* p) { ThreadHeap::deallocate(p); }
    };
    ```
*   **注意**：`T` 必须是 Copy Constructible 的。

### 2.2 事务变量容器 (TMVar)
*   **结构**：
    *   内部仅持有一个原子头指针：`std::atomic<VersionNode<T>*> head_`。
*   **功能**：
    *   作为用户持有的句柄。
    *   **禁止**：用户不能直接访问 `head_`，必须通过后续的 `Transaction` 句柄访问。

### 🧪 阶段二测试计划
1.  **分配测试**：循环创建和销毁 100万个 `VersionNode`，验证 `ThreadHeap` 是否工作正常，内存是否泄露。
2.  **链表构建测试**：手动构建一个长度为 5 的版本链，验证指针连接正确。

---

## 阶段三：事务上下文与日志 (Context Layer)

**目标**：实现事务的“工作台”。每个线程拥有独立的事务描述符。

### 3.1 读写集 (ReadSet / WriteSet)
为了支持 TL2 算法，我们需要记录事务读了什么、写了什么。

*   **WriteSet (写缓冲)**：
    *   存储结构：`struct WriteLog { void* addr; std::any val; ... }` (或使用模板擦除技术)。
    *   **优化**：由于我们知道类型 `T`，最好在 `Transaction` 内部使用异构容器或特定类型的 Log。为了通用性，通常使用 `vector<WriteEntry>`，其中 `WriteEntry` 包含一个用于提交的 `commit_function` 闭包。
*   **ReadSet (读记录)**：
    *   存储：`vector<const void*>` (记录读取过的对象地址)。
    *   用途：在提交阶段校验，确保我在 `Start` 到 `Commit` 期间，读过的对象没有被别人修改（防止 Write Skew）。

### 3.2 事务描述符 (Transaction Descriptor)
*   **成员**：
    *   `uint64_t read_version_`: 事务开始时的全局时钟快照。
    *   `std::vector<...> read_set_`: 记录已读对象。
    *   `std::vector<...> write_set_`: 记录待写数据。
*   **生命周期**：`thread_local`，复用以减少 vector 分配开销。

### 🧪 阶段三测试计划
1.  **日志操作测试**：
    *   模拟事务：向 WriteSet 添加数据，向 ReadSet 添加地址。
    *   验证 `clear()` 操作后 vector 容量是否保留（避免反复 malloc）。

---

## 阶段四：核心逻辑引擎 (Engine Layer)

**目标**：实现 MVCC 的核心算法（Load, Store, Commit）。这是最复杂的阶段。

### 4.1 读逻辑 (Load)
*   **输入**：`TMVar<T>& var`
*   **流程**：
    1.  检查 WriteSet：如果自己修改过，返回 WriteSet 中的值（Read-Your-Own-Writes）。
    2.  读取 `var.head_`。
    3.  **MVCC 遍历**：
        *   遍历链表 `curr = head`。
        *   如果 `curr->wts <= tx.read_version_`，说明该版本可见。
        *   记录到 ReadSet。
        *   返回 `curr->payload`。
    4.  如果找不到可见版本（且链表不为空），说明版本过新且无历史，作为空处理或抛出重试。

### 4.2 写逻辑 (Store)
*   **输入**：`TMVar<T>& var`, `T value`
*   **流程**：
    1.  将 `(var地址, value)` 存入 WriteSet。
    2.  **不**立即修改内存。

### 4.3 提交逻辑 (Commit) - 核心难点
*   **流程**：
    1.  **加锁 (Lock)**：对 WriteSet 中所有对象地址对应的条带锁进行加锁（需对地址排序以防死锁）。
    2.  **获取写版本 (Acquire WV)**：`wv = global_clock.fetch_add(1) + 1`。
    3.  **校验 (Validate)**：
        *   检查 `read_version + 1 == wv`？如果是，说明期间无竞争，跳过校验。
        *   否则，遍历 ReadSet，检查每个对象的**最新版本** `wts` 是否仍 `<= read_version`。如果被更新了，则校验失败 -> **Abort**。
    4.  **写入 (Write Phase)**：
        *   遍历 WriteSet。
        *   创建新 `VersionNode` (wts = wv)，挂载到 `head` 前面。
        *   **剪枝与回收**：检测链表长度，如果过长，断开尾部节点，调用 `EBRManager::retire()`。
    5.  **释放 (Unlock)**：解锁所有条带锁。

### 🧪 阶段四测试计划
1.  **单线程事务**：Start -> Store -> Commit。验证数据是否更新，版本号是否正确。
2.  **读写测试**：
    *   Tx1 修改 A 提交。
    *   Tx2 (RV < Tx1.WV) 读取 A，应该读到旧值（如果做了历史保留）。
    *   Tx3 (RV > Tx1.WV) 读取 A，应该读到新值。

---

## 阶段五：API 封装与 EBR 集成 (API Layer)

**目标**：提供对外友好的 C++17 接口，集成 EBR 保护。

### 5.1 事务句柄 (Transaction Handle)
*   封装 `load/store` 接口。
*   提供 `tx[var]` 语法糖。

### 5.2 驱动函数 (atomically)
*   **实现**：
    ```cpp
    template<typename F>
    auto atomically(F&& func) {
        EBRManager::enter(); // 1. 进入 Epoch
        while (true) {
            try {
                Transaction& tx = get_local_tx();
                tx.begin();
                auto result = func(tx); // 执行用户逻辑
                if (tx.commit()) {      // 提交
                    EBRManager::leave(); // 离开 Epoch
                    return result;
                }
            } catch (const RetryException&) {
                tx.clear(); // 重置状态，准备重试
                // 可选：指数回退 (Exponential Backoff)
            }
        }
    }
    ```

### 5.3 智能指针风格 (Smart Pointer API)
*   提供 `tx.read_ptr(var)` 返回 `const T*`，零拷贝。
*   提供 `tx.write_ptr(var)` 返回 `T*`（副本），支持原地修改语义。

### 🧪 阶段五（最终）测试计划
1.  **转账测试**：经典的 Alice 转账给 Bob，高并发测试，验证总金额守恒。
2.  **链表测试**：并发插入删除链表节点，验证无环、无断链、无内存泄露。
3.  **异常测试**：在事务 Lambda 中抛出异常，验证事务是否正确回滚（不提交）。

---

## 总结

这份文档构成了我们接下来的开发路线图。
*   **底层**保证了内存分配的高效（ThreadHeap）和 内存回收的安全（EBR）。
*   **中间层**保证了并发的正确性（MVCC + TL2）。
*   **顶层**提供了现代 C++ 的易用性。

**下一步建议**：请确认此计划是否符合预期，如果没有问题，我们将从 **阶段一：全局基础设施** 开始编写代码。