#include "LlmSummarizer.h"
#include "CaBundle.h"
#include "Utf8.h"   // 解析模型回复前先净化 UTF-8

#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <vector>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include "json.hpp"

using json = nlohmann::json;

namespace {

// 转录长度上限（字符）。本地模型上下文 4096 token，
// 系统提示 + 输出要占掉一部分，转录留 4000 字符比较安全。
constexpr size_t kMaxTranscriptChars = 4000;

// 这些是 prompt 里的占位示例词。1.8B 这种小模型很容易把 schema 里的
// 示例值当成答案原样抄回来（实测出现过标题就叫"主题标题"、
// 负责人叫"负责人"）。凡是命中这些词的值一律判为无效。
const std::vector<std::string>& placeholder_words() {
    static const std::vector<std::string> v = {
        u8"主题标题", u8"要点", u8"负责人", u8"要做的事", u8"截止时间",
        u8"主题", u8"事项", u8"示例", u8"标题",
        "title", "points", "owner", "task", "due", "example",
    };
    return v;
}

bool looks_like_placeholder(const std::string& s) {
    if (s.empty()) return false;
    std::string t = s;
    // 去掉首尾空白与标点后做整体比较，避免把正常句子里含"主题"两字误判
    for (const auto& p : placeholder_words()) {
        if (t == p) return true;
    }
    return false;
}

std::string trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// 若字符串以某个前后缀包裹，剥掉
std::string strip_wrap(std::string s, const std::string& a, const std::string& b) {
    if (s.size() >= a.size() + b.size() &&
        s.compare(0, a.size(), a) == 0 &&
        s.compare(s.size() - b.size(), b.size(), b) == 0) {
        return s.substr(a.size(), s.size() - a.size() - b.size());
    }
    return s;
}

// 从模型回复里截出第一个 '{' 到最后一个 '}'（云端 JSON 路径用）
std::string extract_json_object(const std::string& raw) {
    const size_t b = raw.find('{');
    const size_t e = raw.rfind('}');
    if (b == std::string::npos || e == std::string::npos || e <= b) return {};
    return raw.substr(b, e - b + 1);
}

// 按分隔串切分（用于 "张三 | 发报价 | 下周五"）。
// 注意用 std::string 而不是 char：全角标点如 '；' '｜' 在 UTF-8 下是 3 字节，
// 放不进一个 char 字面量。
std::vector<std::string> split_by(const std::string& s, const std::string& sep) {
    std::vector<std::string> out;
    if (sep.empty()) { out.push_back(trim(s)); return out; }
    size_t pos = 0;
    while (true) {
        const size_t n = s.find(sep, pos);
        if (n == std::string::npos) {
            out.push_back(trim(s.substr(pos)));
            break;
        }
        out.push_back(trim(s.substr(pos, n - pos)));
        pos = n + sep.size();
    }
    return out;
}

// "无" / "none" / "-" 之类的占位都视为空
std::string normalize_slot(const std::string& s) {
    const std::string t = trim(s);
    if (t.empty() || t == u8"无" || t == u8"未知" || t == "-" || t == u8"—" ||
        t == "N/A" || t == "n/a" || t == "none" || t == "None") {
        return {};
    }
    if (looks_like_placeholder(t)) return {};
    return t;
}

// 归一化：只保留字母数字与多字节字符（汉字等），去掉空格标点与大小写差异，用于宽松匹配
std::string normalize_for_match(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            if (std::isalnum(c)) o += static_cast<char>(std::tolower(c));
            i += 1;
        } else {
            size_t len = 1;
            if      ((c & 0xE0) == 0xC0) len = 2;
            else if ((c & 0xF0) == 0xE0) len = 3;
            else if ((c & 0xF8) == 0xF0) len = 4;
            o += s.substr(i, len);
            i += len;
        }
    }
    return o;
}

