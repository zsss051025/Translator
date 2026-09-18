#include "ConfirmGaps.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>
#include <ostream>

namespace knowledge {

// ===============================================================
// 输入解释（纯函数）
// ===============================================================

namespace {

// 去掉首尾的**不可见字符**。
//
// 【现象】真实路径实测：`"y`nn`nMarco" | Translator.exe --ask` 之后，
// 输出是「记住了：﻿y」—— 第一个回答被当成"用户给的新写法"，
// 于是字面量 "y"（前面还带个 BOM）被写进知识库，而且是 **confirmed**，
// 会直接进翻译约束。而自检里那 13 个输入解释用例全是绿的。
//
// 【原因】PowerShell 往管道里写第一行时会带上 UTF-8 BOM（EF BB BF）。
// 原来只去 ASCII 空白，BOM 不是空白，于是 "﻿y" != "y"，
// 既不是肯定词也不是否定词，就掉进了"其余一律当新值"那个分支。
//
// 【判断】把这些不可见字符一并当空白去掉，而且**不能只处理 BOM**：
//   · EF BB BF  BOM —— 管道/文件/记事本粘贴都会带
//   · E2 80 8B  零宽空格 —— 从网页复制专名时常见
//   · E2 80 8C/8D 零宽非连接/连接符
//   · E3 80 80  全角空格 —— 中文输入法下很容易打出来
// 这些字符用户看不见，但会让"是不是/要不要"的判定全部失效，
// 而且失效方式是**静默存进一条错知识**，比报错难查得多。
std::string strip_invisible(const std::string& s) {
    size_t b = 0, e = s.size();
    auto ascii_blank = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
    };
    // 从左边吃：ASCII 空白，或某个多字节不可见字符
    for (;;) {
        if (b >= e) break;
        const unsigned char c = static_cast<unsigned char>(s[b]);
        if (ascii_blank(c)) { ++b; continue; }
        auto at = [&](size_t i, const char* seq) {
            const size_t n = std::strlen(seq);
            return i + n <= e && std::memcmp(s.data() + i, seq, n) == 0;
        };
        if (at(b, "\xEF\xBB\xBF")) { b += 3; continue; }
        if (at(b, "\xE2\x80\x8B")) { b += 3; continue; }
        if (at(b, "\xE2\x80\x8C")) { b += 3; continue; }
        if (at(b, "\xE2\x80\x8D")) { b += 3; continue; }
        if (at(b, "\xE3\x80\x80")) { b += 3; continue; }
        break;
    }
    // 从右边吃
    for (;;) {
        if (e <= b) break;
        const unsigned char c = static_cast<unsigned char>(s[e - 1]);
        if (ascii_blank(c)) { --e; continue; }
        if (e >= b + 3) {
            const std::string tail = s.substr(e - 3, 3);
            if (tail == "\xEF\xBB\xBF" || tail == "\xE2\x80\x8B" ||
                tail == "\xE2\x80\x8C" || tail == "\xE2\x80\x8D" ||
                tail == "\xE3\x80\x80") { e -= 3; continue; }
        }
        break;
    }
    return s.substr(b, e - b);
}

std::string lower_ascii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x80) c = static_cast<char>(std::tolower(u));
    }
    return out;
}

