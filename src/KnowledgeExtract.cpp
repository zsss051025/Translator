#include "KnowledgeExtract.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>   // 报告动词的佐证回调
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

// 一颗 token 提供的"证据类型"
enum class Ev {
    None,      // 不提供任何证据
    WeakCap,   // 只是"首字母大写 + 不在句首" —— 这个信号**太弱**，需要再佐证
    StrongShape,  // 形状本身就是强信号：CamelCase / 全大写缩写
};

// R1~R3 判定一颗 token 是哪种证据。
//
// 【为什么要把"弱证据"单独分出来 —— 真实数据给的教训】
// 会话 #47（真实播客，53 段）里，只靠"首字母大写 + 非句首"抽出了 8 个假阳性：
//     down(6) Keep(4) Midnight movies(3) Speaking Learners Exactly preview
// 它们全是普通词，大写只是因为**被当作词汇标题写**（`Keep It Down.`）、
// 或者标题式大写（`Speaking of Movies`）、或者称呼语（`Hello English Learners`）。
//
// 而英文里"句中的普通词被大写"太常见了 —— 所以 **R1 单独用一定会滥**。
// 和中文 R6 一样：弱证据必须再有一条佐证才算数（见 qualifies()）。
Ev classify_token(const Token& t) {
    const std::string& w = t.text;
    if (w.size() < 2 || w.size() > 40) return Ev::None;

    const unsigned char first = static_cast<unsigned char>(w[0]);
    if (first < 'A' || first > 'Z') return Ev::None;          // 首字母必须大写

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
    if (is_never_name(lower_ascii(w))) return Ev::None;

    // R3：全大写 = 缩写形状（API / CRM / TV / PC）。
    //
    // ⚠️ **必须限长。** 真实数据给的教训（用户真跑会话 #13）：
    // 播客的小标题是整行大写 —— `PUTTING IT TOGETHER` —— 里面 8 个字母的
    // `TOGETHER` 满足"连续 ≥2 个大写"，于是被当成缩写抽出来，
    // 还被问了一句「第一次听到「TOGETHER」。这个词的写法对吗？」
    // 真缩写没有超过 5 个字母的（CEO/CFO/KPI/OKR/SLA/SDK/HR/AI 全在 4 以内），
    // 而"被大写强调的普通词"可以有任意长度 —— 长度就是这两者的分界。
    constexpr size_t kMaxAcronymLen = 5;
    if (all_upper && !any_lower && w.size() <= kMaxAcronymLen) return Ev::StrongShape;
    if (internal_upper)          return Ev::StrongShape;      // R2: EnglishPod

    // 【含撇号的一律不当"普通首字母大写"】I'm / It's / That's 全是收缩式，
    // 实测 `I'm` 被当成过专名。只有"词中间有大写"（O'Brien 的 B）才放行 ——
    // 那才是名字的形状。这个判断必须在 R2 之后。
    if (has_apos) return Ev::None;

    // 句首大写是**语法**不是证据 —— 但也不算"小写出现"（那两件事不能混）
    if (t.sentence_initial) return Ev::None;

    return Ev::WeakCap;                                       // R1，需要佐证
}

// 整段都是大写吗？—— 是的话这段是**标题行**，里面的大写一个都不算证据。
//
// 【真实来源】用户真跑会话 #13，段 98 是播客的小标题 `PUTTING IT TOGETHER`。
// 整行大写是**排版**，不是专名的形状：同一个词在正文里就是普通小写词。
// 所以标题行里"某词大写"这件事提供的信息量是**零**，应当整段作废，
// 而不是逐个词去猜哪个像专名。
//
// 要求 ≥2 个词：单个词的段（`OKAY`）可能真的是在喊一个名字，
// 或者就是个缩写，信息不足，不据此整段作废 —— 交给别的判据处理。
bool is_all_caps_heading(const std::string& s) {
    int words = 0, upper_words = 0;
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const bool alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        if (!alpha) { ++i; continue; }
        const size_t b = i;
        while (i < s.size()) {
            const unsigned char d = static_cast<unsigned char>(s[i]);
            if ((d >= 'A' && d <= 'Z') || (d >= 'a' && d <= 'z') || d == '\'') ++i;
            else break;
        }
        const std::string word = s.substr(b, i - b);
        ++words;
        bool all_upper = true;
        for (const unsigned char d : word) {
            if (d >= 'a' && d <= 'z') { all_upper = false; break; }
        }
        if (all_upper) ++upper_words;
    }
    return words >= 2 && upper_words == words;
}

