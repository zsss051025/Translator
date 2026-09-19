#include "DeliverableWriter.h"
#include "Evidence.h"   // 出处格式的唯一来源（5.7）
#include "Utf8.h"   // 写出边界的 UTF-8 净化

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>   // 写出边界的 UTF-8 净化失败时要报给用户
#include <sstream>

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------
// 基础工具
// ---------------------------------------------------------------

// 解析 "YYYY-MM-DD HH:MM:SS.mmm" -> 毫秒时间戳；失败返回 -1
long long parse_ts_ms(const std::string& s) {
    int Y = 0, M = 0, D = 0, h = 0, m = 0, sec = 0, ms = 0;
    const int n = std::sscanf(s.c_str(), "%d-%d-%d %d:%d:%d.%d",
                              &Y, &M, &D, &h, &m, &sec, &ms);
    if (n < 6) return -1;

    std::tm tm_buf{};
    tm_buf.tm_year = Y - 1900;
    tm_buf.tm_mon  = M - 1;
    tm_buf.tm_mday = D;
    tm_buf.tm_hour = h;
    tm_buf.tm_min  = m;
    tm_buf.tm_sec  = sec;
    tm_buf.tm_isdst = -1;

    const std::time_t t = std::mktime(&tm_buf);
    if (t == static_cast<std::time_t>(-1)) return -1;
    return static_cast<long long>(t) * 1000 + (n >= 7 ? ms : 0);
}

// 毫秒 -> "HH:MM:SS,mmm"（SRT 格式）
std::string srt_time(long long ms) {
    if (ms < 0) ms = 0;
    const long long h = ms / 3600000; ms %= 3600000;
    const long long m = ms / 60000;   ms %= 60000;
    const long long s = ms / 1000;    ms %= 1000;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld,%03lld", h, m, s, ms);
    return buf;
}

// 秒 -> "X 分 Y 秒"
std::string human_duration(long long sec) {
    if (sec < 0) sec = 0;
    if (sec < 60) return std::to_string(sec) + " 秒";
    std::ostringstream oss;
    oss << (sec / 60) << " 分 " << (sec % 60) << " 秒";
    return oss.str();
}

std::string html_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;";  break;
            case '<': o += "&lt;";   break;
            case '>': o += "&gt;";   break;
            case '"': o += "&quot;"; break;
            default:  o += c;        break;
        }
    }
    return o;
}

std::string csv_escape(const std::string& s) {
    if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
    std::string o = "\"";
    for (char c : s) {
        if (c == '"') o += "\"\"";
        else          o += c;
    }
    o += "\"";
    return o;
}

// Markdown 表格单元格：管道符转义，换行折成空格（否则表格会被撑破）
std::string md_cell(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '|')                    o += "\\|";
        else if (c == '\n' || c == '\r') o += ' ';
        else                             o += c;
    }
    return o;
}

std::string lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool contains_any(const std::string& hay, const std::vector<std::string>& needles) {
    for (const auto& n : needles) {
        if (hay.find(n) != std::string::npos) return true;
    }
    return false;
}

// 按"词"匹配（英文动作词专用），不用裸 find()。
//
// 【现象】"The latest results are in." 被当成行动项留了下来
// 【原因】裸 find("test") 命中了 "latest"；同理 "fix" 会命中 "prefix"，
//         这种误命中让纯陈述句通过了"必须有动作词"这一关
// 【判断】加新的英文短动词时，务必确认它不会被更长的常见词包进去
bool contains_word(const std::string& hay_lower, const std::vector<std::string>& words) {
    auto word_char = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '\'';
    };
    for (const auto& w : words) {
        if (w.empty()) continue;
        size_t pos = 0;
        while ((pos = hay_lower.find(w, pos)) != std::string::npos) {
            const size_t end = pos + w.size();
            const bool left_ok  = (pos == 0) || !word_char(hay_lower[pos - 1]);
            const bool right_ok = (end >= hay_lower.size()) || !word_char(hay_lower[end]);
            if (left_ok && right_ok) return true;
            pos = end;
        }
    }
    return false;
}

// UTF-8 安全的"取前 n 个字符"
std::string utf8_prefix(const std::string& s, size_t max_chars) {
    size_t chars = 0, i = 0;
    while (i < s.size() && chars < max_chars) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if      ((c & 0x80) == 0x00) i += 1;
        else if ((c & 0xE0) == 0xC0) i += 2;
        else if ((c & 0xF0) == 0xE0) i += 3;
        else if ((c & 0xF8) == 0xF0) i += 4;
        else                          i += 1;
        ++chars;
    }
    return s.substr(0, std::min(i, s.size()));
}

// UTF-8 码点数（中文一个字算一个，不是三个）
size_t utf8_count(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n;
    return n;
}

// 归一化：去掉标点与空白，ASCII 转小写。
// 目的：判断"以什么开头""有没有动作词"时只关心内容，
// 不被"那么，……"里的逗号或句末句号干扰。
std::string normalize_action_text(const std::string& s) {
    static const std::vector<std::string> skip_cp = {
        u8"。", u8"，", u8"、", u8"？", u8"！", u8"；", u8"：", u8"“", u8"”",
        u8"‘", u8"’", u8"（", u8"）", u8"《", u8"》", u8"　", u8"—", u8"…", u8"·",
    };
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            const bool punct = (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                                c == '.' || c == ',' || c == '!' || c == '?' ||
                                c == ';' || c == ':' || c == '"' || c == '\'' ||
                                c == '(' || c == ')' || c == '-' || c == '*' ||
                                c == '/');
            if (!punct) out.push_back(static_cast<char>(std::tolower(c)));
            ++i;
            continue;
        }
        const size_t len = (c & 0xF8) == 0xF0 ? 4 : (c & 0xF0) == 0xE0 ? 3 : 2;
        if (i + len > s.size()) break;
        const std::string cp = s.substr(i, len);
        bool skip = false;
        for (const auto& k : skip_cp) if (cp == k) { skip = true; break; }
        if (!skip) out += cp;
        i += len;
    }
    return out;
}

