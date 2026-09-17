#pragma once
#include <string>
#include <vector>

#include "KnowledgeStore.h"

// 知识缺口检测 —— 见 PROJECT.md §6.6 / §6.7
//
// 作用：在一场会话结束时，从知识库里找出**值得问用户的那几条**，
// 产出候选问题交给确认交互（2.4）去问。
//
// 三条不可动摇的约束（都在 PROJECT.md 里有出处）：
//
// 1. **只用规则，不上模型**（§6.6）。让模型判断"哪里该问"成本高且不稳定，
//    而四条规则已经覆盖绝大多数情况。而且规则版能进 L1，秒级可验证。
//
// 2. **提问是稀缺资源**（§1.3 红线）。最多问 5 个、只问影响后续识别/翻译的内容。
//    按字面"有陌生知识就问"会让用户第三次使用就不看了 ——
//    所以 confirmed 且没变化的条目**一个字都不问**。
//
// 3. **不问 archived**。那已经被用户否掉了，再问就是烦人。

namespace knowledge {

// 六条规则
enum class GapRule {
    ValueChanged,            // 旧值 ≠ 新值：与已有 Confirmed 值冲突（§6.6 第四条）
    ConflictingSpellings,    // 同一个实体的两种写法（Erica / Erika）—— 见下面 struct 的说明
    InconsistentRendering,   // 译法不一致：同一个键的值变过多次（§6.6 第一条）
    HighFreqUnconfirmed,     // 高频未确认：hits >= 3 却还是 candidate（§6.6 第二条）
    NewlySeen,               // 本场第一次见到的专名（§6.4① 自动抽取的出口）
    LowConfidenceName,       // 低置信度专名：confidence < 0.5（§6.6 第三条）
};

const char* to_string(GapRule r);        // 稳定标识，用于日志与测试
int         priority_of(GapRule r);      // 越小越先问

// 一条待确认的问题
struct GapQuestion {
    GapRule     rule = GapRule::HighFreqUnconfirmed;
    long long   knowledge_id = 0;
    std::string kind;
    std::string key;
    std::string value;
    int         hits       = 0;
    double      confidence = -1.0;
    std::string old_value;        // 仅 ValueChanged 有意义

    // **同一个实体的其它写法**（仅 ConflictingSpellings 非空）。
    //
    // 【为什么需要这个字段 —— 用户真跑会话 #13 暴露的问题】
    // 库里本来有 `Erica`（会话 #43 听到的，3 次），这一场 Whisper 听成了 `Erika`。
    // 旧代码把它们**当成两条互不相干的知识**，于是问了两个独立的问题：
    //     3. 已经听到 3 次「Erica」，一直没确认过。它是对的说法吗？   → 用户跳过
    //     4. 第一次听到「Erika」。这个词的写法对吗？                  → 用户按了 y
    // 两个回答互相矛盾，而用户**没有任何办法知道**这两个写法指的是同一个人 ——
    // 因为两个问题都没提到对方。结果 `Erika` 成了 confirmed，
    // 直接进了下一场的 `initial_prompt`（实测：`initial_prompt = "Marco, Erika"`。
    // 播客主持人叫 Erica，Erika 是听错的）。
    //
    // 【判断】正确答案是**我们自己已经握在手里了**：库里有两条只差一个字母的记录，
    // 本来就是"疑似同一个实体"。这种情况下把判断推给用户之前，
    // 必须先把冲突摆出来 —— 否则用户只能瞎猜，而且猜错会被记成约束。
    // 所以：合并成一个问题、一次给出全部选项。
    struct Alt {
        long long   knowledge_id = 0;
        std::string value;
        int         hits = 0;
    };
    std::vector<Alt> alternatives;

    // 面向用户的一句话（含"回车跳过"的暗示，由 2.4 渲染时补选项）
    std::string question;
};

// 一条知识 + 它的变化历史。**历史是必需输入**：
// "译法不一致"和"旧值≠新值"两条规则都只能从历史里看出来。
struct KnowledgeWithHistory {
    KnowledgeItem                          item;
    std::vector<KnowledgeStore::HistoryRow> history;
};

// 阈值（集中在这里，方便调参与测试）
constexpr int    kHighFreqHits        = 3;      // 出现这么多次还没确认就该问
constexpr double kLowConfidenceBelow  = 0.5;    // 识别置信度低于此值
constexpr size_t kMaxQuestionsDefault = 5;      // §1.3 红线：最多问 5 个

// 同一条知识最多被问几次。
//
// 【为什么必须是 2 而不是无限】"提问是稀缺资源"（§1.3）要能落地，
// 就得有个地方记住"这条问过了"。没有它，用户每次跳过之后下一场会话
// 还会看到一模一样的问题。2 次是这个取舍的中点：
// 用户第一次可能没想好（允许再问一次），第二次还是跳过说明他不关心。
// 值发生变化时计数清零（KnowledgeStore::upsert），那时值得重新问。
constexpr int    kMaxAsks             = 2;

// **核心纯函数**：输入知识+历史，输出候选问题。
// 不碰数据库、不碰模型、不依赖时间 —— 所以能进 L1 自检。
//
// `current_session`：本场会话 id，用于 `NewlySeen` 规则（判断"这条是不是本场新听到的"）。
// **必须 > 0 该规则才生效** —— 默认 -1 时它一律不触发，
// 因为 KnowledgeItem::source_session 的默认值也是 -1，不挡住的话
// "两者都是 -1 所以相等"会让所有测试数据都变成"本场新见"。
//
// 同一个键最多出一条问题（取优先级最高的那条规则），
// 结果按优先级排序后截断到 max_questions。
std::vector<GapQuestion> detect_gaps(const std::vector<KnowledgeWithHistory>& entries,
                                     size_t max_questions = kMaxQuestionsDefault,
                                     long long current_session = -1);

// 薄封装：从库里取候选条目（candidate + 最近变化的），套用上面的规则。
// 真正的判定逻辑全在 detect_gaps 里，这里只负责取数。
std::vector<GapQuestion> detect_gaps_from_store(size_t max_questions = kMaxQuestionsDefault,
                                                long long current_session = -1);

}  // namespace knowledge