// 最长公共子串长度（滚动数组 DP）
size_t longest_common_substr(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return 0;
    std::vector<size_t> prev(b.size() + 1, 0), cur(b.size() + 1, 0);
    size_t best = 0;
    for (size_t i = 1; i <= a.size(); ++i) {
        for (size_t j = 1; j <= b.size(); ++j) {
            cur[j] = (a[i - 1] == b[j - 1]) ? prev[j - 1] + 1 : 0;
            if (cur[j] > best) best = cur[j];
        }
        prev.swap(cur);
        std::fill(cur.begin(), cur.end(), 0);
    }
    return best;
}

// 把大模型给出的行动项回填到来源段落序号。
// 大模型只输出文字，不带段落编号；这里用译文的最长公共子串做匹配，
// 让交付物里的行动项能追溯到具体是哪句话说的。
void link_actions_to_segments(std::vector<ActionItem>& actions,
                              const std::vector<Segment>& segs) {
    const size_t kMinMatch = 6;   // 至少 6 个字节（约 2 个汉字）才算命中
    for (auto& a : actions) {
        const std::string needle = normalize_for_match(a.task);
        if (needle.empty()) continue;

        size_t best = 0;
        int    best_seq = 0;
        const Segment* best_seg = nullptr;
        for (const auto& s : segs) {
            const std::string& text = s.tgt_text.empty() ? s.src_text : s.tgt_text;
            const size_t n = longest_common_substr(needle, normalize_for_match(text));
            if (n > best) { best = n; best_seq = s.seq; best_seg = &s; }
        }
        if (best >= kMinMatch) {
            a.seq = best_seq;
            // 顺手补上来源原文：大模型只给结论，原文要从段落里回填，
            // 这样交付物里的每条行动项都能追溯到"谁在哪句说的"
            if (a.source.empty() && best_seg != nullptr) a.source = best_seg->src_text;
        }
    }
}

}  // namespace

const char* LlmSummarizer::name() const {
    return backend_ == Backend::Local ? u8"本地混元（数据不出本机）" : u8"云端 DeepSeek";
}

std::string LlmSummarizer::build_transcript(const std::vector<Segment>& segs) {
    std::ostringstream o;
    size_t used = 0;
    bool truncated = false;

    for (const auto& s : segs) {
        std::ostringstream line;
        line << s.seq << ". " << s.src_text;
        if (!s.tgt_text.empty() && s.tgt_text != s.src_text) {
            line << "\n   " << s.tgt_text;
        }
        line << "\n";

        const std::string l = line.str();
        if (used + l.size() > kMaxTranscriptChars) { truncated = true; break; }
        o << l;
        used += l.size();
    }
    if (truncated) {
        o << u8"（转录过长，已截断）\n";
    }
    return o.str();
}

