#include "KnowledgeGap.h"

#include <algorithm>
#include <sstream>

#include "CommonWords.h"   // 通用概念过滤：别问人人皆知的东西

namespace knowledge {

const char* to_string(GapRule r) {
    switch (r) {
    case GapRule::ValueChanged:          return "value_changed";
    case GapRule::ConflictingSpellings:  return "conflicting_spellings";
    case GapRule::InconsistentRendering: return "inconsistent_rendering";
    case GapRule::HighFreqUnconfirmed:   return "high_freq_unconfirmed";
    case GapRule::AskDefinition:         return "ask_definition";
    case GapRule::NewlySeen:             return "newly_seen";
    case GapRule::LowConfidenceName:     return "low_confidence_name";
    }
    return "unknown";
}

int priority_of(GapRule r) {
    // 越小越先问。排序依据是"不问的代价"：
    //   值冲突最危险 —— 库里可能已经是错的，而且它会进约束影响后续翻译
    //   写法冲突次之 —— 同上，而且用户**答错的可能性最大**（选项不摆出来他只能猜）
    //   译法不一致再次 —— 同一个键的值变过多次
    //   高频未确认再次 —— 提了很多次却一直没定性，问一次收益最大
    //   **问含义** 排在这里 —— 它不会让现状变坏（不问只是"少懂一点"），
    //                       但它是**唯一能让系统真正学到内容**的问题，
    //                       所以排在"没有知识"那两条前面，且每场限量 1 个
    //   本场新见再次 —— 自动抽取的出口，用户第一次用就是靠它把名字确定下来
    //   低置信度最后 —— 大概率只是听错了，价值最低
    switch (r) {
    case GapRule::ValueChanged:          return 1;
    case GapRule::ConflictingSpellings:  return 2;
    case GapRule::InconsistentRendering: return 3;
    case GapRule::HighFreqUnconfirmed:   return 4;
    case GapRule::AskDefinition:         return 5;
    case GapRule::NewlySeen:             return 6;
    case GapRule::LowConfidenceName:     return 7;
    }
    return 99;
}

namespace {

// 历史里出现过多少个**不同**的值（含当前值之外的历史值）
//
// ⚠️ **必须跳过 `definition_defined`**：含义不是"写法"。
// 2.8 把"用户教了含义"也记进了 knowledge_history（否则时间线里看不见这一步），
// 如果不在这里排除，一句含义就会被当成一个"不同的值" ——
// 于是"译法不一致"（同一个键的值变过好几次）会凭空误报，
// 而那条问题会问用户"「EnglishPod」的写法变过好几次，固定成哪一种？" ——
// 用户完全不知道它在说什么。
size_t distinct_values(const std::vector<KnowledgeStore::HistoryRow>& hist) {
    std::vector<std::string> vals;
    for (const auto& h : hist) {
        if (h.reason == "definition_defined") continue;
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

// 【fmt_conf 已删除】它唯一的作用是把 confidence 格式化进面向用户的句子里
//（「识别置信度只有 0.40」）。文案搬去 Interaction 之后这个函数没有调用者了 ——
// 而它没有调用者正好说明**内部数值不再出现在任何给用户看的文本里**。

// ---- 同一个实体的多种写法（ConflictingSpellings）---------------------------

// 只差一个字符吗？要求同首字母、长度差 ≤1、总长 ≥4。
//
// 【为什么条件卡这么紧】判错的代价是"把两个不相干的东西问成同一个"
// —— 那会让用户在两个无关词之间做选择，比不问更糟。
//   · 首字母必须相同：`Marco`/`Narco` 是编辑距离 1，但它们是不同的词，
//     而现实里听错的首字母极少变（辅音听错更常见：c/k、b/p、s/z）。
//   · 长度 ≥4：`PC`/`PB`/`TV` 这种两字母词编辑距离 1 太容易碰上，
//     而且它们本来就靠 R3 缩写形状进来的，含义完全不同。
//   · 编辑距离 ≤1：`Erica`/`Erika`(1)、`Marco`/`Marko`(1)、`Samuel`/`Samual`(1)。
//     `EnglishPod`/`EnglishPot`(1) 也会命中 —— 这是想要的。
bool likely_same_entity(const std::string& a, const std::string& b) {
    if (a == b) return false;
    if (a.size() < 4 || b.size() < 4) return false;
    if (a[0] != b[0]) return false;
    const size_t la = a.size(), lb = b.size();
    if (la > lb + 1 || lb > la + 1) return false;

    // 编辑距离（只有插入/替换/删除，不需要完整 DP 表）
    std::vector<int> prev(lb + 1), cur(lb + 1);
    for (size_t j = 0; j <= lb; ++j) prev[j] = static_cast<int>(j);
    for (size_t i = 1; i <= la; ++i) {
        cur[0] = static_cast<int>(i);
        for (size_t j = 1; j <= lb; ++j) {
            const int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        prev.swap(cur);
    }
    return prev[lb] <= 1;
}

}  // namespace

std::vector<GapQuestion> detect_gaps(const std::vector<KnowledgeWithHistory>& entries,
                                     size_t max_questions, long long current_session) {
    std::vector<GapQuestion> out;

    // 本场已经问出去几个"它指什么"（见 kMaxDefinitionAsksPerSession）
    size_t def_asked = 0;

    // ---- 第 0 步：先找出"同一个实体的多种写法"，合并成一条问题 ----
    //
    // 这一步必须在逐条出题**之前**做，否则那几个键会各自被问一遍
    // —— 那正是用户真跑 #13 遇到的情况（Erica 和 Erika 被问成两个独立问题，
    // 他的两个回答互相矛盾，而且他无从知道该矛盾）。
    //
    // 命中的键会被标记 covered，逐条循环里直接跳过：一件事只问一次。
    std::vector<bool> covered(entries.size(), false);
    {
        // 候选**和已确认的都要看**。
        //
        // 【为什么不能只看 candidate —— 用户真跑 #13 就是这个形态】
        // 那一场之后库里是：`Erika` = confirmed（用户按了 y）、`Erica` = candidate。
        // 只扫 candidate 的话，这个冲突**永远问不出来**：
        // 而它恰恰是最该问的一次 —— confirmed 的那个正在进识别提示
        // （实测 `initial_prompt = "Marco, Erika"`），错的那一个**已经在生效了**。
        //
        // 【和"confirmed 不再问"冲突吗】不冲突。那条规矩（§1.3）防的是
        // "**同一件事**反复问"。这里是一个**新出现的、与已确认值矛盾的事实** ——
        // 与"值被改掉了要重新问"（ValueChanged）是同一个道理。
        // 而且 `asked_count >= kMaxAsks` 这道闸仍然生效，问两次就停。
        std::vector<size_t> cand;
        for (size_t i = 0; i < entries.size(); ++i) {
            const KnowledgeItem& k = entries[i].item;
            if (k.status == "archived") continue;
            if (k.value.empty() || k.asked_count >= kMaxAsks) continue;
            if (!is_name_like_kind(k.kind)) continue;
            // 通用概念不参与写法冲突 —— 否则「TV/TVs」这种会被当成
            // "同一个实体的两种写法"去问，而它们根本不需要被讨论。
            if (commonwords::is_general(k.key)) continue;
            cand.push_back(i);
        }

        for (size_t x = 0; x < cand.size(); ++x) {
            if (covered[cand[x]]) continue;
            std::vector<size_t> group{cand[x]};
            for (size_t y = x + 1; y < cand.size(); ++y) {
                if (covered[cand[y]]) continue;
                if (likely_same_entity(entries[cand[x]].item.key,
                                       entries[cand[y]].item.key)) {
                    group.push_back(cand[y]);
                }
            }
            if (group.size() < 2) continue;

            // hits 多的排前面 —— 选项 1 是"证据更多的那一个"。
            //
            // 【为什么不把 confirmed 排前面】看起来"已经确认过的"更该当默认，
            // 但用户这个真实案例恰恰相反：confirmed 的是听错的 `Erika`（2 次），
            // candidate 的才是对的 `Erica`（3 次）。按状态排序会把错的那个
            // 摆在第一位，等于**用一个错误答案去引导用户**。
            // 所以按纯证据（hits）排，并且把"现在用的是哪个"显式写进问题里
            // —— 让用户知道改动的影响面，而不是靠位置暗示。
            std::sort(group.begin(), group.end(), [&](size_t a, size_t b) {
                return entries[a].item.hits > entries[b].item.hits;
            });

            GapQuestion q;
            q.rule = GapRule::ConflictingSpellings;
            const KnowledgeItem& first = entries[group[0]].item;
            q.knowledge_id = first.id;
            q.kind         = first.kind;
            q.key          = first.key;
            q.value        = first.value;
            q.hits         = first.hits;

            for (const size_t gi : group) {
                // ⚠️ `gi` **就是** entries 的下标（group 里装的是下标）。
                // 2.8 改写这段时我写成了 `entries[group[gi]]` —— 多套了一层，
                // 于是选项顺序被打乱（自检报 `Erika(2) Erica(3)`，没按 hits 排），
                // 而且 gi 一大就越界。自检抓住的就是这个。
                const KnowledgeItem& it = entries[gi].item;
                GapQuestion::Alt alt;
                alt.knowledge_id = it.id;
                alt.value        = it.value;
                alt.hits         = it.hits;
                q.alternatives.push_back(alt);
                covered[gi] = true;
                if (it.status == "confirmed") q.in_use = it.value;
            }
            out.push_back(std::move(q));
        }
    }

    for (size_t ei = 0; ei < entries.size(); ++ei) {
        const auto& e = entries[ei];
        const KnowledgeItem& k = e.item;

        // 已经被"写法冲突"那条合并问过，不再单独问（一件事只问一次）
        if (covered[ei]) continue;

        // archived 是用户明确否掉的，再问就是烦人（§1.3 提问稀缺）
        if (k.status == "archived") continue;
        if (k.value.empty()) continue;

        // ---- 通用概念：不问 --------------------------------------------------
        //
        // 【这一条是干什么的】用户的原话是要"只问那些可能只会在我会话里出现的名词"。
        // 而这一层就是那句要求的落点：**人人皆知的东西，问了只是浪费用户的注意力。**
        //
        // 【为什么不能交给模型判断】可以，但要等 agent 那条路（5.2）。
        // 在那之前，先把**真实发生过的冤枉提问**用一张确定性表挡掉 ——
        // 表里每一条都来自真实会话（见 inc/CommonWords.h 的注释），
        // 所以它能进 L1、秒级可验，而且不会因为换了个模型就失效。
        //
        // 【为什么放在提问阶段，而不是提取阶段】提取阶段的职责是"尽量别漏"，
        // 提问阶段的职责是"值得问才问"。两个阶段目标相反（见 CommonWords.h 的说明）。
        // 在这里挡掉，候选仍然留在库里（`--gaps` 会标出"[通用词，不问]"），
        // 看得见、可复查，而不是悄悄消失。
        if (commonwords::is_general(k.key)) continue;

        // 问够次数了就不再问。
        //
        // 【为什么这是"用户表过态"而不是"稀缺"】没有这一条，用户每次按回车跳过，
        // 下一场会话**同样的问题原样再来一遍** —— 那是**无视用户的表达**。
        // 跟"少问几个"没关系，跟"问得准不准"也没关系。
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
            hit = true;
        } else if (k.status == "candidate" && distinct_values(e.history) >= 2) {
            q.rule     = GapRule::InconsistentRendering;
            hit = true;
        } else if (k.kind == "term" && k.definition.empty() &&
                   (k.status == "confirmed" || k.hits >= 1)) {
            // ③ 术语还没有**含义** → 问"它指什么"。
            //
            // 【为什么排在高频未确认**之前**】因为这一问本来就是**一问两用** ——
            // 需求原话就是这么写的：
            //     「它是你们项目中的一个重要技术概念吗？如果是，它具体指什么？」
            // 用户答"是，指 Compile Once – Run Everywhere"这一句，
            // 同时完成了**确认**（它值得记）和**学含义**（它是什么）。
            // 排在高频后面的话，一个 hits=3 的术语会先被问"我打算把它记成术语，对吗？"，
            // 用户按个 y 就结束了 —— 我们拿到了"确认"却永远拿不到"含义"，
            // 而含义才是闭环里唯一真正让系统"懂"点什么的东西。
            //
            // 【为什么只问 term，不问 person】"Erica 指什么"是个没有意义的问题
            // （人名不需要定义）。机构/项目名勉强可以问，但那更像闲聊，
            // 而提问是稀缺资源 —— 先只做"技术术语"这一类最明确有价值的。
            //
            // 【没有它 `knowledge.definition` 永远是空的】
            // 这是 §6.4③（含义复用）唯一的入口。
            //
            // 【为什么每场只问 1 个】见 kMaxDefinitionAsksPerSession：
            // 用户要打一整句话，问三个他就不答了。
            if (def_asked < kMaxDefinitionAsksPerSession) {
                q.rule = GapRule::AskDefinition;
                hit = true;
                ++def_asked;
            }
        } else if (k.status == "candidate" && k.hits >= kHighFreqHits) {
            q.rule     = GapRule::HighFreqUnconfirmed;
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
            hit = true;
        } else if (k.status == "candidate" &&
                   k.confidence >= 0.0 && k.confidence < kLowConfidenceBelow) {
            // 【旧文案曾经把 confidence 直接打给用户】——「识别置信度只有 0.40」。
            // 那是我们的内部数值，用户既看不懂也不知道该怎么用。
            // 现在这里只留事实（这条规则命中了），措辞由 Interaction 层决定。
            q.rule     = GapRule::LowConfidenceName;
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
