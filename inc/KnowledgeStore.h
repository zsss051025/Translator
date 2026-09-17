#pragma once
#include <string>
#include <vector>

// 长期记忆（跨会话知识库）—— 见 PROJECT.md §6
//
// 这个文件里有两部分，**刻意分开**：
//   · 纯规则：normalize_key / usable_as_constraint / is_valid_kind ...
//     —— 不碰数据库、不碰模型，所以能进 L1 自检（秒级）
//   · KnowledgeStore：落库。它复用 SessionStore 的连接（见 .cpp 里的 friend 说明）
//
// 设计取向见 PROJECT.md §6.5：**模型猜的东西（Candidate）绝不能进识别提示和翻译约束**。
// 那条红线在这里由 usable_as_constraint() 单点守住，并且有自检用例。

// ---------------------------------------------------------------
// 一条知识条目（对应 knowledge 表的一行）
// ---------------------------------------------------------------
struct KnowledgeItem {
    long long   id             = 0;
    std::string kind;                  // term | fact | decision | person | project
    std::string key;                   // 归一化键：'erika' / 'asr_engine'
    std::string value;                 // 当前值：'Erika' / 'Qwen ASR'
    std::string status;                // confirmed | candidate | archived
    double      confidence     = 0.0;
    int         hits           = 0;    // 被听到过多少次（决定"该问什么"的排序）

    // **含义 / 定义**（步骤 2.7）。
    //
    // 【为什么必须有这个字段 —— 它是闭环里"越用越懂你"真正的内容】
    // 没有它，一条知识能表达的全部内容是"这个词该写成什么样"（key → value）。
    // 于是系统只能做到"把 Erica 认成 Erica"，**永远做不到"知道 CO-RE 是什么"**。
    // 而用户能教给系统的最有价值的东西恰恰是后者：
    //     CO-RE = Compile Once – Run Everywhere，我们 eBPF 项目的核心方案
    //
    // 【出口只有两个】摘要背景 + 检索。
    // **绝不进识别提示 / 翻译约束** —— 一句定义塞进 Whisper 的 initial_prompt
    // 只会挤掉真正的专名（那串只有 224 token 预算），对识别毫无帮助。
    std::string definition;

    // 问过用户几次 / 最后一次是什么时候（§1.3：提问是稀缺资源）
    // 没有这两个字段，用户跳过的同一个问题会每次会话原样再问一遍。
    int         asked_count    = 0;
    std::string last_asked_at;

    std::string first_seen_at;
    std::string updated_at;

    // 证据：哪次会话、哪一段、原话。不可省 —— 每条知识都要能回答"你凭什么这么说"
    long long   source_session = -1;
    long long   source_seq     = -1;
    std::string source_text;
};

