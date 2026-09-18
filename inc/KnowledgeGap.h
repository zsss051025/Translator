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
//    而这几条规则已经覆盖绝大多数情况。而且规则版能进 L1，秒级可验证。
//    （5.2 会把提问交给 agent —— 那时规则退化成 agent 的工具，而不是被删掉。）
//
// 2. **只问"值得占用注意力"的东西**（§1.3，2026-09-18 修正过措辞）。
//
//    ⚠️ **不是"提问是稀缺资源"** —— 用户当面否掉了这个说法，他是对的：
//    提问量会**自然衰减**（已确认的不再问、只问本场新见的、跳过两次就沉底），
//    在固定语料上收敛到零，在持续有新内容的真实工作流里稳定在"每场几个"。
//    这不是靠上限压出来的。
//
//    真正稀缺的是**用户对这套系统的信任**，而信任的杀手是**蠢问题**，
//    不是"多几个好问题"。所以杠杆在精度（`CommonWords` 的通用概念过滤 +
//    `Interaction` 的类型化提问），不在数量。数量上限见 `kMaxQuestionsDefault`
//    的说明 —— 它已经降级成安全网。
//
// 3. **不问 archived**。那已经被用户否掉了，再问就是无视他的表达。

namespace knowledge {

// 七条规则
enum class GapRule {
    ValueChanged,            // 旧值 ≠ 新值：与已有 Confirmed 值冲突（§6.6 第四条）
    ConflictingSpellings,    // 同一个实体的两种写法（Erica / Erika）—— 见下面 struct 的说明
    InconsistentRendering,   // 译法不一致：同一个键的值变过多次（§6.6 第一条）
    HighFreqUnconfirmed,     // 高频未确认：hits >= 3 却还是 candidate（§6.6 第二条）
    AskDefinition,           // 术语还没有含义 —— 问"它指什么"（§6.4③，闭环的核心）
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
    //    // 【为什么需要这个字段 —— 用户真跑会话 #13 暴露的问题】
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

    // **现在正在生效的那个写法**（仅 ConflictingSpellings 有意义；空表示没有已确认的）。
    //
    // 【为什么这是数据而不是文案】交互层要跟用户说"现在用的是「Erika」" ——
    // 用户必须知道改动的后果（改错了会影响下一场的识别）。
    // 但"哪个在生效"是**事实**，不是措辞，所以它留在这里；
    // 怎么把它说出口由 Interaction 决定。
    std::string in_use;

    // 【这里以前有一个 std::string question 字段，装的是拼好的中文句子。】
    //
    // 它被删掉是这一层解耦的关键一步：只要"该问什么"的代码里能拼出面向用户的文案，
    // 内部状态就会顺着它漏出去（旧文案里有 `已经听到 N 次` 和
    // `识别置信度只有 0.40`，后者直接把 confidence 打给了用户）。
    // 现在 GapQuestion 只装**事实**，措辞全部由 Interaction 层负责。
    // 改文案不需要动这个文件，也不需要重跑缺口检测的自检。
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

// ---------------------------------------------------------------------------
// 单场问题数上限 —— **这是安全网，不是政策**
// ---------------------------------------------------------------------------
//
// 【框架修正，2026-09-18，用户当面否掉了我原来的说法】
// 我原来把这里写成"§1.3 红线：提问是稀缺资源"，并且用它同时解释了
// 两件**性质完全不同**的事：
//
//   ① 别问蠢问题      ← 有真实证据：真实会话里出现过 15 次以上的冤枉提问
//                        （TV / down / movies / putting / together / …，
//                          用户甚至认真回答了"TV 是电视节目的缩写"）
//   ② 别问太多        ← **我没有任何证据**，是假设
//
// 用户的反驳是对的：**提问不是稀缺资源，而且它会自然衰减** ——
// 已确认的不再问、只问本场新见的、跳过两次就沉底（见 kMaxAsks）。
// 在一份固定语料上，提问量会收敛到零；而在持续产生新内容的真实工作流里，
// 它会稳定在"每场几个新名字"这个**非零稳态**。这不是靠上限压出来的。
//
// 所以真正稀缺的**不是提问数量，是用户对这套系统的信任**；
// 而信任的杀手是**蠢问题**，不是"多几个好问题"。杠杆在**精度**（见 CommonWords.h
// 的通用概念过滤 + Interaction 层的类型化提问），不在数量上限。
//
// 【那这个数还留着干什么】纯粹当安全网：如果哪天提取器失控吐出 50 个候选，
// 不该把 50 个问题全砸给用户。正常会话根本碰不到这个数
// （实测用户的真实会话最多一场 3 个问题）。
// 所以它从一个"红线"降级成一个"兜底" —— 措辞变了，因为**它约束的不是用户，
// 是我们的提取器**。
constexpr size_t kMaxQuestionsDefault = 12;

// 同一条知识最多被问几次。
//
// 【这条的依据不是"稀缺"，是"用户已经表过态"】跳过也是一种回答：
// 连按回车的意思是"我不关心这个"。没有这一条，用户每次跳过之后，
// 下一场会话还会看到一模一样的问题 —— 那是**无视用户的表达**，
// 跟"少问几个"没关系。
//
// 2 次是取舍的中点：第一次可能没想好（允许再问一次），第二次还跳过说明不关心。
// 值发生变化时计数清零（KnowledgeStore::upsert）—— 那时问题本身变了，值得再问。
constexpr int    kMaxAsks             = 2;

// **每场会话最多问几个"它指什么"**。
//
// 【这条同样被我原来的框架污染过】原来写的是"别的规则按个 y 就行，
// 这个要用户打一句话，一场问三个他就不答了"—— 那句"他就不答了"是**我猜的**，
// 用户刚刚明确否掉：他愿意答，而且"问三个就不答"这种担心不该由我替他决定。
//
// 【保留这个常数，但理由变了】它区分的是**两种成本量级**，不是稀缺性：
// 别的规则是选择题（一个字符），这个是**开放式**（要打一整句话）。
// 让开放式问题占满一场的所有配额，会让用户在一次会话末尾连续打八句话 ——
// 那不是"稀缺"，那是**形态不合理**。
//
// 所以：放开到 4，剩下的名额留给选择题。正常会话碰不到这个数。
constexpr size_t kMaxDefinitionAsksPerSession = 4;


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
