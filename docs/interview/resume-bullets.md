# CaSTM 简历项目描述

以下内容只使用当前仓库已经实现和验证过的事实。性能数字对应本地记录的
benchmark/allocator-ablation 结果，不应脱离 workload、机器和有效性规则单独引用。

## 中文

- 实现 C++17 软件事务内存 runtime CaSTM，提供 OccSTM（optimistic validation/retry）和 WwSTM（Wound-Wait）两条并发控制路线，基于 MVCC 版本节点完成事务读写与冲突处理。
- 为 WwSTM 设计并实现 single-head Locator，将共享状态统一为 tagged VersionNode | WriteRecord，以 ACTIVE -> COMMITTED descriptor CAS 作为多变量事务唯一 linearization point，并通过 helping 完成物理展平。
- 实现 EBR 延迟回收和 CentralHeap/ThreadHeap/SizeClassPool 分层分配器；修复 premature reclaim、published WriteRecord、over-aligned TxDescriptor 等生命周期问题，并补充对应的压力与回归测试。
- 在统一 Shared KV/Array workload 上完成 432-run STM 矩阵和 allocator ablation；WwSTM 在矩阵中 108/108 完成，OccSTM 为 97/108，失败点被明确标记为 retry-guard progress failure 而非 checksum correctness failure。

## English

- Implemented CaSTM, a C++17 software transactional memory runtime with two concurrency-control paths: OccSTM optimistic validation/retry and WwSTM Wound-Wait conflict management over MVCC versioned variables.
- Designed and implemented a single-head WwSTM Locator using a tagged VersionNode | WriteRecord state, with one descriptor ACTIVE -> COMMITTED CAS as the transaction linearization point for multi-variable commits and helping-based physical cleanup.
- Implemented EBR deferred reclamation and a CentralHeap/ThreadHeap/SizeClassPool allocator hierarchy; diagnosed and fixed premature reclaim, published WriteRecord lifetime, and over-aligned TxDescriptor allocation/lifetime bugs with stress and regression tests.
- Built a reproducible Shared KV/Array benchmark and allocator ablation. In the recorded 432-run matrix, WwSTM completed 108/108 points while OccSTM completed 97/108; retry-guard failures are reported separately from checksum correctness.

## 面试时的限定说明

- benchmark 是单机 WSL2、Intel i5-14600KF、20 logical CPUs、GCC 11.4、Release -O3 环境下的结果。
- 不把 mutex baseline 或 STM 宣称为普遍赢家；高冲突下 OccSTM 可能耗尽 retry guard，WwSTM 也会付出 abort 和 tail-latency 成本。
- TSan/LSan runtime validation 受当前 WSL/ptrace 环境限制，不能写成已完整通过。