// 一颗 token 是不是"全小写出现的普通词"。
//
// 【这条判据为什么有说服力】真专名在一场会话里不会又大写又小写：
//   实测 #47：`down` 大写 2 次、**小写 4 次**；`movies` 大写 1 次、**小写 2 次**
//   —— 这两个直接靠这条就挡掉了，不需要任何词表。
bool looks_lowercase_word(const Token& t) {
    const std::string& w = t.text;
    if (w.size() < 2 || w.size() > 40) return false;
    const unsigned char c = static_cast<unsigned char>(w[0]);
    return c >= 'a' && c <= 'z';
}

// R5：把 "my name is X" / "我叫X" 这类模式里的 X 找出来。
//
// 【"I'm X" 加回来了，而且这次是安全的】当初砍掉它是因为实测抽出
// `doing` / `really`（"I'm doing really well"）。后来加了"后面那个词必须
// 首字母大写"这道闸，于是 "I'm doing" 自动被挡、"I'm Erica" 保留 ——
// 而 Erica 这种"只出现在句首"的名字正需要它（#47 里 Erica 的非句首出现只有 2 次，
// 靠频率门槛刚好够，但没有余量）。
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
        // 【"i'm " 回来了，但这次安全了】当初删掉它是因为实测抽出 `doing`/`really`
        // （"I'm doing really well"）。后来加了"后面那个词必须首字母大写"这道闸
        // （见下面的 c0 >= 'A' && c0 <= 'Z'），于是 "I'm doing" 自动被挡、
        // "I'm Erica" 保留 —— 而 Erica 这种"只出现在句首"的名字正需要它。
        "my name is ", "my name's ", "this is ", "these are ", "meet ", "i'm ",
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

// ===============================================================
// 中文专名抽取（R6 ~ R8）
//
// 【为什么单独一套规则】中文**没有大小写、没有词边界** —— 英文那套
// （首字母大写 / CamelCase / 缩写）在中文里一条都用不上。
// 结果就是：中文会议里"张伟负责下周的报价"这种句子，**一个候选都抽不出来**，
// 于是知识库不长 → 没问题可问 → 三条腿没输入 → "越用越懂你"在中文场景下不成立。
//
// 【取向：靠强信号，不靠分词】
// 真正的中文分词需要词典 + 统计模型，那是一条大工程，而且会引入一堆依赖。
// 这里改用两条**结构性**的强信号，误报率低得多：
//   R6 人名：姓氏字 + 佐证。光看"张伟"两个字没法判断是不是人名
//           （"张开""明显"都能凑出来），所以**必须**有佐证才算数：
//             ① 后面跟称谓：张总 / 李经理 / 王老师   ← 最强的信号
//             ② 后面跟动词：张伟负责 / 李总说 / 王工提出
//   R7 机构/项目名：后缀是强信号 —— "凤凰项目""星辰科技"里的"项目""科技"
//           几乎不可能出现在普通短语里。反过来取后缀前面 2~8 个字即可。
//
// **宁可漏，不可滥**：这里只做这两条。中文人名如果没带称谓也没跟动词，
// 就是抽不到 —— 那属于漏，代价只是少记一条；而抽错会写进知识库、
// 进而进识别提示和翻译约束（§6.5 红线防的就是这个）。
// ===============================================================

struct CjkChar {
    size_t   off = 0;   // 在原字符串里的字节偏移
    size_t   len = 1;   // 占几个字节
    uint32_t cp  = 0;
};

size_t utf8_len_of(unsigned char c) {
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    if ((c & 0xE0) == 0xC0) return 2;
    return 1;
}

