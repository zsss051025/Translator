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
    //   · 库里没有 (kind, key) → 新建，hits = 1
    //   · 已有且 value 相同     → 只累加 hits（**不写历史**：值没变，不是一次"变化"）
    //   · 已有且 value 不同     → 更新 value，把旧值写进 knowledge_history，
    //                             并且**把 status 降回 candidate** ——
    //                             值变了就意味着原来那条 confirmed 不再成立，
    //                             必须让用户重新确认一次（§6.5）
    //
    // 调用方负责先 normalize_key()。
    long long upsert(const KnowledgeItem& item, std::string* err = nullptr);

    bool get(const std::string& kind, const std::string& key, KnowledgeItem* out) const;

    // 按状态列条目。status 为空表示不过滤。
    std::vector<KnowledgeItem> list(const std::string& status = "", int limit = 200) const;

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
