#include "AgentLoop.h"

#include <chrono>
#include <iostream>

#include "CaBundle.h"
#include "httplib.h"      // HTTPS 支持由 CMake 的 target_compile_definitions 提供
#include "json.hpp"

namespace agent {

namespace {

using nlohmann::json;

// 系统提示。**四件事必须说清楚，缺一个模型就会答歪**（和分诊层的 prompt 同一个道理）：
//   ① 它的任务是"用工具查证"，不是"凭印象回答"
//   ② 资料不够时**必须说做不到**，而且要说出缺什么 —— 这是赛题"可验收性"的落点
//   ③ 结论要**带出处**（哪场会话、哪条知识），否则用户无法核对
//   ④ 不要把工具结果原文照抄给用户，要归纳
const char* kSystemPrompt =
    u8"你是一个会议助手。用户会给你一个任务，你要**用工具去查证**，然后给出结论。\n"
    u8"\n"
    u8"铁律：\n"
    u8"1. **先查再答。** 关于用户的会议、项目、人名、术语的问题，"
    u8"一律先用工具查，不要凭印象回答。\n"
    u8"2. **查不到就说查不到。** 如果工具返回的结果不足以支撑结论，"
    u8"必须明确说\"我查不到\"或\"资料不足\"，并说清**缺什么**"
    u8"（比如\"最近一周没有相关会话\"）。\n"
    u8"   ⚠️ **绝对不要编造**任何会议内容、人名、日期、数字。"
    u8"编一份看起来完整的报告是最严重的错误 —— 用户会当真。\n"
    u8"3. **结论要带出处。** 引用具体是哪一场会话（会话号/时间）或哪一条知识。\n"
    u8"4. **不要原文照抄工具返回的一大段文本**，要归纳成给用户看的东西。\n"
    u8"\n"
    u8"工具调用要节制：够用就停，同一个查询不要重复调。";

// 把一次工具调用渲染成"人看的一句话"（进 --audit）
std::string render_call(const ToolCall& c) {
    std::string args = c.args_json;
    if (args.size() > 80) args = args.substr(0, 80) + u8"…";
    return u8"调用 " + c.name + (args.empty() || args == "{}" ? "" : "(" + args + ")");
}

// 从 DeepSeek 的响应里取 step
bool parse_step(const std::string& body, Step* out, std::string* err) {
    try {
        const auto j = json::parse(body);
        if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty()) {
            if (err) *err = u8"响应里没有 choices";
            return false;
        }
        const auto& msg = j["choices"][0]["message"];
        if (msg.contains("content") && !msg["content"].is_null()) {
            out->content = msg["content"].get<std::string>();
        }
        if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
            for (const auto& tc : msg["tool_calls"]) {
                ToolCall c;
                c.id   = tc.value("id", std::string());
                c.name = tc.value("function", json::object()).value("name", std::string());
                // arguments 是**字符串**（里面再是 JSON）—— OpenAPI 的约定，
                // 不是嵌套对象。踩过一次就会取到空。
                c.args_json = tc.value("function", json::object())
                                .value("arguments", std::string("{}"));
                if (!c.name.empty()) out->calls.push_back(std::move(c));
            }
        }
        return true;
    } catch (const std::exception& e) {
        if (err) *err = std::string(u8"解析响应失败: ") + e.what();
        return false;
    }
}

}  // namespace