// 严格动作词：**判定**用的表，比 action_markers_*（**抽取**用的触发表）窄得多。
//
// 为什么必须分成两套：触发词表为了不漏，刻意收了"需要/应该/必须"这类情态词；
// 而判定"是不是真的待办"要看有没有能落地执行的动作。
// 会话 #11 那句"那么，您应该向我们介绍一下今天这堂课的内容。"就是被"应该"触发的，
// 它一个严格动作词都没有 —— 这正是要拦的形态。
//
// 收录标准：**能接宾语的动词**。名词一律不收——
// 曾把 u8"报价" 放进来（想覆盖"报价给客户"），结果"这个报价太高了。"这种
// 纯陈述句也会通过，等于开了个后门。
const std::vector<std::string>& strict_verbs_zh() {
    static const std::vector<std::string> v = {
        u8"完成", u8"提交", u8"发送", u8"发给", u8"提供", u8"确认", u8"安排",
        u8"准备", u8"跟进", u8"更新", u8"评审", u8"联系", u8"回复", u8"检查",
        u8"整理", u8"汇总", u8"输出", u8"交付", u8"对接", u8"落实", u8"推动",
        u8"部署", u8"上线", u8"测试", u8"推迟", u8"协调", u8"起草", u8"核对",
        u8"统计", u8"归档", u8"预约", u8"修订", u8"补齐", u8"反馈", u8"同步",
    };
    return v;
}

// 英文动作词。注意：匹配走 contains_word()（按词边界），
// 所以这里可以放 "book" / "test" 这类短词，不会误命中 "facebook" / "latest"。
const std::vector<std::string>& strict_verbs_en() {
    static const std::vector<std::string> v = {
        "send", "submit", "provide", "confirm", "schedule", "prepare", "follow up",
        "update", "review", "contact", "reply", "check", "organize", "summarize",
        "deliver", "deploy", "release", "test", "postpone", "coordinate", "draft",
        "verify", "collect", "archive", "book", "finalize", "complete", "share",
        "fix", "revise", "circle back", "sync up", "reach out", "take care of",
    };
    return v;
}

// 过场语 / 寒暄 / 主观臆断开头：这样开头的句子几乎不可能是待办。
// 刻意**不收** "让我们/咱们"——"让我们下周五前提交报告"是真待办，
// 收了会把真条目误杀（宁可漏杀，不可误杀）。
const std::vector<std::string>& preamble_prefixes() {
    static const std::vector<std::string> v = {
        u8"那么", u8"好了", u8"好吧", u8"嗯", u8"哦", u8"谢谢", u8"感谢",
        u8"欢迎", u8"大家好", u8"你好", u8"我觉得", u8"我认为", u8"听起来",
        u8"看起来", u8"应该说",
    };
    return v;
}

// 把中英日期表达归一成同一套英文 token，供**跨语言比对**。
//
// 【现象】实测云端大模型给出的截止日期「下周五」被判为"无依据"并清空，
//         但来源原文确实写着 "...by next Friday." —— 是**误杀**
// 【原因】只比字面。而云端大模型把日期翻成中文是**正常行为**，不是异常，
//         所以这不是低频边界，是常规路径
// 【判断】凡是要比对中英混排的字段，都得先归一化再比，不能直接 find
//
// 表按**长词优先**排列：u8"下周五" 必须排在 u8"下周" 前面，
// 否则 "下周五" 会被 "下周" 先吃掉，剩一个孤零零的 "五"。
std::string canonicalize_date(const std::string& s) {
    static const std::vector<std::pair<std::string, std::string>> map = {
        // 复合形式：下/这/本 + 周X（最常见的相对日期）
        {u8"下周一", " next monday "},    {u8"下周二", " next tuesday "},
        {u8"下周三", " next wednesday "}, {u8"下周四", " next thursday "},
        {u8"下周五", " next friday "},    {u8"下周六", " next saturday "},
        {u8"下周日", " next sunday "},    {u8"下星期天", " next sunday "},
        {u8"这周五", " this friday "},    {u8"本周五", " this friday "},
        {u8"这周", " this week "},        {u8"本周", " this week "},
        {u8"下周", " next week "},        {u8"上周", " last week "},
        {u8"月底", " end of the month "}, {u8"月末", " end of the month "},
        {u8"季度末", " end of the quarter "},
        {u8"今天", " today "},            {u8"今日", " today "},
        {u8"明天", " tomorrow "},         {u8"明日", " tomorrow "},
        {u8"周一", " monday "},   {u8"周二", " tuesday "},  {u8"周三", " wednesday "},
        {u8"周四", " thursday "}, {u8"周五", " friday "},   {u8"周六", " saturday "},
        {u8"周日", " sunday "},   {u8"星期天", " sunday "}, {u8"星期日", " sunday "},
    };

    std::string out;
    out.reserve(s.size() + 16);
    for (size_t i = 0; i < s.size();) {
        bool hit = false;
        for (const auto& kv : map) {
            if (s.compare(i, kv.first.size(), kv.first) == 0) {
                out += kv.second;
                i += kv.first.size();
                hit = true;
                break;
            }
        }
        if (hit) continue;

        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            // 这里用不带 locale 的简单小写，避免 <cctype> 的 tolower 受区域设置影响
            out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c));
            ++i;
        } else {
            // 多字节字符整段拷贝，不能只拷首字节
            const size_t len = (c & 0xF8) == 0xF0 ? 4 : (c & 0xF0) == 0xE0 ? 3 : 2;
            if (i + len > s.size()) break;
            out += s.substr(i, len);
            i += len;
        }
    }

    // 折叠连续空格并去掉首尾：否则 " next friday " 匹配不上 "…next friday."
    std::string folded;
    folded.reserve(out.size());
    bool prev_space = true;                 // 初值 true 让开头的空格被吃掉
    for (char c : out) {
        if (c == ' ') {
            if (!prev_space) folded.push_back(' ');
            prev_space = true;
        } else {
            folded.push_back(c);
            prev_space = false;
        }
    }
    while (!folded.empty() && folded.back() == ' ') folded.pop_back();
    return folded;
}