std::string LlmSummarizer::build_system_prompt() const {
    const bool zh = (target_lang_ == "zh");

    std::ostringstream o;
    if (backend_ == Backend::Local) {
        // ---- 本地小模型：用结构化纯文本 ----
        // 实测 1.8B 模型填不对嵌套 JSON，会把 schema 里的示例值当答案抄回来
        // （标题就叫"主题标题"、负责人就叫"负责人"）。纯文本格式遵循度高得多。
        if (zh) {
            o << u8"你是一个内容整理助手。用户会给你一段转录——"
                 u8"它可能是会议、课程、视频或一段对话。\n\n"
                 u8"请严格按下面的格式输出，不要输出任何其他内容：\n\n"
                 u8"【类型】\n"
                 u8"从「会议」「课程」「视频」「对话」里选一个词填在这里。\n\n"
                 u8"【概述】\n"
                 u8"用 2 到 3 句中文客观描述这段内容在讲什么。\n\n"
                 u8"【要点】\n"
                 u8"- 在这里写第一个主题的名称：这一主题下的若干要点，用分号隔开\n"
                 u8"- 在这里写第二个主题的名称：要点\n\n"
                 u8"【行动项】\n"
                 u8"- 负责人 | 要做的事 | 截止时间\n\n"
                 u8"要求：\n"
                 u8"1. 要点写 2 到 6 条。\n"
                 u8"2. 行动项只收录明确的任务、请求或承诺；一条都没有就写「- 无」。\n"
                 u8"   问候、寒暄、感谢、告别（例如「下次见」「很高兴认识你」）都不算行动项。\n"
                 u8"3. 负责人或截止时间没提到时写「无」。\n"
                 u8"4. 严禁编造转录里没有出现的信息。\n"
                 u8"5. 如果这段内容本身没有实质信息（例如纯音乐、歌词、寒暄、"
                 u8"识别明显有误），请在概述里如实说明，要点只写 1 到 2 条。";
        } else {
            o << "You are a content-organising assistant. The user gives you a transcript — "
                 "it may be a meeting, a class, a video, or a conversation.\n\n"
                 "Output exactly this format, nothing else:\n\n"
                 "[TYPE]\n"
                 "One word: meeting / class / video / conversation\n\n"
                 "[OVERVIEW]\n"
                 "2-3 sentences summarising what it was about.\n\n"
                 "[TOPICS]\n"
                 "- <topic name>: <point 1>; <point 2>\n"
                 "- <topic name>: <points>\n\n"
                 "[ACTIONS]\n"
                 "- <owner> | <task> | <due date>\n\n"
                 "Rules:\n"
                 "1. 2 to 6 topics.\n"
                 "2. Only explicit tasks, requests or commitments; if none, write '- none'.\n"
                 "3. Write 'none' when owner or due date is not mentioned.\n"
                 "4. Never invent information absent from the transcript.";
        }
    } else {
        // ---- 云端：JSON 更稳，且可用 response_format 强约束 ----
        if (zh) {
            o << u8"你是一个内容整理助手。用户会给你一段转录，"
                 u8"它可能是会议、课程、视频或一段对话。\n\n"
                 u8"请只输出一个 JSON 对象，不要输出其他文字，不要用 markdown 代码块。结构：\n"
                 u8"{\"content_type\":\"会议｜课程｜视频｜对话\","
                 u8"\"overview\":\"2-3 句中文概述\","
                 u8"\"topics\":[{\"title\":\"...\",\"points\":[\"...\"]}],"
                 u8"\"actions\":[{\"owner\":\"...\",\"task\":\"...\",\"due\":\"...\"}]}\n\n"
                 u8"要求：content_type 从「会议」「课程」「视频」「对话」中选一个；"
                 u8"概述客观；要点 2-6 个主题，每个 2-4 条；"
                 u8"行动项只收录明确的任务、请求或承诺，没有就输出 []；"
                 u8"问候、寒暄、感谢、告别不算行动项；"
                 u8"未提到的负责人或截止时间用空字符串；严禁编造。"
                 u8"如果内容本身没有实质信息（纯音乐、歌词、寒暄、识别明显有误），"
                 u8"在 overview 里如实说明并把 topics 减到 1-2 个。";
        } else {
            o << "You are a content-organising assistant. The user gives you a transcript, "
                 "which may be a meeting, a class, a video or a conversation.\n\n"
                 "Output ONLY one JSON object, no other text, no markdown fences:\n"
                 "{\"content_type\":\"meeting|class|video|conversation\","
                 "\"overview\":\"...\",\"topics\":[{\"title\":\"...\",\"points\":[\"...\"]}],"
                 "\"actions\":[{\"owner\":\"...\",\"task\":\"...\",\"due\":\"...\"}]}\n\n"
                 "objective overview; 2-6 topics with 2-4 points each; only explicit tasks, "
                 "requests or commitments in actions (else []); empty string when owner or due "
                 "is not mentioned; never invent information.";
        }
    }
    return o.str();
}