Result run(const std::string& goal,
           const ToolRegistry& tools,
           const ToolContext&  ctx,
           const Planner&      planner,
           const Budget&       budget,
           std::ostream*       audit_out) {
    Result r;
    const auto t0 = std::chrono::steady_clock::now();

    if (!planner) {
        // 没有规划器（通常 = 没配 Key）。**这不是崩溃，是能力降级** ——
        // 说清楚，并指出**真的能用的**退路。
        //
        // ⚠️ 自检抓到过我一版写错：我原来提示"配置 Key 之后可以用 `--report` 派活"——
        // 而**那正是需要 Key 的命令**，等于告诉用户"你去用那个用不了的东西"。
        // 用户此刻需要的是一条**现在就能跑**的命令。
        r.stop_reason = u8"没有可用的规划器（未配置云端 API Key）";
        r.gave_up     = true;
        r.answer      = u8"我现在只能做单次检索，做不了多步查证。\n"
                        u8"现在就能用的是：`--search \"<关键词>\"`（不联网、不需要 Key）。\n"
                        u8"想要多步查证（派活），需要配置 DeepSeek API Key。";
        return r;
    }
    if (tools.empty()) {
        r.stop_reason = u8"没有任何可用的工具";
        r.gave_up     = true;
        r.answer      = u8"我没有任何可用的工具，查不了。";
        return r;
    }

    const std::string tools_json = tools.tools_json();

    r.history.push_back(Message{"system", kSystemPrompt, {}, "", ""});
    r.history.push_back(Message{"user", goal, {}, "", ""});

    size_t observed_chars = 0;

    for (int step = 1; step <= budget.max_steps; ++step) {
        // ---- 预算：墙钟 ----
        {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::steady_clock::now() - t0).count();
            if (elapsed > budget.max_seconds) {
                r.partial     = true;
                r.stop_reason = u8"超过时间预算（" + std::to_string(budget.max_seconds) +
                                u8" 秒）";
                break;
            }
        }
        // ---- 预算：上下文长度 ----
        if (observed_chars > budget.max_chars) {
            r.partial     = true;
            r.stop_reason = u8"查到的资料太多，超过上下文预算";
            break;
        }

        Step s;
        std::string perr;
        if (!planner(r.history, tools_json, &s, &perr)) {
            // **判断器坏了**：这是唯一该硬停的情况。但也要说人话，不要吐错误码。
            r.stop_reason = u8"调用大模型失败：" + (perr.empty() ? u8"未知原因" : perr);
            if (!r.answer.empty()) r.answer += "\n\n";
            r.answer += u8"（我在查证过程中断线了，上面是我已经查到的部分。）";
            r.partial = true;
            break;
        }

        r.steps_used = static_cast<size_t>(step);

        // ---- 模型不再要求调工具 → 这一轮就是最终回答 ----
        if (s.calls.empty()) {
            r.answer = s.content;
            r.ok     = true;
            // **全程没调过工具就给结论** → 高度可疑，明确标出来。
            // 这是"不能编"那条铁律唯一的机械抓手：编没编我们判不了，
            // 但"一次都没查就下结论"是可判的。
            if (r.tool_calls == 0) {
                r.no_tools_used = true;
                r.stop_reason   = u8"直接给了结论，全程没有调用任何工具";
            } else {
                r.stop_reason = u8"模型认为已经查够了";
            }
            if (audit_out && !r.answer.empty()) {
                *audit_out << u8"  ◆ 结论：" << r.answer.substr(0, 200)
                           << (r.answer.size() > 200 ? u8"…" : "") << std::endl;
            }
            break;
        }

        // ---- 执行工具调用 ----
        //
        // 把 assistant 这一轮（**含它要求的调用**）先记进历史 —— 少了这条，
        // 下一轮模型看不到自己刚才要求过什么，会**反复调同一个工具**。
        // `calls` 必须原样带上（协议要求，而且反推是有损的，见 Message 的说明）。
        {
            Message am;
            am.role    = "assistant";
            am.content = s.content;
            am.calls   = s.calls;
            r.history.push_back(std::move(am));
        }

        for (const auto& c : s.calls) {
            const ToolResult tr = tools.call(c.name, c.args_json, ctx);
            ++r.tool_calls;

            AuditLine line;
            line.step       = step;
            line.tool_ok    = tr.ok;
            line.action     = render_call(c);
            line.observation = tr.ok ? tr.audit
                                     : (u8"失败：" + (tr.error.empty() ? u8"未知" : tr.error));

            if (audit_out) {
                *audit_out << u8"  → " << line.action << std::endl
                           << u8"  ← " << line.observation << std::endl;
            }
            r.audit.push_back(std::move(line));

            // 工具结果回给模型。**失败也回**（附上原因）——
            // 让模型有机会换个工具或换个参数，而不是整个任务崩掉。
            std::string payload = tr.ok ? tr.content
                                        : (u8"{\"error\":\"" + tr.error + u8"\"}");
            observed_chars += payload.size();
            Message tm;
            tm.role         = "tool";
            tm.tool_call_id = c.id;
            tm.tool_name    = c.name;
            tm.content      = std::move(payload);
            r.history.push_back(std::move(tm));
        }
    }

    if (r.stop_reason.empty()) {
        r.partial     = true;
        r.stop_reason = u8"步数用完了（" + std::to_string(budget.max_steps) + u8" 步）";
    }

    // 预算耗尽 / 断线时，**把已经查到的部分交出去 + 说清为什么停**。
    // 用户看到"半成品加一句实话"是有用的；看到"Error: budget exceeded"是没用的。
    if (r.partial) {
        if (r.answer.empty()) {
            r.answer = u8"我没能在预算内完成这个任务。";
        }
        r.answer += u8"\n\n（说明：这次" + r.stop_reason + u8"，"
                    u8"上面是我已经完成的 " + std::to_string(r.tool_calls) +
                    u8" 次查询的结果，可能不完整。）";
    }
    return r;
}

