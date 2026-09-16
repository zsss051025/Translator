#include "KnowledgeStore.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>

#include "SessionStore.h"   // 复用 db_ 与 now_string()

// sqlite3 的声明。SessionStore.h 里只做了前向声明，这里需要真正的 API。
#include "sqlite3.h"

namespace {

// 允许的 kind / status —— 与 knowledge 表的注释、PROJECT.md §6.8 保持一致
const std::vector<std::string>& kinds() {
    static const std::vector<std::string> v = {
        "term", "fact", "decision", "person", "project",
    };
    return v;
}

const std::vector<std::string>& statuses() {
    static const std::vector<std::string> v = {"confirmed", "candidate", "archived"};
    return v;
}

bool in_list(const std::vector<std::string>& list, const std::string& x) {
    return std::find(list.begin(), list.end(), x) != list.end();
}

// 常见标点（ASCII），归一化键时当分隔符处理。
//
// 【为什么刻意不含下划线】`_` 是指示符字符，不是标点：
// PROJECT.md §6.8 的示例键就是 `asr_engine`。把它当分隔符会让
// `asr_engine` 变成 `asr engine`，示例和实现对不上；
// 更直接的是自检自己踩了：`__selftest_` 前缀被吃掉，清理匹配不到任何行。
bool is_punct(unsigned char c) {
    static const char* kAscii = " \t\r\n.,;:!?'\"()[]{}<>-–—/\\|@#$%^&*+=`~";
    return std::strchr(kAscii, static_cast<char>(c)) != nullptr;
}

// CJK 标点（码点）。
//
// 【为什么必须单独一张表】ASCII 标点表管不到它们，而 `to_halfwidth()` 只覆盖
// 全角 ASCII 区（U+FF01..U+FF5E）—— `。` 是 U+3002，在区外。
// 实测踩过：自检里 '埃里卡。' 归一化后仍带句号，于是"埃里卡"和"埃里卡。"
// 会变成两个不同的键，知识库从第一句起就分裂。
bool is_cjk_punct(unsigned cp) {
    switch (cp) {
    case 0x3001:  // 、
    case 0x3002:  // 。
    case 0x3008: case 0x3009:    // 〈〉
    case 0x300A: case 0x300B:    // 《》
    case 0x300C: case 0x300D:    // 「」
    case 0x300E: case 0x300F:    // 『』
    case 0x3010: case 0x3011:    // 【】
    case 0x3014: case 0x3015:    // 〔〕
    case 0x30FB:                 // ・
    case 0x00B7:                 // ·
    case 0x2014:                 // —
    case 0x2018: case 0x2019:    // ‘’
    case 0x201C: case 0x201D:    // “”
    case 0x2026:                 // …
        return true;
    default:
        return false;
    }
}

// UTF-8 解码一个码点；len 回传该码点占几个字节
unsigned decode_cp(const std::string& s, size_t i, size_t* len) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) { *len = 1; return c; }
    if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
        *len = 2;
        return ((c & 0x1F) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F);
    }
    if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
        *len = 3;
        return ((c & 0x0F) << 12) |
               ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
                (static_cast<unsigned char>(s[i + 2]) & 0x3F);
    }
    if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
        *len = 4;
        return ((c & 0x07) << 18) |
               ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 12) |
               ((static_cast<unsigned char>(s[i + 2]) & 0x3F) << 6) |
                (static_cast<unsigned char>(s[i + 3]) & 0x3F);
    }
    *len = 1;
    return c;
}

// 全角 -> 半角：只处理全角 ASCII 区（U+FF01..U+FF5E）与全角空格（U+3000）
// 为什么需要：识别结果里偶尔出现 'Ｅｒｉｋａ' 这种全角写法，
// 不归一会让同一个专名在库里分裂成两条。
std::string to_halfwidth(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { out.push_back(s[i]); ++i; continue; }

        // 3 字节的 UTF-8：E0-EF 开头
        if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
            const unsigned cp = ((c & 0x0F) << 12) |
                                ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
                                 (static_cast<unsigned char>(s[i + 2]) & 0x3F);
            if (cp == 0x3000) {                       // 全角空格
                out.push_back(' ');
            } else if (cp >= 0xFF01 && cp <= 0xFF5E) {  // 全角 ASCII
                out.push_back(static_cast<char>(cp - 0xFF01 + 0x21));
            } else {
                out.append(s, i, 3);
            }
            i += 3;
            continue;
        }
        // 其它多字节原样拷贝（中文汉字等）
        const size_t len = (c & 0xF8) == 0xF0 ? 4 : 2;
        if (i + len > s.size()) break;
        out.append(s, i, len);
        i += len;
    }
    return out;
}

}  // namespace

