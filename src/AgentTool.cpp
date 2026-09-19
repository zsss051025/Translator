#include "AgentTool.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "KnowledgeExtract.h"
#include "KnowledgeStore.h"
#include "ActionStore.h"        // list_actions / propose_action（5.5）
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
constexpr int    kListActionsMax     = 80;      // 行动项条数上限
constexpr size_t kProposeActionMaxTitle = 400;  // 一条行动项的标题上限（字节）

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

// ===============================================================
// 写工具：**提议**，不是写入
// ===============================================================
//
// 【这一段的全部意义在一句话上】agent 可以让库**多一条候选**，
// 但**没有任何办法**让它变成 confirmed。
//
// 做法上刻意绕开 `KnowledgeStore::upsert`（它接受 status 字段，
// 理论上能写 confirmed —— 那就等于把闸门开在代码里，靠"我们不会那么调"来守）。
// 这里只走 `save_candidates()`：那个函数写死 `status = candidate`，
// 而且它的入参结构 `ExtractedCandidate` **根本没有 status 字段** ——
// 也就是说，**这条路径连表达"confirmed"的能力都不存在**。
ToolResult tool_propose_knowledge(const std::string& args_json, const ToolContext&,
                                  std::string* err) {
    ToolResult r;
    const json a = json::parse(args_json);   // 和别的工具一样；坏参数由 ToolRegistry::call 兜

    const std::string value = a.value("value", std::string());
    if (value.empty()) { r.error = u8"缺少 value"; return r; }

    knowledge::ExtractedCandidate c;
    // kind 限定在**名字类**里：fact / decision 的值是整句话，
    // 让 agent 自己写一整句话进库风险大（它会进摘要背景影响后续所有纪要）。
    c.kind = a.value("kind", std::string("term"));
    if (!knowledge::is_name_like_kind(c.kind)) {
        r.error = u8"kind 只允许 person / project / product / term（实际：" + c.kind + u8"）";
        return r;
    }
    c.key          = knowledge::normalize_key(value);
    c.value        = value;
    c.hits         = 1;
    c.confidence   = 0.5;                    // 刻意压低：这是**提议**，不是识别出来的
    c.source_text  = u8"由 agent 在任务中提议：" + a.value("evidence", std::string());
    c.why          = a.value("reason", std::string("agent 提议"));

    std::string e;
    const int n = knowledge::save_candidates({c}, &e);
    if (n <= 0) {
        r.error = e.empty() ? u8"写入候选失败" : e;
        return r;
    }

    r.ok      = true;
    r.content = json{{"ok", true},
                     {"status", "candidate"},
                     {"value", value},
                     {"note", u8"已记为**候选**，未经用户确认，不会被用作识别提示或翻译约束"}}
                    .dump(-1, ' ', false, json::error_handler_t::replace);
    r.audit   = u8"提出候选「" + value + u8"」（待用户确认，不生效）";
    return r;
}

}  // namespace