uint32_t utf8_cp(const std::string& s, size_t i, size_t len) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (len == 1) return c;
    if (len == 2) return ((c & 0x1F) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F);
    if (len == 3) return ((c & 0x0F) << 12) |
                        ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
                         (static_cast<unsigned char>(s[i + 2]) & 0x3F);
    return ((c & 0x07) << 18) |
           ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 12) |
           ((static_cast<unsigned char>(s[i + 2]) & 0x3F) << 6) |
            (static_cast<unsigned char>(s[i + 3]) & 0x3F);
}

// 汉字（含扩展 A 与兼容区）。标点、假名、ASCII 都不算 —— 它们同时充当**词边界**。
bool is_cjk_ideograph(uint32_t cp) {
    return (cp >= 0x3400 && cp <= 0x4DBF) ||    // 扩展 A
           (cp >= 0x4E00 && cp <= 0x9FFF) ||    // 基本区
           (cp >= 0xF900 && cp <= 0xFAFF);      // 兼容汉字
}

// 把文本切成"连续的汉字串"。标点、ASCII、空格都是断点。
// 这相当于一个"极简分词"：不做词切分，只把汉字和符号分开。
std::vector<std::vector<CjkChar>> cjk_runs(const std::string& s) {
    std::vector<std::vector<CjkChar>> runs;
    std::vector<CjkChar> cur;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const size_t len = utf8_len_of(c);
        if (i + len > s.size()) break;
        const uint32_t cp = utf8_cp(s, i, len);
        if (is_cjk_ideograph(cp)) {
            cur.push_back({i, len, cp});
        } else if (!cur.empty()) {
            runs.push_back(std::move(cur));
            cur.clear();
        }
        i += len;
    }
    if (!cur.empty()) runs.push_back(std::move(cur));
    return runs;
}

// 百家姓（按人口的常见单字姓）+ 常见复姓。
// 收得广一点：漏掉一个姓 = 那个人名永远抽不到；而多收一个字的代价，
// 会被后面"必须有佐证"那一步挡掉大半。
bool is_surname_char(uint32_t cp) {
    static const char* kSurnames =
        u8"王李张刘陈杨黄赵吴周徐孙马朱胡郭何高林罗郑梁谢宋唐许韩冯邓曹彭曾萧田董袁潘于蒋蔡余杜叶程苏魏吕丁任沈姚卢姜崔钟谭陆汪范金石廖贾夏韦付方白邹孟熊秦邱江尹薛闫段雷侯龙史陶黎贺顾毛郝龚邵万钱严赖覃洪武莫孔汤向常温康施文牛樊葛邢安齐易乔伍庞颜倪庄聂章鲁岳翟殷詹申欧耿关兰焦俞左柳甘祝包宁尚符舒阮柯纪梅童凌毕单季裴霍涂成苗谷盛曲翁冉骆蓝路游辛靳管柴蒙鲍华喻祁蒲房滕屈饶解牧艾尤阳时穆农司卓古吉缪简车项连芦麦褚娄窦戚岑景党宫费卜冷晏席卫米柏宗瞿桂全佟应臧闵苟邬边卞姬邰盖";
    for (size_t i = 0; i < std::strlen(kSurnames);) {
        const size_t len = utf8_len_of(static_cast<unsigned char>(kSurnames[i]));
        if (utf8_cp(kSurnames, i, len) == cp) return true;
        i += len;
    }
    return false;
}

// 称谓 —— 最强的"这是个人名"信号。"张总""李经理"里的称谓不可能出现在别的地方。
bool is_title_start(const char* p) {
    static const char* kTitles[] = {
        // 【为什么有单字"工"】工程/技术团队里"王工""李工"是标准叫法，
        // 而这类会议正是本产品的主场 —— 漏掉它等于漏掉一大类人名。
        // 代价是"施"是姓氏、"工"是称谓 → "施工"会被误判成人名，
        // 所以 is_false_person_word 里有"施工"兜着（见下面的说明）。
        u8"工",
        u8"总", u8"经理", u8"老师", u8"主任", u8"医生", u8"教授", u8"博士",
        u8"总监", u8"主管", u8"组长", u8"校长", u8"院长", u8"部长", u8"处长",
        u8"科长", u8"队长", u8"律师", u8"记者", u8"老板", u8"先生", u8"女士",
        u8"同学", u8"同事", u8"工程师", u8"设计师",
    };
    for (const char* t : kTitles) if (std::strcmp(p, t) == 0) return true;
    return false;
}

