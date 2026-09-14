#include "TermFixer.h"

#include <algorithm>
#include <cctype>
#include <cstdint>

namespace {

// ---------------- UTF-8 ----------------

// 解码成码点序列。按码点算编辑距离，否则中文的字节距离会被放大 3 倍。
std::vector<uint32_t> utf8_decode(const std::string& s) {
    std::vector<uint32_t> out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp = c;
        size_t len = 1;
        if      ((c & 0x80) == 0x00) { len = 1; cp = c; }
        else if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
        else { ++i; continue; }                       // 非法字节，跳过

        if (i + len > s.size()) break;
        for (size_t k = 1; k < len; ++k) {
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        }
        out.push_back(cp);
        i += len;
    }
    return out;
}

std::string utf8_encode(const std::vector<uint32_t>& cps) {
    std::string out;
    for (uint32_t cp : cps) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

// ---------------- 编辑距离 ----------------

int levenshtein(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    const size_t n = a.size(), m = b.size();
    if (n == 0) return static_cast<int>(m);
    if (m == 0) return static_cast<int>(n);

    std::vector<int> prev(m + 1), cur(m + 1);
    for (size_t j = 0; j <= m; ++j) prev[j] = static_cast<int>(j);

    for (size_t i = 1; i <= n; ++i) {
        cur[0] = static_cast<int>(i);
        for (size_t j = 1; j <= m; ++j) {
            const int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        prev.swap(cur);
    }
    return prev[m];
}

// 码点是否属于 CJK（粗略范围，够用）
bool is_cjk(uint32_t cp) {
    return (cp >= 0x4E00 && cp <= 0x9FFF) ||    // 基本汉字
           (cp >= 0x3400 && cp <= 0x4DBF) ||    // 扩展 A
           (cp >= 0x3040 && cp <= 0x30FF) ||    // 日文假名
           (cp >= 0xAC00 && cp <= 0xD7AF);      // 韩文
}

// 这个候选词是否"值得纠错"。
// 门槛设高是刻意的：误改一个真实词汇，比漏改一个名字更糟——
// 漏改只是难看，误改会让整句意思跑偏。
bool acceptable_candidate(const std::vector<uint32_t>& w,
                          const std::vector<uint32_t>& term) {
    if (w.empty() || term.empty()) return false;

    // 编辑距离为 1 时长度最多差 1。
    // 注意不能要求长度相等——"Erikka"(6) -> "Erika"(5) 是删一个字符，
    // 正是最典型的误识别形态，卡等长会把主要用例挡掉。
    const int len_diff = static_cast<int>(w.size()) - static_cast<int>(term.size());
    if (len_diff > 1 || len_diff < -1) return false;

    const bool cjk = is_cjk(term[0]);
    if (cjk) {
        return term.size() >= 3;                 // 中文名至少 3 个字才敢改
    }

    // ASCII：要求长度 >= 4 且首字母都是大写，
    // 这样 "mark"（动词）不会被改成 "Mark"（人名）
    if (term.size() < 4) return false;
    if (w.size() < 3)    return false;
    if (!std::isupper(static_cast<unsigned char>(term[0]))) return false;
    if (!std::isupper(static_cast<unsigned char>(w[0])))    return false;
    return true;
}

// ---------------- 分词（按空白切，保留原样） ----------------

struct Token {
    std::string text;
    size_t      begin = 0;
    size_t      end   = 0;
};

std::vector<Token> split_tokens(const std::string& s) {
    std::vector<Token> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i >= s.size()) break;
        const size_t b = i;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        out.push_back(Token{s.substr(b, i - b), b, i});
    }
    return out;
}

// 去掉词首尾的标点，返回核心部分与前后缀
struct Stripped {
    std::string prefix;
    std::string core;
    std::string suffix;
};

Stripped strip_punct(const std::string& w) {
    auto is_punct_byte = [](unsigned char c) {
        // 只把 ASCII 标点视为可剥离；多字节标点（，。等）整块处理太麻烦，
        // 且不影响专名匹配（专名基本是 ASCII 或纯汉字）
        return c < 0x80 && !std::isalnum(c);
    };
    size_t b = 0, e = w.size();
    // 首部标点
    while (b < e && is_punct_byte(static_cast<unsigned char>(w[b]))) ++b;
    // 注意：多字节字符首字节 >= 0x80，不会被当成标点，所以这里安全
    // 尾部标点
    while (e > b && is_punct_byte(static_cast<unsigned char>(w[e - 1]))) --e;

    Stripped s;
    s.prefix = w.substr(0, b);
    s.core   = w.substr(b, e - b);
    s.suffix = w.substr(e);
    return s;
}

}  // namespace

void TermFixer::set_terms(const std::vector<std::string>& terms) {
    terms_.clear();
    for (const auto& t : terms) {
        if (!t.empty()) terms_.push_back(t);
    }
}

int TermFixer::count_exact_hits(const std::string& text) const {
    int n = 0;
    for (const auto& tok : split_tokens(text)) {
        const std::string core = strip_punct(tok.text).core;
        for (const auto& term : terms_) {
            if (core == term) { ++n; break; }
        }
    }
    return n;
}

std::string TermFixer::fix(const std::string& text,
                           std::vector<std::string>* replacements) const {
    if (terms_.empty() || text.empty()) return text;

    // 预解码术语，避免在每个词上重复解码
    std::vector<std::vector<uint32_t>> term_cps;
    term_cps.reserve(terms_.size());
    for (const auto& t : terms_) term_cps.push_back(utf8_decode(t));

    std::string out;
    out.reserve(text.size() + 16);

    size_t cursor = 0;
    for (const auto& tok : split_tokens(text)) {
        out.append(text, cursor, tok.begin - cursor);   // 补上空白
        cursor = tok.end;

        const Stripped sp = strip_punct(tok.text);
        std::string replaced = sp.core;

        if (!sp.core.empty()) {
            const auto core_cps = utf8_decode(sp.core);
            for (size_t k = 0; k < terms_.size(); ++k) {
                const auto& tc = term_cps[k];
                if (core_cps == tc) break;                       // 已经一致，无需处理
                if (!acceptable_candidate(core_cps, tc)) continue;
                if (levenshtein(core_cps, tc) != 1) continue;    // 只接受差一个字

                replaced = terms_[k];
                if (replacements != nullptr) {
                    replacements->push_back(sp.core + " -> " + terms_[k]);
                }
                break;
            }
        }

        out += sp.prefix;
        out += replaced;
        out += sp.suffix;
    }
    out.append(text, cursor, text.size() - cursor);
    return out;
}