bool write_file(const fs::path& p, const std::string& content, bool with_bom) {
    // ---- UTF-8 净化：交付物必须是合法 UTF-8 ----
    //
    // 【放在这里而不是每个字段各自处理】出口只有一个，漏不掉；
    // 将来多一种交付物也自动被覆盖。
    //
    // 【为什么是"净化 + 大声报告"而不是拒绝】真实事故：用 `--summarizer local`（混元）
    // 生成的 meeting-42.md **整份不是合法 UTF-8**（`### 询问你\xe7` —— `\xe7` 是个
    // 没写完的三字节首字节）。
    // 根因**不是模型**，是 `LlmSummarizer::parse_reply` 里
    // `item.find_first_of(u8"：:")` 按单字节比较，把「的」（E7 9A 84）里的 0x9A
    // 当成了冒号，一刀切在字符中间 —— 那一处已修。详见 Utf8.h 的说明。
    // 拒绝写盘意味着用户一份交付物都拿不到，而那份交付物 99% 的内容是好的；
    // 所以净化保证文件永远合法，但**必须打警告** —— 静默替换才是真正违反
    // "绝不静默给错数据"那条规矩的做法。
    size_t fixed = 0;
    const std::string safe = utf8::sanitize(content, &fixed);
    if (fixed > 0) {
        std::cerr << "[Deliverable] 警告：" << p.filename().string() << " 里有 " << fixed
                  << " 处非法 UTF-8 字节，已替换成 U+FFFD。文件本身现在合法，"
                     "但那几处内容不可信。" << std::endl;
        std::cerr << "[Deliverable] 这是**代码 bug 的症状**（按字节切了多字节字符），"
                     "不是模型的问题。请查最近的字符串截断/分隔符比较改动。"
                  << std::endl;
    }

    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    if (with_bom) f.write("\xEF\xBB\xBF", 3);   // Excel 打开 CSV 需要 BOM 才不乱码
    f.write(safe.data(), static_cast<std::streamsize>(safe.size()));
    return f.good();
}

// ---------------------------------------------------------------
// 规则版抽取
// ---------------------------------------------------------------

const std::vector<std::string>& action_markers_en() {
    static const std::vector<std::string> v = {
        "need to", "needs to", "have to", "has to", "should ", "must ",
        "will ", "we'll", "i'll",
        "please ", "follow up", "action item", "make sure", "by the end of",
        "deadline", "take care of", "send me", "reach out",
        // 说明：这里刻意不收 "let's" / "let us" / "schedule"。
        // "Let's start" / "Let's wrap up" 只是过场语，"shipping schedule" 是名词，
        // 收了会把大量非行动项误判进来。真正的行动项由 "follow up" 等具体动词命中。
    };
    return v;
}

const std::vector<std::string>& action_markers_zh() {
    static const std::vector<std::string> v = {
        u8"需要", u8"应该", u8"必须", u8"负责", u8"跟进", u8"安排",
        u8"截止", u8"请", u8"记得", u8"别忘了", u8"下次", u8"确认一下",
        u8"提交", u8"发给", u8"落实",
    };
    return v;
}

const std::vector<std::string>& due_markers_en() {
    static const std::vector<std::string> v = {
        // 多词短语在前，但真正的保证来自 find_best_marker 的"取最长匹配"
        "end of the month", "end of the quarter", "end of the week", "end of day",
        "next week", "next month", "this week", "this month",
        "tomorrow", "today", "asap", "eod", "eow",
        "monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday",
        "january", "february", "march", "april", "may", "june",
        "july", "august", "september", "october", "november", "december",
    };
    return v;
}

// 在一组候选词里挑出**最长**的命中项。
// 这样 "next friday" 不会被更短的 "friday" 抢先命中，
// 结果与候选列表的书写顺序无关。
std::string find_longest_marker(const std::string& hay, const std::vector<std::string>& markers) {
    std::string best;
    for (const auto& m : markers) {
        if (hay.find(m) != std::string::npos && m.size() > best.size()) best = m;
    }
    return best;
}

const std::vector<std::string>& due_markers_zh() {
    static const std::vector<std::string> v = {
        u8"下周", u8"本周", u8"这周", u8"明天", u8"今天",
        u8"月底前", u8"月底", u8"季末",
        u8"周一", u8"周二", u8"周三", u8"周四", u8"周五",
        u8"周六", u8"周日", u8"周天", u8"尽快",
    };
    return v;
}