// 剥掉句尾的**语气助词**。
//
// 【为什么必须有这一步 —— 真实事故，而且是数据损坏】
// 用户在"这是正确的人名写法吗？"下面回答了「是的」。
// 而词表里只有「是」「对的」「好的」，**没有「是的」** ——
// 于是它掉进了"其余内容一律当用户给出的正确写法"那个分支：
//     term/person  marco = 是的   status=confirmed
//     term/person  erica = 是的   status=confirmed
// 真实后果（实测 demo.db）：`initial_prompt = "EnglishPod, CarsPacked, 是的, TV"`，
// 翻译术语表里也有「是的」；而 Marco / Erica 这两个**正确的人名整个消失了**。
// 也就是说：**用户答了一句"是的"，系统把两个人名删了、塞进去两个字。**
//
// 【为什么用"剥语气词"而不是继续往表里加词】
// 「是的」「对的」「好的」「是啊」「对呀」「可以啊」「行吧」……这类说法穷举不完，
// 加一个漏一个。而它们的共同结构是**核心词 + 句尾语气助词**。
// 把尾巴剥掉再查表，一整类问题一起解决。
//
// 【为什么这样剥是安全的】只在"查肯定/否定表"时生效，不影响真正的值：
// 一个专名以「啊/吧/呀」结尾虽然可能（"小啊"），但那种情况下它也不会
// 正好等于表里的「是」「对」「好」—— 剥离后仍然不命中，原样当新值处理。
std::string strip_trailing_particles(const std::string& low) {
    // 只剥 ASCII 空白 + 常见句尾助词/标点。逐个剥，支持「是的呀」这种叠用。
    static const char* kParticles[] = {
        " ", "\t", "!", ".", "~", u8"的", u8"了", u8"呀", u8"啊", u8"吧",
        u8"嘛", u8"哦", u8"喔", u8"噢", u8"嘞", u8"啦", u8"咯", u8"咧",
        u8"哈", u8"哟", u8"唷", u8"呢", u8"滴", u8"。", u8"！", u8"～",
    };
    std::string s = low;
    for (;;) {
        bool cut = false;
        for (const char* p : kParticles) {
            const size_t n = std::strlen(p);
            if (s.size() >= n && s.compare(s.size() - n, n, p) == 0) {
                const std::string next = s.substr(0, s.size() - n);
                if (next.empty()) break;      // 别把整个词剥没
                s = next;
                cut = true;
                break;
            }
        }
        if (!cut) break;
    }
    return s;
}

// **触发表**（宁宽勿漏）：这些词算"是"。
//
// 和判定表分开是项目的既定规矩（PROJECT.md §8.3）：
// 往这里加词是安全的，改 interpret_answer 里的分支不是。
bool is_affirmative(const std::string& low) {
    static const char* kYes[] = {
        "y", "ye", "yes", "yeah", "yep", "yup", "ok", "okay", "okey", "sure",
        "right", "correct", "yes please",
        u8"是", u8"对", u8"好", u8"嗯", u8"行", u8"中", u8"能", u8"要",
        u8"没错", u8"正确", u8"确认", u8"可以", u8"就这样", u8"就是这个",
        u8"是这样的", u8"就是这样", u8"没问题", u8"对头", u8"妥", u8"妥了",
        u8"嗯嗯", u8"是滴", u8"好滴", u8"阔以",
    };
    for (const char* w : kYes) if (low == w) return true;
    return false;
}

// **触发表**：这些词算"否"。
//
// 【为什么必须有这张表】把 "n" 当成"用户输入的新值"，就会把字面量 "no"
// 写进知识库当专名 —— 而且是 confirmed，会直接进翻译约束。
// 这类污染一旦发生，用户看到的译文会莫名其妙地变成 "no"。
bool is_negative(const std::string& low) {
    static const char* kNo[] = {
        "n", "no", "nope", "wrong", "not", "never",
        u8"不", u8"不是", u8"不对", u8"错", u8"错了", u8"否", u8"不用",
        u8"不要", u8"没有", u8"没用", u8"不行", u8"别", u8"甭", u8"算了",
        u8"拉倒", u8"取消", u8"不必",
    };
    for (const char* w : kNo) if (low == w) return true;
    return false;
}

}  // namespace

