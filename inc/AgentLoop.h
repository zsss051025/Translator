#pragma once
#include <functional>
#include <ostream>
#include <string>
#include <vector>

#include "AgentTool.h"

// ===========================================================================
// Agent 规划循环 —— 见 PROJECT.md §7 第 5 阶段（步骤 5.2）
// ===========================================================================
//
// 这一层的职责只有一件：**让模型自己决定调哪个工具、调几次、什么时候收手**。
// 工具层（5.1）已经就绪，这里补的是"自主性"。
//
// 赛题赛道维度的原话是「工具调用……是区别于『问答机器人』的关键」——
// 所以这一层不是锦上添花，它是**定性**所在。
//
// ---------------------------------------------------------------------------
// 四条设计取向（每一条都有代价，别随手改）
// ---------------------------------------------------------------------------
//
// 1. **"调 LLM"做成可注入回调**（和确认交互的 `LineReader`、分诊的 `Judge` 同一招）。
//    规划是联网 + 不确定的，直接写进循环就**永远进不了 L1**：自检跑不了、
//    行为随模型变、回归无从谈起。抽成回调之后，自检喂**脚本化的工具调用序列**，
//    走真实的循环逻辑（预算、审计、失败处理全都能验），云端实现只是另一个回调。
//
// 2. **超预算要返回"已完成的部分 + 为什么停"，不是报错。**
//    用户看到"我已经查到 3 场会话，但步数用完了没来得及汇总"是有用的；
//    看到"Error: budget exceeded"是没用的。**半成品加一句实话，好过一个错误码。**
//
// 3. **失败模式必须是"说清楚我做不到"，绝不能编一份报告。**
//    这直接决定赛题"结果交付与可验收性"是加分还是负分。
//    ⚠️ 诚实标注：这一条**靠 prompt 约束 + 审计留痕**，不是机械保证 ——
//    我们没法在代码里判断"模型这段话是不是编的"。能机械保证的是两件事：
//    ① 全程没调过任何工具就直接给结论 → 标记出来；② 每一步都留痕可复查。
//
// 4. **单个工具失败不该让任务崩掉**：错误作为**观察**回给模型，让它有机会换个办法。
//    循环只在"判断器本身坏了"或"预算耗尽"时收手。
//
// ---------------------------------------------------------------------------
// 与知识库红线的边界（§6.5）
// ---------------------------------------------------------------------------
// 规划循环可以调用**只读**工具任意次。写操作（5.4）只允许"**提出候选**"，
// **永远不能自己确认** —— 因为 confirmed 只能由用户点头产生，那是红线的本体。
// 这一层不自己实现工具，所以边界由工具层保证；这里只负责不越权调用。

namespace agent {

// 模型要求的一次工具调用
struct ToolCall {
    std::string id;
    std::string name;
    std::string args_json;
};

// 一条消息（OpenAI / DeepSeek function calling 的形状）
//
// ⚠️ `calls` 是**必需**的，不是可有可无的便利字段：
// 协议要求"要求了工具调用的 assistant 消息"必须**自己带上** tool_calls 数组，
// 否则服务端不认后面那些 tool 消息。
//
// 【我第一版的做法是错的，记在这里】我为了不让这个结构"承担两件事"，
// 故意不带 `calls`，改成在发请求时**从随后那段 role=tool 的消息反推**。
// 那是有损的：反推出来的 `arguments` 只能是 `{}`，模型看到自己
// "用空参数调过工具"，后续轮次可能因此做出错误判断。
// **协议要什么就存什么** —— 想省字段的代价是丢信息，而丢信息在 agent 里会放大。
struct Message {
    std::string           role;           // system | user | assistant | tool
    std::string           content;        // 自然语言内容
    std::vector<ToolCall> calls;          // role=assistant 时：它要求了哪些调用
    std::string           tool_call_id;   // role=tool 时：回应哪一次调用
    std::string           tool_name;      // role=tool 时：工具名（便于审计与排错）
};

// 模型的一步输出：要么说话，要么调工具（也可以两者都有）
struct Step {
    std::string           content;
    std::vector<ToolCall> calls;
};

// 可注入的"规划器"。
// 返回 false = 这次调用没成功（网络 / 超时 / 解析失败）→ 循环按失败处理。
using Planner = std::function<bool(const std::vector<Message>& history,
                                   const std::string& tools_json,
                                   Step* out,
                                   std::string* err)>;

// 预算。**三个都要有**：步数防死循环，字符数防上下文爆炸（token 的代理指标），
// 墙钟时间防模型卡住 —— 只设一个都会漏掉一种失控方式。
struct Budget {
    int    max_steps   = 8;       // 最多几次"模型回合"
    size_t max_chars   = 60000;   // 观察累积上限
    int    max_seconds = 90;      // 墙钟
};

// 逐步留痕 —— `--audit` 打的就是它。**这是评审判断"它真的在做事"的唯一依据。**
struct AuditLine {
    int         step = 0;
    std::string action;      // 人看的一句话，如「调用 list_sessions({"days":7})」
    std::string observation; // 人看的一句话，如「3 场会话」
    bool        tool_ok = true;
};

struct Result {
    bool        ok = false;         // 循环本身跑完了（不代表答案一定对）
    std::string answer;             // 给用户的最终回答
    bool        gave_up = false;    // 模型明确说了"我做不到"
    bool        no_tools_used = false;  // 全程没调过工具就给结论 ← 高度可疑，要标出来
    bool        partial = false;    // 预算耗尽，只有半成品
    size_t      steps_used = 0;
    size_t      tool_calls = 0;
    std::string stop_reason;        // 为什么停（给人看的）
    std::vector<AuditLine> audit;
    std::vector<Message>   history; // 完整对话（排查用）
};

// **核心纯逻辑**：跑循环。不碰数据库、不碰网络（一切经由 tools 与 planner）。
//
// `audit_out` 非空时逐步打印 —— 真实运行时传 std::cout，自检传 stringstream。
Result run(const std::string& goal,
           const ToolRegistry& tools,
           const ToolContext&  ctx,
           const Planner&      planner,
           const Budget&       budget = Budget(),
           std::ostream*       audit_out = nullptr);

// ---------------------------------------------------------------------------
// 云端规划器（DeepSeek function calling）
// ---------------------------------------------------------------------------
//
// ⚠️ **只有云端能做**：本地混元 HY-MT1.5-1.8B 是**翻译模型**，
// 既不支持 tools 接口、也不会按 JSON 发起调用（实测它做摘要会原样回显转录）。
// 所以 agent 这条链路上云是**必然**的 —— 演示时要能说清"离线时退化成什么"
// （答案：退化成单次检索，即 5.3 的 `--ask`，没有多步规划）。
//
// 没配 Key 时返回**空** planner（和分诊层的 `make_cloud_judge` 一致）：
// 调用方据此降级，而不是拿到一个"永远失败"的规划器。
Planner make_deepseek_planner(const std::string& api_key, int timeout_sec = 45);

}  // namespace agent