// 佐证动词 —— 人名后面跟着这些，说明前面那个词是"做这件事的人"。
// 只收指向性明确的：负责/提到/建议/认为…；**不收** 来/去/让/找 这种太泛的。
//
// ⚠️ 这里是**前缀匹配**，不是全等 —— 传进来的是"后面剩下的整串"
//（如"负责下周的报价"）。用 strcmp 全等，就只有动词正好落在串尾时才匹配得上，
// 「张伟负责下周的报价」里的张伟会完全抽不到。
// **这个 bug 是中文用例抓出来的**（张伟漏了，而「那边的反馈」里的"边的"反而被抽到 ——
// 因为那里"反馈"正好是串尾）。
bool is_person_verb_start(const char* p) {
    static const char* kVerbs[] = {
        u8"负责", u8"提到", u8"建议", u8"认为", u8"同意", u8"表示", u8"提出",
        u8"补充", u8"强调", u8"回复", u8"确认", u8"安排", u8"跟进", u8"参加",
        u8"主持", u8"汇报", u8"反馈", u8"接手", u8"推动", u8"介绍", u8"邀请",
        u8"通知", u8"告诉", u8"说", u8"问", u8"答", u8"做",
    };
    for (const char* v : kVerbs) {
        if (std::strncmp(p, v, std::strlen(v)) == 0) return true;
    }
    return false;
}

// 机构 / 项目 / 产品的后缀。
//
// 【分成两档，因为精度差很多】
//   强后缀：公司/集团/科技/大学… —— 几乎只出现在专名后面，命中即可报
//   弱后缀：项目/系统/平台 —— 很常见也很宽泛（"交付项目""这个系统"），
//          所以额外要求**本场出现 ≥2 次**才报（见 CjkHit::needs_repeat）
//
// 刻意**不收** 计划 / 方案 / 产品 / 版本 / 部门 / 团队 / 中心 ——
// 它们和普通名词的重合度太高，实测会把「交付计划」这种当成专名。
// 报错了的代价不是"多一条数据"，而是**用户被问一个蠢问题**（§1.3 提问是稀缺资源）。
bool is_org_suffix_start(const char* p, bool* weak) {
    static const char* kStrong[] = {
        u8"公司", u8"集团", u8"科技", u8"网络", u8"传媒", u8"软件",
        u8"大学", u8"学院", u8"医院", u8"银行", u8"基金",
        u8"研究院", u8"实验室", u8"事业部", u8"工作室",
    };
    static const char* kWeak[] = {
        u8"项目", u8"系统", u8"平台",
    };
    for (const char* s : kStrong) {
        if (std::strcmp(p, s) == 0) { if (weak) *weak = false; return true; }
    }
    for (const char* s : kWeak) {
        if (std::strcmp(p, s) == 0) { if (weak) *weak = true; return true; }
    }
    return false;
}

// 虚词 —— 在汉字串里当**临时词边界**用。
//
// 【为什么需要】中文没有空格，"凤凰项目"的左边到底取几个字？
// 取最长的会把一整句话吃进来（实测「下个季度的交付计划」被当成专名）。
// 所以从后缀往左扫，**碰到虚词就停**，只取它右边那一小段：
//     下 个 季 度 的 交 付 项 目
//                    ↑ 的 是虚词 → 左边只取「交付」
// 这样至少不会把整句吃进去。
bool is_function_char(uint32_t cp) {
    static const char* kFn =
        u8"的了在是和与就都也还要会能把被对给从到向为因所以这那个我你他她它们上下里外"
        u8"之其并且但而或则于把让使"; 
    for (size_t i = 0; i < std::strlen(kFn);) {
        const size_t len = utf8_len_of(static_cast<unsigned char>(kFn[i]));
        if (utf8_cp(kFn, i, len) == cp) return true;
        i += len;
    }
    return false;
}

