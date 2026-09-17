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

// **触发表**（宁宽勿漏）：这些词算"是"。
//
// 和判定表分开是项目的既定规矩（PROJECT.md §8.3）：
// 往这里加词是安全的，改 interpret_answer 里的分支不是。
bool is_affirmative(const std::string& low) {
    static const char* kYes[] = {
        "y", "ye", "yes", "yeah", "yep", "ok", "okay", "sure", "right", "correct",
        u8"是", u8"对", u8"对的", u8"好", u8"好的", u8"嗯", u8"没错", u8"正确",
        u8"确认", u8"可以", u8"就这样",
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
        "n", "no", "nope", "wrong", "not",
        u8"不", u8"不是", u8"不对", u8"错", u8"错了", u8"否", u8"不用", u8"不要",
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
    if (is_affirmative(low)) { a.kind = AnswerKind::Affirm; return a; }
    if (is_negative(low))    { a.kind = AnswerKind::Reject; return a; }

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

std::string answer_hint_for(GapRule rule) {
    switch (rule) {
    // 低置信度专名问的是"正确的写法是什么"，肯定词没有意义 —— 让提示说实话
    case GapRule::LowConfidenceName:
        return u8"（直接输入正确写法；认可当前写法就按 y；回车跳过）";
    case GapRule::ValueChanged:
        return u8"（回车 = 以后再问；y = 就用新的；n = 用回旧的；也可直接输入正确写法）";
    case GapRule::ConflictingSpellings:
        return u8"（输入编号选正确写法；n = 不是同一个东西，两个都留着；回车跳过）";
    case GapRule::AskDefinition:
        // 这条要用户**打一句话**，所以提示必须说清楚"直接说它的意思就行"。
        // 长度上限见 interpret_answer（60 字节）：定义通常够用，
        // 太长会被判成 Skip 而不是截断 —— 宁可少记一条，不可记错一条。
        return u8"（直接输入它的意思；只确认这是个重要概念就按 y；回车跳过）";
    case GapRule::InconsistentRendering:
    case GapRule::HighFreqUnconfirmed:
        return u8"（回车跳过；y = 认可；也可以直接输入正确写法）";
    }
    return u8"（回车跳过）";
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
        r.detail  = u8"记住了：" + a.value;
        if (archived > 0) {
            r.detail += u8"（另外 " + std::to_string(archived) +
                        u8" 个写法已标为听错，不再使用）";
        }
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
            r.detail  = u8"已记下 —— 它不会被当作重要概念";
            return r;
        }

        // **只改 definition，不走 upsert。**
        //
        // 【为什么】upsert 的同值分支会 `hits + 1`，而"用户答了一句它指什么"
        // **不是又听到一次**。实测走 upsert 之后 `EnglishPod` 的 hits 从 3 变成 4 ——
        // 上一场会话明明只听到 3 次，界面上却写 4 次。给用户看的数字必须是真数字。
        if (a.kind == AnswerKind::NewValue) {
            std::string de;
            if (!ks.set_definition(q.kind, q.value, a.value, &de)) {
                r.outcome = ConfirmOutcome::Failed;
                r.detail  = de.empty() ? u8"写入含义失败" : de;
                set_err(r.detail);
                return r;
            }
        }

        // 状态：已经是 confirmed 就不必再 set（`can_promote` 里 from == to 返回 false，
        // 那会让"用户答对了却报失败"—— 这个坑 2.6d 已经踩过一次）。
        KnowledgeItem cur;
        const bool known = ks.get(q.kind, knowledge::normalize_key(q.value), &cur);
        if (!(known && cur.status == "confirmed")) {
            std::string e2;
            if (!ks.set_status(q.knowledge_id, "confirmed", "user_confirmed", &e2)) {
                r.outcome = ConfirmOutcome::Failed;
                r.detail  = e2.empty() ? u8"确认状态失败" : e2;
                set_err(r.detail);
                return r;
            }
        }

        if (a.kind == AnswerKind::NewValue) {
            r.outcome = ConfirmOutcome::ConfirmedNewValue;
            r.detail  = u8"记住了「" + q.value + u8"」的含义：" + a.value;
        } else {
            r.outcome = ConfirmOutcome::ConfirmedExisting;
            r.detail  = u8"已确认：「" + q.value + u8"」是个重要概念（没记含义）";
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
        r.detail  = u8"记住了：" + a.value;
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
        r.detail  = u8"已确认：" + q.value;
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
        r.detail  = u8"已改回：" + q.old_value;
        return r;
    }

    // 其它规则上答"否"：用户说它不对，但没给正确写法。
    //
    // 这里**故意不 archive**：归档是不可逆的"用户否掉"，而他只是说这个写法不对，
    // 可能下一句就会给正确的。保持 candidate 就够了 —— candidate 本来就进不了
    // 识别提示和翻译约束（§6.5），不会污染任何东西；
    // 而且 asked_count 会让它最多再被问一次（见 kMaxAsks），不会没完没了。
    r.outcome = ConfirmOutcome::RejectedUnchanged;
    r.detail  = u8"已记下这个写法不对 —— 它不会被用作约束";
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

    out << u8"\n[确认] 这场会话里有 " << n << u8" 条知识想跟你核对（回车跳过，Enter 直接过）：\n";

    for (size_t i = 0; i < n; ++i) {
        const GapQuestion& q = questions[i];
        out << "  " << (i + 1) << ". " << q.question << "\n      "
            << answer_hint_for(q.rule) << "\n";

        std::string line;
        if (!read_line || !read_line(line)) {
            // 输入结束（EOF / Ctrl+C / GUI 里没有对话框）。
            // **不是错误** —— 交付物早就写完了，这里收尾即可。
            out << u8"      （输入结束，剩下的先不问；以后还会再提）\n";
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
            r.detail  = u8"已记下它们是两个不同的东西，两个都留着";
        } else {
            r = apply_answer(q, a, &ae);
        }
        switch (r.outcome) {
        case ConfirmOutcome::Skipped:
            ++stats.skipped;
            out << u8"      跳过（以后还会再问，最多再问一次）\n";
            break;
        case ConfirmOutcome::ConfirmedExisting:
        case ConfirmOutcome::ConfirmedNewValue:
        case ConfirmOutcome::RevertedToOld:
        case ConfirmOutcome::RejectedUnchanged:
            ++stats.confirmed;
            out << "      " << r.detail << "\n";
            break;
        case ConfirmOutcome::Failed:
            ++stats.failed;
            out << u8"      [失败] " << r.detail << "\n";
            break;
        }
    }

    out << u8"[确认] 记下 " << stats.confirmed << u8" 条，跳过 " << stats.skipped
        << u8" 条";
    if (stats.failed > 0) out << u8"，失败 " << stats.failed << u8" 条";
    out << "\n";
    return stats;
}

}  // namespace knowledge
