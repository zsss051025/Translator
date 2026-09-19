#include "UtteranceMerge.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace uttermerge {

namespace {

// "话没说完"的结尾词。
//
// ⚠️ **这张表刻意比 `tools/segment_tune.py` 里那份窄。**
//    那个工具的表是拿来**数碎片**的（允许有噪声 —— 它只用来排序）；
//    这里判错的代价是"把两句本来无关的话合并成一句"，所以只收
//    **明确不可能作为句子结尾**的词：连词、介词、冠词、助动词、
//    疑问词、人称代词、数词。
//
//    · **不收** 副词（actually / really）—— "I know, actually." 可以成立
//    · **不收** 名词和动词（wife / got）—— 残句常常以它们结尾，
//      但那也是完整短句的常见结尾，收进来会合并得太多
//      （代价是有些真残句认不出来，那只是回到"翻译替它补全"的老问题，
//        比合并错两句轻）
const std::vector<std::string>& dangling_tail_words() {
    static const std::vector<std::string> v = {
        // 连词
        "and", "or", "but", "so", "because", "if", "when", "while", "that",
        "although", "though", "unless", "until", "whether",
        // 介词
        "of", "to", "in", "on", "at", "for", "with", "from", "by", "about",
        "into", "over", "under", "between", "during", "before", "after", "as",
        // 冠词 / 物主 / 指示
        "the", "a", "an", "my", "your", "his", "her", "its", "our", "their",
        "this", "these", "those",
        // 助动词 / be 动词
        "is", "are", "was", "were", "be", "been", "being", "am",
        "do", "does", "did", "have", "has", "had",
        "will", "would", "shall", "should", "can", "could", "may", "might", "must",
        "i'm", "you're", "we're", "they're", "it's", "that's", "don't", "doesn't",
        "didn't", "can't", "won't", "isn't", "aren't", "wasn't", "weren't",
        // 疑问词
        "what", "how", "why", "where", "who", "whom", "whose", "which",
        // 人称代词（句子一般不以它们结尾，除非是 "I love you" 那种 —— 那也常常
        //  还有下文，合并掉基本无害）
        "i", "you", "he", "she", "it", "we", "they", "me", "him", "us", "them",
        // 数词（"let's look at our two" 后面一定还有词）
        "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
        // 其他明确的悬空词
        "very", "too", "just", "not", "also", "then", "than",
    };
    return v;
}

// 末尾是不是"终止标点"（有它就说明这句话说完了）。
bool ends_with_terminal(const std::string& t) {
    if (t.empty()) return false;
    const unsigned char c = static_cast<unsigned char>(t.back());
    if (c == '.' || c == '!' || c == '?') return true;
    // 中文/全角终止标点（多字节，比对尾部序列）
    static const char* kTerm[] = {u8"。", u8"！", u8"？", u8"…", u8"．"};
    for (const char* s : kTerm) {
        const std::string p = s;
        if (t.size() >= p.size() && t.compare(t.size() - p.size(), p.size(), p) == 0) {
            return true;
        }
    }
    return false;
}

// 末尾是不是"话还要继续"的标点（逗号/分号/破折号/冒号）。
//
// 【为什么把逗号也算"没说完"】Whisper 打逗号基本都对应一个真实停顿，
// 而那个停顿后面**通常就是同一句的下一半**。jfk 上就是这样：
//     "Ask not what your country can do for you,"  ← 切在逗号处
//     "ask what you can do for your country."      ← 下一段就是它的后半句
// 合起来正是完整的那句名言。
//
// ⚠️ 而且这个判据**两种结局都不会坏**：
//     · 下一段很快到  → 合并成完整句（更好）
//     · 说话人停下了  → 超时吐出，独立译一个完整从句（也很好）
// 所以它是个"稳赚"的弱信号 —— 代价只是最多晚 1.5 秒出字幕。
bool ends_with_continuation(const std::string& t) {
    if (t.empty()) return false;
    const unsigned char c = static_cast<unsigned char>(t.back());
    if (c == ',' || c == ';' || c == ':' || c == '-') return true;
    static const char* kCont[] = {u8"，", u8"、", u8"；", u8"：", u8"—"};
    for (const char* s : kCont) {
        const std::string p = s;
        if (t.size() >= p.size() && t.compare(t.size() - p.size(), p.size(), p) == 0) {
            return true;
        }
    }
    return false;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// 取出最后一个"词"（去标点、转小写）。英文按空白切；中文整体无法这样处理，
// 所以中文只在有终止标点时判为完整，否则**不动**（宁可漏判）。
std::string last_word(const std::string& t) {
    std::string cur;
    for (size_t i = t.size(); i > 0; --i) {
        const unsigned char c = static_cast<unsigned char>(t[i - 1]);
        if (std::isalnum(c) || c == '\'') {
            cur.push_back(static_cast<char>(std::tolower(c)));
        } else if (!cur.empty()) {
            break;
        }
    }
    std::reverse(cur.begin(), cur.end());
    return cur;
}

bool is_ascii_only(const std::string& s) {
    for (unsigned char c : s) {
        if (c >= 0x80) return false;
    }
    return true;
}

}  // namespace

bool looks_incomplete(const std::string& text) {
    const std::string t = trim(text);
    if (t.empty()) return false;

    // 有终止标点 → 说完了
    if (ends_with_terminal(t)) return false;

    // 末尾是"话还要继续"的标点（逗号/分号等）→ 没说完。
    // 这一条**不看尾巴上的词**，可以覆盖中文（中文里这个判据同样成立：
    // "接口文档需要重写，" 后面一定还有半句）。
    if (ends_with_continuation(t)) return true;

    // ⚠️ 中文：**不判**（除了上面那条标点规则）。
    // 理由：Whisper 对中文经常完全不打标点，用"没有终止标点"当判据会把
    // 两个本来无关的句子合成一句（中文里那是很难看的）。
    // 而中文的虚词判断需要分词，这里没有分词器 —— 所以保守放弃，
    // 宁可中文残句漏判（回到老问题），也不要合并错。
    if (!is_ascii_only(t)) return false;

    const std::string w = last_word(t);
    if (w.empty()) return false;
    const auto& bad = dangling_tail_words();
    return std::find(bad.begin(), bad.end(), w) != bad.end();
}

std::string join_text(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    const unsigned char la = static_cast<unsigned char>(a.back());
    const unsigned char fb = static_cast<unsigned char>(b.front());
    // 两边都是多字节（中文/日文）→ 不加空格；否则加一个空格
    const bool cjk_pair = (la >= 0x80) && (fb >= 0x80);
    return cjk_pair ? (a + b) : (a + " " + b);
}

std::string Joiner::push(const std::string& text, double now_sec) {
    const std::string t = trim(text);
    if (t.empty()) return std::string();

    if (held_.empty()) {
        if (!looks_incomplete(t)) return t;      // 完整 → 直接译
        held_ = t;                               // 残句 → 攒着
        held_at_ = now_sec;
        held_count_ = 1;
        return std::string();
    }

    // 已经攒着了 → 合并
    held_ = join_text(held_, t);
    ++held_count_;

    // 仍然没说完、而且还没攒够上限 → 继续攒
    if (looks_incomplete(held_) && held_count_ < kMaxHoldSegments) {
        // 注意：**不更新 held_at_**。超时是从"第一次开始攒"算的，
        // 否则说话人每 0.4 秒挤一个词、永远触发不了超时，字幕就再也不出来了。
        return std::string();
    }

    const std::string out = held_;
    held_.clear();
    held_count_ = 0;
    return out;
}

std::string Joiner::poll(double now_sec) {
    if (held_.empty()) return std::string();
    if (now_sec - held_at_ < kHoldTimeoutSec) return std::string();
    const std::string out = held_;
    held_.clear();
    held_count_ = 0;
    return out;
}

std::string Joiner::flush() {
    if (held_.empty()) return std::string();
    const std::string out = held_;
    held_.clear();
    held_count_ = 0;
    return out;
}

}  // namespace uttermerge