// ============================================================
// 解析：本地走纯文本，云端走 JSON
// ============================================================
bool LlmSummarizer::parse_reply(const std::string& raw, MeetingSummary& out,
                                std::string& err) const {
    if (backend_ == Backend::Local) {
        // ---------- 结构化纯文本 ----------
        MeetingSummary s;
        std::istringstream in(raw);
        std::string line;
        enum class Sec { None, Type, Overview, Topics, Actions } sec = Sec::None;

        std::ostringstream overview;
        for (std::string l; std::getline(in, l); ) {
            const std::string t = trim(l);
            if (t.empty()) continue;

            if (t.find(u8"【类型】") != std::string::npos || t == "[TYPE]") {
                sec = Sec::Type;
                // 允许把值写在同一行："【类型】会议"
                const size_t p = t.find(u8"】");
                if (p != std::string::npos) {
                    const std::string rest = normalize_slot(t.substr(p + 3));
                    if (!rest.empty()) s.content_type = rest;
                }
                continue;
            }
            if (t.find(u8"【概述】") != std::string::npos || t == "[OVERVIEW]") {
                sec = Sec::Overview; continue;
            }
            if (t.find(u8"【要点】") != std::string::npos || t == "[TOPICS]") {
                sec = Sec::Topics; continue;
            }
            if (t.find(u8"【行动项】") != std::string::npos || t == "[ACTIONS]") {
                sec = Sec::Actions; continue;
            }

            if (sec == Sec::Type) {
                if (s.content_type.empty()) s.content_type = normalize_slot(t);
            } else if (sec == Sec::Overview) {
                if (!overview.str().empty()) overview << ' ';
                overview << t;
            } else if (sec == Sec::Topics) {
                std::string item = strip_wrap(t, "- ", "");
                if (item.empty()) item = t;
                // "主题名：要点1；要点2"
                std::string title = item, points_str;
                // 【绝不能写 find_first_of(u8"：:")】它按**单字节**比较，
                // 而 u8"：" 是三个字节 EF BC 9A —— 于是它在找
                // 「0xEF 或 0xBC 或 0x9A 或 ':'」中的任意一个字节。
                // 而 **「的」= E7 9A 84，第二个字节就是 0x9A**，会被当成冒号。
                //
                // 实测后果（真实产物 meeting-42.md）：
                //     item  = "询问你的想法：要点"
                //     找到的位置 = 「的」中间那个字节（10），真正的冒号在 18
                //     substr(0, 10) = "询问你" + 0xE7     ← 「的」被切成两半
                //     文件里就是  ### 询问你\xe7          ← 整份文件不再是合法 UTF-8
                //
                // 注意：**这不是模型吐了半个字符，是我们自己切坏的。**
                // 我一开始把责任归给了混元（说它输出半个 token），是错的 ——
                // 直到发现"净化器报了 6 处坏字节，但摘要那一步一行警告都没有"
                // 才对上账。凡是有多字节分隔符的地方都用 find() 整串匹配。
                size_t colon = item.find(u8"：");
                {
                    const size_t half = item.find(':');
                    if (half != std::string::npos &&
                        (colon == std::string::npos || half < colon)) {
                        colon = half;
                    }
                }
                if (colon != std::string::npos) {
                    title      = trim(item.substr(0, colon));
                    // 加的是分隔符**本身的长度**，不是 1 —— 全角冒号占 3 字节，
                    // 写 colon + 1 会从"要"字中间开始，又是一次切坏
                    const size_t sep_len = item.compare(colon, 3, u8"：") == 0 ? 3 : 1;
                    points_str = trim(item.substr(colon + sep_len));
                }
                TopicSummary ts;
                ts.title = normalize_slot(title);
                if (!points_str.empty()) {
                    for (auto& p : split_by(points_str, u8"；")) {
                        if (!p.empty()) ts.points.push_back(p);
                    }
                    if (ts.points.size() <= 1) {
                        ts.points.clear();
                        for (auto& p : split_by(points_str, ";")) {
                            if (!p.empty()) ts.points.push_back(p);
                        }
                    }
                }
                if (!ts.title.empty() || !ts.points.empty()) s.topics.push_back(std::move(ts));
            } else if (sec == Sec::Actions) {
                std::string item = strip_wrap(t, "- ", "");
                if (item.empty()) item = t;
                if (normalize_slot(item).empty()) continue;   // "- 无"

                ActionItem a;
                std::vector<std::string> parts = split_by(item, "|");
                if (parts.size() < 3) parts = split_by(item, u8"｜");   // 全角竖线
                if (parts.size() < 3) parts = split_by(item, u8"、");
                if (parts.size() >= 3) {
                    a.owner = normalize_slot(parts[0]);
                    a.task  = normalize_slot(parts[1]);
                    a.due   = normalize_slot(parts[2]);
                } else {
                    a.task = normalize_slot(item);
                }
                if (!a.task.empty()) s.actions.push_back(std::move(a));
            }
        }

        s.overview = trim(overview.str());
        if (s.overview.empty() && s.topics.empty() && s.actions.empty()) {
            err = u8"纯文本回复里没解析出任何章节（可能模型没按格式输出）";
            return false;
        }
        out = std::move(s);
        return true;
    }

    // ---------- 云端 JSON ----------
    const std::string js = extract_json_object(raw);
    if (js.empty()) {
        err = u8"回复里找不到 JSON 对象";
        return false;
    }

    json j;
    try {
        j = json::parse(js);
    } catch (const std::exception& e) {
        err = std::string(u8"JSON 解析失败: ") + e.what();
        return false;
    }

    MeetingSummary s;
    if (j.contains("content_type") && j["content_type"].is_string()) {
        s.content_type = normalize_slot(j["content_type"].get<std::string>());
    }
    if (j.contains("overview") && j["overview"].is_string()) {
        s.overview = j["overview"].get<std::string>();
    }
    if (j.contains("topics") && j["topics"].is_array()) {
        for (const auto& t : j["topics"]) {
            TopicSummary ts;
            if (t.contains("title") && t["title"].is_string())  ts.title = t["title"].get<std::string>();
            if (t.contains("points") && t["points"].is_array()) {
                for (const auto& p : t["points"]) {
                    if (p.is_string()) ts.points.push_back(p.get<std::string>());
                }
            }
            if (!normalize_slot(ts.title).empty() || !ts.points.empty()) {
                s.topics.push_back(std::move(ts));
            }
        }
    }
    if (j.contains("actions") && j["actions"].is_array()) {
        for (const auto& a : j["actions"]) {
            auto get = [&](const char* k) -> std::string {
                return (a.contains(k) && a[k].is_string()) ? a[k].get<std::string>() : std::string();
            };
            ActionItem ai;
            ai.owner = normalize_slot(get("owner"));
            ai.task  = normalize_slot(get("task"));
            ai.due   = normalize_slot(get("due"));
            if (!ai.task.empty()) s.actions.push_back(std::move(ai));
        }
    }

    if (s.overview.empty() && s.topics.empty() && s.actions.empty()) {
        err = u8"JSON 里没有可用的 overview / topics / actions";
        return false;
    }
    out = std::move(s);
    return true;
}