// ===============================================================
// 纯规则
// ===============================================================

namespace knowledge {

bool is_valid_kind(const std::string& kind)        { return in_list(kinds(), kind); }
bool is_valid_status(const std::string& status)    { return in_list(statuses(), status); }

std::string normalize_key(const std::string& raw) {
    const std::string half = to_halfwidth(raw);

    std::string out;
    out.reserve(half.size());
    bool prev_space = true;                 // 初值 true：吃掉开头的空白
    for (size_t i = 0; i < half.size();) {
        const unsigned char c = static_cast<unsigned char>(half[i]);
        if (c < 0x80) {
            if (is_punct(c)) {
                // 标点当分隔符：折成一个空格（"Q4, 2024" -> "q4 2024"）
                if (!prev_space) { out.push_back(' '); prev_space = true; }
            } else {
                out.push_back(static_cast<char>(std::tolower(c)));
                prev_space = false;
            }
            ++i;
            continue;
        }
        // 多字节：CJK 标点当分隔符，其它（汉字等）整段拷贝且**不算空白**
        size_t len = 1;
        const unsigned cp = decode_cp(half, i, &len);
        if (is_cjk_punct(cp)) {
            if (!prev_space) { out.push_back(' '); prev_space = true; }
        } else {
            out.append(half, i, len);
            prev_space = false;
        }
        i += len;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

bool usable_as_constraint(const KnowledgeItem& item) {
    // §6.5 的红线，就这一行。
    // 猜不出来只是没帮上忙；猜错了会污染整条链路，而且用户看不出来。
    return item.status == "confirmed" && !item.value.empty();
}

bool can_promote(const std::string& from_status, const std::string& to_status) {
    if (from_status == to_status) return false;
    if (!is_valid_status(from_status) || !is_valid_status(to_status)) return false;

    // 升到 confirmed：candidate 和 archived 都允许（用户可能改主意）
    if (to_status == "confirmed") return true;

    // 降到 candidate / archived：允许，但这是"用户明确否掉"的路径，
    // 由 set_status 记录 reason，不算越权。
    return true;
}

std::string fts_query_from_user_text(const std::string& raw) {
    std::string out, tok;
    auto flush = [&]() {
        if (tok.empty()) return;
        if (!out.empty()) out += " AND ";
        out += '"';
        out += tok;
        out += '"';
        tok.clear();
    };

    for (size_t i = 0; i < raw.size();) {
        size_t len = 0;
        const unsigned cp = decode_cp(raw, i, &len);

        if (cp < 0x80) {
            const bool alnum = (cp >= '0' && cp <= '9') ||
                               (cp >= 'a' && cp <= 'z') ||
                               (cp >= 'A' && cp <= 'Z');
            if (alnum) tok += static_cast<char>(cp);
            else       flush();
        } else if (cp >= 0xFF01 && cp <= 0xFF5E) {
            // 全角 ASCII：折成半角再判断。这样 'Ｅｒｉｋａ' 能被当成一个词、
            // '，'（U+FF0C）能被当成分隔符 —— 它落在全角 ASCII 区里。
            const char h = static_cast<char>(cp - 0xFF01 + 0x21);
            const bool alnum = (h >= '0' && h <= '9') ||
                               (h >= 'a' && h <= 'z') ||
                               (h >= 'A' && h <= 'Z');
            if (alnum) tok += h;
            else       flush();
        } else if (cp == 0x3000 || is_cjk_punct(cp)) {
            // 【为什么必须显式列中文标点】它们的字节全都 >= 0x80，
            // "只保留非 ASCII 字节"这种按字节过滤的写法会把 `，` `。` 一起留下，
            // 于是 `埃里卡，你好` 变成**一个**短语去匹配，永远查不到东西。
            // 这个坑是自检用例逼出来的。
            flush();
        } else {
            // CJK 汉字、假名、谚文等：整字保留
            tok += raw.substr(i, len);
        }
        i += len;
    }
    flush();
    return out;
}

}  // namespace knowledge

// ===============================================================
// 落库
// ===============================================================

KnowledgeStore& KnowledgeStore::instance() {
    static KnowledgeStore store;
    return store;
}

long long KnowledgeStore::upsert(const KnowledgeItem& item, std::string* err) {
    auto set_err = [&](const std::string& m) { if (err) *err = m; };

    if (!knowledge::is_valid_kind(item.kind)) {
        set_err("kind 非法: " + item.kind);
        return -1;
    }
    const std::string key = knowledge::normalize_key(item.key);
    if (key.empty()) {
        set_err("key 归一化后为空");
        return -1;
    }
    if (item.value.empty()) {
        set_err("value 为空");
        return -1;
    }
    std::string status = item.status.empty() ? "candidate" : item.status;
    if (!knowledge::is_valid_status(status)) {
        set_err("status 非法: " + status);
        return -1;
    }

    // 直接复用 SessionStore 的连接（friend）。同一把锁也一起用，
    // 否则两个类各自加锁会对同一连接产生并发访问。
    SessionStore& ss = SessionStore::instance();
    std::lock_guard<std::mutex> lock(ss.mutex_);
    if (ss.db_ == nullptr) {
        set_err("数据库未初始化");
        return -1;
    }
    sqlite3* db = ss.db_;

    const std::string ts = SessionStore::now_string();

    // 先查现有条目
    KnowledgeItem old;
    bool exists = false;
    {
        sqlite3_stmt* st = nullptr;
        const char* sql = "SELECT id, value, status, hits FROM knowledge "
                          "WHERE kind = ? AND key = ?;";
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
            set_err(std::string("查询失败: ") + sqlite3_errmsg(db));
            return -1;
        }
        sqlite3_bind_text(st, 1, item.kind.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, key.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            exists     = true;
            old.id     = sqlite3_column_int64(st, 0);
            old.value  = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
            old.status = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
            old.hits   = sqlite3_column_int(st, 3);
        }
        sqlite3_finalize(st);
    }

    if (!exists) {
        sqlite3_stmt* st = nullptr;
        const char* sql =
            "INSERT INTO knowledge"
            "(kind,key,value,status,confidence,hits,first_seen_at,updated_at,"
            " source_session,source_seq,source_text) "
            "VALUES (?,?,?,?,?,1,?,?,?,?,?);";
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
            set_err(std::string("插入失败: ") + sqlite3_errmsg(db));
            return -1;
        }
        sqlite3_bind_text  (st, 1, item.kind.c_str(),  -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (st, 2, key.c_str(),        -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (st, 3, item.value.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (st, 4, status.c_str(),     -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(st, 5, item.confidence);
        sqlite3_bind_text  (st, 6, ts.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (st, 7, ts.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64 (st, 8, item.source_session);
        sqlite3_bind_int64 (st, 9, item.source_seq);
        sqlite3_bind_text  (st, 10, item.source_text.c_str(), -1, SQLITE_TRANSIENT);

        const int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) {
            set_err(std::string("插入失败: ") + sqlite3_errmsg(db));
            return -1;
        }
        const long long id = sqlite3_last_insert_rowid(db);
        // 全文索引不用管：knowledge 上的 AFTER INSERT 触发器会自己维护。
        // （早先这里手写 INSERT INTO knowledge_fts，结果索引压根没建起来——见 SessionStore.cpp 的说明）
        return id;
    }

    // 已存在：值相同只累加 hits，**不写历史** —— 值没变，不是一次"变化"。
    if (old.value == item.value) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "UPDATE knowledge SET hits = hits + 1, "
                                   "updated_at = ?, confidence = ? WHERE id = ?;",
                               -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text  (st, 1, ts.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(st, 2, item.confidence);
            sqlite3_bind_int64 (st, 3, old.id);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
        return old.id;
    }

    // 值变了：更新 + 写历史 + **把 confirmed 降回 candidate**。
    //
    // 为什么必须降级：原来那条 confirmed 是用户针对**旧值**确认的，
    // 值一变，那个确认就不再成立。留着 confirmed 就等于让模型的新猜测
    // 借着用户对旧值的信任进到识别提示和翻译约束里 —— 正是 §6.5 要防的事。
    const bool demote = (old.status == "confirmed");
    const std::string new_status = demote ? "candidate" : old.status;

    {
        sqlite3_stmt* st = nullptr;
        const char* sql =
            "UPDATE knowledge SET value = ?, status = ?, confidence = ?, "
            "updated_at = ?, source_session = ?, source_seq = ?, source_text = ? "
            "WHERE id = ?;";
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
            set_err(std::string("更新失败: ") + sqlite3_errmsg(db));
            return -1;
        }
        sqlite3_bind_text  (st, 1, item.value.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (st, 2, new_status.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(st, 3, item.confidence);
        sqlite3_bind_text  (st, 4, ts.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64 (st, 5, item.source_session);
        sqlite3_bind_int64 (st, 6, item.source_seq);
        sqlite3_bind_text  (st, 7, item.source_text.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64 (st, 8, old.id);
        const int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) {
            set_err(std::string("更新失败: ") + sqlite3_errmsg(db));
            return -1;
        }
    }
    {
        sqlite3_stmt* st = nullptr;
        const char* sql =
            "INSERT INTO knowledge_history"
            "(knowledge_id,old_value,new_value,changed_at,source_session,source_seq,"
            " source_text,reason) VALUES (?,?,?,?,?,?,?,?);";
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
            const std::string reason = demote ? "value_changed_demoted" : "model_extracted";
            sqlite3_bind_int64(st, 1, old.id);
            sqlite3_bind_text (st, 2, old.value.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text (st, 3, item.value.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text (st, 4, ts.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 5, item.source_session);
            sqlite3_bind_int64(st, 6, item.source_seq);
            sqlite3_bind_text (st, 7, item.source_text.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text (st, 8, reason.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    }
    // 全文索引跟着改：由 knowledge 上的 AFTER UPDATE 触发器负责，这里不写任何 FTS 语句。
    return old.id;
}

bool KnowledgeStore::get(const std::string& kind, const std::string& key,
                         KnowledgeItem* out) const {
    if (out == nullptr) return false;
    const std::string nk = knowledge::normalize_key(key);

    SessionStore& ss = SessionStore::instance();
    std::lock_guard<std::mutex> lock(ss.mutex_);
    if (ss.db_ == nullptr) return false;

    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT id,kind,key,value,status,confidence,hits,first_seen_at,updated_at,"
        "source_session,source_seq,source_text FROM knowledge "
        "WHERE kind = ? AND key = ?;";
    if (sqlite3_prepare_v2(ss.db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, nk.c_str(), -1, SQLITE_TRANSIENT);

    bool found = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        found = true;
        auto text = [&](int i) -> std::string {
            const unsigned char* p = sqlite3_column_text(st, i);
            return p ? reinterpret_cast<const char*>(p) : "";
        };
        out->id             = sqlite3_column_int64(st, 0);
        out->kind           = text(1);
        out->key            = text(2);
        out->value          = text(3);
        out->status         = text(4);
        out->confidence     = sqlite3_column_double(st, 5);
        out->hits           = sqlite3_column_int(st, 6);
        out->first_seen_at  = text(7);
        out->updated_at     = text(8);
        out->source_session = sqlite3_column_int64(st, 9);
        out->source_seq     = sqlite3_column_int64(st, 10);
        out->source_text    = text(11);
    }
    sqlite3_finalize(st);
    return found;
}

std::vector<KnowledgeItem> KnowledgeStore::list(const std::string& status, int limit) const {
    std::vector<KnowledgeItem> out;

    SessionStore& ss = SessionStore::instance();
    std::lock_guard<std::mutex> lock(ss.mutex_);
    if (ss.db_ == nullptr) return out;

    std::string sql =
        "SELECT id,kind,key,value,status,confidence,hits,first_seen_at,updated_at,"
        "source_session,source_seq,source_text FROM knowledge";
    if (!status.empty()) sql += " WHERE status = ?";
    sql += " ORDER BY hits DESC, updated_at DESC LIMIT ?;";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(ss.db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return out;
    int idx = 1;
    if (!status.empty()) sqlite3_bind_text(st, idx++, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, idx, limit > 0 ? limit : 200);

    while (sqlite3_step(st) == SQLITE_ROW) {
        auto text = [&](int i) -> std::string {
            const unsigned char* p = sqlite3_column_text(st, i);
            return p ? reinterpret_cast<const char*>(p) : "";
        };
        KnowledgeItem k;
        k.id             = sqlite3_column_int64(st, 0);
        k.kind           = text(1);
        k.key            = text(2);
        k.value          = text(3);
        k.status         = text(4);
        k.confidence     = sqlite3_column_double(st, 5);
        k.hits           = sqlite3_column_int(st, 6);
        k.first_seen_at  = text(7);
        k.updated_at     = text(8);
        k.source_session = sqlite3_column_int64(st, 9);
        k.source_seq     = sqlite3_column_int64(st, 10);
        k.source_text    = text(11);
        out.push_back(std::move(k));
    }
    sqlite3_finalize(st);
    return out;
}

std::vector<KnowledgeItem> KnowledgeStore::search(const std::string& query, int limit,
                                                  std::string* err) const {
    auto set_err = [&](const std::string& m) { if (err) *err = m; };
    std::vector<KnowledgeItem> out;

    const std::string match = knowledge::fts_query_from_user_text(query);
    // 全是标点（比如用户只打了个 "??"）→ 没有可检索的词。
    // 这里必须提前返回：空 MATCH 串会让 SQLite 报错，而"什么都没输入"不该是个错误。
    if (match.empty()) return out;

    SessionStore& ss = SessionStore::instance();
    std::lock_guard<std::mutex> lock(ss.mutex_);
    if (ss.db_ == nullptr) { set_err("数据库未初始化"); return out; }

    // knowledge_fts 是**外部内容表**：MATCH 负责筛选，列值仍然从 knowledge 读。
    //
    // 【为什么 FTS 表不能起别名】实测：写成 `knowledge_fts f ... WHERE f MATCH ?`
    // SQLite 直接报 `no such column: f` —— MATCH 左侧必须是**表的原名**，别名不认。
    // 这个坑的代价被放大了，因为 prepare 失败时旧代码静默 return 空 vector，
    // 自检只看到"查不到"，看起来像索引坏了，其实是 SQL 根本没编译过。
    // 所以下面两件事一起做：用原名、失败时报错。
    const char* sql =
        "SELECT k.id,k.kind,k.key,k.value,k.status,k.confidence,k.hits,"
        "       k.first_seen_at,k.updated_at,k.source_session,k.source_seq,k.source_text "
        "FROM knowledge_fts JOIN knowledge k ON k.id = knowledge_fts.rowid "
        "WHERE knowledge_fts MATCH ? "
        "ORDER BY rank LIMIT ?;";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(ss.db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        // 不静默：SQL 有问题必须让人看见，而不是当成"没搜到"
        set_err(std::string("检索语句准备失败: ") + sqlite3_errmsg(ss.db_));
        return out;
    }
    sqlite3_bind_text(st, 1, match.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (st, 2, limit > 0 ? limit : 20);

    while (sqlite3_step(st) == SQLITE_ROW) {
        auto text = [&](int i) -> std::string {
            const unsigned char* p = sqlite3_column_text(st, i);
            return p ? reinterpret_cast<const char*>(p) : "";
        };
        KnowledgeItem k;
        k.id             = sqlite3_column_int64(st, 0);
        k.kind           = text(1);
        k.key            = text(2);
        k.value          = text(3);
        k.status         = text(4);
        k.confidence     = sqlite3_column_double(st, 5);
        k.hits           = sqlite3_column_int(st, 6);
        k.first_seen_at  = text(7);
        k.updated_at     = text(8);
        k.source_session = sqlite3_column_int64(st, 9);
        k.source_seq     = sqlite3_column_int64(st, 10);
        k.source_text    = text(11);
        out.push_back(std::move(k));
    }
    sqlite3_finalize(st);
    return out;
}

bool KnowledgeStore::set_status(long long id, const std::string& status,
                                const std::string& reason, std::string* err) {
    auto set_err = [&](const std::string& m) { if (err) *err = m; };
    if (!knowledge::is_valid_status(status)) {
        set_err("status 非法: " + status);
        return false;
    }

    SessionStore& ss = SessionStore::instance();
    std::lock_guard<std::mutex> lock(ss.mutex_);
    if (ss.db_ == nullptr) { set_err("数据库未初始化"); return false; }

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(ss.db_, "SELECT value, status FROM knowledge WHERE id = ?;",
                           -1, &st, nullptr) != SQLITE_OK) {
        set_err("查询失败"); return false;
    }
    sqlite3_bind_int64(st, 1, id);
    std::string old_value, old_status;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char* v = sqlite3_column_text(st, 0);
        const unsigned char* s = sqlite3_column_text(st, 1);
        old_value  = v ? reinterpret_cast<const char*>(v) : "";
        old_status = s ? reinterpret_cast<const char*>(s) : "";
    } else {
        sqlite3_finalize(st);
        set_err("条目不存在: " + std::to_string(id));
        return false;
    }
    sqlite3_finalize(st);

    if (!knowledge::can_promote(old_status, status)) {
        set_err("状态不允许从 " + old_status + " 变为 " + status);
        return false;
    }

    const std::string ts = SessionStore::now_string();
    st = nullptr;
    if (sqlite3_prepare_v2(ss.db_, "UPDATE knowledge SET status = ?, updated_at = ? WHERE id = ?;",
                           -1, &st, nullptr) != SQLITE_OK) {
        set_err("更新失败"); return false;
    }
    sqlite3_bind_text (st, 1, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 2, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, id);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { set_err("更新失败"); return false; }

    // 状态变化也进历史 —— 用户以后要能回答"这条什么时候被确认的"
    st = nullptr;
    if (sqlite3_prepare_v2(ss.db_,
            "INSERT INTO knowledge_history"
            "(knowledge_id,old_value,new_value,changed_at,source_session,source_seq,"
            " source_text,reason) VALUES (?,?,?,?,NULL,NULL,?,?);",
            -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        sqlite3_bind_text (st, 2, old_value.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (st, 3, old_value.c_str(), -1, SQLITE_TRANSIENT);  // 值没变，变的是状态
        sqlite3_bind_text (st, 4, ts.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (st, 5, (old_status + " -> " + status).c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (st, 6, reason.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    return true;
}

std::vector<KnowledgeStore::HistoryRow> KnowledgeStore::history(long long knowledge_id) const {
    std::vector<HistoryRow> out;

    SessionStore& ss = SessionStore::instance();
    std::lock_guard<std::mutex> lock(ss.mutex_);
    if (ss.db_ == nullptr) return out;

    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT old_value,new_value,changed_at,reason FROM knowledge_history "
        "WHERE knowledge_id = ? ORDER BY id ASC;";
    if (sqlite3_prepare_v2(ss.db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_int64(st, 1, knowledge_id);
    while (sqlite3_step(st) == SQLITE_ROW) {
        auto text = [&](int i) -> std::string {
            const unsigned char* p = sqlite3_column_text(st, i);
            return p ? reinterpret_cast<const char*>(p) : "";
        };
        HistoryRow h;
        h.old_value  = text(0);
        h.new_value  = text(1);
        h.changed_at = text(2);
        h.reason     = text(3);
        out.push_back(std::move(h));
    }
    sqlite3_finalize(st);
    return out;
}

int KnowledgeStore::purge_key_prefix(const std::string& prefix) {
    if (prefix.empty()) return 0;   // 空前缀等于删全表，直接拒绝

    SessionStore& ss = SessionStore::instance();
    std::lock_guard<std::mutex> lock(ss.mutex_);
    if (ss.db_ == nullptr) return 0;

    const std::string like = prefix + "%";

    // 先删历史（按 knowledge_id 关联），最后删主表。
    // FTS 索引不在这里删：knowledge 上的 AFTER DELETE 触发器会负责。
    const char* pre[] = {
        "DELETE FROM knowledge_history WHERE knowledge_id IN "
        "  (SELECT id FROM knowledge WHERE key LIKE ?);",
    };
    for (const char* sql : pre) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(ss.db_, sql, -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, like.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    }
    sqlite3_stmt* st = nullptr;
    int removed = 0;
    if (sqlite3_prepare_v2(ss.db_, "DELETE FROM knowledge WHERE key LIKE ?;",
                           -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, like.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
        removed = sqlite3_changes(ss.db_);
    }
    return removed;
}