Planner make_deepseek_planner(const std::string& api_key, int timeout_sec) {
    if (api_key.empty()) return nullptr;   // 和分诊层一致：没 Key 就没有规划器

    return [api_key, timeout_sec](const std::vector<Message>& history,
                                  const std::string& tools_json,
                                  Step* out,
                                  std::string* err) -> bool {
        if (!out) return false;

        // 拼 OpenAI / DeepSeek 兼容的 messages。**协议要什么就发什么**：
        // 要求了工具调用的 assistant 消息必须带上它自己的 tool_calls 数组
        // （含原始 arguments）—— 反推是有损的，见 Message 的说明。
        json msgs = json::array();
        for (const auto& m : history) {
            if (m.role == "tool") {
                msgs.push_back({{"role", "tool"},
                                {"tool_call_id", m.tool_call_id},
                                {"content", m.content}});
                continue;
            }
            if (m.role == "assistant") {
                json am = {{"role", "assistant"}};
                // content 与 tool_calls 至少有一个非空。只有 tool_calls 时
                // content 要显式给 null（给空串某些实现会被当成"结束"）。
                am["content"] = m.content.empty() ? json(nullptr) : json(m.content);
                if (!m.calls.empty()) {
                    json tcs = json::array();
                    for (const auto& c : m.calls) {
                        tcs.push_back({{"id", c.id},
                                       {"type", "function"},
                                       {"function", {{"name", c.name},
                                                     {"arguments", c.args_json}}}});
                    }
                    am["tool_calls"] = std::move(tcs);
                }
                msgs.push_back(std::move(am));
                continue;
            }
            msgs.push_back({{"role", m.role}, {"content", m.content}});
        }

        json payload = {
            {"model", "deepseek-chat"},
            {"messages", msgs},
            {"tools", json::parse(tools_json)},
            {"temperature", 0.2}
        };

        httplib::Client cli("https://api.deepseek.com");
        cli.set_connection_timeout(5, 0);
        cli.set_read_timeout(timeout_sec, 0);
        const std::string ca = find_ca_bundle();
        if (!ca.empty()) cli.set_ca_cert_path(ca);

        httplib::Headers headers = {
            {"Authorization", "Bearer " + api_key},
            {"Content-Type", "application/json"}
        };
        auto res = cli.Post("/chat/completions", headers,
                            payload.dump(-1, ' ', false, json::error_handler_t::replace),
                            "application/json");
        if (!res) {
            if (err) *err = std::string(u8"网络请求失败: ") + httplib::to_string(res.error());
            return false;
        }
        if (res->status != 200) {
            if (err) *err = u8"HTTP " + std::to_string(res->status) +
                            u8"：" + res->body.substr(0, 200);
            return false;
        }
        return parse_step(res->body, out, err);
    };
}

}  // namespace agent