// ---------------------------------------------------------------
// 纯规则（可进 L1 自检）
// ---------------------------------------------------------------
namespace knowledge {

// 允许的 kind。不在这里的一律拒绝 —— 与其让拼错的 kind 悄悄进库，
// 不如让它落不进去（这条库将来要用来约束识别和翻译，脏数据代价很高）。
bool is_valid_kind(const std::string& kind);

// 允许的 status
bool is_valid_status(const std::string& status);

// 归一化键。
//   · ASCII 转小写
//   · 去掉首尾空白
//   · 去掉常见标点（中英）
//   · 内部连续空白折成一个空格
//   · **全角字母数字转半角** —— 识别结果里 'Ｅｒｉｋａ' 和 'Erika' 必须归成同一个键
// 目的：同一个专名的不同写法要落到同一行，否则知识库会分裂成好几条。
std::string normalize_key(const std::string& raw);

// **§6.5 的红线，单点守在这里。**
// 只有 confirmed 的知识才允许影响识别提示 / 翻译约束 / 摘要背景。
// Candidate 只能展示和当背景参考 —— 模型猜错会污染整条链路，而且用户看不出来。
bool usable_as_constraint(const KnowledgeItem& item);

// 状态提升是否合法（candidate -> confirmed 允许；archived -> confirmed 也允许，
// 因为用户可能改主意；confirmed -> candidate 不允许，那是降级，要显式走 archive）
bool can_promote(const std::string& from_status, const std::string& to_status);

// 把用户随手打的一句话变成**合法且安全**的 FTS5 MATCH 查询串。
//
// 【现象】用户输入 `Q4, 2024` / `-foo` / `AND` / `(` 时，原文直接塞进 MATCH 会报
//         sqlite 语法错 —— 搜索框一打标点就崩。
// 【原因】MATCH 有自己的一套查询语法：双引号是短语，AND/OR/NOT/-/^/* 都是操作符。
//         用户输入的是**自然文本**，不是查询表达式。
// 【判断】按码点过滤：ASCII 只留字母数字；全角 ASCII 折半角后同样处理；
//         中文标点（。，、？！「」…）当分隔符；汉字/假名整字保留。
//         每个词包成双引号短语，词之间 AND。双引号在第一步就被当分隔符丢掉了，
//         所以**不可能**拼出非法语法。
std::string fts_query_from_user_text(const std::string& raw);

// ---------------------------------------------------------------
// §6.5 红线的**唯一出口**：把知识条目转成"可以影响识别/翻译/摘要"的东西
// ---------------------------------------------------------------
//
// 三条腿（Whisper 识别提示 / 翻译约束 / 摘要背景）都必须经由这里，
// 不许自己写过滤条件 —— 一旦有第二处判定，"只有 confirmed 能进约束"
// 就变成了 N 处各自为政，迟早有一处写漏（§6.5 的整条红线就废了）。
//
// 这三个都是纯函数，能进 L1。

// 哪些 kind 的值适合当"词条"（进识别提示和翻译约束）。
//
// 【为什么必须区分】term/person/project 的值是**短名字**（"Erika"、"Phoenix"），
// 放进 Whisper 的 initial_prompt 和翻译术语约束都有用。
// 而 fact/decision 的值是**句子**（"ASR 从 Whisper large-v3 换成了 Qwen ASR"）——
// 塞进 initial_prompt 只会让 Whisper 去续写那句话（实测提示回显就是这样来的），
// 塞进翻译约束更是毫无意义。它们只适合当摘要背景。
bool is_name_like_kind(const std::string& kind);

// kind 的中文标签，给摘要背景用
std::string kind_label_zh(const std::string& kind);

// 词条列表：usable_as_constraint() 为真 + name-like kind + 长度合理，去重后限量。
// 排序按 hits 降序 —— 提得多的更可能是真专名。
std::vector<std::string> constraint_terms(const std::vector<KnowledgeItem>& items,
                                          size_t max_terms = 40);

// 摘要背景行：形如 `- Erika（人名）`。**所有 kind 都收**（fact/decision 的价值正在这里）。
std::vector<std::string> background_lines(const std::vector<KnowledgeItem>& items,
                                          size_t max_lines = 15);

}  // namespace knowledge

// ---------------------------------------------------------------
// 落库
// ---------------------------------------------------------------
class KnowledgeStore {
public:
    static KnowledgeStore& instance();

    // 写入或更新一条知识，返回它的 id（失败返回 -1）。
    //
    // 语义：
    //   · 库里没有 (kind, key) → 新建，hits = max(1, item.hits)
    //   · 已有且 value 相同     → hits += max(1, item.hits)（**不写历史**：值没变，不是一次"变化"）
    //   · 已有且 value 不同     → 更新 value，把旧值写进 knowledge_history，
    //                             并且**把 status 降回 candidate** ——
    //                             值变了就意味着原来那条 confirmed 不再成立，
    //                             必须让用户重新确认一次（§6.5）
    //
    // `item.hits` **会被采信**（不是固定 +1）：抽取器知道"这个名字这场听到了几次"。
    // 第一版把它写死成 1，导致问题里"已经听到 N 次"是错数字，
    // 而且 `hits >= 3` 那条规则在一场会话内永远不可能为真。
    //
    // `reason_override`：历史里记的原因。空 = 按上下文自动判定
    //   （值变了 → `value_changed_demoted`，否则 → `model_extracted`）。
    //   用户主动改写的路径（2.4 确认交互）传 `user_edited` —— §6.8 定义了这三个原因，
    //   而"用户说的"和"模型猜的"必须能区分：前者不需要再问，后者才需要。
    //
    // 调用方负责先 normalize_key()。
    long long upsert(const KnowledgeItem& item, std::string* err = nullptr,
                     const std::string& reason_override = "");