// 从原文里挑出一个"截止"片段：优先英文月份+日期，其次中文月日，最后相对时间词
std::string find_due(const std::string& src) {
    const std::string low = lower_ascii(src);

    // 英文 "october 31" / "oct 31" / "october 31st"
    static const char* kMonths[] = {
        "january","february","march","april","may","june",
        "july","august","september","october","november","december"
    };
    for (const char* mon : kMonths) {
        const size_t pos = low.find(mon);
        if (pos == std::string::npos) continue;
        size_t end = pos + std::strlen(mon);
        while (end < low.size() && low[end] == ' ') ++end;
        size_t d0 = end;
        while (end < low.size() && std::isdigit(static_cast<unsigned char>(low[end]))) ++end;
        if (end > d0) {
            while (end < low.size() && std::isalpha(static_cast<unsigned char>(low[end]))) ++end;  // 吃掉 st/nd/rd/th
            return src.substr(pos, end - pos);
        }
        return src.substr(pos, std::strlen(mon));
    }

    // 中文 "10月31日" / "10月31号"
    for (size_t i = 0; i + 3 <= src.size(); ++i) {
        if (src.compare(i, 3, u8"月") != 0) continue;   // compare 返回 0 表示相等
        // 往前找数字
        size_t s = i;
        while (s > 0 && std::isdigit(static_cast<unsigned char>(src[s - 1]))) --s;
        if (s == i) continue;
        size_t e = i + 3;
        while (e < src.size() && std::isdigit(static_cast<unsigned char>(src[e]))) ++e;
        if (e + 3 <= src.size() &&
            (src.compare(e, 3, u8"日") == 0 || src.compare(e, 3, u8"号") == 0)) {
            e += 3;
        }
        return src.substr(s, e - s);
    }

    // 相对时间词：中英文都找，取最长的命中
    // （用最长匹配而不是"先到先得"，避免 "next friday" 被 "friday" 抢先）
    std::string best_en = find_longest_marker(low, due_markers_en());

    // 星期名前若带 next / this 修饰，把修饰词一起带走：
    // "next friday" 比裸的 "friday" 信息量大得多
    if (!best_en.empty()) {
        for (const char* prefix : {"next ", "this "}) {
            const std::string cand = std::string(prefix) + best_en;
            if (low.find(cand) != std::string::npos) { best_en = cand; break; }
        }
    }

    const std::string best_zh = find_longest_marker(src, due_markers_zh());
    return best_zh.size() >= best_en.size() ? best_zh : best_en;
}

// 从原文里挑出负责人。命中率有限，抽不到就留空——宁可空着也不写错。
std::string find_owner(const std::string& src) {
    // 中文："张三负责" / "由张三跟进"
    // 注意：不能用 find_first_of 查标点——它按单字节比较，
    // 而 UTF-8 汉字的后续字节会与标点字节撞车，导致合法姓名被误判。
    // 这里改为要求候选"整段都是非 ASCII 字节"（即纯 CJK）。
    for (const char* verb : {u8"负责", u8"跟进", u8"落实"}) {
        const size_t pos = src.find(verb);
        if (pos == std::string::npos || pos < 6) continue;
        size_t s = pos;
        size_t chars = 0;
        while (s > 0 && chars < 4) {
            --s;
            while (s > 0 && (static_cast<unsigned char>(src[s]) & 0xC0) == 0x80) --s;
            ++chars;
        }
        const std::string cand = src.substr(s, pos - s);
        if (cand.size() < 6 || cand.size() > 12) continue;      // 2~4 个汉字
        bool pure_cjk = true;
        for (unsigned char c : cand) {
            if (c < 0x80) { pure_cjk = false; break; }
        }
        if (pure_cjk) return cand;
    }

    // 英文："John will" / "Alice should"
    for (size_t i = 0; i + 1 < src.size(); ++i) {
        if (!std::isupper(static_cast<unsigned char>(src[i]))) continue;
        if (i > 0 && src[i - 1] != ' ') continue;
        // 从 i+1 开始吃小写字母——首字母是大写，从 i 开始循环会一个字符都吃不到
        size_t e = i + 1;
        while (e < src.size() && std::islower(static_cast<unsigned char>(src[e]))) ++e;
        if (e - i < 2 || e - i > 12) continue;
        size_t sp = e;
        while (sp < src.size() && src[sp] == ' ') ++sp;
        const std::string after = lower_ascii(src.substr(sp, 12));
        if (after.rfind("will ", 0) == 0 || after.rfind("should ", 0) == 0 ||
            after.rfind("is going", 0) == 0 || after.rfind("needs to", 0) == 0) {
            return src.substr(i, e - i);
        }
    }
    return {};
}

}  // namespace

// ===============================================================
// 规则版摘要 + 行动项提取
// ===============================================================
MeetingSummary DeliverableWriter::extract_by_rules(const std::vector<Segment>& segs) {
    MeetingSummary s;
    s.generator = u8"规则提取（离线，未使用大模型）";

    if (segs.empty()) {
        s.overview = u8"本场会话没有记录到任何内容。";
        return s;
    }

    // --- 概述：用统计事实说话，不编内容 ---
    const long long t0 = parse_ts_ms(segs.front().ts);
    const long long t1 = parse_ts_ms(segs.back().ts);
    const long long dur = (t0 > 0 && t1 > t0) ? (t1 - t0) / 1000 : 0;

    {
        std::ostringstream oss;
        oss << u8"本场会话共记录 " << segs.size() << u8" 条双语片段";
        if (dur > 0) oss << u8"，时间跨度约 " << human_duration(dur);
        oss << u8"。以下内容由规则引擎按时间顺序整理，未做语义改写。";
        s.overview = oss.str();
    }

    // 规则版没有大模型，只能靠关键词粗判场景，判不出来就留空（按"会议"处理）。
    // 大模型版本会直接给出 content_type，比这里准得多。
    {
        int cls = 0, mtg = 0;
        for (const auto& s : segs) {
            const std::string hay = s.src_text + " " + s.tgt_text;
            if (hay.find(u8"课") != std::string::npos ||
                hay.find("lesson") != std::string::npos ||
                hay.find("lecture") != std::string::npos) ++cls;
            if (hay.find(u8"会议") != std::string::npos ||
                hay.find("meeting") != std::string::npos) ++mtg;
        }
        if (cls >= 2 && cls > mtg)      s.content_type = u8"课程";
        else if (mtg >= 2)              s.content_type = u8"会议";
    }

    // --- 要点：按时间分桶，每桶给标题与中文译文条目 ---
    const size_t kBucket = 8;   // 每 8 条一段
    for (size_t i = 0; i < segs.size(); i += kBucket) {
        TopicSummary topic;
        const size_t end = std::min(i + kBucket, segs.size());
        {
            std::ostringstream oss;
            oss << u8"时段 " << (i / kBucket + 1) << u8"（" << segs[i].ts << u8"）";
            topic.title = oss.str();
        }
        for (size_t k = i; k < end; ++k) {
            const std::string& line = segs[k].tgt_text.empty() ? segs[k].src_text
                                                               : segs[k].tgt_text;
            topic.points.push_back(utf8_prefix(line, 60));
        }
        s.topics.push_back(std::move(topic));
    }

    // --- 行动项：命中动作词则收录 ---
    for (const auto& seg : segs) {
        const std::string low = lower_ascii(seg.src_text);
        const bool hit_en = contains_any(low, action_markers_en());
        const bool hit_zh = contains_any(seg.src_text, action_markers_zh());
        if (!hit_en && !hit_zh) continue;

        ActionItem a;
        a.seq    = seg.seq;
        a.owner  = find_owner(seg.src_text);
        a.due    = find_due(seg.src_text);
        a.task   = seg.tgt_text.empty() ? seg.src_text : seg.tgt_text;
        a.source = seg.src_text;
        s.actions.push_back(std::move(a));
    }

    return s;
}

