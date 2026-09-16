#include "ITranslator.h"

namespace {

// 术语约束最多带多少个。
// 提示词不能无限长——术语表长到几百条时，一是会挤占上下文，
// 二是模型反而会开始"自由发挥"。超出的部分不是不管，
// 而是交给 TermFixer 的识别后纠错兜底（那条路不花上下文）。
constexpr size_t kMaxGlossaryTerms = 40;

// 单个术语的长度上限。超过这个长度基本不是专名，而是误入的词组/句子，
// 塞进提示词只会干扰模型。
constexpr size_t kMaxTermLen = 40;

}  // namespace

void ITranslator::set_glossary(const std::vector<std::string>& terms) {
    glossary_ = terms;
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
