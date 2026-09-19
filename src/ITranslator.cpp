#include "ITranslator.h"

#include <cctype>
#include <string>
#include <vector>

namespace {

// 术语约束最多带多少个。
// 提示词不能无限长——术语表长到几百条时，一是会挤占上下文，
// 二是模型反而会开始"自由发挥"。超出的部分不是不管，
// 而是交给 TermFixer 的识别后纠错兜底（那条路不花上下文）。
constexpr size_t kMaxGlossaryTerms = 40;

// 单个术语的长度上限。超过这个长度基本不是专名，而是误入的词组/句子，
// 塞进提示词只会干扰模型。
constexpr size_t kMaxTermLen = 40;

// 只折 ASCII 大小写，CJK 原样（术语里中文没有大小写）
std::string lower_ascii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x80 && u >= 'A' && u <= 'Z') c = static_cast<char>(u - 'A' + 'a');
    }
    return out;
}

}  // namespace

void ITranslator::set_glossary(const std::vector<std::string>& terms) {
    glossary_ = terms;
}

void ITranslator::set_context_lines(int n) {
    std::lock_guard<std::mutex> lock(context_mutex_);
    context_lines_ = n < 0 ? 0 : n;
    context_.clear();
}

void ITranslator::note_source(const std::string& src) {
    if (src.empty()) return;
    std::lock_guard<std::mutex> lock(context_mutex_);
    if (context_lines_ <= 0) return;
    context_.push_back(src);
    // 只留最近 N 段 —— 前文是**有代价**的（prompt 变长、小模型更容易回显），
    // 所以不该无限增长。
    while (static_cast<int>(context_.size()) > context_lines_) {
        context_.erase(context_.begin());
    }
}

std::string ITranslator::context_block() const {    std::lock_guard<std::mutex> lock(context_mutex_);
    if (context_lines_ <= 0 || context_.empty()) return "";

    // 措辞是刻意的，三个要点缺一不可：
    //   ① **说明用途**："供你理解上下文" —— 不说，模型不知道拿它们干什么
    //   ② **明确禁止翻译它们**：不说，模型很可能把前文也译一遍，
    //      于是屏幕上把上一句的译文又显示一次
    //   ③ **明确"不要照抄"**：不说，模型会把前文里的词硬套到本段 ——
    //      这一条吃过亏：按段过滤术语之前，原文 `and wife get ready to go`
    //      被译成「埃丽卡和马可准备出发了」，凭空补出了没出现的名字
    std::string out =
        "\n\nContext (preceding lines of the SAME conversation, given only so you "
        "understand what is being talked about; do NOT translate them, do NOT repeat "
        "them, and do NOT copy words from them that are not in the message):\n";
    for (const auto& c : context_) {
        out += "- ";
        out += c;
        out += "\n";
    }
    return out;
}

std::vector<std::string> ITranslator::glossary_for_text(const std::vector<std::string>& terms,
                                                        const std::string& text) {
    std::vector<std::string> out;
    if (terms.empty() || text.empty()) return out;

    const std::string hay = lower_ascii(text);
    for (const auto& t : terms) {
        if (t.empty()) continue;
        if (lower_ascii(t).empty()) continue;
        // 子串匹配，大小写不敏感。故意放宽 —— 见头文件里的说明。
        if (hay.find(lower_ascii(t)) != std::string::npos) out.push_back(t);
    }
    return out;
}

std::string ITranslator::glossary_constraint(const std::vector<std::string>& terms) {
    std::string list;
    size_t used = 0;
    for (const auto& t : terms) {
        if (t.empty() || t.size() > kMaxTermLen) continue;
        if (used >= kMaxGlossaryTerms) break;
        if (!list.empty()) list += ", ";
        list += t;
        ++used;
    }
    if (list.empty()) return {};

    // 措辞要点：明确说"不要音译/意译/改写"。
    // 只说"注意以下术语"是不够的——实测里模型会照着上下文自己选一种译法，
    // 而它上一句选了保留原文、这一句选了音译。
    return "\nTerminology (use these exact renderings; do not transliterate, "
           "translate, or paraphrase them): " + list + ".";
}

