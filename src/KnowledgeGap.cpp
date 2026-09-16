#include "KnowledgeGap.h"

#include <algorithm>
#include <sstream>

namespace knowledge {

const char* to_string(GapRule r) {
    switch (r) {
    case GapRule::ValueChanged:          return "value_changed";
    case GapRule::InconsistentRendering: return "inconsistent_rendering";
    case GapRule::HighFreqUnconfirmed:   return "high_freq_unconfirmed";
    case GapRule::NewlySeen:             return "newly_seen";
    case GapRule::LowConfidenceName:     return "low_confidence_name";
    }
    return "unknown";
}

int priority_of(GapRule r) {
    // 越小越先问。排序依据是"不问的代价"：
    //   值冲突最危险 —— 库里可能已经是错的，而且它会进约束影响后续翻译
    //   译法不一致次之 —— 直接影响用户读到的译文质量
    //   高频未确认再次 —— 提了很多次却一直没定性，问一次收益最大
    //   本场新见再次 —— 自动抽取的出口，用户第一次用就是靠它把名字确定下来
    //                     （优先级低于上面三条：那三条是"已有知识出了问题"，
    //                      这条只是"还没有知识"，不问不会让现状变坏）
    //   低置信度最后 —— 大概率只是听错了，价值最低
    switch (r) {
    case GapRule::ValueChanged:          return 1;
    case GapRule::InconsistentRendering: return 2;
    case GapRule::HighFreqUnconfirmed:   return 3;
    case GapRule::NewlySeen:             return 4;
    case GapRule::LowConfidenceName:     return 5;
    }
    return 99;
}

namespace {

// 历史里出现过多少个**不同**的值（含当前值之外的历史值）
size_t distinct_values(const std::vector<KnowledgeStore::HistoryRow>& hist) {
    std::vector<std::string> vals;
    for (const auto& h : hist) {
        if (!h.new_value.empty() &&
            std::find(vals.begin(), vals.end(), h.new_value) == vals.end()) {
            vals.push_back(h.new_value);
        }
    }
    return vals.size();
}

// 历史里有没有"确认过的值被改掉"的记录 —— 对应 §6.6 的"旧值 ≠ 新值"
bool has_demote_record(const std::vector<KnowledgeStore::HistoryRow>& hist) {
    for (const auto& h : hist) {
        if (h.reason == "value_changed_demoted") return true;
    }
    return false;
}

// 找出被顶掉的旧值（最近一次 value_changed_demoted 的 old_value）
std::string last_demoted_old(const std::vector<KnowledgeStore::HistoryRow>& hist) {
    for (auto it = hist.rbegin(); it != hist.rend(); ++it) {
        if (it->reason == "value_changed_demoted") return it->old_value;
    }
    return {};
}

std::string fmt_conf(double c) {
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(2);
    oss << c;
    return oss.str();
}

}  // namespace

std::vector<GapQuestion> detect_gaps(const std::vector<KnowledgeWithHistory>& entries,
                                     size_t max_questions, long long current_session) {
    std::vector<GapQuestion> out;

    for (const auto& e : entries) {
        const KnowledgeItem& k = e.item;

        // archived 是用户明确否掉的，再问就是烦人（§1.3 提问稀缺）
        if (k.status == "archived") continue;
        if (k.value.empty()) continue;

        // 问够次数了就不再问。
        //
        // 【为什么这是"提问稀缺"的关键一笔】没有这一条，用户每次按回车跳过，
        // 下一场会话**同样这 5 个问题原样再来一遍** —— 第三次使用他就不看了。
        // 跳过也是回答：连按 5 次回车的意思就是"别再问了"。
        // 值发生变化时计数会被 upsert 清零（那时问题本身变了，值得再问）。
        if (k.asked_count >= kMaxAsks) continue;

        // 按优先级从高到低试四条规则，命中第一条就出题 ——
        // 同一个键最多出一条问题，否则用户会看到三条问同一件事的题。
        GapQuestion q;
        bool hit = false;

        // ① 值冲突：历史里有"确认过的值被改掉"的记录。
        //
        // 【必须有 status != confirmed 这个条件】实测踩过：
        // 用户答完"以后都用 Qwen ASR"之后，这条已经是 confirmed，
        // 但历史里那条 value_changed_demoted 还在 —— 只查历史的话，
        // **它下一场会话又是第 1 问**，直接违反 §1.3「confirmed 且无变化一个字都不问」。
        //
        // 语义上也说得通：confirmed + 有降级记录 = 用户**已经就这次变化做过决定**了。
        // （如果之后模型又改了值，upsert 会把它降回 candidate，那时自然会重新问。）
        if (k.status != "confirmed" && has_demote_record(e.history)) {
            q.rule      = GapRule::ValueChanged;
            q.old_value = last_demoted_old(e.history);
            q.question  = u8"这个改过：「" + q.old_value + u8"」→「" + k.value +
                          u8"」。以后都用「" + k.value + u8"」吗？";
            hit = true;
        } else if (k.status == "candidate" && distinct_values(e.history) >= 2) {
            q.rule     = GapRule::InconsistentRendering;
            q.question = u8"「" + k.value + u8"」的写法变过好几次。固定成哪一种？";
            hit = true;
        } else if (k.status == "candidate" && k.hits >= kHighFreqHits) {
            q.rule     = GapRule::HighFreqUnconfirmed;
            q.question = u8"已经听到 " + std::to_string(k.hits) + u8" 次「" + k.value +
                         u8"」，一直没确认过。它是对的说法吗？";
            hit = true;
        } else if (current_session > 0 && k.status == "candidate" &&
                   k.source_session == current_session && is_name_like_kind(k.kind)) {
            // 本场第一次见到的专名 → 问一次。
            //
            // 【为什么必须有这条规则】自动抽取（2.6）把专名落成候选之后，
            // 现有三条规则**一条都不会触发**：hits 只有 1~2（不到 kHighFreqHits）、
            // 置信度 0.7~0.9（不低）、没有历史（没变化）。
            // 于是抽出来的东西永远没人问，闭环在"抽取 → 询问"之间还是断的。
            //
            // 这条直接对应需求原话：「执行完后如果有陌生的知识就向用户提问然后更新知识库」。
            //
            // 只问专名形状的 kind（term/person/project）—— fact/decision 是句子，
            // 让用户去确认一整句话的措辞没有意义。
            q.rule     = GapRule::NewlySeen;
            q.question = u8"第一次听到「" + k.value + u8"」。这个词的写法对吗？";
            hit = true;
        } else if (k.status == "candidate" &&
                   k.confidence >= 0.0 && k.confidence < kLowConfidenceBelow) {
            q.rule     = GapRule::LowConfidenceName;
            q.question = u8"「" + k.value + u8"」的识别置信度只有 " + fmt_conf(k.confidence) +
                         u8"，可能是听错了。正确的写法是什么？";
            hit = true;
        }

        if (!hit) continue;

        q.knowledge_id = k.id;
        q.kind         = k.kind;
        q.key          = k.key;
        q.value        = k.value;
        q.hits         = k.hits;
        q.confidence   = k.confidence;
        out.push_back(std::move(q));
    }

    // 按优先级排；同优先级按 hits 多的在前（提得多的更值得问）
    std::stable_sort(out.begin(), out.end(),
                     [](const GapQuestion& a, const GapQuestion& b) {
                         const int pa = priority_of(a.rule), pb = priority_of(b.rule);
                         if (pa != pb) return pa < pb;
                         return a.hits > b.hits;
                     });

    if (out.size() > max_questions) out.resize(max_questions);
    return out;
}

std::vector<GapQuestion> detect_gaps_from_store(size_t max_questions, long long current_session) {
    std::vector<KnowledgeWithHistory> entries;

    // 取数口径：**全部条目**，不只 candidate。
    // 因为"值冲突"要看历史，而历史可能挂在任意状态的行上（比如用户手动改过的）。
    // 库规模是几百条量级，一次全取没有性能问题；判定逻辑全在 detect_gaps 里，
    // 那里才是"该不该问"的唯一口径。
    KnowledgeStore& ks = KnowledgeStore::instance();
    for (const auto& item : ks.list("", 1000)) {
        KnowledgeWithHistory e;
        e.item    = item;
        e.history = ks.history(item.id);
        entries.push_back(std::move(e));
    }
    return detect_gaps(entries, max_questions, current_session);
}

}  // namespace knowledge