// 后缀左边的开头如果是这些，说明是泛指而不是专名："这个项目""那个系统"。
bool is_generic_prefix(const char* p) {
    static const char* kBad[] = {
        u8"这个", u8"那个", u8"一个", u8"每个", u8"某个", u8"哪个", u8"第一",
        u8"第二", u8"第三", u8"我们", u8"你们", u8"他们", u8"咱们", u8"各种",
        u8"新的", u8"老的", u8"大的", u8"小的", u8"主要", u8"相关", u8"以上",
        u8"以下", u8"此项", u8"该项", u8"本项", u8"整个", u8"全部", u8"任何",
        u8"什么", u8"怎么", u8"为了", u8"因为", u8"所以", u8"以及", u8"其他",
    };
    for (const char* b : kBad) if (std::strncmp(p, b, std::strlen(b)) == 0) return true;
    return false;
}

// 以姓氏字开头、但其实是常用词的两字组合 —— 光靠"必须有佐证"挡不住它们
// （"于是说""成为负责人"都长得像人名+动词）。
bool is_false_person_word(const std::string& w) {
    static const char* kBad[] = {
        u8"于是", u8"为了", u8"成为", u8"和平", u8"由于", u8"关于", u8"对于",
        u8"至于", u8"等于", u8"位于", u8"属于", u8"在于", u8"用来", u8"向来",
        u8"向前", u8"看成", u8"免得", u8"好在", u8"善于", u8"过于", u8"终于",
        u8"成人", u8"成本", u8"成功", u8"成立", u8"成员", u8"完成", u8"当前",
        u8"当时", u8"当然", u8"内部", u8"外面", u8"上面", u8"下面", u8"后面",
        u8"分开", u8"分析", u8"分享", u8"分散", u8"方案", u8"方式", u8"方面",
        u8"白色", u8"白天", u8"百万", u8"万一", u8"公里", u8"施工",
        u8"工作", u8"工具", u8"工资", u8"员工", u8"加工", u8"施工",
    };
    for (const char* b : kBad) if (w == b) return true;
    return false;
}

// 判断一个候选词从第 2 个字起有没有虚词。
//
// 【为什么从第 2 个字起】姓氏本身可能就是虚词字 —— 「于」是常见姓氏（于是/由于的"于"），
// 所以第 1 个字不能查。但**名**里不会出现"的/了/是/在"：
// 实测「那边**的**反馈」里"边"是姓氏、"反馈"是佐证动词 → 抽出个假人名「边的」。
// 查第 2 个字起，「边的」被挡掉，「于伟」照常通过。
bool name_tail_has_function_char(const std::string& w) {
    size_t i = 0;
    size_t idx = 0;
    while (i < w.size()) {
        const size_t len = utf8_len_of(static_cast<unsigned char>(w[i]));
        if (i + len > w.size()) break;
        if (idx >= 1 && is_function_char(utf8_cp(w, i, len))) return true;
        i += len;
        ++idx;
    }
    return false;
}

struct CjkHit {
    std::string value;      // 原样写法
    std::string kind;       // person | term
    size_t      offset = 0; // 在段内原文的字节偏移
    const char* why    = "";
    // 只有弱后缀（项目/系统/平台）命中的条目才为 true：
    // 这类名字必须**本场出现 ≥2 次**才值得报（见 is_org_suffix_start 的说明）
    bool        needs_repeat = false;
};