// ---- list_actions：跨会话的待办总表（5.5）----
//
// 【为什么这个工具对"生成周报"是必需的】周报的核心内容之一就是
// "上周定的事这周怎么样了"。这件事的状态**只存在于这张表里** ——
// 转录里只有零散的句子，读 5 场转录去自己拼"哪些事一直挂着"既贵又不准。
static ToolResult tool_list_actions(const std::string& args_json,
                                    const ToolContext&, std::string* err) {
    (void)err;
    const json args = json::parse(args_json);

    const std::string status = str_arg(args, "status");
    if (!status.empty() &&
        std::find(actions::valid_statuses().begin(), actions::valid_statuses().end(),
                  status) == actions::valid_statuses().end()) {
        return fail("status 只能是 todo / doing / done");
    }
    const int limit = int_arg(args, "limit", 30, 1, kListActionsMax);

    const auto rows = actions::ActionStore::list(status, limit);
    if (rows.empty()) {
        // ⚠️ **空结果也必须是 JSON。**
        // 第一版这里返回的是一句中文散文 —— 于是跨进程回归立刻报
        // `list_actions({}) 结果不是合法 JSON`。而"工具结果必须是 JSON"
        // 不是格式洁癖：模型那边的解析器期待的是 JSON，散文会让它
        // 把工具结果当内容读，而这**只在"库是空的"这一天**发作 ——
        // 也就是最不容易被自己撞到的时刻。空列表 + 一句 note 才对。
        const auto st = actions::ActionStore::stats();
        const std::string why = st.total == 0
            ? u8"行动项表是空的。行动项是在**导出交付物**时自动落库的，"
              u8"所以如果还没有导出过任何一场会，这里就没有内容 —— "
              u8"这种情况下改用 get_session 读转录，从里面自己找待办。"
            : (u8"没有" + (status.empty() ? std::string(u8"任何") : status)
               + u8"状态的行动项（表里共 " + std::to_string(st.total) + u8" 条）。");
        ToolResult r;
        r.ok      = true;
        r.content = json_dump(json{{"count", 0}, {"items", json::array()},
                                   {"note", why}});
        r.audit   = u8"读行动项：0 条";
        return r;
    }

    json arr = json::array();
    for (const auto& it : rows) {
        arr.push_back({
            {"id", it.id},
            {"status", it.status},
            {"title", it.title},
            {"owner", it.owner},
            {"due", it.due},
            // 跨会话的痕迹 —— "这件事一直挂着"唯一有据可依的表述
            {"seen_sessions", it.seen_sessions},
            {"last_session", it.last_session},
            {"origin", it.origin},
            {"evidence", utf8::truncate(it.evidence, 200)},
        });
    }
    ToolResult r;
    r.ok      = true;
    r.content = json_dump(json{{"count", rows.size()}, {"items", arr},
                               {"note", u8"你只能读；改状态要用户自己做（--actions --done <id>）"}});
    r.audit   = u8"读行动项：" + std::to_string(rows.size()) + u8" 条";
    return r;
}

// ---- propose_action：提议一条行动项（5.5 的写工具）----
//
// 【为什么它和 propose_knowledge 的"闸"不一样】
// 知识走的是 candidate → 用户确认 → 才生效（§6.5 红线：模型猜的不能变成约束）。
// 行动项**没有那条红线** —— 它不进识别提示、不进翻译约束，
// 提议错了的代价只是"待办列表里多一条"，而那条**就摆在 `--actions` 里**
// 等用户自己删或标完成。所以它可以真写进去。
// 但它带 `origin='agent'` + confidence 0.5（压低），
// 于是"模型提议的"和"会上真说的"在列表里能分得清。
//
// ⚠️ **状态一律 todo**：agent 不能替用户宣称某件事做完了（见 ActionStore.h）。
static ToolResult tool_propose_action(const std::string& args_json,
                                      const ToolContext&, std::string* err) {
    (void)err;
    const json args = json::parse(args_json);

    const std::string title = str_arg(args, "title");
    if (title.empty()) return fail(u8"缺少参数 title");
    if (title.size() > kProposeActionMaxTitle) {
        return fail(u8"title 太长 —— 行动项应该是一句话，不是一整段");
    }

    actions::Incoming in;
    in.title    = title;
    in.owner    = str_arg(args, "owner");
    in.due      = str_arg(args, "due");
    in.evidence = str_arg(args, "evidence");

    // session_id 用 -1：**这条不是从某场会话里抽出来的**，是 agent 归纳的。
    // 传个假会话号会让"出自哪几场"变成假话，而那个数字是要给用户看的。
    const auto oc = actions::ActionStore::ingest(-1, {in}, nullptr, "agent", 0.5);
    if (!oc.err.empty()) return fail(u8"写入失败：" + oc.err);

    ToolResult r;
    r.ok = true;
    // 同上：成功路径也一律 JSON，别因为"这次只是并入已有条目"就换成散文。
    r.content = json_dump(json{
        {"inserted", oc.inserted},
        {"merged",   oc.merged},
        {"title",    title},
        {"status",   "todo"},
        {"note", oc.inserted
            ? u8"已记下（状态 todo，来源标记 agent，用户会在待办列表里看到它）"
            : u8"这条已经在列表里了（并入已有条目，没有重复添加）"},
    });
    r.audit   = oc.inserted ? (u8"新增行动项「" + title + u8"」")
                            : (u8"行动项已存在「" + title + u8"」");
    return r;
}

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
        "list_actions",
        "读**跨会话来袭的任务清单**（待办/进行中/已完成），带负责人、截止、以及"
        "“这件事在几场会话里被提到过”。\n"
        "**做周报、月报、进度汇报时必须先看它** —— “上周定的事这周怎么样了”"
        "只有这张表里有；一场一场去读转录既慢又会漏。\n"
        "你只能读。**改状态要用户自己来做**（--actions --done <id>），"
        "因为“做完了吗”是只有他知道的事实。",
        R"({"type":"object","properties":{
            "status":{"type":"string","enum":["todo","doing","done"],
                      "description":"只看某一档；不填 = 全部"},
            "limit":{"type":"integer","description":"最多返回几条，默认 30，最大 80"}
        }})",
        tool_list_actions,
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

