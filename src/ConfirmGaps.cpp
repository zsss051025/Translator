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

Answer interpret_answer(const std::string& raw) {
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
    constexpr size_t kMaxValueLen = 60;
    if (t.size() > kMaxValueLen) return a;

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

std::string answer_hint_for(GapRule rule) {
    switch (rule) {
    // 低置信度专名问的是"正确的写法是什么"，肯定词没有意义 —— 让提示说实话
    case GapRule::LowConfidenceName:
        return u8"（直接输入正确写法；认可当前写法就按 y；回车跳过）";
    case GapRule::ValueChanged:
        return u8"（回车 = 以后再问；y = 就用新的；n = 用回旧的；也可直接输入正确写法）";
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
        std::string me;
        if (!KnowledgeStore::instance().mark_asked(q.knowledge_id, &me)) {
            // 记不上不算致命：最坏的后果是这个问题下次还会问一遍。
            // 但它必须可见 —— 静默失败正是本项目反复栽的坑。
            out << u8"      [警告] 问答次数没记上：" << me << "\n";
        }

        const Answer a = interpret_answer(line);
        std::string ae;
        const ConfirmResult r = apply_answer(q, a, &ae);

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
