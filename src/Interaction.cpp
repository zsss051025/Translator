#include "Interaction.h"

#include <cstring>   // std::strlen（剥系词时比对字面量长度）

// 注意：这个文件**不许** include KnowledgeGap.h / KnowledgeStore.h。
// 理由见头文件开头 —— 那是这一层存在的意义。

namespace interaction {

namespace {

// "不止一次 / 多次" 的措辞。分档是为了别说错：
// hits==2 时说"多次"略夸张，hits 很大时说"不止一次"又太轻。
const char* familiarity_phrase(Familiarity f) {
    switch (f) {
    case Familiarity::FirstTime: return u8"你刚才提到了";
    case Familiarity::AFewTimes: return u8"我注意到你不止一次提到";
    case Familiarity::ManyTimes: return u8"我注意到你多次提到";
    }
    return u8"你刚才提到了";
}

// kind → 人话里的"这是个什么东西"
const char* entity_word(Kind k) {
    switch (k) {
    case Kind::ConfirmPersonName:
    case Kind::SuggestCorrectSpelling: return u8"人名";
    case Kind::ConfirmProjectName:     return u8"项目名";
    case Kind::ConfirmTerm:
    case Kind::AskTermMeaning:
    case Kind::UpdateChangedValue:
    case Kind::PickSpelling:
    case Kind::UnifySpelling:          return u8"术语";
    }
    return u8"术语";
}

std::string quoted(const std::string& s) {
    return u8"「" + s + u8"」";
}

// 剥掉用户答案开头的**系词**，避免回执里出现「指的是指的是…」。
//
// 【真实数据】用户答「指的是电视节目的缩写」，回执就成了
//     「「TV」是指指的是电视节目的缩写」
// 读起来像结巴 —— 而这是"我记住了"那一句，是全篇最该干净的地方。
//
// 【为什么只剥多字形式】「指的是」「是指」「就是」这些是明确的系词短语，
// 剥掉不会有歧义。**不剥单字「是」「指」**：`是非` / `指标` 这类词
// 会被剥坏（`指的是非` 就错了）。宁可留一点冗余，不能改坏用户的答案。
std::string strip_leading_copula(const std::string& s) {
    static const char* kCopulas[] = {
        u8"指的是", u8"是指", u8"就是指", u8"就是", u8"的意思就是",
        u8"意思就是", u8"是一个", u8"是一种", u8"是一种叫做",
    };
    // 允许前面有空白/冒号/逗号
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) ++b;
    for (const char* c : kCopulas) {
        const size_t n = std::strlen(c);
        if (s.size() >= b + n && s.compare(b, n, c) == 0) {
            std::string rest = s.substr(b + n);
            // 系词后面可能还跟一个冒号/逗号
            size_t rb = 0;
            while (rb < rest.size() &&
                   (rest[rb] == ' ' || rest[rb] == '\t' ||
                    rest.compare(rb, 3, u8"：") == 0 || rest.compare(rb, 3, u8"，") == 0 ||
                    rest[rb] == ':' || rest[rb] == ',')) {
                rb += (rest[rb] == ':' || rest[rb] == ',') ? 1 : 3;
            }
            rest = rest.substr(rb);
            if (!rest.empty()) return rest;      // 剥空了就保留原文
        }
    }
    return s;
}

}  // namespace

std::string intro(size_t n) {
    if (n == 0) return {};
    if (n == 1) {
        return u8"有件事想跟你确认一下（直接回车可以跳过）：";
    }
    return u8"这场会我注意到几件事，想跟你确认一下"
           u8"（直接回车可以跳过任何一条）：";
}