Answer interpret_answer(const std::string& raw, size_t max_value_len) {
    Answer a;
    const std::string t = strip_invisible(raw);
    if (t.empty()) return a;                    // 回车 = 跳过，这是最主要的用法

    const std::string low = lower_ascii(t);
    // 先按原样查表，再按"剥掉句尾语气词"的形式查一遍。
    //
    // 两步都做是刻意的：「是的」剥成「是」命中；而表里若真有一个以「的」结尾的
    // 肯定词（比如「就是这样」不以此结尾，但将来可能加），原样查也不会漏。
    const std::string core = strip_trailing_particles(low);
    if (is_affirmative(low) || is_affirmative(core)) { a.kind = AnswerKind::Affirm; return a; }
    if (is_negative(low) || is_negative(core))        { a.kind = AnswerKind::Reject; return a; }

    // 其余内容一律当"用户给出的正确写法"。
    //
    // 【为什么要限长】用户可能随手粘一整句话进来（"我觉得应该是 Erica 吧，
    // 之前那个 Marko 好像是听错了"）。把它当值写进库，这条知识就废了，
    // 而且它会作为"术语约束"进到翻译 prompt 里。
    // 限长之后这种输入退化成 Skip —— 宁可少记一条，不可记错一条。
    //
    // 上限由调用方给：专名 60 字节足够，而"它指什么"要一整句话（见头文件）。
    if (t.size() > max_value_len) return a;

    // 【为什么必须要求"含字母或汉字"】真实事故（会话 #46/#47）：
    // 用户在某个问题上答了 `1`，于是库里多出一条
    //     term  preview = 1   status=confirmed
    // —— 一个值是纯数字的"专名"。它随后进了识别提示和翻译约束
    // （`constraint_terms` 只检查长度上限，没有下限）。
    //
    // 而"用数字回答"是很自然的习惯（有人会按"1/2/3"来选项）。
    // 专名不可能是纯数字或纯符号，所以这里直接判成 Skip —— 不猜他的意思。
    {
        bool has_word_char = false;
        for (const unsigned char c : t) {
            if ((c >= '0' && c <= '9') || c == ' ' || c == '\t') continue;
            has_word_char = true;      // 任何非数字非空白的字节都算（含 UTF-8 汉字/全角字符）
            break;
        }
        if (!has_word_char) return a;
    }

    a.kind  = AnswerKind::NewValue;
    a.value = t;
    return a;
}

ChoiceAnswer interpret_choice(const std::string& raw, size_t n_options) {
    ChoiceAnswer ca;
    const std::string t = strip_invisible(raw);

    // 纯数字（1..n）→ 选了第几个。**只认单个数字**：
    // 多位数、带别的字符都退回常规解释，避免把 "12" 这类输入
    // 在选项只有 2 个时误判成"选项 1 和 2"。
    if (t.size() == 1 && t[0] >= '1' && t[0] <= '9') {
        const int n = t[0] - '0';
        if (static_cast<size_t>(n) <= n_options) {
            ca.choice = n;
            return ca;
        }
    }

    // "n / 不是" 在选择题里是"这两个不是同一个东西"——
    // 和普通问题里的"否掉当前值"语义不同，所以单独一个标志。
    const std::string low = lower_ascii(t);
    if (!t.empty() && is_negative(low)) {
        ca.not_same = true;
        return ca;
    }

    ca.answer = interpret_answer(raw);
    // 「y」在选择题里没有确定含义（选哪个？），不替用户猜 —— 退化成 Skip。
    // 提示语里已经写明要输入编号。
    if (ca.answer.kind == AnswerKind::Affirm) ca.answer = Answer();
    return ca;
}

// ---------------------------------------------------------------------------
// 适配器：内部结构 → 交互层的输入模型
// ---------------------------------------------------------------------------
//
// 这个函数**只做事实搬运，不写任何面向用户的话**。
// 判断标准很简单：这里出现的每个字符串都必须是**数据**（用户说过/写过的原文），
// 一旦出现"请""吗""。「"这类语气词，就说明文案漏到这一层了。
//
// 【关于 familiarity】hits 是跨会话累加的（upsert 里 `hits = hits + 本次次数`），
// 所以它**不能**翻译成"这场听到 N 次"——那是错的数字。
// 它只能翻译成定性描述：1 次 / 不止一次 / 多次，三种说法在跨会话语义下都是真的。
namespace {

interaction::Familiarity familiarity_of(int hits) {
    if (hits >= 3) return interaction::Familiarity::ManyTimes;
    if (hits >= 2) return interaction::Familiarity::AFewTimes;
    return interaction::Familiarity::FirstTime;
}

bool is_person_kind(const std::string& kind) { return kind == "person"; }
bool is_project_kind(const std::string& kind) {
    return kind == "project";
}

}  // namespace

