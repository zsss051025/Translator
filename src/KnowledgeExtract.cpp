#include "KnowledgeExtract.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace knowledge {

namespace {

// ---------------------------------------------------------------
// 词表：**触发表与判定表分开**（§8.3）
//
// 这张表是"永远不算专名"的**判定**表 —— 放错一个词的代价是漏掉一个真专名，
// 所以只收那些几乎不可能当专名的：功能词、问候语、语气词、常见句首词。
// 往这里加词要谨慎；想把"可能不是专名"的东西也过滤掉是另一回事（那属于宁滥勿缺，与本项目取向相反）。
// ---------------------------------------------------------------
bool is_never_name(const std::string& low) {
    static const char* kW[] = {
        // 功能词 / 代词 / 助动词
        "i", "a", "an", "the", "and", "or", "but", "so", "if", "in", "on", "at", "to",
        "for", "of", "with", "by", "from", "as", "is", "are", "was", "were", "be",
        "been", "being", "do", "does", "did", "have", "has", "had", "will", "would",
        "shall", "should", "can", "could", "may", "might", "must", "not", "no", "yes",
        "this", "that", "these", "those", "it", "its", "he", "she", "they", "them",
        "we", "you", "your", "my", "his", "her", "our", "their", "me", "us", "him",
        "what", "when", "where", "which", "who", "whom", "why", "how", "there", "here",
        "all", "some", "any", "every", "each", "both", "few", "more", "most", "other",
        "such", "than", "then", "too", "very", "just", "only", "also", "now", "up",
        "out", "about", "into", "over", "after", "before", "again", "because", "while",
        "one", "two", "three", "first", "second", "next", "last", "let", "lets", "get",
        "got", "go", "going", "gone", "come", "came", "see", "saw", "know", "knew",
        "think", "thought", "say", "said", "tell", "told", "give", "gave", "take",
        "took", "make", "made", "look", "looks", "want", "need", "like", "well",
        // 问候 / 语气 / 客套
        "hello", "hi", "hey", "bye", "goodbye", "thanks", "thank", "please", "sorry",
        "excuse", "oh", "ah", "uh", "um", "huh", "wow", "yeah", "yep", "nope", "okay",
        "ok", "sure", "right", "great", "good", "nice", "welcome", "morning",
        "afternoon", "evening", "night",
        // 常见的"非专名"首字母大写词：语言 / 国别 / 星期 / 月份（见 .h 里的说明）
        "english", "chinese", "japanese", "korean", "french", "german", "spanish",
        "american", "americans", "british", "european", "asian", "african",
        "monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday",
        "january", "february", "march", "april", "may", "june", "july", "august",
        "september", "october", "november", "december",
    };
    for (const char* w : kW) if (low == w) return true;
    return false;
}

std::string lower_ascii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x80) c = static_cast<char>(std::tolower(u));
    }
    return out;
}

bool is_ascii_word_char(unsigned char c) {
    // 撇号留在词内：O'Brien / I'm 是一个词；当成词边界会把 O 和 Brien 拆开
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') || c == '_' || c == '\'';
}

struct Token {
    std::string text;
    size_t      offset = 0;
    bool        sentence_initial = false;   // 是否是句子的第一个词
};

// 把一段 ASCII 文本切成词，并标出"句首词"。
//
// 【为什么必须标出句首】英文里句首词必然大写，那是**语法**不是**专名**证据。
// 不排除的话，每句话的第一个词都会被抽成专名（实测 jfk.wav 会抽出 Ask / What）。
// 句界判定：文首，或前一个非空白字符是 . ? ! 或换行。
std::vector<Token> tokenize(const std::string& s) {
    std::vector<Token> out;
    char prev_significant = '\0';
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (is_ascii_word_char(c)) {
            const size_t b = i;
            while (i < s.size() && is_ascii_word_char(static_cast<unsigned char>(s[i]))) ++i;
            Token t;
            t.text   = s.substr(b, i - b);
            t.offset = b;
            t.sentence_initial = (prev_significant == '\0' || prev_significant == '.' ||
                                  prev_significant == '?' || prev_significant == '!' ||
                                  prev_significant == '\n');
            out.push_back(std::move(t));
            prev_significant = s[i - 1];
            continue;
        }
        if (c != ' ' && c != '\t' && c != '\r' && c < 0x80) prev_significant = static_cast<char>(c);
        ++i;
    }
    return out;
}