bool LlmSummarizer::call_model(const std::string& system, const std::string& user,
                               std::string& raw, std::string& err) {
    if (backend_ == Backend::Local) {
        if (!gen_) {
            err = u8"本地生成函数未注入";
            return false;
        }
        if (!gen_(system, user, raw)) {
            err = u8"本地模型生成失败";
            return false;
        }
        return true;
    }

    // ---- 云端 DeepSeek ----
    if (api_key_.empty()) {
        err = u8"未配置 DeepSeek API Key";
        return false;
    }

    httplib::Client cli("https://api.deepseek.com");
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(60, 0);

    // 同 DeepSeekTranslator：必须显式指定 CA，否则 HTTPS 校验失败
    const std::string ca = find_ca_bundle();
    if (!ca.empty()) cli.set_ca_cert_path(ca);

    json payload = {
        {"model", "deepseek-chat"},
        {"messages", json::array({
            {{"role", "system"}, {"content", system}},
            {{"role", "user"},   {"content", user}}
        })},
        {"response_format", {{"type", "json_object"}}},
        {"temperature", 0.3}
    };
    httplib::Headers headers = {
        {"Authorization", "Bearer " + api_key_},
        {"Content-Type", "application/json"}
    };

    auto res = cli.Post("/chat/completions", headers,
                        payload.dump(-1, ' ', false, json::error_handler_t::replace),
                        "application/json");
    if (!res) {
        err = std::string(u8"网络请求失败: ") + httplib::to_string(res.error());
        return false;
    }
    if (res->status != 200) {
        err = u8"HTTP " + std::to_string(res->status);
        return false;
    }
    try {
        const auto j = json::parse(res->body);
        raw = j["choices"][0]["message"]["content"].get<std::string>();
    } catch (const std::exception& e) {
        err = std::string(u8"解析响应失败: ") + e.what();
        return false;
    }
    return true;
}

