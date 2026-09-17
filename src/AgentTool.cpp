#include "AgentTool.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "KnowledgeStore.h"
#include "SessionStore.h"
#include "Utf8.h"
#include "json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace agent {

// ===============================================================
// 工具结果的长度上限
//
// 【为什么每个工具都要有】工具结果直接进模型上下文。不设上限的两个后果：
//   ① 长会议（几百段）一次就撑爆上下文，后面的规划全废
//   ② 烧 token —— 而这会按调用次数放大（多步循环每步都要带上历史）
// 所以**每个工具自己封顶**，而不是指望循环层统一裁。
// ===============================================================
namespace {

constexpr int    kSearchKnowledgeMax = 20;      // 条
constexpr int    kHistoryMaxChanges  = 20;      // 条
constexpr int    kListSessionsMax    = 50;      // 场
constexpr int    kListSessionsDefault = 10;
constexpr int    kGetSessionMaxSegs  = 60;      // 段
constexpr int    kGetSessionDefaultSegs = 30;
constexpr size_t kSegmentChars       = 400;     // 单段原文+译文的上限
constexpr size_t kReportChars        = 6000;    // 整份纪要的上限
constexpr size_t kResultChars        = 12000;   // 任何工具结果的硬上限（最后一道闸）

// 当前本地时间，格式与 SessionStore::now_string() 一致：
// "YYYY-MM-DD HH:MM:SS.mmm"。**这个格式按字典序比较 = 按时间比较**，
// 所以"过去 N 天"不需要解析时间，直接比字符串即可。
std::string time_string_days_ago(int days) {
    auto tp = std::chrono::system_clock::now() - std::chrono::hours(24 * std::max(0, days));
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S.000", &tm_buf);
    return buf;
}

// 从参数里取整数，缺省时用 fallback，并夹到 [lo, hi]
int int_arg(const json& args, const char* key, int fallback, int lo, int hi) {
    int v = fallback;
    if (args.contains(key) && args[key].is_number()) v = args[key].get<int>();
    else if (args.contains(key) && args[key].is_string()) {
        try { v = std::stoi(args[key].get<std::string>()); } catch (...) {}
    }
    return std::max(lo, std::min(hi, v));
}

std::string str_arg(const json& args, const char* key) {
    if (!args.contains(key)) return {};
    if (args[key].is_string()) return args[key].get<std::string>();
    return {};
}

ToolResult fail(std::string why) {
    ToolResult r;
    r.ok    = false;
    r.error = std::move(why);
    r.content = json{{"error", r.error}}.dump();
    return r;
}

// 统一收口：任何工具结果都要过这一道（纵长、UTF-8 合法）
ToolResult finish(ToolResult r) {
    r.content = utf8::truncate(r.content, kResultChars);
    return r;
}

std::string json_dump(const json& j) {
    // 缩进 0：省 token，模型照样能读
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

}  // namespace

// ===============================================================
// ToolRegistry
// ===============================================================

bool ToolRegistry::add(Tool t, std::string* err) {
    auto say = [&](const std::string& m) { if (err) *err = m; };
    if (t.name.empty()) { say("工具名为空"); return false; }
    if (!t.run)         { say("工具 " + t.name + " 没有实现"); return false; }
    if (find(t.name) != nullptr) {
        say("工具重名：" + t.name);
        return false;
    }
    tools_.push_back(std::move(t));
    return true;
}

const Tool* ToolRegistry::find(const std::string& name) const {
    for (const auto& t : tools_) if (t.name == name) return &t;
    return nullptr;
}

std::vector<std::string> ToolRegistry::names() const {
    std::vector<std::string> out;
    out.reserve(tools_.size());
    for (const auto& t : tools_) out.push_back(t.name);
    return out;
}

std::string ToolRegistry::tools_json() const {
    json arr = json::array();
    for (const auto& t : tools_) {
        json params;
        try {
            params = json::parse(t.params_schema);
        } catch (...) {
            // schema 写坏了不能静默 —— 否则模型收到一个残缺的工具定义，
            // 症状是"它就是不调用这个工具"，极难排查
            std::cerr << "[Agent] 工具 " << t.name << " 的 params_schema 不是合法 JSON，"
                         "已降级为空对象 —— 请修 schema" << std::endl;
            params = json{{"type", "object"}, {"properties", json::object()}};
        }
        arr.push_back({
            {"type", "function"},
            {"function", {
                {"name", t.name},
                {"description", t.description},
                {"parameters", params},
            }},
        });
    }
    return arr.dump(-1, ' ', false, json::error_handler_t::replace);
}

ToolResult ToolRegistry::call(const std::string& name, const std::string& args_json,
                              const ToolContext& ctx) const {
    const Tool* t = find(name);
    if (t == nullptr) {
        // 把可用工具名回给模型 —— 它经常是一开始猜错名字，看到列表就会改
        ToolResult r = fail("没有叫「" + name + "」的工具");
        json j = json::parse(r.content);
        j["available_tools"] = names();
        r.content = json_dump(j);
        return r;
    }
    // 参数坏（模型偶尔吐半截 JSON）→ 当错误回给它，别抛
    if (!args_json.empty()) {
        try {
            (void)json::parse(args_json);
        } catch (const std::exception& e) {
            return fail(std::string("参数不是合法 JSON：") + e.what());
        }
    }
    std::string err;
    ToolResult r = t->run(args_json.empty() ? "{}" : args_json, ctx, &err);
    if (!r.ok && r.error.empty()) r.error = err.empty() ? "工具执行失败" : err;
    return finish(std::move(r));
}

// ===============================================================
// 只读工具
// ===============================================================

namespace {

// ---- search_knowledge ----
ToolResult tool_search_knowledge(const std::string& args_json, const ToolContext&, std::string* err) {
    const json args = json::parse(args_json);
    const std::string q = str_arg(args, "query");
    if (q.empty()) return fail("缺少参数 query");

    const int limit = int_arg(args, "limit", 10, 1, kSearchKnowledgeMax);

    std::string serr;
    const auto hits = KnowledgeStore::instance().search(q, limit, &serr);
    if (!serr.empty()) { if (err) *err = serr; return fail(serr); }

    json arr = json::array();
    for (const auto& k : hits) {
        arr.push_back({
            {"kind", k.kind},
            {"key", k.key},
            {"value", k.value},
            {"status", k.status},          // confirmed / candidate —— agent 必须能区分
            {"hits", k.hits},
            {"confidence", k.confidence},
            {"first_seen_at", k.first_seen_at},
            // 证据：让 agent 能说清"你凭什么这么说"
            {"evidence", utf8::truncate(k.source_text, 200)},
            {"source_session", k.source_session},
        });
    }
    ToolResult r;
    r.ok      = true;
    r.content = json_dump(json{{"query", q}, {"count", hits.size()}, {"items", arr}});
    r.audit   = "命中 " + std::to_string(hits.size()) + " 条知识";
    return r;
}

// ---- knowledge_history ----
ToolResult tool_knowledge_history(const std::string& args_json, const ToolContext&, std::string* err) {
    const json args = json::parse(args_json);
    const std::string key = str_arg(args, "key");
    if (key.empty()) return fail("缺少参数 key");

    auto& ks = KnowledgeStore::instance();
    // 先按 key/value 检索定位条目 —— 调用方（模型）不知道 kind，让它传 kind 是没必要的负担
    std::string serr;
    auto hits = ks.search(key, 5, &serr);
    if (!serr.empty()) { if (err) *err = serr; return fail(serr); }

    // 优先精确命中归一化键
    const std::string want = knowledge::normalize_key(key);
    const KnowledgeItem* best = nullptr;
    for (const auto& k : hits) if (k.key == want) { best = &k; break; }
    if (best == nullptr && !hits.empty()) best = &hits.front();
    if (best == nullptr) {
        return fail("库里没有「" + key + "」这条知识（可以先用 search_knowledge 找）");
    }

    json changes = json::array();
    int n = 0;
    for (const auto& h : ks.history(best->id)) {
        if (n++ >= kHistoryMaxChanges) break;
        changes.push_back({
            {"old_value", h.old_value},
            {"new_value", h.new_value},
            {"changed_at", h.changed_at},
            {"reason", h.reason},     // value_changed_demoted / user_confirmed / user_edited …
        });
    }
    ToolResult r;
    r.ok = true;
    r.content = json_dump(json{
        {"key", best->key}, {"value", best->value}, {"status", best->status},
        {"first_seen_at", best->first_seen_at}, {"change_count", changes.size()},
        {"changes", changes},
    });
    r.audit = std::to_string(changes.size()) + " 次变化";
    return r;
}

// ---- list_sessions ----
ToolResult tool_list_sessions(const std::string& args_json, const ToolContext& ctx, std::string*) {
    const json args = json::parse(args_json);
    const int days  = int_arg(args, "days", 0, 0, 3650);          // 0 = 不限时间
    const int limit = int_arg(args, "limit", kListSessionsDefault, 1, kListSessionsMax);

    // 取足够多再按时间筛（库规模是几十到几百场，一次全取没有性能问题）
    const auto all = SessionStore::instance().list_sessions(500);
    const std::string cutoff = (days > 0) ? time_string_days_ago(days) : std::string();

    json arr = json::array();
    for (const auto& s : all) {
        if (arr.size() >= static_cast<size_t>(limit)) break;
        // time 格式 "YYYY-MM-DD HH:MM:SS.mmm" 按字典序比较 = 按时间比较
        if (!cutoff.empty() && s.started_at < cutoff) continue;

        // 有没有已经生成过的纪要 —— agent 靠这个字段决定"读现成的"还是"只能看转录"
        const fs::path md = fs::path(ctx.deliverable_root) /
                            ("session-" + std::to_string(s.id)) /
                            ("meeting-" + std::to_string(s.id) + ".md");
        std::error_code ec;
        const bool has_report = fs::exists(md, ec);

        arr.push_back({
            {"session_id", s.id},
            {"started_at", s.started_at},
            {"ended_at", s.ended_at},
            {"engine", s.engine},
            {"note", s.note},
            {"segments", s.segment_count},
            {"has_report", has_report},
        });
    }
    ToolResult r;
    r.ok = true;
    r.content = json_dump(json{
        {"days", days}, {"count", arr.size()},
        {"range", cutoff.empty() ? "全部" : ("自 " + cutoff + " 起")},
        {"sessions", arr},
    });
    r.audit = std::to_string(arr.size()) + " 场会话"
              + (cutoff.empty() ? "" : "（过去 " + std::to_string(days) + " 天）");
    return r;
}

// ---- get_session ----
ToolResult tool_get_session(const std::string& args_json, const ToolContext&, std::string*) {
    const json args = json::parse(args_json);
    if (!args.contains("session_id")) return fail("缺少参数 session_id");
    const long long sid = args["session_id"].is_number()
                              ? args["session_id"].get<long long>()
                              : std::stoll(args["session_id"].get<std::string>());
    const int max_segs = int_arg(args, "max_segments", kGetSessionDefaultSegs,
                                 1, kGetSessionMaxSegs);

    const auto segs = SessionStore::instance().fetch_segments(sid);
    if (segs.empty()) return fail("会话 #" + std::to_string(sid) + " 没有段落（或不存在）");

    json arr = json::array();
    for (const auto& s : segs) {
        if (arr.size() >= static_cast<size_t>(max_segs)) break;
        arr.push_back({
            {"seq", s.seq},
            {"src", utf8::truncate(s.src_text, kSegmentChars)},
            {"tgt", utf8::truncate(s.tgt_text, kSegmentChars)},
            {"confidence", s.confidence},
        });
    }
    ToolResult r;
    r.ok = true;
    r.content = json_dump(json{
        {"session_id", sid},
        {"total_segments", segs.size()},
        {"shown", arr.size()},
        {"truncated", segs.size() > arr.size()},
        {"segments", arr},
    });
    r.audit = std::to_string(arr.size()) + "/" + std::to_string(segs.size()) + " 段转录";
    return r;
}

// ---- get_session_report ----
ToolResult tool_get_session_report(const std::string& args_json, const ToolContext& ctx, std::string*) {
    const json args = json::parse(args_json);
    if (!args.contains("session_id")) return fail("缺少参数 session_id");
    const long long sid = args["session_id"].is_number()
                              ? args["session_id"].get<long long>()
                              : std::stoll(args["session_id"].get<std::string>());

    const fs::path md = fs::path(ctx.deliverable_root) /
                        ("session-" + std::to_string(sid)) /
                        ("meeting-" + std::to_string(sid) + ".md");
    std::error_code ec;
    if (!fs::exists(md, ec)) {
        // 说清楚"没有"，并给出替代路径 —— 模型据此会改用 get_session
        return fail("会话 #" + std::to_string(sid) + " 还没有生成过纪要（"
                    "可以用 get_session 读转录，或换一场有纪要的）");
    }
    std::ifstream f(md, std::ios::binary);
    if (!f) return fail("纪要文件打不开：" + md.string());
    std::ostringstream ss;
    ss << f.rdbuf();

    ToolResult r;
    r.ok = true;
    r.content = json_dump(json{
        {"session_id", sid},
        {"path", md.string()},
        {"markdown", utf8::truncate(ss.str(), kReportChars)},
    });
    r.audit = "读到纪要（" + std::to_string(ss.str().size()) + " 字节）";
    return r;
}

}  // namespace

void register_readonly_tools(ToolRegistry& reg) {
    std::string err;
    auto add = [&](Tool t) {
        if (!reg.add(std::move(t), &err)) {
            // 注册失败说明代码写错了（重名/空实现），必须立刻可见
            std::cerr << "[Agent] 注册工具失败：" << err << std::endl;
        }
    };

    add({
        "search_knowledge",
        "检索长期记忆里已记录的人名、术语、事实、决定。"
        "当你需要确认“某个名字/说法该怎么写”“某件事是什么”时用它。"
        "返回每条知识的状态（confirmed=用户确认过，candidate=模型猜的还没确认）、"
        "出现次数、首次出现时间和原话证据。"
        "**candidate 不能当成事实使用**，只能作为线索。",
        R"({"type":"object","properties":{
            "query":{"type":"string","description":"要查的词或短语，支持中英文"},
            "limit":{"type":"integer","description":"最多返回几条，默认 10，最大 20"}
        },"required":["query"]})",
        tool_search_knowledge,
    });

    add({
        "knowledge_history",
        "查看某条知识的历史变化：它以前是什么值、什么时候改的、为什么改。"
        "当用户问“这个之前叫什么”“什么时候改的”时用它。",
        R"({"type":"object","properties":{
            "key":{"type":"string","description":"知识条目的键或值，如 erica 或 Erica"}
        },"required":["key"]})",
        tool_knowledge_history,
    });

    add({
        "list_sessions",
        "列出最近的会话（每场一次录音/一次使用）。"
        "**这是所有跨会话任务的起点**：先看有哪些会话，再用 get_session / get_session_report 读内容。"
        "返回每场的时间、时长线索、段数，以及 has_report（是否已经生成过纪要）。",
        R"({"type":"object","properties":{
            "days":{"type":"integer","description":"只看最近 N 天；0 或不填表示不限时间"},
            "limit":{"type":"integer","description":"最多返回几场，默认 10，最大 50"}
        }})",
        tool_list_sessions,
    });

    add({
        "get_session",
        "读某场会话的转录（原文 + 译文逐段）。"
        "当需要**细节**时用它：谁说了什么、具体数字、有没有提到某件事。"
        "结果会限长（默认 30 段），太长时用 max_segments 调整或换策略。",
        R"({"type":"object","properties":{
            "session_id":{"type":"integer","description":"会话编号，从 list_sessions 得到"},
            "max_segments":{"type":"integer","description":"最多读几段，默认 30，最大 60"}
        },"required":["session_id"]})",
        tool_get_session,
    });

    add({
        "get_session_report",
        "读某场会话**已经生成过的**纪要（markdown，含概述/要点/行动项/双语全文）。"
        "**整理历史工作时应优先用它**：又快又准，不用重新总结。"
        "若返回“还没有生成过纪要”，改用 get_session 读转录。",
        R"({"type":"object","properties":{
            "session_id":{"type":"integer","description":"会话编号，从 list_sessions 得到"}
        },"required":["session_id"]})",
        tool_get_session_report,
    });
}

}  // namespace agent