interaction::Prompt to_prompt(const GapQuestion& q) {
    interaction::Prompt p;
    p.subject     = q.value;
    p.familiarity = familiarity_of(q.hits);

    switch (q.rule) {
    case GapRule::ValueChanged:
        p.kind     = interaction::Kind::UpdateChangedValue;
        p.previous = q.old_value;
        p.current  = q.value;
        break;

    case GapRule::ConflictingSpellings:
        p.kind = interaction::Kind::PickSpelling;
        for (const auto& a : q.alternatives) p.options.push_back(a.value);
        p.current = q.in_use;
        break;

    case GapRule::InconsistentRendering:
        p.kind = interaction::Kind::UnifySpelling;
        break;

    case GapRule::AskDefinition:
        p.kind = interaction::Kind::AskTermMeaning;
        // 把别人猜的含义交给交互层 —— 有它问题就变成确认题（"我猜是指 X。对吗？"）。
        // **它只是问法**：写不写库由 apply_answer 决定（用户认了才写）。
        p.suggested = q.suggested_meaning;
        break;

    case GapRule::LowConfidenceName:
        p.kind = interaction::Kind::SuggestCorrectSpelling;
        break;

    case GapRule::HighFreqUnconfirmed:
    case GapRule::NewlySeen:
        // 同一个内部规则会因为**知识类型**给出完全不同的问法 ——
        // 这正是"按类型生成自然的问题"的落点，也是解耦的价值：
        // 规则侧只有两条，用户侧看到的是三种不同的问法。
        if (is_person_kind(q.kind))        p.kind = interaction::Kind::ConfirmPersonName;
        else if (is_project_kind(q.kind))  p.kind = interaction::Kind::ConfirmProjectName;
        else                               p.kind = interaction::Kind::ConfirmTerm;
        break;
    }
    return p;
}

interaction::Outcome to_outcome(const GapQuestion& q, ConfirmOutcome o) {
    switch (o) {
    case ConfirmOutcome::Skipped:           return interaction::Outcome::Skipped;
    case ConfirmOutcome::Failed:            return interaction::Outcome::Failed;
    case ConfirmOutcome::ConfirmedExisting: return interaction::Outcome::Accepted;
    case ConfirmOutcome::ConfirmedNewValue:
        // 教了含义和"改了个写法"是两件不同的事，回执也不该一样
        return q.rule == GapRule::AskDefinition ? interaction::Outcome::MeaningLearned
                                                : interaction::Outcome::Corrected;
    case ConfirmOutcome::RevertedToOld:     return interaction::Outcome::Declined;
    case ConfirmOutcome::RejectedUnchanged: return interaction::Outcome::Declined;
    }
    return interaction::Outcome::Skipped;
}

// ===============================================================
// 落库
// ===============================================================