std::string question(const Prompt& p) {
    switch (p.kind) {
    // 「我为什么觉得这个信息值得记住」—— 每一条都从**我注意到了什么**开头，
    // 而不是从"我库里有个候选"开头。
    case Kind::ConfirmPersonName:
        return std::string(familiarity_phrase(p.familiarity)) + u8" " + p.subject +
               u8"。这是正确的人名写法吗？";

    case Kind::ConfirmProjectName:
        return std::string(familiarity_phrase(p.familiarity)) + u8" " + p.subject +
               u8"。这是个项目名吗？以后我会按这个名字来记。";

    case Kind::ConfirmTerm:
        return std::string(familiarity_phrase(p.familiarity)) + u8" " + p.subject +
               u8"。这是你们项目里的一个重要概念吗？";

    // 一问两用：确认 + 学含义。需求原话就是这个形状
    //（「它是不是重要概念？如果是，它具体指什么？」）。
    // 合成一句是有意的 —— 分成两问会多占用户一次注意力，而含义才是重点。
    //
    // 【为什么说"重要概念"而不是"技术术语"】`kind == term` 里混着两种东西：
    //   ① 英文侧抽出来的技术术语（CO-RE / EnglishPod）
    //   ② 中文侧 R7 按"机构/项目后缀"抽出来的名字（凤凰项目 / 星辰科技）
    // 对 ② 问"这是技术术语吗"是明显的错配，用户会觉得系统在胡乱归类。
    // 改成"重要概念"两种情况都成立 —— 而且这正是需求原话里的用词。
    //
    // 【有引子时换成确认题】"它指什么"要用户打一整句话；如果我们手里已经有一份
    // 猜测（分诊层的模型判断），就变成「我猜是指 X。对吗？」—— 按个 y 就行。
    // 这一步把开放式问题的成本降到选择题量级，**是"多问几个"的前提**。
    // 猜错也不怕：用户不认就会自己写，而且**猜测永不绕过确认入库**。
    case Kind::AskTermMeaning:
        if (!p.suggested.empty()) {
            // 【为什么把猜测放进引号、而不是写成"我猜是指 X"】
            // 实测（真实云端判断）：模型给出的猜测经常**自带那个词**，
            // 于是出现「我猜它是指WGBH 是美国波士顿的一家公共广播机构」——
            // 系词重复、读起来像结巴。和之前那起「指的是指的是」是同一类。
            //
            // 用引号把它框起来当**引述**，就不再需要跟句子语法对齐：
            // 无论猜测是"播客，一种数字音频节目"还是"WGBH 是美国波士顿的…"，
            // 读起来都通顺。这比在文案里剥词头稳（剥词头遇到变形就失效）。
            return std::string(familiarity_phrase(p.familiarity)) + u8" " + p.subject +
                   u8"。我猜它的意思是「" + p.suggested + u8"」。对吗？";
        }
        return std::string(familiarity_phrase(p.familiarity)) + u8" " + p.subject +
               u8"。它是你们项目中的一个重要概念吗？如果是，它具体指什么？";

    // 值变化。**必须把"我原来记的是什么"摆出来** ——
    // 否则用户不知道自己在同意什么。这正是"我学习"的证据所在。
    //
    // 空格只留在**汉字和拉丁词之间**（「记得「凤凰项目」是」），
    // 引号前后不加 —— 中文标点和全角括号旁边留空格看起来像排版事故。
    case Kind::UpdateChangedValue: {
        std::string s = u8"我之前记得" + quoted(p.subject) + u8"是" +
                        quoted(p.previous) + u8"，刚才我听到的是" +
                        quoted(p.current) + u8"。";
        s += u8"需要更新我的记录吗？";
        return s;
    }

    // 同一个东西两种写法。**必须把冲突摆出来**，
    // 否则用户只能凭记忆猜拼写（我们本来就知道有两个版本）。
    case Kind::PickSpelling: {
        std::string s = u8"这个名字我这儿有两种写法：";
        for (size_t i = 0; i < p.options.size(); ++i) {
            if (i) s += u8"、";
            s += quoted(p.options[i]);
        }
        s += u8"。是同一个人吗？哪个对？";
        if (!p.current.empty()) {
            // 说清"现在在用哪个" —— 用户要知道改动的后果
            s += u8"（现在用的是" + quoted(p.current) + u8"）";
        }
        return s;
    }

    case Kind::UnifySpelling:
        return quoted(p.subject) +
               u8"我看到过几种不一样的写法。以后统一用哪个？";

    case Kind::SuggestCorrectSpelling:
        return u8"刚才有个名字我没太听清（" + p.subject +
               u8"）。正确的写法是什么？";
    }
    return p.subject;
}

std::string hint(const Prompt& p) {
    switch (p.kind) {
    case Kind::ConfirmPersonName:
    case Kind::ConfirmTerm:
    case Kind::ConfirmProjectName:
        return u8"（对就按 y；不对就直接输入正确的写法；回车先跳过）";

    // 这条要用户**打一句话**，所以要说清"直接说它的意思就行"，
    // 并且给"只想让我记住这个词"的出口（那时的回答是确认、不是含义）。
    case Kind::AskTermMeaning:
        return u8"（直接输入它的意思；只想让我记住这个词就按 y；回车先跳过）";

    case Kind::UpdateChangedValue:
        return u8"（y = 更新成新的；n = 保持原来的；也可以直接输入正确的值；回车先跳过）";

    case Kind::PickSpelling:
        return u8"（输入编号选正确写法；n = 不是同一个东西，两个都留着；回车先跳过）";

    case Kind::UnifySpelling:
        return u8"（输入要统一成的那种写法；回车先跳过）";

    case Kind::SuggestCorrectSpelling:
        return u8"（直接输入正确的写法；回车先跳过）";
    }
    return u8"（回车跳过）";
}

