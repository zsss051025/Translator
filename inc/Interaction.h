#pragma once
#include <cstddef>
#include <string>
#include <vector>

// ===========================================================================
// 交互层（表现层）—— 见 PROJECT.md §6.7
// ===========================================================================
//
// 【这份头文件存在的全部理由】
// 以前"该问什么"和"怎么说"是揉在一起的：KnowledgeGap.cpp 里直接拼面向用户的中文，
// 而且顺手把内部状态打了出去 ——
//     std::to_string(k.hits)   →「已经听到 4 次」
//     fmt_conf(k.confidence)   →「识别置信度只有 0.40」
//     "最多再问一次"           → 这是 asked_count
// 后果不是"文案难看"，是**用户觉得自己在给数据库做标注**，而不是在教一个助手。
// 改一句话还得去改检测规则的代码。
//
// 所以这里定自己的输入模型 `Prompt`，并且：
//
//   ① **本文件与 Interaction.cpp 不许 include KnowledgeGap.h / KnowledgeStore.h。**
//      这是结构性约束，不是风格偏好。一旦能拿到 KnowledgeItem，
//      迟早会有第一个文案顺手把 status / confidence 打出去。
//      让结构上拿不到，比写一句"注意不要泄漏内部状态"的注释可靠得多。
//   ② `Prompt` 刻意**不含** hits / asked_count / confidence / status。
//      它只描述"要把这件事跟人说清楚，需要知道什么"。
//   ③ 好处不止是干净：这一层能整体替换（将来的 GUI 对话框、多语言），
//      而且能单独做自检 —— 不需要数据库、不需要缺口检测。
//
// 数据流（单向，箭头不回头）：
//
//     KnowledgeGap   逻辑：该问什么        → GapQuestion
//     ConfirmGaps    流程：问、解释回答、落库 → 适配成 Prompt（to_prompt）
//     Interaction    表现：怎么说          → 字符串

namespace interaction {

// ---------------------------------------------------------------------------
// 用户提到过它的**程度**（定性，不定量）
// ---------------------------------------------------------------------------
//
// 【为什么不给次数】两个原因，第二个是真 bug：
//   ① 计数是内部状态。用户没有在数，看到"已经听到 4 次"只会觉得莫名其妙。
//   ② **`hits` 是跨会话累加的** —— 所以说"这场会话里听到 4 次"本身就是假的。
//      实测：`erica` 的 hits=9 是 #43 那一场的 3 次 + #46/#47 若干次累加出来的。
//      旧文案里写的「这场听到 N 次了」是**一个错的数字**。
//      定性描述（"多次提到"）在跨会话语义下是真的，定量描述不是。
enum class Familiarity {
    FirstTime,    // 第一次提到
    AFewTimes,    // 不止一次
    ManyTimes,    // 多次
};

// ---------------------------------------------------------------------------
// 要把哪一类事情告诉用户
// ---------------------------------------------------------------------------
//
// 【注意】这个枚举是**按"用户需要做什么决定"分的**，不是按内部规则分的。
// 内部有七条缺口规则，这里只有八种"决定"，两者是多对多的关系 ——
// 这正是解耦点：将来规则怎么增删，用户看到的问法种类是稳定的。
enum class Kind {
    ConfirmPersonName,       // 这是正确的人名写法吗？
    ConfirmTerm,             // 这是你们项目的技术术语吗？
    AskTermMeaning,          // 它是术语吗？是的话指什么？（确认 + 学含义，一问两用）
    ConfirmProjectName,      // 这是项目名吗？
    UpdateChangedValue,      // 我记的是 A，刚听到 B，要更新吗？
    PickSpelling,            // 同一个东西有两种写法，哪个对？
    UnifySpelling,           // 同一处出现过几种写法，以后统一用哪个？
    SuggestCorrectSpelling,  // 刚才没听清，正确写法是什么？
};

// ---------------------------------------------------------------------------
// 交互层的输入模型
// ---------------------------------------------------------------------------
struct Prompt {
    Kind kind = Kind::ConfirmTerm;

    // 要跟用户说的那个词/概念（**展示形**，比如 "Marco" / "CO-RE"，不是归一化键）
    std::string subject;

    Familiarity familiarity = Familiarity::FirstTime;

    // UpdateChangedValue：我原来记的是 previous，刚听到的是 current
    std::string previous;
    std::string current;

    // PickSpelling：候选写法，**按推荐程度排好序**（第一个是最可能对的）
    std::vector<std::string> options;

    // 用户这一轮给出的新值（由流程层在解释完回答之后填，供回执使用）
    std::string answer;

    // 失败时的**技术原因**（原始错误串）。
    // 这是唯一允许把内部细节说出去的地方 —— 它出现在"没记上"的时候，
    // 那时把原因藏起来才是更坏的行为。
    std::string failure;
};

enum class Outcome {
    Accepted,         // 认可了现有写法/概念
    Corrected,        // 用户给了正确的写法
    MeaningLearned,   // 用户教了含义
    Skipped,          // 先跳过
    Declined,         // 用户说不用记 / 不用改
    Failed,           // 没写进库
};

// ---------------------------------------------------------------------------
// 文案生成（全部是纯函数，能进 L1 自检）
// ---------------------------------------------------------------------------

// 开场。n = 这次要问几条。
std::string intro(size_t n);

// 问题本身。
//
// 【必须满足的一条】读起来要像"一个注意到某事的同事在问你"，
// 而不是"一个数据库在催你标注"。所以每一条都要么说清**为什么值得记**
// （"我注意到你多次提到"），要么说清**我手上已经有的是什么**
// （"我之前记的是 Erika，这次听到的是 Erica"）。
std::string question(const Prompt& p);

// 告诉用户能怎么答。跟着问题打出来。
std::string hint(const Prompt& p);

// 回执 —— **这是整个体验的落点**：用户要看到"我教的东西被怎么用了"。
// 所以不能是"已确认"，要说清"以后我会拿它做什么"。
std::string acknowledgment(const Prompt& p, Outcome o);

// 收尾。给的是**件数**，不是内部状态。
std::string summary(int accepted, int skipped, int failed);

// 输入结束（管道/EOF/GUI 没有对话框）时的收尾语。
std::string no_input_note();

}  // namespace interaction