// R1~R3 的**证据**判定：这个词有没有"它是专名"的证据。
//
// 注意这里回答的是**是不是专名**，不是**出现几次** —— 两件事必须分开：
// 句首的 Marco 本身不构成证据（可能只是语法大写），但一旦 "Marco" 在别处
// 以非句首位置出现过，它在句首的那些出现**也应当计入次数**。
// 第一版把两件事混在一起，结果 `Marco`（第 2 段出现在问号后）只算 1 次。
bool has_name_evidence(const Token& t) {
    const std::string& w = t.text;
    if (w.size() < 2 || w.size() > 40) return false;

    const unsigned char first = static_cast<unsigned char>(w[0]);
    if (first < 'A' || first > 'Z') return false;            // 首字母必须大写

    bool all_upper = true, any_lower = false, internal_upper = false, has_apos = false;
    for (size_t i = 0; i < w.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(w[i]);
        if (c == '\'') {
            has_apos = true;
            any_lower = true;
            all_upper = false;
            continue;
        }
        if (c >= 'a' && c <= 'z') { any_lower = true; all_upper = false; }
        else if (c >= 'A' && c <= 'Z') { if (i > 0) internal_upper = true; }
        else if (c >= '0' && c <= '9') { all_upper = false; }
    }
    if (is_never_name(lower_ascii(w))) return false;

    const bool is_acronym = all_upper && !any_lower;                 // R3: API / CRM / TV
    if (is_acronym) return true;
    // R2: 词中间还有大写（EnglishPod）—— 强信号，句首也算
    if (internal_upper) return true;

    // 【含撇号的一律不当"普通首字母大写"】I'm / It's / That's / Don't 全是收缩式，
    // 实测里 `I'm` 被当成了专名并报了 hits=4。
    // 只有"词中间有大写"（O'Brien 的 B）才放行 —— 那才是名字的形状。
    // 注意：这个判断必须放在 R2 之后，否则 O'Brien 会被一起挡掉。
    if (has_apos) return false;

    // R1: 普通首字母大写 —— **句首不算证据**
    return !t.sentence_initial;
}

// R5：把 "my name is X" / "我叫X" 这类模式里的 X 找出来。
//
// 【为什么砍掉了 "I'm X" / "我是X"】实测在真实转录上它们全是假阳性：
// "I'm doing really well" → doing、"I'm really excited" → really。
// 英文里 "I'm <形容词/动词ing>" 比 "I'm <人名>" 常见得多。
// 中文同理："我是说" / "他是对的" 都不是介绍名字。
// 留下的都是**专门用来介绍名字**的句式。
//
// 【英文侧还要求首字母大写】"my name is marco" 这种全小写（识别成小写）
// 会被漏掉 —— 接受这个代价：宁可漏，不可把 "doing" 当人名写进库。
//
// 用回调而不是返回列表：调用方需要同时拿到**原文写法**、**证据段落**和**位置**（去重要用）。
template <typename Fn>
void for_each_person_name(const std::vector<Segment>& segs, Fn fn) {
    static const char* kAsciiPat[] = {
        "my name is ", "my name's ", "this is ", "these are ", "meet ",
    };
    static const char* kCjkPat[] = {
        u8"我叫", u8"名字是", u8"他叫", u8"她叫", u8"这位是",
    };

    for (const auto& seg : segs) {
        // 英文模式：在**小写化副本**上找位置，再回到原文取词（大小写要保真）
        const std::string low = lower_ascii(seg.src_text);
        for (const char* p : kAsciiPat) {
            const std::string pat = p;
            size_t pos = low.find(pat);
            while (pos != std::string::npos) {
                size_t b = pos + pat.size();
                while (b < seg.src_text.size() && seg.src_text[b] == ' ') ++b;
                size_t e = b;
                while (e < seg.src_text.size() &&
                       is_ascii_word_char(static_cast<unsigned char>(seg.src_text[e]))) ++e;
                if (e > b) {
                    const std::string w = seg.src_text.substr(b, e - b);
                    // 必须首字母大写、且不是"永远不当名字"的词
                    const unsigned char c0 = static_cast<unsigned char>(w[0]);
                    if (w.size() >= 2 && c0 >= 'A' && c0 <= 'Z' &&
                        !is_never_name(lower_ascii(w)) && w.find('\'') == std::string::npos) {
                        fn(w, seg, b);
                    }
                }
                pos = low.find(pat, pos + 1);
            }
        }

        // 中文模式：中文没有大小写，只能靠这类明确的人称句式。
        // 取到下一个标点为止，并约束长度（1 个字或一整句都不像名字）。
        for (const char* p : kCjkPat) {
            const std::string pat = p;
            size_t pos = seg.src_text.find(pat);
            while (pos != std::string::npos) {
                const size_t b = pos + pat.size();
                size_t e = b;
                while (e < seg.src_text.size()) {
                    const unsigned char c = static_cast<unsigned char>(seg.src_text[e]);
                    if (c < 0x80) break;                       // ASCII 一律当边界
                    const size_t len = ((c & 0xF0) == 0xE0) ? 3
                                     : (((c & 0xE0) == 0xC0) ? 2 : 1);
                    const std::string cp = seg.src_text.substr(e, len);
                    if (cp == u8"。") break;
                    if (cp == u8"，") break;
                    if (cp == u8"！") break;
                    if (cp == u8"？") break;
                    if (cp == u8"、") break;
                    if (cp == u8"：") break;
                    if (cp == u8"（") break;
                    e += len;
                }
                const size_t nbytes = e - b;
                if (nbytes >= 6 && nbytes <= 18) fn(seg.src_text.substr(b, nbytes), seg, b);
                pos = seg.src_text.find(pat, pos + 1);
            }
        }
    }
}