// ===============================================================
// 行动项可信度校验
//
// 为什么要有它：行动项来自两条路——大模型抽取（云端/本地）和规则抽取。
// 大模型会犯两类错，而且**改 prompt 治不住**（prompt 是祈祷，不是保证）：
//   ① 把过场语/寒暄/主观评价当成待办
//   ② 给条目凭空填一个截止日期
// 所以这里用纯规则兜底，跑在渲染之前、所有后端的汇合点上。
//
// 设计取向：**宁缺勿滥**。一条假作业比没有作业更糟——用户会当真。
// 因此这里宁可漏杀也不错杀，白名单（严格动作词）只收能落地执行的动作。
// ===============================================================

int DeliverableWriter::sanitize_actions(std::vector<ActionItem>& actions,
                                       std::vector<std::string>* dropped,
                                       const std::vector<Segment>* transcript) {
    std::vector<ActionItem> kept;
    kept.reserve(actions.size());
    int n_dropped = 0;

    // 整场转录的归一化文本，只算一次。
    // 几百段、每条行动项都重算一遍是浪费，而这段代码跑在导出路径上，用户正等着结果。
    std::string transcript_canon;
    if (transcript != nullptr) {
        for (const auto& s : *transcript) {
            transcript_canon += canonicalize_date(s.src_text);
            transcript_canon += ' ';
            transcript_canon += canonicalize_date(s.tgt_text);
            transcript_canon += ' ';
        }
    }

    for (auto& a : actions) {
        const std::string norm = normalize_action_text(a.task);
        std::string reason;

        // R1 内容过短：4 个字符以下不可能是一条待办
        if (utf8_count(norm) < 4) {
            reason = u8"内容过短";
        }

        // R2 以过场语/寒暄/主观评价开头
        if (reason.empty()) {
            for (const auto& p : preamble_prefixes()) {
                if (norm.size() >= p.size() && norm.compare(0, p.size(), p) == 0) {
                    reason = u8"以过场语开头（" + p + u8"）";
                    break;
                }
            }
        }

        // R3 没有任何可落地执行的动作词 —— 这一条是主力
        if (reason.empty()) {
            const bool hit_zh = contains_any(a.task, strict_verbs_zh());
            const bool hit_en = contains_word(lower_ascii(a.task), strict_verbs_en());
            if (!hit_zh && !hit_en) reason = u8"没有可执行的动作词";
        }

        if (!reason.empty()) {
            ++n_dropped;
            if (dropped) {
                dropped->push_back(reason + u8"：" + utf8_prefix(a.task, 40));
            }
            continue;   // 丢弃
        }

        // R4 截止日期必须有文本依据（**只针对相对时间**）。
        //
        // 含阿拉伯数字的日期不判定：「10 月 31 日」与「October 31st」这种
        // 跨语言的月份/序数写法差异太大（10 月 vs October、31 日 vs 31st），
        // 靠文本比对必然误杀；而具体日期几乎都是从转录里翻译来的，捏造概率低。
        // 这里只拦「今天 / 下周 / 月底」这类相对时间被凭空造出来的情况。
        //
        // 比对前必须 canonicalize_date() 归一化 —— 否则中文 due 对英文 source
        // 永远对不上，会把正确日期全清空。
        //
        // 【依据从哪来】三处都算依据：
        //   ① 行动项自己的 task 文本（模型可能把日期写进了任务描述）
        //   ② ActionItem.source（规则路径一定有；云端路径**很可能为空**）
        //   ③ **整场转录**（云端走中文任务文本 + 英文转录时，source 回填不上，只有这条能用）
        //
        // 【"无法判断" ≠ "无依据"】三处依据文本全都没有时，**保留**日期。
        // 实测踩过：只查 source，而云端路径 source 为空 → 所有日期被判无依据清空；
        // 正确做法是"没有依据可查时不下判断"，而不是默认判负。
        if (!a.due.empty() &&
            a.due.find_first_of("0123456789") == std::string::npos) {
            const std::string due_c  = canonicalize_date(a.due);
            const std::string task_c = canonicalize_date(a.task);

            const bool in_task = !due_c.empty() && task_c.find(due_c) != std::string::npos;
            const bool in_src  = !due_c.empty() && !a.source.empty() &&
                                 canonicalize_date(a.source).find(due_c) != std::string::npos;
            const bool in_tr   = !due_c.empty() && !transcript_canon.empty() &&
                                 transcript_canon.find(due_c) != std::string::npos;

            const bool have_evidence_text = !a.source.empty() || !transcript_canon.empty();
            if (have_evidence_text && !in_task && !in_src && !in_tr) {
                if (dropped) dropped->push_back(u8"截止日期无依据，已清空：「" + a.due +
                                                u8"」← " + utf8_prefix(a.task, 30));
                a.due.clear();
            }
        }

        kept.push_back(std::move(a));
    }

    actions = std::move(kept);
    return n_dropped;
}