std::string acknowledgment(const Prompt& p, Outcome o) {
    switch (o) {
    case Outcome::Failed:
        // 失败是唯一允许说技术原因的地方：这时候把原因藏起来更坏
        return u8"这条我没记上：" +
               (p.failure.empty() ? std::string(u8"原因不明") : p.failure);

    case Outcome::Skipped:
        // **不提"以后还会再问""最多再问一次"** —— 那是 asked_count，
        // 是我们的记账方式，不是用户需要知道的事。
        return u8"先不记这条。";

    case Outcome::MeaningLearned:
        return u8"记住了：" + quoted(p.subject) + u8"是指" +
               strip_leading_copula(p.answer) +
               u8"。以后整理纪要和检索的时候我会用上这个理解。";

    case Outcome::Corrected:
        return u8"好的，改成" + quoted(p.answer) +
               u8"了。以后遇到它我都按这个写法来。";

    case Outcome::Declined:
        if (p.kind == Kind::UpdateChangedValue) {
            return u8"好，那我保持原来的记录：" + quoted(p.subject) + u8"还是" +
                   quoted(p.previous) + u8"。";
        }
        return u8"明白，这个不用记。";

    case Outcome::Accepted:
        switch (p.kind) {
        case Kind::ConfirmPersonName:
            return u8"好的，我记住了 " + p.subject +
                   u8"。以后再遇到这个人名，我会按这个写法处理。";
        case Kind::ConfirmProjectName:
            return u8"好的，我记住了" + quoted(p.subject) +
                   u8"。以后纪要里我会按这个名字来写。";
        case Kind::ConfirmTerm:
            return u8"好的，我把" + quoted(p.subject) +
                   u8"记成项目里的固定术语了。以后遇到它我会保留原样，不翻成别的说法。";
        case Kind::AskTermMeaning:
            // 答 y 但没说含义：不许编一句出来，也不假装记了含义
            return u8"好，我记住了" + quoted(p.subject) +
                   u8"这个术语。它指什么我还不清楚，下次有机会再问你。";
        case Kind::UpdateChangedValue:
            return u8"好，我更新了：" + quoted(p.subject) + u8"现在是" +
                   quoted(p.current) + u8"。之前记的" + quoted(p.previous) +
                   u8"我留作历史，以后不会再用它。";
        case Kind::PickSpelling:
            // 「约束」原来写在这里，被自检的禁用词表抓住了 ——
            // 那是我们的内部词（指"喂给识别提示/翻译术语表的东西"），
            // 用户既不知道它，也不知道"不被用作约束"是好事还是坏事。
            return u8"好，以后认这个名字我只用" + quoted(p.answer) +
                   u8"。另一种写法我当成听错了。";
        case Kind::UnifySpelling:
            return u8"好，以后统一用" + quoted(p.answer) + u8"。";
        case Kind::SuggestCorrectSpelling:
            return u8"记下了：" + quoted(p.answer) + u8"。";
        }
        return u8"好的，记下了。";
    }
    return u8"好的，记下了。";
}

std::string summary(int accepted, int skipped, int failed) {
    std::string s;
    if (accepted > 0) {
        s = u8"—— 我记住了 " + std::to_string(accepted) + u8" 件事。";
    } else {
        s = u8"—— 这次没有要记的新东西。";
    }
    if (skipped > 0) {
        s += u8"另外 " + std::to_string(skipped) + u8" 件先放着。";
    }
    if (failed > 0) {
        s += u8"有 " + std::to_string(failed) + u8" 件没能记上（原因见上面）。";
    }
    return s;
}

std::string no_input_note() {
    return u8"（先到这儿，剩下的以后再说）";
}

std::string reuse_intro(const ReuseBrief& b) {
    // 【"另有 N 条"不许说死原因】confirmed_total > terms 有两种来源：
    //   ① 事实/决定类（整句话，本来就只作背景）
    //   ② 同一个词的两条记录（比如 person 和 term 各一条）被按值去重了
    // 我第一版写成"另有 N 条**事实/决定类**"，在 ② 的情况下就是错的
    // （实测 demo.db：多出来的那条是重复的「是的」，不是事实类）。
    // 所以只说它**不参与识别**这个事实，不解释为什么。
    const std::string bg_note =
        b.background_only > 0
            ? u8"\n      （另有 " + std::to_string(b.background_only) +
              u8" 条只用于整理纪要，不参与识别）"
            : std::string();

    if (b.items.empty()) {
        // 第一场演示的起点就是这一句。**不能返回空串** ——
        // "我现在什么都不记得"是要让用户知道的信息（否则他分不清
        // "系统没有记忆功能"和"系统有记忆但现在还是空的"）。
        return u8"我这边还没有关于你的任何记录，这一场从零开始听。" + bg_note;
    }

    std::string s = u8"开工之前先说一句：我带着之前学到的 " +
                    std::to_string(b.items.size()) + u8" 条知识。\n";
    for (const auto& it : b.items) {
        s += u8"        · " + it.name;
        if (!it.meaning.empty()) s += u8" —— " + it.meaning;
        s += u8"\n";
    }
    s += u8"      （识别时会偏向这些词，翻译时统一写法）" + bg_note;
    return s;
}

}  // namespace interaction