ConfirmResult apply_answer(const GapQuestion& q, const Answer& a, std::string* err) {
    auto set_err = [&](const std::string& m) { if (err) *err = m; };
    ConfirmResult r;
    auto& ks = KnowledgeStore::instance();

    // ---- 写法冲突：用户选了一个写法 ----
    //
    // 【为什么要把其余的降级成 archived】它们不是"另一个知识"，而是**同一个词的听错版本**。
    // 留着它们会：① 继续被当成独立候选去问；② 万一被确认就进识别提示
    //    （实测发生过：`initial_prompt = "Marco, Erika"`，而正确的是 Erica）。
    // 降级而不是删除，是因为"我听过 Erika 这个写法"本身是有价值的历史
    // —— 而且删除不可逆（§6.5 的 archived 语义）。
    if (q.rule == GapRule::ConflictingSpellings && !a.value.empty() &&
        !q.alternatives.empty()) {
        KnowledgeItem item;
        item.kind        = q.kind;
        item.key         = knowledge::normalize_key(a.value);
        item.value       = a.value;
        item.status      = "confirmed";
        item.confidence  = 1.0;
        item.source_text = u8"用户在写法冲突时选定";

        std::string e1;
        const long long id = ks.upsert(item, &e1, "user_edited");
        if (id <= 0) {
            r.outcome = ConfirmOutcome::Failed;
            r.detail  = e1.empty() ? u8"写入知识库失败" : e1;
            set_err(r.detail);
            return r;
        }
        std::string e2;
        // 【必须跳过"已经是 confirmed"的情况】`can_promote` 里 `from == to` 一律返回 false
        // （"状态没变就不是一次状态变更"），于是对**已经确认过**的那个写法调
        // set_status(id, "confirmed") 会失败。
        //
        // 而这恰恰是最常见的答案："现在用的这个是对的，另一个是听错的" ——
        // 实测就撞上了：真跑一遍确认循环，第 1 问（Marco/Marko）报
        //     [失败] 状态不允许从 confirmed 变为 confirmed
        // 用户明明答对了，却被告知写不进去。
        //
        // 所以先读当前状态：已经是 confirmed 就算成功（本来就无需变更），
        // 不要拿"没变化"当错误报给用户。
        {
            KnowledgeItem cur;
            const bool known = ks.get(item.kind, item.key, &cur);
            if (known && cur.status == "confirmed") {
                // 已经是用户认可过的写法，什么都不用做
            } else if (!ks.set_status(id, "confirmed", "user_confirmed", &e2)) {
                r.outcome = ConfirmOutcome::Failed;
                r.detail  = e2.empty() ? u8"确认状态失败" : e2;
                set_err(r.detail);
                return r;
            }
        }

        int archived = 0;
        for (const auto& alt : q.alternatives) {
            if (knowledge::normalize_key(alt.value) == item.key) continue;
            std::string e3;
            if (ks.set_status(alt.knowledge_id, "archived", "superseded_by_spelling", &e3)) {
                ++archived;
            }
        }
        r.outcome = ConfirmOutcome::ConfirmedNewValue;
        r.value   = a.value;
        // archived 的数量也不往文案里塞 —— 交互层的 PickSpelling 回执
        // 本来就会说"另一种写法我标成听错了"，不需要一个会变的数字。
        (void)archived;
        return r;
    }

    // ---- 问含义：用户那句话**就是含义本身**，不是"正确的写法" ----
    //
    // 【为什么必须单独一条分支】对别的规则，NewValue 表示"这个词应该写成什么"，
    // 会写进 `value` 并进识别提示/翻译约束。而这条规则问的是"它指什么" ——
    // 把「Compile Once – Run Everywhere，我们 eBPF 项目的核心方案」
    // 当成 value 写进去，会**直接污染识别提示**（一句中文描述去当专名）。
    //
    // 用户答的这句话进 `definition`，出口只有摘要背景和检索。
    // 这是一个很容易写错、而且写错了很难看出来的地方：
    // 识别提示里混进一句中文，Whisper 不会报错，只会识别得更差。
    if (q.rule == GapRule::AskDefinition) {
        if (a.kind == AnswerKind::Skip || q.knowledge_id <= 0) {
            r.outcome = ConfirmOutcome::Skipped;
            return r;
        }

        // Reject：用户说"不是什么重要概念" → 保持 candidate，不改状态、不写含义。
        //
        // 故意**不 archive**：他说的是"这个概念不重要"，不是"这个词根本不存在"。
        // 归档是不可逆的"用户否掉"（§6.5），用在一个"重要性"判断上太重了。
        // 而且 asked_count 会让它最多再被问一次，不会没完没了。
        if (a.kind == AnswerKind::Reject) {
            r.outcome = ConfirmOutcome::RejectedUnchanged;
            // 措辞交给交互层：这一层只知道"用户拒绝了"这一个事实
            return r;
        }

        // 【用户认可引子时，把**那份猜测**写进含义】
        //
        // 有引子的问题（"我猜是指 Compile Once – Run Everywhere。对吗？"）
        // 答 y 的意思是"对，就是这个意思" —— 所以要把引子写成 definition。
        // 不写的话，用户明明确认了一份含义，库里却什么都没存。
        //
        // ⚠️ 这**不违反 §6.5**：模型猜的东西本身不能变成约束，
        // 但"用户确认过的内容"可以。这里写进去的是**用户刚刚点头认可的那句话**，
        // 模型只是起草人。区别就是"有没有经过确认"这一道 —— 那正是红线的本体。
        //
        // 注意：如果没有引子（模型不认识这个词），答 y 就只是"这确实是个重要概念"，
        // **不许编一句含义出来**（编了会污染摘要背景）。
        std::string meaning_to_store;
        if (a.kind == AnswerKind::NewValue) {
            meaning_to_store = a.value;
        } else if (a.kind == AnswerKind::Affirm && !q.suggested_meaning.empty()) {
            meaning_to_store = q.suggested_meaning;
        }

        // **只改 definition，不走 upsert。**
        //
        // 【为什么】upsert 的同值分支会 `hits + 1`，而"用户答了一句它指什么"
        // **不是又听到一次**。实测走 upsert 之后 `EnglishPod` 的 hits 从 3 变成 4 ——
        // 上一场会话明明只听到 3 次，界面上却写 4 次。给用户看的数字必须是真数字。
        if (!meaning_to_store.empty()) {
            std::string de;
            // ⚠️ 用 `q.key`（问题手里本来就有的键），**不要**用 `normalize_key(q.value)` 反推。
            //
            // 【为什么】自检抓到的真问题：我原来写的是 `q.value`，而 set_definition
            // 会自己再 normalize 一次。正常情况下 `normalize_key(value)` 恰好等于 key，
            // 所以看不出问题；但只要展示形和键有一点不一致（多一个标点、
            // 大小写折叠的边界情况），就会**查不到那一行 → 写入失败**，
            // 而用户看到的是"我确认了含义却没记住"。
            // 有现成的键就不要重新推导 —— 推导总会有一天推错。
            if (!ks.set_definition(q.kind, q.key, meaning_to_store, &de)) {
                r.outcome = ConfirmOutcome::Failed;
                r.detail  = de.empty() ? u8"写入含义失败" : de;
                set_err(r.detail);
                return r;
            }
        }

        // 状态：已经是 confirmed 就不必再 set（`can_promote` 里 from == to 返回 false，
        // 那会让"用户答对了却报失败"—— 这个坑 2.6d 已经踩过一次）。
        KnowledgeItem cur;
        const bool known = ks.get(q.kind, q.key, &cur);
        if (!(known && cur.status == "confirmed")) {
            std::string e2;
            if (!ks.set_status(q.knowledge_id, "confirmed", "user_confirmed", &e2)) {
                r.outcome = ConfirmOutcome::Failed;
                r.detail  = e2.empty() ? u8"确认状态失败" : e2;
                set_err(r.detail);
                return r;
            }
        }

        if (!meaning_to_store.empty()) {
            // 用户认可/给出了含义 → 这是"学到了内容"，不是"确认了个写法"
            r.outcome = ConfirmOutcome::ConfirmedNewValue;
            r.value   = meaning_to_store;
        } else {
            r.outcome = ConfirmOutcome::ConfirmedExisting;
            r.value   = q.value;
        }
        return r;
    }

    if (a.kind == AnswerKind::Skip || q.knowledge_id <= 0) {
        r.outcome = ConfirmOutcome::Skipped;
        return r;
    }

    // 用户给出的新写法：先 upsert（走真实写入路径，历史会自动记上），再确认。
    //
    // 【顺序不能反】upsert 发现值变了会**把 status 降回 candidate**（§6.5），
    // 所以必须 upsert 之后再 set_status，反了就会被降级覆盖掉。
    if (a.kind == AnswerKind::NewValue) {
        KnowledgeItem item;
        item.kind         = q.kind;
        item.key          = q.key;
        item.value        = a.value;
        item.status       = "confirmed";
        item.confidence   = 1.0;          // 用户亲口说的，置信度拉满
        item.source_text  = u8"用户在会话结束确认时给出";

        std::string e1;
        // reason = user_edited：§6.8 定义的三个原因之一。
        // 必须是 user_edited 而不是 value_changed_demoted ——
        // 后者会让"旧值≠新值"这条规则下一场会话又把同一个问题问一遍。
        const long long id = ks.upsert(item, &e1, "user_edited");
        if (id <= 0) {
            r.outcome = ConfirmOutcome::Failed;
            r.detail  = e1.empty() ? u8"写入知识库失败" : e1;
            set_err(r.detail);
            return r;
        }
        std::string e2;
        // 状态那条历史记 user_confirmed（值的那条已经由 upsert 记成 user_edited）。
        // §6.8 的三个原因各司其职，别让两条记成同一个 —— 否则以后想统计
        // "有多少条是用户确认的"就分不出来了。
        if (!ks.set_status(id, "confirmed", "user_confirmed", &e2)) {
            r.outcome = ConfirmOutcome::Failed;
            r.detail  = e2.empty() ? u8"确认状态失败" : e2;
            set_err(r.detail);
            return r;
        }
        r.outcome = ConfirmOutcome::ConfirmedNewValue;
        r.value   = a.value;
        return r;
    }

    if (a.kind == AnswerKind::Affirm) {
        std::string e;
        if (!ks.set_status(q.knowledge_id, "confirmed", "user_confirmed", &e)) {
            r.outcome = ConfirmOutcome::Failed;
            r.detail  = e.empty() ? u8"确认失败" : e;
            set_err(r.detail);
            return r;
        }
        r.outcome = ConfirmOutcome::ConfirmedExisting;
        r.value   = q.value;
        return r;
    }

    // Reject
    if (q.rule == GapRule::ValueChanged && !q.old_value.empty()) {
        // "以后还用旧的" —— 把值改回 old_value 并确认。
        // 注意 reason 也是 user_edited：这是用户明确的选择，不是模型的猜测。
        KnowledgeItem item;
        item.kind        = q.kind;
        item.key         = q.key;
        item.value       = q.old_value;
        item.status      = "confirmed";
        item.confidence  = 1.0;
        item.source_text = u8"用户在会话结束确认时选择沿用旧值";

        std::string e1;
        const long long id = ks.upsert(item, &e1, "user_edited");
        if (id <= 0) {
            r.outcome = ConfirmOutcome::Failed;
            r.detail  = e1.empty() ? u8"回退到旧值失败" : e1;
            set_err(r.detail);
            return r;
        }
        std::string e2;
        if (!ks.set_status(id, "confirmed", "user_edited", &e2)) {
            r.outcome = ConfirmOutcome::Failed;
            r.detail  = e2.empty() ? u8"确认状态失败" : e2;
            set_err(r.detail);
            return r;
        }
        r.outcome = ConfirmOutcome::RevertedToOld;
        r.value   = q.old_value;
        return r;
    }

    // 其它规则上答"否"：用户说它不对，但没给正确写法。
    //
    // 这里**故意不 archive**：归档是不可逆的"用户否掉"，而他只是说这个写法不对，
    // 可能下一句就会给正确的。保持 candidate 就够了 —— candidate 本来就进不了
    // 识别提示和翻译约束（§6.5），不会污染任何东西；
    // 而且 asked_count 会让它最多再被问一次（见 kMaxAsks），不会没完没了。
    r.outcome = ConfirmOutcome::RejectedUnchanged;
    // 这里只说"用户拒绝了"这一个**事实** —— 说给用户听的那句话由交互层生成。
    // 旧版这里是 `已记下这个写法不对 —— 它不会被用作约束`：
    // 「约束」是我们的内部词，用户不知道什么叫约束，也不知道"不被用作约束"是好事还是坏事。
    return r;
}

