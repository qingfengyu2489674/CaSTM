# CaSTM 面试讲稿

## 30 秒版本

CaSTM 是我实现的 C++17 软件事务内存 runtime。它把共享变量建模为 MVCC 版本对象，
提供两种不同的并发控制路线：OccSTM 通过乐观验证和重试处理冲突，WwSTM 通过
Wound-Wait 主动中止冲突事务。项目的难点不只是把事务 API 跑通，还包括 EBR 生命周期、
自研分配器，以及多变量提交在并发 helper 存在时仍然保持原子语义。

## 2 分钟版本

应用层通过 Transaction 或 TxContext 读取和写入 TMVar。OccSTM 在提交阶段检查
read set、write set 和条带锁；它在低冲突下结构简单，但高冲突时可能不断
validation failure 并耗尽 retry guard。

WwSTM 的每个共享变量只有一个 tagged head：

~~~text
head -> VersionNode | WriteRecord
~~~

WriteRecord 保存 owner descriptor、旧版本和新版本。读者根据 descriptor 状态选择
旧版本或新版本，helper 可以把终态 Record 展平回 Node。事务先完成 validation 和
prepare，再用一次 ACTIVE -> COMMITTED CAS 确定逻辑提交结果；后面的 Record -> Node
只是物理清理，因此多变量事务不会因为清理顺序不同而产生 partial commit。

生命周期方面，reader、owner 和 helper 都可能暂时持有裸指针，所以 published
Record 不能直接释放，必须通过 EBR 延迟回收；TxDescriptor 也要等所有 Record 清理
完成后再退休。TierAlloc 负责部分小对象分配，但对齐要求更高的 descriptor 和 EBR
元数据保持 system allocator 路径。

## 5 分钟深挖

### 故事一：value/version skew 导致 lost update

旧版 WwSTM 同时维护 data_ptr_ 和 record_ptr_。读操作可能从 stable data pointer
拿到 value，再从另一个 record/node 取得 version。并发切换时，value 和 version 不是
同一个逻辑快照，读集验证可能接受错误组合，最终表现为丢失更新。

修复不是给两个指针各加一个更强的 memory order，而是消除双重 authority：把入口收敛
为一个 tagged head。readSnapshot() 对一次 head 观察解析出同一 VersionNode，同时
复制 value 和 version，再复核 head identity；如果 head 发生变化就重试。这样结构上
保证了 value/version 的来源一致。

### 故事二：EBR premature reclaim 导致 UAF

问题表面上常表现为随机段错误、invalid free 或测试挂死。根因是退休对象使用了不
安全的 epoch，或 ThreadHeap 在线程退出时归还仍可能被其他线程观察的 chunk。此时
reader/helper 仍持有裸指针，但回收线程已经开始析构对象。

取证后分开修复了几层责任：登记后复核当前 epoch，退休时使用正确的已登记 epoch；
GarbageNode 元数据改走与 payload 匹配的 system allocator；垃圾链表使用互斥保护；
线程退出和空 SizeClassPool 不再暴力归还 chunk。修复后的原则是：retire 只表示
进入延迟队列，真正析构必须等 grace period，且所有解引用都发生在 EBR critical
section 内。

### 故事三：OCC retry storm 与 Wound-Wait 的取舍

统一 benchmark 使用相同的 Shared KV/Array workload、8 个操作的逻辑事务和相同的
线程/读写比例。首次 432-run 矩阵中，WwSTM 108/108 正常完成，OccSTM 97/108；
OccSTM 的 11 个无效点集中在高冲突 16T/20T，并触发 1,000,000 次 retry guard。
这些点 checksum 仍正确，所以这是 progress failure，不是数据正确性错误。

WwSTM 的优势不是“永远更快”：在 20T、10R/90W、高冲突场景，它也有约 80% 的
abort rate 和约 883 us 的 p99。更准确的结论是：WwSTM 用主动冲突裁决和较高的
事务代价，换取了该矩阵中更稳定的 progress；mutex 仍然是强 baseline，STM 并不
保证在所有 workload 上获得更高绝对吞吐。

### 面试收束

这个项目最值得展示的不是某一张“最高吞吐”图，而是完整的工程闭环：

~~~text
协议设计
  → 并发错误取证
  → 生命周期/线性化修复
  → 针对性回归测试
  → 可复现 benchmark
  → 对 invalid 和 trade-off 做诚实解释
~~~