// 英文侧的第二类佐证：**报告动词** —— `Kelvin said` / `Marco asked`。
//
// 这是中文侧「张伟说」的英文对应物，用途也一样：给"首字母大写"这条弱证据补一条硬证据。
// 真实的会议录音里，人名第一次出现往往就是这样带出来的。
//
// 【为什么不做成 for_each_person_name 的一部分】那个函数是"取模式后面的词"，
// 这个是"取动词**前面**的词"，方向相反，混在一起会很难读。
void for_each_reporter_name(const std::vector<Segment>& segs,
                            const std::function<void(const std::string&, const Segment&, size_t)>& fn) {
    static const char* kVerbs[] = {
        "said", "says", "asked", "told", "mentioned", "explained",
        "confirmed", "reported", "noted", "added", "agreed", "suggested",
    };
    for (const auto& seg : segs) {
        const auto toks = tokenize(seg.src_text);
        for (size_t i = 1; i < toks.size(); ++i) {
            const std::string low = lower_ascii(toks[i].text);
            bool is_verb = false;
            for (const char* v : kVerbs) if (low == v) { is_verb = true; break; }
            if (!is_verb) continue;
            // 动词前面那个词必须是"专名形状"：首字母大写且不是停用词
            const Token& prev = toks[i - 1];
            if (prev.text.size() < 2 || prev.text.size() > 40) continue;
            const unsigned char c0 = static_cast<unsigned char>(prev.text[0]);
            if (c0 < 'A' || c0 > 'Z') continue;
            if (is_never_name(lower_ascii(prev.text))) continue;
            fn(prev.text, seg, prev.offset);
        }
    }
}