    // 记一次"问过用户"（2.4 的确认交互在**真正问出口之后**调用）。
    //
    // 【为什么在"问"的时候记，而不是在"答"的时候】跳过也是回答。
    // 用户连按 5 次回车的意思就是"别再问了"，如果只在答了才计数，
    // 同一个问题会永远排在候选里 —— 稀缺性直接失效。
    bool mark_asked(long long id, std::string* err = nullptr);

    bool get(const std::string& kind, const std::string& key, KnowledgeItem* out) const;

    // 只写"含义"这一列，**不动其它任何字段**。
    //
    // 【为什么不能走 upsert】upsert 会顺手把 `hits + 1`（同值分支）。
    // 而"用户答了一句它指什么"**不是又听到一次** —— 走 upsert 的话，
    // 库里显示"这场听到 3 次"会变成 4 次，那是**给用户看的错数字**
    // （实测：答完 EnglishPod 的含义，hits 从 3 变成 4）。
    // 这个项目对"界面上显示的数字是真的"很在意，所以宁可为它单开一条写路径。
    //
    // 只做一件事：UPDATE definition。没有对应行时返回 false（不建新行）。
    bool set_definition(const std::string& kind, const std::string& key,
                        const std::string& definition, std::string* err = nullptr);

    // 按状态列条目。status 为空表示不过滤。
    std::vector<KnowledgeItem> list(const std::string& status = "", int limit = 200) const;

    // 全文检索（FTS5）。query 是**用户随手打的自然文本**，不是 FTS5 查询语法，
    // 内部会走 knowledge::fts_query_from_user_text() 清洗后交给 MATCH。
    // 命中 key / value / source_text 任一列，按相关性排序。
    //
    // 【为什么现在就加】2.3 之前整条链路里没有任何代码读 FTS 索引，
    // 于是"索引其实一条都没建起来"这个 bug 藏了两轮都没人发现。
    // 有 search() 之后，自检可以走真实写入路径（upsert）再真实检索，索引就再也藏不住了。
    // 2.6 的 `--ask` 会把它暴露成命令行。
    std::vector<KnowledgeItem> search(const std::string& query, int limit = 20,
                                      std::string* err = nullptr) const;

    // 可以进"识别提示 / 翻译约束 / 摘要背景"的条目 —— §6.5 红线的唯一出口。
    //
    // 只返回 usable_as_constraint() 为真的条目。三条腿都必须调它，
    // 不许自己按 status 过滤（原因见头文件里 knowledge::is_name_like_kind 上面的说明）。
    //
    // 实现上刻意**先取全部再过滤**，而不是 SQL 里写 `WHERE status='confirmed'`：
    // 让 usable_as_constraint() 成为真正唯一的判定点，而不是"两处判定碰巧一致"。
    std::vector<KnowledgeItem> constraint_items(int limit = 1000) const;

    // 改状态并记历史。reason 见 §6.8：user_confirmed | model_extracted | user_edited
    bool set_status(long long id, const std::string& status,
                    const std::string& reason, std::string* err = nullptr);

    // 某条知识的变化历史（供 §6.8 的"当前值 / 历史值"展示）
    struct HistoryRow {
        std::string old_value, new_value, changed_at, reason;
    };
    std::vector<HistoryRow> history(long long knowledge_id) const;

    // ---- 自检清理：删掉 key 以 prefix 开头的条目（含历史与 FTS 索引）----
    //
    // **只给自检用**，业务代码不该有"按前缀批量删知识"的能力。
    //
    // 【为什么是按 key 前缀而不是按 kind】kind 是有限的枚举（term/fact/...），
    // 按 kind 删会把用户真实的 "term" 条目一起删掉。
    // 自检用 `__selftest_` 前缀的 key，就只能删到自己的数据。
    // 这个坑是自检自己抓出来的：第一版用例用 kind="__selftest" 做隔离，
    // 结果被 kind 校验拒了；而"改成按 kind 删"又会误删真数据。
    int purge_key_prefix(const std::string& prefix);
};