// R4：引号里的内容（「」《》 和英文双引号）。返回 (内容, 内容起始偏移)。
std::vector<std::pair<std::string, size_t>> find_quoted(const std::string& s) {
    struct Pair { const char* open; const char* close; };
    static const Pair kPairs[] = {
        {u8"「", u8"」"}, {u8"《", u8"》"}, {u8"‘", u8"’"},
        {"\"", "\""},
    };
    std::vector<std::pair<std::string, size_t>> out;
    for (const auto& pr : kPairs) {
        const std::string o = pr.open, c = pr.close;
        size_t pos = 0;
        while ((pos = s.find(o, pos)) != std::string::npos) {
            const size_t b = pos + o.size();
            const size_t e = s.find(c, b);
            if (e == std::string::npos || e == b) { pos = b; continue; }
            const std::string inner = s.substr(b, e - b);
            if (inner.size() >= 2 && inner.size() <= 40) out.emplace_back(inner, b);
            pos = e + c.size();
        }
    }
    return out;
}

}  // namespace

std::vector<ExtractedCandidate> extract_candidates(const std::vector<Segment>& segs,
                                                   long long session_id,
                                                   size_t max_candidates) {
    // ---------------------------------------------------------------
    // 第 1 步：把所有"出现"收集成带位置的原始记录
    //
    // 【为什么必须带位置并去重】第一版按路径各自计数，同一个 Erica 在
    // "My name is Marco. And I'm Erica." 里被**人称模式**和**首字母大写**
    // 各算一次，于是显示成"已经听到 3 次"，而实际只有 2 次。
    // 用户看到的是"已经听到 3 次「Erica」"这种**错数字** —— 宁可少算，不能报错。
    // ---------------------------------------------------------------
    enum PathPrio { kPerson = 0, kQuoted = 1, kCapital = 2 };
    struct Occ {
        std::string value;
        const Segment* seg = nullptr;
        size_t      offset = 0;
        PathPrio    prio   = kCapital;
        const char* why    = "";
    };
    std::vector<Occ> occs;

    for_each_person_name(segs, [&](const std::string& name, const Segment& seg, size_t off) {
        occs.push_back({name, &seg, off, kPerson, u8"人名句式（my name is / 我叫）"});
    });

    // 第 2 步：先确定"哪些词有专名证据"，再统计它们的**全部**出现次数。
    //
    // 证据与计数必须分开：句首的 Marco 不构成证据（可能只是语法大写），
    // 但 "Marco" 一旦在别处以非句首位置出现过，它在句首的那次**也该计入次数**。
    std::vector<std::string> name_keys;
    for (const auto& seg : segs) {
        for (const auto& t : tokenize(seg.src_text)) {
            if (!has_name_evidence(t)) continue;
            const std::string key = normalize_key(t.text);
            if (key.empty()) continue;
            if (std::find(name_keys.begin(), name_keys.end(), key) == name_keys.end()) {
                name_keys.push_back(key);
            }
        }
    }
    for (const auto& seg : segs) {
        for (const auto& t : tokenize(seg.src_text)) {
            const std::string key = normalize_key(t.text);
            if (key.empty()) continue;
            if (std::find(name_keys.begin(), name_keys.end(), key) == name_keys.end()) continue;
            occs.push_back({t.text, &seg, t.offset, kCapital, u8"首字母大写的词"});
        }
        for (const auto& q : find_quoted(seg.src_text)) {
            // 引号里明确是"被讨论的词"，不管大小写都收 —— 这是最强的信号
            occs.push_back({q.first, &seg, q.second, kQuoted, u8"引号里的词"});
        }
    }

    // 第 3 步：按 (段落号, 位置) 去重 —— 同一个词在同一处只算一次。
    // 同一处命中多条路径时，保留**最具体**的那条（人名句式 > 引号 > 首字母大写），
    // 这样条目上的 why 说明的是最可信的理由。
    std::stable_sort(occs.begin(), occs.end(), [](const Occ& a, const Occ& b) {
        if (a.seg->seq != b.seg->seq) return a.seg->seq < b.seg->seq;
        if (a.offset != b.offset)     return a.offset < b.offset;
        return static_cast<int>(a.prio) < static_cast<int>(b.prio);
    });

    struct Agg {
        std::map<std::string, int> spelling_count;   // 原始拼写 -> 次数
        int         hits = 0;
        double      min_conf = 2.0;                  // 取最低识别置信度（保守）
        long long   session = -1;
        int         seq = -1;
        std::string source_text;
        std::string why;
    };
    std::map<std::string, Agg> agg;
    std::vector<std::string> person_keys;
    std::string last_pos_key;                        // 去重游标：段落号 + 位置

    for (const auto& o : occs) {
        const std::string key = normalize_key(o.value);
        if (key.empty()) continue;
        if (o.value.size() < 2 || o.value.size() > 40) continue;

        char pos_buf[64];
        std::snprintf(pos_buf, sizeof(pos_buf), "%lld:%zu",
                      static_cast<long long>(o.seg->seq), o.offset);
        const std::string pos_key = pos_buf;
        if (pos_key == last_pos_key) continue;       // 同一处的第二条路径 → 丢掉
        last_pos_key = pos_key;

        auto& a = agg[key];
        a.spelling_count[o.value] += 1;
        a.hits += 1;
        if (o.seg->confidence >= 0.0 && o.seg->confidence < a.min_conf) {
            a.min_conf = o.seg->confidence;
        }
        if (a.source_text.empty()) {
            a.session     = session_id;
            a.seq         = o.seg->seq;
            a.source_text = o.seg->src_text;
            a.why         = o.why;
        }
        if (o.prio == kPerson &&
            std::find(person_keys.begin(), person_keys.end(), key) == person_keys.end()) {
            person_keys.push_back(key);
        }
    }

    std::vector<ExtractedCandidate> out;
    for (auto& kv : agg) {
        auto& a = kv.second;
        // 取出现最多的拼写（并列时取字典序最小 → 结果稳定可复现）
        std::string best;
        int best_n = -1;
        for (const auto& sc : a.spelling_count) {
            if (sc.second > best_n || (sc.second == best_n && sc.first < best)) {
                best_n = sc.second;
                best   = sc.first;
            }
        }
        ExtractedCandidate c;
        c.key            = kv.first;
        c.value          = best;
        c.hits           = a.hits;
        c.confidence     = (a.min_conf > 1.0) ? 0.7 : a.min_conf;   // 没有置信度时给个中位值
        c.source_session = a.session;
        c.source_seq     = a.seq;
        c.source_text    = a.source_text;
        c.why            = a.why;
        c.kind = (std::find(person_keys.begin(), person_keys.end(), kv.first) != person_keys.end())
                     ? "person" : "term";
        out.push_back(std::move(c));
    }

    // 排序：出现次数多的在前；同次数按键字典序（稳定，便于自检与复现）
    std::stable_sort(out.begin(), out.end(),
                     [](const ExtractedCandidate& a, const ExtractedCandidate& b) {
                         if (a.hits != b.hits) return a.hits > b.hits;
                         return a.key < b.key;
                     });
    if (out.size() > max_candidates) out.resize(max_candidates);
    return out;
}