std::string ITranslator::clean_source(const std::string& text) {
    // 去掉首尾空白：ASCII 空白 + 全角空格（U+3000）。
    // 为什么连全角也要处理：中文 ASR 的产物里出现过全角空格，
    // 而它和半角空格在这个 bug 上是同一类触发器（都让 user 段"看起来是空的"）。
    const std::string kFullWidthSpace = "\xE3\x80\x80";
    size_t b = 0, e = text.size();
    auto is_ws_at = [&](size_t i, size_t* len) -> bool {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
            *len = 1;
            return true;
        }
        if (text.compare(i, kFullWidthSpace.size(), kFullWidthSpace) == 0) {
            *len = kFullWidthSpace.size();
            return true;
        }
        return false;
    };
    size_t l = 0;
    while (b < e && is_ws_at(b, &l)) b += l;
    while (e > b) {
        // 从尾部往回找字符起点：续字节（10xxxxxx）不是字符开头
        size_t i = e - 1;
        while (i > b && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) --i;
        if (!is_ws_at(i, &l) || i + l != e) break;
        e = i;
    }
    return text.substr(b, e - b);
}

bool ITranslator::looks_like_prompt_echo(const std::string& output) {
    if (output.empty()) return false;

    // 归一化：转小写、去掉空白与标点，只留字母/数字/汉字。
    // （模型回显时标点和大小写都可能变，不能按原样比。）
    std::string n;
    n.reserve(output.size());
    for (size_t i = 0; i < output.size(); ) {
        const unsigned char c = static_cast<unsigned char>(output[i]);
        if (c < 0x80) {
            if (std::isalnum(c)) n += static_cast<char>(std::tolower(c));
            ++i;
        } else {
            size_t len = 1;
            if      ((c & 0xE0) == 0xC0) len = 2;
            else if ((c & 0xF0) == 0xE0) len = 3;
            else if ((c & 0xF8) == 0xF0) len = 4;
            n += output.substr(i, len);
            i += len;
        }
    }
    if (n.empty()) return false;

    // 特征词：必须满足**"日常对白里不会出现"** —— 和 ASR 侧的幻觉黑名单同一条规矩。
    //
    // ⚠️ 第一版我收进了「不要解释」「只输出译文」「无需解释」这类**短而通用**的短语，
    //    自检当场报出假阳性：正常台词「不要解释了，我们继续。」会被当成回显，
    //    于是**一句正常的译文被替换成了英文原文**。
    //    两边的代价都不小，但方向不同：
    //      · 漏判 → 伪造的中文留在交付物里，用户会当真
    //      · 误判 → 正常译文变成原文，用户看得见（只是要多翻一次）
    //    取"**宁可漏判，不可误判**"：只收长到不可能出现在对白里的措辞。
    //    回显的特征是它会**从指令开头一路吐下来**，所以长特征词够用。
    static const std::vector<std::string> kSigns = {
        // 英文：prompt 原文（长、且是完整短语）
        "youareaprofessionaltranslator",
        "outputonlythetranslation",
        "norepetitionoftheseinstructions",
        "noquotationmarks",
        // 中文：模型把指令翻过来的样子 —— **实测出现的就是这一类**
        u8"专业的翻译人员", u8"专业翻译人员", u8"专业翻译助手",
    };
    for (const std::string& s : kSigns) {
        // 表里可能有带空格的写法，统一归一化后再比
        std::string p;
        for (char ch : s) {
            const unsigned char c = static_cast<unsigned char>(ch);
            if (c < 0x80) {
                if (std::isalnum(c)) p += static_cast<char>(std::tolower(c));
            } else {
                // 多字节原样带过（表里的中文是完整 UTF-8）
                p += ch;
            }
        }
        if (!p.empty() && n.find(p) != std::string::npos) return true;
    }
    return false;
}