// ===============================================================
// 问答循环
// ===============================================================

ConfirmStats run_confirmation(const std::vector<GapQuestion>& questions,
                              const LineReader& read_line,
                              std::ostream& out,
                              size_t max_questions) {
    ConfirmStats stats;

    const size_t n = std::min(questions.size(), max_questions);
    if (n == 0) {
        // §1.3：没有问题就一个字都不说。这里刻意不打任何东西 ——
        // "这次没有问题"这种提示本身也是噪音，调用方需要就自己打。
        return stats;
    }

    out << "\n" << interaction::intro(n) << "\n";

    for (size_t i = 0; i < n; ++i) {
        const GapQuestion& q = questions[i];

        // 内部问题结构 → 交互层的输入模型。**这一层只搬事实，不写措辞。**
        interaction::Prompt prompt = to_prompt(q);

        out << "  " << (i + 1) << ". " << interaction::question(prompt) << "\n      "
            << interaction::hint(prompt) << "\n";

        std::string line;
        if (!read_line || !read_line(line)) {
            // 输入结束（EOF / Ctrl+C / GUI 里没有对话框）。
            // **不是错误** —— 交付物早就写完了，这里收尾即可。
            out << "      " << interaction::no_input_note() << "\n";
            break;
        }

        ++stats.asked;

        // 问过就记一次。**在 apply_answer 之前记** ——
        // 用户跳过也是"问过了"，只在答了才计数的话，同一个问题会永远排在候选里。
        //
        // 【写法冲突必须把组里**每一个**都记上】那个问题一次问了 N 个写法，
        // 只记第一个的话，剩下的几个下一场还会被单独问一遍 ——
        // 用户会觉得"我不是刚回答过这个吗"。一件事只问一次，计数就要跟着问题的范围走。
        std::vector<long long> asked_ids{q.knowledge_id};
        if (q.rule == GapRule::ConflictingSpellings) {
            for (const auto& alt : q.alternatives) {
                if (alt.knowledge_id != q.knowledge_id) asked_ids.push_back(alt.knowledge_id);
            }
        }
        for (const long long id : asked_ids) {
            std::string me;
            if (!KnowledgeStore::instance().mark_asked(id, &me)) {
                // 记不上不算致命：最坏的后果是这个问题下次还会问一遍。
                // 但它必须可见 —— 静默失败正是本项目反复栽的坑。
                out << u8"      [警告] 问答次数没记上（#" << id << "）：" << me << "\n";
            }
        }

        // 选择题（写法冲突）走**另一条**解析：它的数字输入是有意义的，
        // 而 interpret_answer 刻意把纯数字判成 Skip（见 interpret_choice 的说明）。
        Answer a;
        bool not_same = false;
        if (q.rule == GapRule::ConflictingSpellings) {
            const ChoiceAnswer ca = interpret_choice(line, q.alternatives.size());
            not_same = ca.not_same;
            if (ca.choice >= 1 && static_cast<size_t>(ca.choice) <= q.alternatives.size()) {
                a.kind  = AnswerKind::NewValue;
                a.value = q.alternatives[static_cast<size_t>(ca.choice) - 1].value;
            } else {
                a = ca.answer;
            }
        } else {
            // 「它指什么」要一整句话，所以单独放宽长度上限（见头文件）。
            a = interpret_answer(line, q.rule == GapRule::AskDefinition
                                            ? kMaxDefinitionLen : 60);
        }

        std::string ae;
        ConfirmResult r;
        if (not_same) {
            // "不是同一个东西" —— 两个都留着，但都还是 candidate。
            // **不 archive**：用户说的是"它们不是一回事"，不是"这两个都不对"。
            // 它们是两个真实存在的词，只是我误以为相近而已。
            r.outcome = ConfirmOutcome::RejectedUnchanged;
        } else {
            r = apply_answer(q, a, &ae);
        }

        // 回执：**由交互层根据"内部结论 + 用户给的答案"生成**。
        // 这一层不拼任何面向用户的句子，只负责把结论和值递过去。
        prompt.answer  = r.value.empty() ? a.value : r.value;
        prompt.failure = r.detail;      // detail 只在失败时有内容（见头文件）
        const interaction::Outcome oc = to_outcome(q, r.outcome);

        switch (r.outcome) {
        case ConfirmOutcome::Skipped:
            ++stats.skipped;
            break;
        case ConfirmOutcome::ConfirmedExisting:
        case ConfirmOutcome::ConfirmedNewValue:
        case ConfirmOutcome::RevertedToOld:
        case ConfirmOutcome::RejectedUnchanged:
            ++stats.confirmed;
            break;
        case ConfirmOutcome::Failed:
            ++stats.failed;
            break;
        }
        out << "      " << interaction::acknowledgment(prompt, oc) << "\n";
    }

    out << interaction::summary(stats.confirmed, stats.skipped, stats.failed) << "\n";
    return stats;
}

}  // namespace knowledge
