#pragma once
#include <functional>
#include <iosfwd>
#include <string>
#include <vector>

#include "KnowledgeGap.h"
#include "KnowledgeStore.h"

// 会话结束时的**确认交互** —— 见 PROJECT.md §6.7 / §7 步骤 2.4
//
// 缺口检测（2.3）算出"该问什么"，这一步把问题真问出来，并把用户的回答落到库里。
// 闭环八步里"询问 → 更新"这两步就是它。
//
// ---------------------------------------------------------------------------
// 三条硬约束（都有出处，改代码前先读）
// ---------------------------------------------------------------------------
//
// 1. **必须发生在交付物写完之后**（§6.7）。
//    `write()` 落了盘再问 —— 用户中途 Ctrl+C、或者干脆走开，
//    产出的会议纪要一个字都不会少。反过来就是"问了半天结果没生成纪要"，
//    这是最不可原谅的失败方式。**main.cpp 里的调用位置就是这条约束的体现。**
//
// 2. **绝不能挂住流程**。
//    stdin 不是终端（管道、重定向、将来的 GUI）→ 直接不问，一句都不问。
//    用 std::getline 而不是 _getch 裸读，是为了**输入法能用** ——
//    答案经常是中文专名的正确写法，裸读会把中文输入法打得七零八落。
//    代价是没有超时；接受这个代价，因为此时交付物已经落盘，进程多等一会儿不丢任何东西。
//
// 3. **答过的东西不能再问**（§1.3 提问是稀缺资源）。
//    回答成功 → 状态变 confirmed → 下次 `detect_gaps` 自然不会再问它。
//    这条有专门的自检用例：**问一次之后，同一个键必须从候选里消失**。
//
// ---------------------------------------------------------------------------
// 为什么把"读一行"做成可注入的函数
// ---------------------------------------------------------------------------
// 交互逻辑最容易变成"只能手动点一遍"的代码，于是没人回归它。
// 把输入源抽成 `read_line` 回调之后，自检可以喂一段脚本化的回答，
// 走**真实的知识库写入路径**验证结果 —— 而不是构造一个"理想的"数据结构
// （§8.8⑨ 那条反模式已经栽过两次）。
// 同一个循环，main.cpp 注入读控制台的实现，自检注入读 stringstream 的实现。

namespace knowledge {

// ---------------------------------------------------------------
// 用户那一行输入是什么意思（纯函数，L1 可验）
// ---------------------------------------------------------------
enum class AnswerKind {
    Skip,        // 回车 / 空白 / 无意义的输入 —— 跳过，什么都不改
    Affirm,      // "y" / "是" / "对" —— 认可问题里那个当前值
    Reject,      // "n" / "不是" —— 否掉当前值
    NewValue,    // 其它内容 —— 用户直接给出了正确的写法
};

struct Answer {
    AnswerKind  kind = AnswerKind::Skip;
    std::string value;      // 仅 NewValue 有意义（已 trim、已截断保护）
};

// **纯函数**：一行原始输入 → 动作。
//
// 【为什么必须区分 Reject 和 NewValue】把 "n" 当成"用户输入的新值"，
// 就会把字面量 "no" 写进知识库当专名 —— 数据库污染，而且会进翻译约束。
// 这个判定不能靠"看起来像不像值"来蒙，必须有明确的是/否词表。
Answer interpret_answer(const std::string& raw);

// **选择题**的输入解释（仅 ConflictingSpellings 用）。
//
// 为什么不直接复用 interpret_answer：那条路径**刻意**把纯数字判成 Skip
// （真实事故：用户答了 `1`，于是库里多出一条 `term preview = 1 status=confirmed`）。
// 那个判定现在仍然是对的 —— 对"是/否 + 请给正确写法"这类问题，数字不是有意义的回答。
//
// 但"这儿有两种写法，哪个对？"这类问题里，**数字就是最自然的回答**。
// 两种语义不能共用一条解析，否则必然要牺牲一边：要么允许数字当值
// （重新引入那起事故），要么用户没法选（只能把那个词重打一遍）。
struct ChoiceAnswer {
    Answer answer;             // 非选择题语义时的常规解释
    int    choice   = -1;      // >=1 表示选了第几个选项（1-based）
    bool   not_same = false;   // 用户说"不是同一个东西"（n / 不是）
};
ChoiceAnswer interpret_choice(const std::string& raw, size_t n_options);

// 面向用户的提示：告诉用户可以怎么答。跟着每条问题打出来。
std::string answer_hint_for(GapRule rule);

// ---------------------------------------------------------------
// 把回答落到库里
// ---------------------------------------------------------------
enum class ConfirmOutcome {
    Skipped,              // 用户跳过 / 输入无意义
    ConfirmedExisting,    // 认可当前值
    ConfirmedNewValue,    // 采纳用户给的新写法
    RevertedToOld,        // 否掉当前值，回到旧值
    RejectedUnchanged,    // 说了"不对"但没给正确写法 —— 库里不变，保持 candidate
    Failed,               // 落库失败（err 里有原因）
};

struct ConfirmResult {
    ConfirmOutcome outcome = ConfirmOutcome::Skipped;
    std::string    detail;        // 给用户看的一句话，已经在库里改过了
};

// 落库。失败不抛异常，走 err。
ConfirmResult apply_answer(const GapQuestion& q, const Answer& a, std::string* err = nullptr);

// ---------------------------------------------------------------
// 问答循环
// ---------------------------------------------------------------
struct ConfirmStats {
    int asked     = 0;   // 实际问出去的条数
    int confirmed = 0;   // 用户给出肯定/新值/否定的条数
    int skipped   = 0;
    int failed    = 0;
};

// `read_line`：读一行。返回 false = 输入结束（EOF / 用户按了 Ctrl+C），循环立即收尾。
using LineReader = std::function<bool(std::string&)>;

ConfirmStats run_confirmation(const std::vector<GapQuestion>& questions,
                              const LineReader& read_line,
                              std::ostream& out,
                              size_t max_questions = kMaxQuestionsDefault);

}  // namespace knowledge