int save_candidates(const std::vector<ExtractedCandidate>& cands, std::string* err) {
    int saved = 0;
    for (const auto& c : cands) {
        KnowledgeItem item;
        item.kind           = c.kind;
        item.key            = c.key;
        item.value          = c.value;
        // **一律候选**：§6.4① 自动抽取不需要用户参与，所以它没有资格直接进 Confirmed。
        // 这一行是 §6.5 红线在写入侧的体现 —— 猜出来的东西必须等用户点头。
        item.status         = "candidate";
        item.confidence     = c.confidence;
        // 【必须传 hits】抽取器数出来的"这场听到几次"就是这里唯一的值来源。
        // 漏了这一行，upsert 只能按 0 处理 → 一律记 1：
        //   · 问题里"已经听到 N 次"是假数字
        //   · hits >= 3 那条规则一场会话内永远不为真
        // （这是真实会话 #43 上抓到的：--extract 说 2 次，库里查到 1 次。）
        item.hits           = c.hits;
        item.source_session = c.source_session;
        item.source_seq     = c.source_seq;
        item.source_text    = c.source_text;

        std::string e;
        if (KnowledgeStore::instance().upsert(item, &e) > 0) {
            ++saved;
        } else if (err && err->empty()) {
            *err = e;
        }
    }
    return saved;
}

}  // namespace knowledge