// 收集所有"有佐证句式的键"
std::vector<std::string> person_pattern_keys(const std::vector<Segment>& segs) {
    std::vector<std::string> keys;
    auto add = [&](const std::string& w) {
        const std::string k = normalize_key(w);
        if (!k.empty() && std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
    };
    for_each_person_name(segs, [&](const std::string& n, const Segment&, size_t) { add(n); });
    for_each_reporter_name(segs, [&](const std::string& n, const Segment&, size_t) { add(n); });
    return keys;
}

// 在**一段文本**里找中文专名候选。
//
// 逐条汉字串处理：`cjk_runs` 已经把标点当边界切好了，
// 所以"张伟负责报价，李总跟进"会切成两串，避免跨标点把两句话连起来。
std::vector<CjkHit> find_cjk_names(const std::string& s) {
    std::vector<CjkHit> out;
    const auto runs = cjk_runs(s);

    for (const auto& run : runs) {
        const size_t n = run.size();
        const size_t run_end = run.back().off + run.back().len;

        // 取从第 k 个字开始的剩余 UTF-8 文本（用于判断后面跟的是不是称谓/动词/后缀）
        auto rest_at = [&](size_t k) -> std::string {
            if (k >= n) return {};
            return s.substr(run[k].off, run_end - run[k].off);
        };
        auto text_of = [&](size_t k, size_t cnt) -> std::string {
            if (k + cnt > n) return {};
            const size_t b = run[k].off;
            const size_t e = run[k + cnt - 1].off + run[k + cnt - 1].len;
            return s.substr(b, e - b);
        };

        for (size_t k = 0; k < n; ++k) {
            // ---- R7：机构 / 项目名（后缀是强信号，先判它）----
            {
                bool hit_org = false;
                for (size_t slen : {2u, 3u}) {
                    if (k + slen > n) continue;
                    const std::string suf = text_of(k, slen);
                    bool weak = false;
                    if (!is_org_suffix_start(suf.c_str(), &weak)) continue;

                    // 从后缀往左扫，碰到虚词就停；只取 2~4 个字
                    size_t left = 0;
                    while (left < 4 && left < k) {
                        if (is_function_char(run[k - left - 1].cp)) break;
                        ++left;
                    }
                    if (left < 2) break;    // 前面不够 2 个字，不是名字

                    const size_t b = k - left;
                    const std::string name = text_of(b, left + slen);
                    if (name.empty()) break;
                    if (is_generic_prefix(name.c_str())) break;

                    out.push_back({name, "term", run[b].off, u8"机构/项目后缀", weak});
                    hit_org = true;
                    break;
                }
                if (hit_org) continue;
            }

            // ---- R6：人名（姓氏 + 佐证）----
            if (!is_surname_char(run[k].cp)) continue;

            // ① 姓氏 + 称谓（"张总""李经理"）—— 只有姓、没有名也算，
            //    因为口语里就是这么叫的，存下来才有用
            {
                bool done = false;
                for (size_t tlen : {1u, 2u, 3u}) {
                    if (k + 1 + tlen > n) continue;
                    const std::string title = text_of(k + 1, tlen);
                    if (!is_title_start(title.c_str())) continue;
                    const std::string name = text_of(k, 1 + tlen);
                    // 称谓路径也要过两道闸：常用词黑名单 + 名里不能有虚词
                    if (is_false_person_word(name)) break;
                    if (name_tail_has_function_char(name)) break;
                    out.push_back({name, "person", run[k].off, u8"姓氏+称谓（张总/李经理）", false});
                    done = true;
                    break;
                }
                if (done) continue;
            }

            // ② 姓氏 + 1~2 个字的名字 + 佐证动词（"张伟负责""李小明提到"）
            {
                bool done = false;
                for (size_t nlen : {1u, 2u}) {
                    if (k + 1 + nlen >= n + 1) continue;
                    const size_t name_len = 1 + nlen;
                    if (k + name_len > n) continue;
                    const std::string name = text_of(k, name_len);
                    if (is_false_person_word(name)) break;      // 常用词，直接放弃这个起点
                    if (name_tail_has_function_char(name)) break;   // 「边的」这类
                    const std::string rest = rest_at(k + name_len);
                    if (rest.empty()) continue;
                    if (!is_person_verb_start(rest.c_str())) continue;
                    out.push_back({name, "person", run[k].off, u8"姓氏+佐证动词（张伟负责）", false});
                    done = true;
                    break;
                }
                if (done) continue;
            }
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
    enum PathPrio { kCjk = 0, kPerson = 1, kQuoted = 2, kCapital = 3 };
    struct Occ {
        std::string value;
        const Segment* seg = nullptr;
        size_t      offset = 0;
        PathPrio    prio   = kCapital;
        const char* why    = "";
        // 这条出现是否命中"人名"证据。中文专名自己知道（姓氏+称谓/动词 → person），
        // 英文侧靠"人名句式"路径。最终 kind 由"有没有任何一条人名证据"决定。
        bool        is_person = false;
        // 只有弱后缀（项目/系统/平台）命中的才是 true
        bool        weak_org  = false;
    };
    std::vector<Occ> occs;

    // 中文专名（R6/R7）—— 优先级最高：它带了结构性的佐证，比"首字母大写"可信得多
    for (const auto& seg : segs) {
        for (const auto& h : find_cjk_names(seg.src_text)) {
            occs.push_back({h.value, &seg, h.offset, kCjk, h.why,
                            h.kind == "person", h.needs_repeat});
        }
    }

    for_each_person_name(segs, [&](const std::string& name, const Segment& seg, size_t off) {
        occs.push_back({name, &seg, off, kPerson, u8"人名句式（my name is / 我叫）", true, false});
    });

    // 第 2 步：先确定"哪些词有专名证据"，再统计它们的**全部**出现次数。
    //
    // 证据与计数必须分开：句首的 Marco 不构成证据（可能只是语法大写），
    // 但 "Marco" 一旦在别处以非句首位置出现过，它在句首的那次**也该计入次数**。
    //
    // 【为什么"弱证据"要两条新判据 —— 真实数据给的教训】
    // 会话 #47（真实播客 53 段）只靠 R1 抽出 8 个假阳性：
    //     down(6) Keep(4) Midnight movies(3) Speaking Learners Exactly
    // 全是普通词，大写只是因为被当作**词汇标题**写（`Keep It Down.`）、
    // 标题式大写（`Speaking of Movies`）、或者称呼语（`Hello English Learners`）。
    // 英文里"句中普通词被大写"太常见了，**R1 单独用一定会滥**。
    //
    // 现在的判据（不需要词表）：
    //   ① 形状强（CamelCase / 缩写）→ 直接算数
    //   ② 命中佐证句式（my name is X / I'm X / X said…）→ 算数
    //   ③ 否则必须"大写且非句首"出现 **≥2 次**，而且**本场没有小写出现过**
    //      —— 真专名不会又大写又小写：实测 `down` 大写 2 / 小写 4、`movies` 大写 1 / 小写 2
    struct KeyStat {
        int  cap_noninit = 0;   // 首字母大写且非句首
        int  lower_seen  = 0;   // 全小写出现（可能是同一个普通词）
        bool strong      = false;// CamelCase / 缩写
    };
    std::map<std::string, KeyStat> stats;
    for (const auto& seg : segs) {
        // 标题行整段作废（见 is_all_caps_heading）—— 它在"大写"这件事上零信息。
        const bool heading = is_all_caps_heading(seg.src_text);
        for (const auto& t : tokenize(seg.src_text)) {
            const std::string key = normalize_key(t.text);
            if (key.empty()) continue;
            if (looks_lowercase_word(t)) { stats[key].lower_seen += 1; continue; }
            if (heading) continue;
            switch (classify_token(t)) {
            case Ev::StrongShape: stats[key].strong = true; break;
            case Ev::WeakCap:     stats[key].cap_noninit += 1; break;
            case Ev::None:        break;
            }
        }
    }

    // 佐证句式的键（人名句式 + 报告动词）
    std::vector<std::string> corroborated;
    for (const auto& name : person_pattern_keys(segs)) {
        const std::string k = normalize_key(name);
        if (!k.empty() && std::find(corroborated.begin(), corroborated.end(), k) == corroborated.end()) {
            corroborated.push_back(k);
        }
    }

    constexpr int kMinCapOccurrences = 2;   // 见上面 ③ 的说明
    std::vector<std::string> name_keys;
    for (const auto& kv : stats) {
        const auto& k = kv.first;
        const auto& st = kv.second;
        const bool ok = st.strong
                     || std::find(corroborated.begin(), corroborated.end(), k) != corroborated.end()
                     || (st.cap_noninit >= kMinCapOccurrences && st.lower_seen == 0);
        if (ok) name_keys.push_back(k);
    }

    // 统计这些词的**全部**出现 —— 证据和计数是两件事。
    //
    // ⚠️ 这里**不能**再加 `classify_token(t) == Ev::None` 的过滤。
    // 我加过一次，自检立刻报「Marco 出现次数应为 2」——
    // 因为 `Marco` 在第 2 段的问号后（句首）那次被跳过了。
    // 一旦某个键被判定为专名，它的**每一次出现都该计入 hits**（含句首那几次），
    // 否则界面上会显示"已经听到 1 次"，而实际听到 2 次 —— **给用户看的数字错了**。
    for (const auto& seg : segs) {
        for (const auto& t : tokenize(seg.src_text)) {
            const std::string key = normalize_key(t.text);
            if (key.empty()) continue;
            if (std::find(name_keys.begin(), name_keys.end(), key) == name_keys.end()) continue;
            occs.push_back({t.text, &seg, t.offset, kCapital, u8"首字母大写的词", false, false});
        }
        for (const auto& q : find_quoted(seg.src_text)) {
            // 引号里明确是"被讨论的词"，不管大小写都收 —— 这是最强的信号
            occs.push_back({q.first, &seg, q.second, kQuoted, u8"引号里的词", false, false});
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
        // "强证据"的条数。弱后缀（项目/系统/平台）命中的不算强证据 ——
        // 如果一个名字**只有**弱后缀支撑，必须出现 ≥2 次才报（见 is_org_suffix_start）
        int         strong_hits = 0;
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
        if (!o.weak_org) a.strong_hits += 1;
        if (o.seg->confidence >= 0.0 && o.seg->confidence < a.min_conf) {
            a.min_conf = o.seg->confidence;
        }
        if (a.source_text.empty()) {
            a.session     = session_id;
            a.seq         = o.seg->seq;
            a.source_text = o.seg->src_text;
            a.why         = o.why;
        }
        if (o.is_person &&
            std::find(person_keys.begin(), person_keys.end(), key) == person_keys.end()) {
            person_keys.push_back(key);
        }
    }

    std::vector<ExtractedCandidate> out;
    for (auto& kv : agg) {
        auto& a = kv.second;
        // 【弱后缀单独把门】"交付项目"这种只有弱后缀支撑、又只出现一次的，
        // 报出去就是让用户回答一个蠢问题。真实的项目名在一场会里通常被提多次。
        if (a.strong_hits == 0 && a.hits < 2) continue;

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
