#pragma once
#include <string>
#include <vector>

#include "CandidateTriage.h"

// ===========================================================================
// 判断缓存的持久化实现（第 ② 层）
// ===========================================================================
//
// 【为什么它单独一个文件，而不是写进 CandidateTriage.cpp】
// CandidateTriage 是**纯编排**：三层谁先谁后、失败怎么退化。它必须能进 L1 自检，
// 而且它的头文件刻意不 include KnowledgeStore.h（"这个模块没有写知识库的能力"
// 是一条结构性保证，不是注释里的承诺）。
// 一旦把 SQLite 塞进去，这个保证就没了，自检也得先建库。
//
// 所以对半分：
//   · `triage::decide()` 知道**什么时候**该读/写缓存（策略）
//   · `TriageCache` 知道**怎么**读/写（SQLite）
// 两者之间只有 CacheLookup / CacheStore 两个回调 —— 自检可以换成内存 map
// 走完全相同的编排逻辑，不碰数据库。这是同一个"可注入回调"模式在本项目的第四次使用
// （前三次：knowledge::LineReader / triage::Judge / agent::Planner）。
//
// 【它和知识库的关系：只有一处，而且是一张单独的表】
// triage_verdicts 表**只进不出**到知识库：没有任何一行代码把判决搬进 knowledge。
// 判决错了的代价只是"少问一次/多问一次"，永远不会污染识别提示和翻译约束。
//
// ⚠️ 本文件 include 了 KnowledgeStore.h，**只为了 normalize_key()**。
//    自己再写一份归一化是本项目明确禁止的事（PROJECT.md §8.8：两份实现迟早走散，
//    已经栽过三次）。所以宁可 include 也不复制 —— 真正的保证不在这里，
//    而在"只写 skip、只写进 triage_verdicts、没有任何代码从它读回 knowledge"。

namespace triage {

// 一条缓存记录（给 `--triage-cache` 看，也用于自检核对）
struct CacheEntry {
    std::string key;
    std::string value;
    std::string verdict;
    std::string kind;
    std::string primer;
    std::string why;
    std::string judged_at;
    std::string last_used_at;
    int         reused = 0;
};

class TriageCache {
public:
    static TriageCache& instance();

    // 表可用吗？老库（2.12 之前建的）没有这张表，或者库根本没打开。
    // **表不存在不是错误**：调用方应当退化成"没有缓存"，会话照常进行。
    // 与 `long_term_memory_ready()` 同一个态度：缓存是**可选加速层**，
    // 它的缺席绝不能让主流程失败。
    bool ready() const;

    // 接进 triage::decide() 的两个回调。
    //
    // ⚠️ 拿到的 `Cache` 只在 ready() 为真时有意义；不 ready 时返回空 Cache
    //    （operator bool 为假），decide() 会自动跳过第 ② 层。
    Cache hooks();

    struct Stats {
        int total   = 0;   // 缓存里的条目数
        int reused  = 0;   // 累计被复用的次数（= 省下的模型调用次数）
        int stale   = 0;   // 超过 90 天没用过的（提示可以清理）
    };
    Stats stats() const;

    // 列出来（最近判定在前）
    std::vector<CacheEntry> list(int limit = 200) const;

    // 忘掉一个词（**纠错出口**）。传原样或归一化后的词都能找到。
    // 这是"同一个词换了语境、旧判决不再合适"时的唯一补救手段 ——
    // 有它才敢在缓存键上做"不含上下文"的简化。
    bool forget(const std::string& word, std::string* err = nullptr);

    // 清空。返回删掉的行数。
    int clear();

private:
    TriageCache() = default;
    TriageCache(const TriageCache&) = delete;
    TriageCache& operator=(const TriageCache&) = delete;

    bool exec(const std::string& sql, std::string* err = nullptr) const;
};

}  // namespace triage