void register_write_tools(ToolRegistry& reg) {
    std::string err;
    auto add = [&](Tool t) {
        if (!reg.add(std::move(t), &err)) {
            std::cerr << "[Agent] 注册工具失败：" << err << std::endl;
        }
    };

    add({
        "propose_knowledge",
        "**提议**把一个人名/项目/产品/术语记进长期记忆。"
        "当你在任务中发现某个名字反复出现、而且它看起来是**他们组织私有**的"
        "（不是通用词），但库里查不到时，用它提议记下来。\n"
        "⚠️ 你**只能提议**：记下来的是 candidate（候选），"
        "要等用户在会话结束时确认才会生效。你**不能**让它立刻生效。\n"
        "⚠️ 不确定就不要提议 —— 提议会在下次会话结束时占用用户一次注意力。",
        R"({"type":"object","properties":{
            "value":{"type":"string","description":"要记的名字（原样写法，如 凤凰项目 / CO-RE / Penny）"},
            "kind":{"type":"string","enum":["person","project","product","term"],
                    "description":"类型：人名 / 项目名 / 产品名 / 术语"},
            "evidence":{"type":"string","description":"你在哪句话里看到它的（原话，作为证据）"},
            "reason":{"type":"string","description":"为什么觉得它值得记（一句话）"}
        },"required":["value","kind"]})",
        tool_propose_knowledge,
    });

    add({
        "propose_action",
        "**提议**一条行动项（待办）记进跨会话清单。"
        "当你在整理会议/任务时发现一件“明确该做、但还没在任何纪要里被记下”的事，用它。\n"
        "⚠️ 和 propose_knowledge 不同：行动项**没有**用户确认环节，写进去就直接出现在"
        "用户的待办列表里。所以**只在有明确依据时用** —— 依据写进 evidence，"
        "用户会看到它，也会照着它判断该不该留着。\n"
        "⚠️ 状态一律是 todo：你**不能**替用户宣称某件事做完了。",
        R"({"type":"object","properties":{
            "title":{"type":"string","description":"要做的事，一句话（如 重写凤凰项目的接口文档）"},
            "owner":{"type":"string","description":"负责人（不确定就留空，不要猜）"},
            "due":{"type":"string","description":"截止时间（不确定就留空，不要编）"},
            "evidence":{"type":"string","description":"依据：哪句话/哪场会话让你认为有这件事"}
        },"required":["title"]})",
        tool_propose_action,
    });
}

}  // namespace agent