// 元信息
// ===============================================================
SessionMeta DeliverableWriter::make_meta(const SessionInfo& info,
                                         int count,
                                         long long avg_ms,
                                         const std::vector<Segment>& segs) {
    SessionMeta m;
    m.id       = info.id;
    m.engine   = info.engine;
    m.note     = info.note;
    m.count    = count;
    m.avg_ms   = avg_ms;
    m.started_at = info.started_at;
    m.ended_at   = info.ended_at;

    if (!segs.empty()) {
        if (m.started_at.empty()) m.started_at = segs.front().ts;
        if (m.ended_at.empty())   m.ended_at   = segs.back().ts;
        const long long t0 = parse_ts_ms(m.started_at);
        const long long t1 = parse_ts_ms(m.ended_at);
        m.duration_sec = (t0 > 0 && t1 > t0) ? (t1 - t0) / 1000 : 0;
    }
    return m;
}

// ===============================================================
// 渲染
// ===============================================================
namespace {

// 按内容类型决定文档标题与各章节名称。
// 这是"通用助手"在产出上的体现：同一个交互，不同场景给不同的东西。
struct OutputLabels {
    std::string doc_title;
    std::string overview;
    std::string topics;
    std::string actions;
    std::string actions_empty;
    std::string fulltext;
};

OutputLabels labels_for(const std::string& type) {
    if (type.find(u8"课") != std::string::npos || type.find(u8"讲") != std::string::npos) {
        return { u8"课堂笔记", u8"一、本讲内容", u8"二、知识点",
                 u8"三、作业与任务", u8"（本讲没有布置任务）", u8"四、双语全文" };
    }
    if (type.find(u8"对话") != std::string::npos || type.find(u8"交谈") != std::string::npos) {
        return { u8"对话记录", u8"一、谈话内容", u8"二、要点",
                 u8"三、约定与待办", u8"（本次谈话没有明确约定）", u8"四、双语全文" };
    }
    if (type.find(u8"视频") != std::string::npos || type.find(u8"影") != std::string::npos ||
        type.find(u8"播客") != std::string::npos) {
        return { u8"内容笔记", u8"一、内容概述", u8"二、要点",
                 u8"三、提到的待办", u8"（本段内容没有提到待办）", u8"四、双语全文" };
    }
    // 默认按会议处理
    return { u8"会议纪要", u8"一、概述", u8"二、要点",
             u8"三、行动项", u8"（本场未识别到明确行动项）", u8"四、双语全文" };
}

// 标记里的出处单元格：`[#9002·3](#seg-3)`，没有段号时给一个破折号。
//
// 【为什么是链接而不是纯文本】"可跳回段落"这个要求，纯文本做不到 ——
// 读者还是得自己滚到下面找。而 markdown 的 `[文字](#锚点)` 在 GitHub/编辑器里
// 是真能点的。锚点用 `seg-<seq>`，和全文表格里的 `<a id="seg-<seq>">` 对上。
//
// ⚠️ 段号为 0 表示"规则路径没回填上来源"（不是错误，是已知情况）——
// 这种情况下**不编一个出处出来**，写 `—`。编出来的出处比没有出处更坏。
static std::string cite_md(long long sid, int seq) {
    if (seq <= 0) return u8"—";
    evidence::Locator loc;
    loc.session_id = sid;
    loc.seq        = seq;
    return "[" + evidence::format(loc) + "](#seg-" + std::to_string(seq) + ")";
}

// HTML 里的出处：`<a class="cite" href="#seg-3">#9002·3</a>`
static std::string cite_html(long long sid, int seq) {
    if (seq <= 0) return u8"—";
    evidence::Locator loc;
    loc.session_id = sid;
    loc.seq        = seq;
    return "<a class=\"cite\" href=\"#seg-" + std::to_string(seq) + "\">" +
           html_escape(evidence::format(loc)) + "</a>";
}

std::string render_markdown(long long sid,
                            const std::vector<Segment>& segs,
                            const SessionMeta& meta,
                            const MeetingSummary& sum) {
    std::ostringstream o;
    const OutputLabels L = labels_for(sum.content_type);
    o << "# " << L.doc_title << " · 会话 #" << sid << "\n\n";

    o << u8"| 项目 | 内容 |\n|---|---|\n";
    o << u8"| 开始时间 | " << (meta.started_at.empty() ? "-" : meta.started_at) << " |\n";
    o << u8"| 结束时间 | " << (meta.ended_at.empty() ? "-" : meta.ended_at) << " |\n";
    o << u8"| 时长 | " << human_duration(meta.duration_sec) << " |\n";
    o << u8"| 翻译引擎 | " << meta.engine << " |\n";
    o << u8"| 记录条数 | " << meta.count << " |\n";
    o << u8"| 平均延迟 | " << meta.avg_ms << " ms |\n";
    o << u8"| 摘要来源 | " << sum.generator << " |\n";
    if (!meta.note.empty()) o << u8"| 备注 | " << meta.note << " |\n";
    o << "\n";

    o << "## " << L.overview << "\n\n" << sum.overview << "\n\n";

    o << "## " << L.topics << "\n\n";
    if (sum.topics.empty()) {
        o << u8"_（无）_\n\n";
    } else {
        for (const auto& t : sum.topics) {
            o << "### " << t.title << "\n\n";
            for (const auto& p : t.points) o << "- " << p << "\n";
            o << "\n";
        }
    }

    o << "## " << L.actions << "\n\n";
    if (sum.actions.empty()) {
        o << "_" << L.actions_empty << "_\n\n";
    } else {
        // 【出处这一列从"只写段号"改成"会话号·段号 + 可点链接"】
        // 原来写的是 `#3` —— 在一份**跨会话**的周报里，`#3` 指的是哪一场的第 3 段？
        // 每个会话都有一模一样的段号，所以那种引用在跨会话场景下**根本没法核对**。
        // 格式统一由 evidence::format 出，且能点回下面的全文表格。
        o << u8"| # | 负责人 | 事项 | 截止 | 出处 |\n|---|---|---|---|---|\n";
        for (size_t i = 0; i < sum.actions.size(); ++i) {
            const auto& a = sum.actions[i];
            o << "| " << (i + 1)
              << " | " << (a.owner.empty() ? u8"—" : md_cell(a.owner))
              << " | " << md_cell(a.task)
              << " | " << (a.due.empty() ? u8"—" : md_cell(a.due))
              << " | " << md_cell(cite_md(sid, a.seq))
              << " |\n";
        }
        o << "\n";
    }

    o << "## " << L.fulltext << "\n\n";
    if (segs.empty()) {
        o << u8"_（无）_\n";
    } else {
        // 行首加 `<a id="seg-N">` 锚点：上面"出处"列里点一下就跳到这里。
        // **锚点必须和出处用同一个段号来源**（都是 s.seq）——
        // 一个用数组下标、一个用 seq 的话，在段号不连续时会静默跳错行。
        o << u8"| # | 时间 | 原文 | 译文 | 引擎 | 耗时 |\n|---|---|---|---|---|---|\n";
        for (const auto& s : segs) {
            o << "| <a id=\"seg-" << s.seq << "\"></a>" << s.seq
              << " | " << md_cell(s.ts)
              << " | " << md_cell(s.src_text)
              << " | " << md_cell(s.tgt_text)
              << " | " << md_cell(s.engine)
              << " | " << s.ms << "ms |\n";
        }
    }

    o << u8"\n---\n\n_由 AudioTranslator 自动生成。_\n";
    return o.str();
}

std::string render_html(long long sid,
                        const std::vector<Segment>& segs,
                        const SessionMeta& meta,
                        const MeetingSummary& sum) {
    std::ostringstream o;
    o << "<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n"
      << "<meta charset=\"utf-8\">\n"
      << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
      << "<title>" << u8"会议纪要 · 会话 #" << sid << "</title>\n"
      << "<style>\n"
      << ":root{--fg:#1a1a1a;--muted:#6b7280;--line:#e5e7eb;--accent:#2563eb;--bg:#fff;--soft:#f9fafb}\n"
      << "*{box-sizing:border-box}\n"
      << "body{margin:0;padding:40px 20px;background:var(--soft);color:var(--fg);"
         "font:15px/1.75 -apple-system,'Segoe UI','Microsoft YaHei',sans-serif}\n"
      << ".wrap{max-width:960px;margin:0 auto;background:var(--bg);border:1px solid var(--line);"
         "border-radius:12px;padding:40px 44px;box-shadow:0 1px 3px rgba(0,0,0,.06)}\n"
      << "h1{margin:0 0 4px;font-size:24px}\n"
      << "h2{margin:36px 0 12px;font-size:18px;padding-bottom:8px;border-bottom:2px solid var(--line)}\n"
      << "h3{margin:22px 0 8px;font-size:15px;color:var(--accent)}\n"
      << "table{width:100%;border-collapse:collapse;margin:12px 0;font-size:14px}\n"
      << "th,td{border:1px solid var(--line);padding:8px 10px;text-align:left;vertical-align:top}\n"
      << "th{background:var(--soft);font-weight:600;white-space:nowrap}\n"
      << "td.num{color:var(--muted);white-space:nowrap;font-variant-numeric:tabular-nums}\n"
      << "ul{margin:8px 0;padding-left:22px}\n"
      << ".meta td:first-child{width:110px;color:var(--muted);white-space:nowrap}\n"
      << ".muted{color:var(--muted)}\n"
      << "footer{margin-top:36px;padding-top:16px;border-top:1px solid var(--line);"
         "color:var(--muted);font-size:13px}\n"
      << "@media print{body{background:#fff;padding:0}.wrap{border:none;box-shadow:none;padding:0}}\n"
      << "</style>\n</head>\n<body>\n<div class=\"wrap\">\n";

    const OutputLabels L = labels_for(sum.content_type);
    o << "<h1>" << html_escape(L.doc_title) << u8" · 会话 #" << sid << "</h1>\n";
    o << "<p class=\"muted\">" << html_escape(meta.engine) << u8" 引擎 · 自动生成</p>\n";

    o << "<table class=\"meta\">\n";
    auto row = [&](const char* k, const std::string& v) {
        o << "<tr><td>" << k << "</td><td>" << (v.empty() ? "-" : html_escape(v)) << "</td></tr>\n";
    };
    row(u8"开始时间", meta.started_at);
    row(u8"结束时间", meta.ended_at);
    row(u8"时长",     human_duration(meta.duration_sec));
    row(u8"记录条数", std::to_string(meta.count));
    row(u8"平均延迟", std::to_string(meta.avg_ms) + " ms");
    row(u8"摘要来源", sum.generator);
    o << "</table>\n";

    o << "<h2>" << html_escape(L.overview) << "</h2>\n<p>" << html_escape(sum.overview) << "</p>\n";

    o << "<h2>" << html_escape(L.topics) << "</h2>\n";
    if (sum.topics.empty()) {
        o << "<p class=\"muted\">" << u8"（无）" << "</p>\n";
    } else {
        for (const auto& t : sum.topics) {
            o << "<h3>" << html_escape(t.title) << "</h3>\n<ul>\n";
            for (const auto& p : t.points) o << "<li>" << html_escape(p) << "</li>\n";
            o << "</ul>\n";
        }
    }

    o << "<h2>" << html_escape(L.actions) << "</h2>\n";
    if (sum.actions.empty()) {
        o << "<p class=\"muted\">" << html_escape(L.actions_empty) << "</p>\n";
    } else {
        o << "<table>\n<tr><th>#</th><th>" << u8"负责人" << "</th><th>" << u8"事项"
          << "</th><th>" << u8"截止" << "</th><th>" << u8"出处" << "</th></tr>\n";
        for (size_t i = 0; i < sum.actions.size(); ++i) {
            const auto& a = sum.actions[i];
            o << "<tr><td class=\"num\">" << (i + 1) << "</td>"
              << "<td>" << (a.owner.empty() ? u8"—" : html_escape(a.owner)) << "</td>"
              << "<td>" << html_escape(a.task) << "</td>"
              << "<td>" << (a.due.empty() ? u8"—" : html_escape(a.due)) << "</td>"
              << "<td class=\"num\">" << cite_html(sid, a.seq) << "</td></tr>\n";
        }
        o << "</table>\n";
    }

    o << "<h2>" << html_escape(L.fulltext) << "</h2>\n";
    if (segs.empty()) {
        o << "<p class=\"muted\">" << u8"（无）" << "</p>\n";
    } else {
        o << "<table>\n<tr><th>#</th><th>" << u8"时间" << "</th><th>" << u8"原文"
          << "</th><th>" << u8"译文" << "</th><th>" << u8"耗时" << "</th></tr>\n";
        for (const auto& s : segs) {
            // `id="seg-N"` 是上面"出处"列的落点 —— 点一下直接跳到这一行。
            // ⚠️ 用 `s.seq` 而不是循环下标：段号不连续时（清洗过、或跨场合并过）
            //    下标和 seq 会对不上，于是"点过去跳到隔壁一行"，
            //    而这种错**没人会怀疑是 bug**，只会以为"记错了"。
            o << "<tr id=\"seg-" << s.seq << "\"><td class=\"num\">" << s.seq << "</td>"
              << "<td class=\"num\">" << html_escape(s.ts) << "</td>"
              << "<td>" << html_escape(s.src_text) << "</td>"
              << "<td>" << html_escape(s.tgt_text) << "</td>"
              << "<td class=\"num\">" << s.ms << "ms</td></tr>\n";
        }
        o << "</table>\n";
    }

    o << "<footer>" << u8"由 AudioTranslator 自动生成 · 全部内容在本机处理，未上传云端"
      << "</footer>\n</div>\n</body>\n</html>\n";
    return o.str();
}

// CSV 也要带上会话号 —— 这个文件最常见的用法是**几场的 CSV 拼起来看**，
// 只写段号的话拼完之后就分不清哪一行出自哪一场了。
std::string render_actions_csv(long long sid, const MeetingSummary& sum) {
    std::ostringstream o;
    o << u8"序号,负责人,事项,截止,出处,来源原文\r\n";
    for (size_t i = 0; i < sum.actions.size(); ++i) {
        const auto& a = sum.actions[i];
        evidence::Locator loc;
        loc.session_id = sid;
        loc.seq        = a.seq;
        o << (i + 1) << ','
          << csv_escape(a.owner)  << ','
          << csv_escape(a.task)   << ','
          << csv_escape(a.due)    << ','
          << csv_escape(loc.valid() ? evidence::format(loc) : std::string())
          << ','
          << csv_escape(a.source) << "\r\n";
    }
    return o.str();
}

std::string render_srt(const std::vector<Segment>& segs) {
    std::ostringstream o;
    if (segs.empty()) return o.str();

    const long long base = parse_ts_ms(segs.front().ts);

    for (size_t i = 0; i < segs.size(); ++i) {
        const long long cur = parse_ts_ms(segs[i].ts);
        long long next = (i + 1 < segs.size()) ? parse_ts_ms(segs[i + 1].ts) : -1;

        long long start = (base > 0 && cur > 0) ? cur - base : static_cast<long long>(i) * 3000;
        long long end   = (base > 0 && next > 0) ? next - base : start + 3000;

        // 时间戳只有秒级分辨率时，同秒内的多条会退化成零长字幕，补一个最小时长
        if (end - start < 800) end = start + 800;
        if (end - start > 15000) end = start + 15000;   // 长静默不要撑满整屏

        o << (i + 1) << "\r\n"
          << srt_time(start) << " --> " << srt_time(end) << "\r\n"
          << segs[i].src_text << "\r\n";
        if (!segs[i].tgt_text.empty()) o << segs[i].tgt_text << "\r\n";
        o << "\r\n";
    }
    return o.str();
}

}  // namespace