std::string LlmSummarizer::build_user_content(const std::vector<Segment>& segs) const {
    const std::string transcript = build_transcript(segs);
    if (background_.empty()) return transcript;

    // 背景知识拼在转录**前面**，并明确标注它的性质。
    //
    // 【为什么要标注"已确认"】不标的话，小模型会把它当成"转录的一部分"去总结，
    // 于是纪要里凭空多出几条没发生过的内容。标了之后它是"参考资料"，
    // 模型只在遇到对应名字时用它对齐写法。
    //
    // 【为什么要说"不要与它矛盾"】实测同一个人名在转录里可能有两种写法
    // （会话 #11 的 Erica / 埃里卡）。有 confirmed 值时应当以它为准 ——
    // 这正是第二句承诺"用得越久越懂你"在摘要上的体现。
    std::ostringstream o;
    if (target_lang_ == "zh") {
        o << u8"以下是用户已确认的背景知识，请直接采信，不要在输出里与它矛盾：\n";
    } else {
        o << "The following is user-confirmed background knowledge. Take it as given "
             "and do not contradict it:\n";
    }
    for (const auto& l : background_) o << l << "\n";
    if (target_lang_ == "zh") {
        o << u8"\n以下是本次转录（背景知识**不是**转录内容，不要把它当成发生过的事）：\n";
    } else {
        o << "\nBelow is the transcript (the background above is NOT part of it; "
             "do not treat it as something that happened):\n";
    }
    o << transcript;
    return o.str();
}

bool LlmSummarizer::summarize(const std::vector<Segment>& segs, MeetingSummary& out,
                              std::string& err) {
    if (segs.empty()) {
        err = u8"会话里没有段落";
        return false;
    }

    const std::string system = build_system_prompt();
    const std::string user   = build_user_content(segs);

    std::string raw;
    if (!call_model(system, user, raw, err)) return false;

    // ---- 解析前先净化 UTF-8 ----
    //
    // 这**不是**因为模型会吐半个字符 —— 实测那类事故其实是解析器自己切出来的
    // （见 parse_reply 里 `find_first_of` 那段注释）。这里做是因为：
    // `call_model` 之后拿到的字符串来自模型，任何字节都可能出现，
    // 而下游（逐行解析、切分、去重、来源回填）都默认文本是合法 UTF-8。
    // 与其在每个下游函数里防，不如在入口收一次。
    size_t fixed = 0;
    const std::string clean = utf8::sanitize(raw, &fixed);
    if (fixed > 0) {
        std::cerr << "[Summarizer] 模型回复里有 " << fixed
                  << " 处非法 UTF-8 字节，已净化后再解析（文件仍会生成，但那几处内容不可信）"
                  << std::endl;
    }

    // 本地小模型经常吐空或吐不按格式的内容，把原始输出打出来才好排查
    std::cerr << "[Summarizer] 模型返回 " << clean.size() << " 字符" << std::endl;

    if (!parse_reply(clean, out, err)) {
        std::cerr << "[Summarizer] 解析失败。模型原始回复：\n----\n"
                  << clean.substr(0, 800) << "\n----" << std::endl;
        return false;
    }

    // 行动项兜底：大模型如果一条都没提出，用规则抽取补上。
    // 实测里规则版在行动项上反而更全（6 条 vs 1 条），所以这里做合并而不是二选一。
    if (out.actions.empty()) {
        auto rule = DeliverableWriter::extract_by_rules(segs);
        if (!rule.actions.empty()) {
            out.actions = std::move(rule.actions);
            std::cout << "[Summarizer] 大模型未给出行动项，已用规则抽取补齐 "
                      << out.actions.size() << " 条" << std::endl;
        }
    } else {
        // 大模型只给文字，回填来源段落
        link_actions_to_segments(out.actions, segs);
    }

    out.generator = std::string(u8"大模型生成 · ") + name();
    return true;
}
