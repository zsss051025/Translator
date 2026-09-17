#pragma once
#include <functional>
#include <string>
#include <vector>

// Agent 工具层 —— 见 PROJECT.md §7 第 5 阶段（步骤 5.1）
//
// ---------------------------------------------------------------------------
// 为什么先做这一层，而不是直接写"能规划"的循环
// ---------------------------------------------------------------------------
// 规划循环本身不难（function calling + 循环 + 预算）。难的是**它有什么可调**。
// 没有工具，模型再会规划也只能干聊 —— 那就是个套壳聊天机器人，
// 而赛题的赛道维度明确说"工具调用是区别于问答机器人的关键"。
//
// 所以顺序是：**先把已有能力封成工具**（本文件），再写循环（5.2）。
//
// ---------------------------------------------------------------------------
// 设计取向
// ---------------------------------------------------------------------------
// 1. **工具结果必须限长**。它要进模型上下文，不设上限就是拿 token 烧钱，
//    而且长会话的转录能轻松撑爆上下文。每个工具有自己的 per-tool 上限。
// 2. **结果用 JSON**，不是自然语言。模型能精确读字段，也不容易误解；
//    同时另给一段 `audit`（人看的一行摘要）用于 `--audit` 逐步打印。
// 3. **5.1 只做只读工具**。写工具推迟到 5.4，而且知识库**永远不给写权限**
//    （见 §6.5 红线：只有 confirmed 知识能进约束，而 confirmed 只能由用户确认产生）。
// 4. **不注册"现在一定返回空"的工具**。`actions` 表目前从不写入（0 行），
//    现在注册 `list_actions` 只会让 Agent 看起来像坏了 —— 它跟 5.5 一起上。

namespace agent {

// 一次工具调用的结果
struct ToolResult {
    bool        ok = false;
    std::string content;   // 给模型看的（JSON 文本）
    std::string audit;     // 给人看的一行摘要，如 "3 场会话" / "命中 2 条知识"
    std::string error;     // ok=false 时的原因（会作为工具结果回给模型，不抛异常）
};

// 工具运行需要的环境。**刻意做成参数而不是全局**：
// 自检要能注入一个临时 deliverables 目录，不去碰真实产物。
struct ToolContext {
    std::string deliverable_root = "deliverables";
};

// 一个工具
struct Tool {
    std::string name;           // 稳定标识，模型用它发起调用
    std::string description;    // 给模型看的说明：**说清什么时候该用它**
    std::string params_schema;  // JSON Schema（只含 parameters 对象），供 function calling
    // args_json 是模型给的参数（JSON 文本）；错误走返回值，不抛
    std::function<ToolResult(const std::string& args_json, const ToolContext& ctx,
                             std::string* err)> run;
};

class ToolRegistry {
public:
    // 重名**必须拒绝**：模型按名字调用，重名会让行为取决于注册顺序（不可预测）
    bool add(Tool t, std::string* err = nullptr);

    const Tool* find(const std::string& name) const;

    size_t size() const { return tools_.size(); }
    bool   empty() const { return tools_.empty(); }

    // 按注册顺序，便于自检与 --tools 打印时结果稳定
    std::vector<std::string> names() const;

    // 拼成 OpenAI / DeepSeek function calling 的 tools 数组（JSON 文本）
    std::string tools_json() const;

    // 执行。未知工具 / 参数坏 / 工具内部错，一律返回 ok=false 并把原因写进 error
    // —— **绝不抛异常**：循环里一次工具失败不该让整个任务崩掉，模型应该有机会换个办法。
    ToolResult call(const std::string& name, const std::string& args_json,
                    const ToolContext& ctx) const;

private:
    std::vector<Tool> tools_;
};

// 把已有能力包成**只读**工具（5.1 的全部内容）
//
// 注册的 5 个：
//   search_knowledge     检索长期记忆（人名/术语/事实，含状态与出现次数）
//   knowledge_history    某条知识的变化历史（"这个说法什么时候变的"）
//   list_sessions        最近的会话列表（支持"过去 N 天"）
//   get_session          某场会话的转录（限长）
//   get_session_report   某场会话**已经生成过的**纪要（直接读交付物，不重新总结）
//
// 刻意**没有**的：任何写操作；`list_actions` / `get_action`（表里没数据，见文件头）；
// 以及"重新总结一场会话"（贵、慢，而且 90% 情况下已有交付物可读）。
void register_readonly_tools(ToolRegistry& reg);

}  // namespace agent