// ===============================================================
// 写盘
// ===============================================================
DeliverableWriter::Result DeliverableWriter::write(long long session_id,
                                                   const std::vector<Segment>& segs,
                                                   const SessionMeta& meta,
                                                   const MeetingSummary& summary,
                                                   const std::string& out_root) {
    Result r;
    std::error_code ec;

    // 兜底：本函数自己再过一遍行动项校验。
    //
    // 校验的主调用点在 main.cpp（那里要打印"丢弃了几条"的日志），但**只放在那里不够**——
    // 以后谁直接调 write()，假待办就会原样写进交付物。
    // ActionItem 归本类所有，所以这个不变量就该由本类自己保证。
    // sanitize_actions 幂等（第二遍没有可改的），重复调用无副作用。
    MeetingSummary safe = summary;
    sanitize_actions(safe.actions, nullptr, &segs);
    const MeetingSummary& sum = safe;

    const fs::path dir = fs::path(out_root) / ("session-" + std::to_string(session_id));
    fs::create_directories(dir, ec);
    if (ec) {
        r.error = std::string(u8"创建输出目录失败: ") + ec.message();
        return r;
    }
    r.dir = dir.string();

    const std::string base = "meeting-" + std::to_string(session_id);

    struct Item {
        fs::path    path;
        std::string content;
        bool        bom;
    };
    const Item items[] = {
        {dir / (base + ".md"),  render_markdown(session_id, segs, meta, sum), false},
        {dir / (base + ".html"), render_html(session_id, segs, meta, sum),    false},
        {dir / "actions.csv",    render_actions_csv(session_id, sum),        true},
        {dir / "transcript.srt", render_srt(segs),                            false},
    };

    for (const auto& it : items) {
        if (!write_file(it.path, it.content, it.bom)) {
            r.error = std::string(u8"写入失败: ") + it.path.string();
            return r;
        }
        r.files.push_back(it.path.string());
    }

    r.ok = true;
    return r;
}
