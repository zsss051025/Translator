#include "Interaction.h"

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
               u8"。这是你们项目里的一个技术术语吗？";

    // 一问两用：确认 + 学含义。需求原话就是这个形状
    //（「它是不是重要概念？如果是，它具体指什么？」）。
    // 合成一句是有意的 —— 分成两问会多占用户一次注意力，而含义才是重点。
    case Kind::AskTermMeaning:
        return std::string(familiarity_phrase(p.familiarity)) + u8" " + p.subject +
               u8"。这是你们项目里的一个技术术语吗？如果是，它指什么？";

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
        return u8"记住了：" + quoted(p.subject) + u8"是指" + p.answer +
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

}  // namespace interaction
