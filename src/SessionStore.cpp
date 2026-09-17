#include "SessionStore.h"
#include "sqlite3.h"

#include <chrono>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace {


// 【为什么拆成两段 + 拼起来】FTS 那段 DDL 需要被**第二次**用到：
// 给 `definition` 加列时，老库的 FTS 表和触发器必须整个重建
// （`CREATE VIRTUAL TABLE IF NOT EXISTS` 不会给已存在的表加列，
//   `CREATE TRIGGER IF NOT EXISTS` 同理不会替换触发器），
// 重建时要用同一份 DDL。抄第二份就是等着两边走散 —— 本项目已经栽过三次。
// 合并定义放在两个常量**之后**（见 kFtsDdl 末尾）。
const char* kSchemaCore =
    "CREATE TABLE IF NOT EXISTS sessions ("
    "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  started_at TEXT NOT NULL,"
    "  ended_at   TEXT,"
    "  engine     TEXT NOT NULL,"
    "  note       TEXT NOT NULL DEFAULT ''"
    ");"
    "CREATE TABLE IF NOT EXISTS segments ("
    "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  session_id INTEGER NOT NULL,"
    "  seq        INTEGER NOT NULL,"
    "  ts         TEXT    NOT NULL,"
    "  src_text   TEXT    NOT NULL,"
    "  tgt_text   TEXT    NOT NULL,"
    "  engine     TEXT    NOT NULL,"
    "  ms         INTEGER NOT NULL,"
    "  confidence REAL    NOT NULL DEFAULT -1.0"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_segments_session ON segments(session_id, seq);"

    // ============================================================
    // 长期记忆（跨会话知识库）—— 见 PROJECT.md §6.8
    //
    // 全部用 IF NOT EXISTS，所以对**已存在的旧库**就是一次自动迁移：
    // 老库打开后会把这三张表补上，不需要单独的版本号/迁移脚本。
    // ============================================================

    // 知识条目（当前值）。
    // kind/key 唯一：同一个键只保留一个当前值，历史值进 knowledge_history。
    "CREATE TABLE IF NOT EXISTS knowledge ("
    "  id             INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  kind           TEXT NOT NULL,"              // term | fact | decision | person | project
    "  key            TEXT NOT NULL,"              // 归一化键：'erika' / 'asr_engine'
    "  value          TEXT NOT NULL,"              // 当前值：'Erika' / 'Qwen ASR'
    "  status         TEXT NOT NULL,"              // confirmed | candidate | archived
    "  confidence     REAL NOT NULL DEFAULT 0.0,"
    "  hits           INTEGER NOT NULL DEFAULT 0," // 被听到过多少次（决定"该问什么"的排序）
    // 问过用户几次 / 最后一次是什么时候。
    //
    // 【为什么必须有这两列】没有它们，"提问是稀缺资源"（§1.3）就是句口号：
    // 用户每次按回车跳过，下一次会话**同样这 5 个问题原样再问一遍**。
    // 有了计数，每条知识最多问 kMaxAsks 次（见 KnowledgeGap.h），问过就沉底。
    // 值发生变化时计数会清零（在 upsert 里）—— 那时候问题本身变了，值得再问一次。
    "  asked_count    INTEGER NOT NULL DEFAULT 0,"
    "  last_asked_at  TEXT,"
    // **含义 / 定义**（2026-09-17 补，步骤 2.7）。
    //
    // 【为什么 knowledge 表必须有这一列 —— 这是"闭环"最要紧的一环】
    // 在它之前，一条知识能表达的全部内容是"这个词该写成什么样"（key → value 的写法映射）。
    // 于是系统能做的只有"把 Erica 认成 Erica"，永远做不到**知道 CO-RE 是什么**。
    // 而"越用越懂你"的真实价值恰恰在后一半：
    //     第一次会议：用户提到 CO-RE，系统只知道多了个陌生词
    //     问用户 →「指 Compile Once – Run Everywhere，我们 eBPF 项目的核心方案」
    //     第二次会议：摘要背景里带上这句，模型就知道 CO-RE 不是错别字、是技术方案
    //
    // 【它绝不进识别提示 / 翻译约束】一句定义塞进 Whisper 的 initial_prompt
    // 只会挤掉真正的专名（那串有 224 token 预算），对识别也毫无帮助。
    // 它的出口只有两个：**摘要背景**和**检索**（`search_knowledge`）。
    "  definition     TEXT,"
    "  first_seen_at  TEXT NOT NULL,"
    "  updated_at     TEXT NOT NULL,"
    // 证据：哪次会话、哪一段、原话。不可省——每条知识都要能回答"你凭什么这么说"
    "  source_session INTEGER,"
    "  source_seq     INTEGER,"
    "  source_text    TEXT"
    ");"
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_knowledge_kind_key ON knowledge(kind, key);"
    "CREATE INDEX IF NOT EXISTS idx_knowledge_status ON knowledge(status);"

    // 变化历史（Event / Decision 的落地）。**只追加不修改** —— 历史不删。
    "CREATE TABLE IF NOT EXISTS knowledge_history ("
    "  id             INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  knowledge_id   INTEGER NOT NULL,"
    "  old_value      TEXT,"
    "  new_value      TEXT NOT NULL,"
    "  changed_at     TEXT NOT NULL,"
    "  source_session INTEGER,"
    "  source_seq     INTEGER,"
    "  source_text    TEXT,"
    "  reason         TEXT NOT NULL"               // user_confirmed | model_extracted | user_edited
    ");"
    "CREATE INDEX IF NOT EXISTS idx_khistory_kid ON knowledge_history(knowledge_id);"

    // 行动项（跨会话追踪）
    "CREATE TABLE IF NOT EXISTS actions ("
    "  id             INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  title          TEXT NOT NULL,"
    "  owner          TEXT NOT NULL DEFAULT '',"
    "  due            TEXT NOT NULL DEFAULT '',"
    "  status         TEXT NOT NULL DEFAULT 'todo',"   // todo | doing | done
    "  source_session INTEGER,"
    "  source_seq     INTEGER,"
    "  confidence     REAL NOT NULL DEFAULT -1.0,"
    "  created_at     TEXT NOT NULL,"
    "  updated_at     TEXT NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_actions_status ON actions(status);";

// FTS5 全文索引（--ask / search_knowledge / 定义检索）。
//
// 【为什么必须有这一条】它同时验证了 SQLITE_ENABLE_FTS5 宏真的生效了 ——
// 宏没定义时这句会直接报 "no such module: fts5"，schema 执行失败、整个程序起不来，
// 而不是等用到 --ask 时才发现。
//
// 用 content='' 的外部内容表（contentless）不合适，因为知识条目会被改写；
// 这里就用普通 FTS5 表，写入时由 KnowledgeStore 同步。
const char* kFtsDdl =
    "CREATE VIRTUAL TABLE IF NOT EXISTS knowledge_fts USING fts5("
    // definition 也要进索引：用户答的那句话（"Compile Once – Run Everywhere"）
    // 是**检索时最该命中的内容** —— 它在转录里根本不存在，
    // 不索引它就等于"用户教会了系统、系统却检索不到"。
    "  key, value, definition, source_text,"
    "  content='knowledge', content_rowid='id',"
    "  tokenize='unicode61'"                       // unicode61 对中英混排都能用
    ");"

    // ---- FTS5 索引由触发器自己维护 ----
    //
    // 【为什么必须用触发器，而不是在 C++ 里手动同步】
    // 我第一版就是在 upsert/purge 里手写 INSERT/DELETE knowledge_fts，
    // 结果是**索引根本没建起来**：外部内容表的 count(*) 读的是内容表，
    // 所以"看起来有 4 行"，但 MATCH 一条也查不到。
    // 更糟的是 `DELETE FROM knowledge_fts` 这种写法会让 SQLite 直接报
    // "database disk image is malformed"（外部内容表不支持普通 DELETE，
    // 必须用特殊的 'delete' 命令并带上旧值）。
    //
    // 触发器是官方推荐的写法，而且把"不能忘、不能写错"变成了数据库自己的责任。
    "CREATE TRIGGER IF NOT EXISTS knowledge_ai AFTER INSERT ON knowledge BEGIN"
    "  INSERT INTO knowledge_fts(rowid,key,value,definition,source_text)"
    "  VALUES (new.id,new.key,new.value,new.definition,new.source_text);"
    "END;"
    "CREATE TRIGGER IF NOT EXISTS knowledge_ad AFTER DELETE ON knowledge BEGIN"
    "  INSERT INTO knowledge_fts(knowledge_fts,rowid,key,value,definition,source_text)"
    "  VALUES ('delete',old.id,old.key,old.value,old.definition,old.source_text);"
    "END;"
    "CREATE TRIGGER IF NOT EXISTS knowledge_au AFTER UPDATE ON knowledge BEGIN"
    "  INSERT INTO knowledge_fts(knowledge_fts,rowid,key,value,definition,source_text)"
    "  VALUES ('delete',old.id,old.key,old.value,old.definition,old.source_text);"
    "  INSERT INTO knowledge_fts(rowid,key,value,definition,source_text)"
    "  VALUES (new.id,new.key,new.value,new.definition,new.source_text);"
    "END;";

const std::string kSchema = std::string(kSchemaCore) + kFtsDdl;

}  // namespace

SessionStore& SessionStore::instance() {
    static SessionStore store;   // C++11 保证多线程安全，只构造一次
    return store;
}

// 当前本地时间，格式 "YYYY-MM-DD HH:MM:SS.mmm"
// 带毫秒是为了让导出层能算出字幕（SRT）所需的相对时间轴。
//
// 从 .cpp 的匿名函数提成公开 static：KnowledgeStore 也要用它写
// first_seen_at / updated_at / changed_at，**必须是同一格式**，
// 否则跨表按时间排序会错乱。放这里之后，SessionStore.cpp 内部原有的
// 5 处 now_string() 调用不需要改（成员函数里非限定名直接解析到它）。
std::string SessionStore::now_string() {
    auto now = std::chrono::system_clock::now();
    auto ms_part = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now.time_since_epoch()).count() % 1000;
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms_part;
    return oss.str();
}

bool SessionStore::init(const std::string& db_path) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (db_) return true;   // 幂等

    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::cerr << "[DB] open failed: " << sqlite3_errmsg(db_) << std::endl;
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }

    char* err = nullptr;
    rc = sqlite3_exec(db_, kSchema.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::cerr << "[DB] schema failed: " << (err ? err : "?") << std::endl;
        if (err) sqlite3_free(err);
        return false;
    }

    if (!ensure_schema()) return false;

    std::cout << "[DB] session store ready: " << db_path << std::endl;
    return true;
}

bool SessionStore::ensure_schema() {
    // 【现象】用 2.3 之前版本写过的库，knowledge 表有数据但全文检索一条也查不到。
    // 【原因】那版在 C++ 里手动同步 FTS 索引，而外部内容表的索引必须靠触发器或
    //         'rebuild' 重建，手动 INSERT 进的是影子表，MATCH 不认。
    // 【判断】每次打开库时无条件 rebuild 一次。表规模是几十到几百条，代价可忽略，
    //         而且能自愈所有历史库——比写版本号迁移更省事、更不容易漏。
    {
        char* rb_err = nullptr;
        int rc = sqlite3_exec(db_,
            "INSERT INTO knowledge_fts(knowledge_fts) VALUES('rebuild');",
            nullptr, nullptr, &rb_err);
        if (rc != SQLITE_OK) {
            std::cerr << "[DB] knowledge_fts rebuild failed: "
                      << (rb_err ? rb_err : "?") << std::endl;
            if (rb_err) sqlite3_free(rb_err);
            return false;
        }
    }

    // 【现象】2.4 之前建的库里没有 asked_count / last_asked_at 两列。
    // 【原因】`CREATE TABLE IF NOT EXISTS` 对**已存在**的表是空操作，
    //         它只会补表，**永远不会补列** —— 所以加列必须另走 ALTER TABLE。
    // 【判断】用 PRAGMA table_info 先查在不在，不在才 ALTER。
    //         SQLite 没有 "ADD COLUMN IF NOT EXISTS"，只能这样幂等。
    //         加列语句写在**自检也能跑到的路径**上，所以老库第一次被打开就自动补上。
    {
        struct ColumnFix { const char* table; const char* column; const char* decl; };
        const ColumnFix fixes[] = {
            {"knowledge", "asked_count",   "ALTER TABLE knowledge ADD COLUMN asked_count INTEGER NOT NULL DEFAULT 0;"},
            {"knowledge", "last_asked_at", "ALTER TABLE knowledge ADD COLUMN last_asked_at TEXT;"},
            // 2.7：含义/定义。**这一列的缺失后果最隐蔽** ——
            // 老库不会有报错，只是"用户教会它的东西无处可存"，
            // 表现为问完定义之后 `--terms` 里什么都没有。
            {"knowledge", "definition",    "ALTER TABLE knowledge ADD COLUMN definition TEXT;"},
        };
        for (const auto& fx : fixes) {
            const std::string pragma = std::string("PRAGMA table_info(") + fx.table + ");";
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db_, pragma.c_str(), -1, &st, nullptr) != SQLITE_OK) {
                std::cerr << "[DB] table_info failed: " << sqlite3_errmsg(db_) << std::endl;
                return false;
            }
            bool exists = false;
            while (sqlite3_step(st) == SQLITE_ROW) {
                const unsigned char* name = sqlite3_column_text(st, 1);
                if (name && std::strcmp(reinterpret_cast<const char*>(name), fx.column) == 0) {
                    exists = true;
                    break;
                }
            }
            sqlite3_finalize(st);
            if (exists) continue;

            char* alt_err = nullptr;
            if (sqlite3_exec(db_, fx.decl, nullptr, nullptr, &alt_err) != SQLITE_OK) {
                std::cerr << "[DB] 补列失败 " << fx.table << "." << fx.column
                          << ": " << (alt_err ? alt_err : "?") << std::endl;
                if (alt_err) sqlite3_free(alt_err);
                return false;
            }
        }
    }

    // ---- FTS 索引的重建迁移（2.7）----
    //
    // 【为什么 ALTER TABLE 不够】`definition` 加进了 FTS 的列清单，
    // 但 `CREATE VIRTUAL TABLE IF NOT EXISTS` 对已存在的表是空操作、
    // `CREATE TRIGGER IF NOT EXISTS` 同理不会替换触发器 ——
    // 于是老库里会出现最难看的一种状态：**内容表有 definition 列、索引里没有**，
    // 查询不报错，只是永远搜不到用户教它的那句话。
    //
    // 【做法】整块 DROP 再按同一份 kFtsDdl 建回来，最后 rebuild 从内容表灌数据。
    // 内容表（knowledge）一个字都不动 —— 索引是可重建的派生数据。
    {
        bool need = false;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, "PRAGMA table_info(knowledge_fts);", -1, &st, nullptr) == SQLITE_OK) {
            bool has_def = false, any = false;
            while (sqlite3_step(st) == SQLITE_ROW) {
                any = true;
                const unsigned char* name = sqlite3_column_text(st, 1);
                if (name && std::strcmp(reinterpret_cast<const char*>(name), "definition") == 0) {
                    has_def = true;
                    break;
                }
            }
            sqlite3_finalize(st);
            need = any && !has_def;
        }
        if (need) {
            std::cout << "[DB] knowledge_fts 缺少 definition 列，正在重建索引"
                         "（内容表不动，索引是可重建的派生数据）..." << std::endl;
            const char* ddl =
                "DROP TRIGGER IF EXISTS knowledge_ai;"
                "DROP TRIGGER IF EXISTS knowledge_ad;"
                "DROP TRIGGER IF EXISTS knowledge_au;"
                "DROP TABLE IF EXISTS knowledge_fts;";
            char* derr = nullptr;
            if (sqlite3_exec(db_, ddl, nullptr, nullptr, &derr) != SQLITE_OK) {
                std::cerr << "[DB] 重建 FTS 失败（DROP）: " << (derr ? derr : "?") << std::endl;
                if (derr) sqlite3_free(derr);
                return false;
            }
            char* cerr2 = nullptr;
            if (sqlite3_exec(db_, kFtsDdl, nullptr, nullptr, &cerr2) != SQLITE_OK) {
                std::cerr << "[DB] 重建 FTS 失败（CREATE）: " << (cerr2 ? cerr2 : "?") << std::endl;
                if (cerr2) sqlite3_free(cerr2);
                return false;
            }
            // 内容表的数据灌回索引
            char* rerr = nullptr;
            if (sqlite3_exec(db_, "INSERT INTO knowledge_fts(knowledge_fts) VALUES('rebuild');",
                             nullptr, nullptr, &rerr) != SQLITE_OK) {
                std::cerr << "[DB] rebuild 失败: " << (rerr ? rerr : "?") << std::endl;
                if (rerr) sqlite3_free(rerr);
                return false;
            }
            std::cout << "[DB] FTS 索引已重建" << std::endl;
        }
    }

    struct Prepared {
        const char*   sql;
        sqlite3_stmt** out;
    };
    const Prepared items[] = {
        {"INSERT INTO sessions (started_at, engine, note) VALUES (?, ?, ?);", &stmt_begin_session_},
        {"UPDATE sessions SET ended_at = ? WHERE id = ?;",                    &stmt_end_session_},
        {"INSERT INTO segments (session_id, seq, ts, src_text, tgt_text, engine, ms, confidence) "
         "VALUES (?, ?, ?, ?, ?, ?, ?, ?);",                                  &stmt_insert_seg_},
        {"SELECT COALESCE(MAX(seq), 0) FROM segments WHERE session_id = ?;",  &stmt_next_seq_},
    };

    for (const auto& it : items) {
        int rc = sqlite3_prepare_v2(db_, it.sql, -1, it.out, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "[DB] prepare failed: " << sqlite3_errmsg(db_) << std::endl;
            return false;
        }
    }
    return true;
}

long long SessionStore::begin_session(const std::string& engine, const std::string& note) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_ || !stmt_begin_session_) return -1;

    // 已有未结束的会话，先收尾，避免会话悬空
    if (current_session_ >= 0 && stmt_end_session_) {
        const std::string ts = now_string();
        sqlite3_bind_text(stmt_end_session_, 1, ts.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt_end_session_, 2, current_session_);
        sqlite3_step(stmt_end_session_);
        sqlite3_reset(stmt_end_session_);
        sqlite3_clear_bindings(stmt_end_session_);
    }

    const std::string ts = now_string();
    sqlite3_bind_text(stmt_begin_session_, 1, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_begin_session_, 2, engine.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_begin_session_, 3, note.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt_begin_session_) != SQLITE_DONE) {
        sqlite3_reset(stmt_begin_session_);
        sqlite3_clear_bindings(stmt_begin_session_);
        std::cerr << "[DB] begin_session failed: " << sqlite3_errmsg(db_) << std::endl;
        return -1;
    }
    sqlite3_reset(stmt_begin_session_);
    sqlite3_clear_bindings(stmt_begin_session_);

    current_session_ = sqlite3_last_insert_rowid(db_);
    return current_session_;
}

void SessionStore::end_session() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_ || current_session_ < 0 || !stmt_end_session_) return;

    const std::string ts = now_string();
    sqlite3_bind_text(stmt_end_session_, 1, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt_end_session_, 2, current_session_);
    sqlite3_step(stmt_end_session_);
    sqlite3_reset(stmt_end_session_);
    sqlite3_clear_bindings(stmt_end_session_);

    current_session_ = -1;
}

long long SessionStore::current_session() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_session_;
}

bool SessionStore::log_segment(const std::string& src_text,
                               const std::string& tgt_text,
                               const std::string& engine,
                               long long latency_ms,
                               double confidence) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_ || !stmt_insert_seg_ || current_session_ < 0) return false;

    // 计算会话内序号
    int next_seq = 1;
    if (stmt_next_seq_) {
        sqlite3_bind_int64(stmt_next_seq_, 1, current_session_);
        if (sqlite3_step(stmt_next_seq_) == SQLITE_ROW) {
            next_seq = sqlite3_column_int(stmt_next_seq_, 0) + 1;
        }
        sqlite3_reset(stmt_next_seq_);
        sqlite3_clear_bindings(stmt_next_seq_);
    }

    const std::string ts = now_string();
    sqlite3_bind_int64(stmt_insert_seg_, 1, current_session_);
    sqlite3_bind_int(  stmt_insert_seg_, 2, next_seq);
    sqlite3_bind_text( stmt_insert_seg_, 3, ts.c_str(),       -1, SQLITE_TRANSIENT);
    sqlite3_bind_text( stmt_insert_seg_, 4, src_text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text( stmt_insert_seg_, 5, tgt_text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text( stmt_insert_seg_, 6, engine.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt_insert_seg_, 7, latency_ms);
    sqlite3_bind_double(stmt_insert_seg_, 8, confidence);

    const bool ok = sqlite3_step(stmt_insert_seg_) == SQLITE_DONE;
    if (!ok) {
        std::cerr << "[DB] log_segment failed: " << sqlite3_errmsg(db_) << std::endl;
    }
    sqlite3_reset(stmt_insert_seg_);
    sqlite3_clear_bindings(stmt_insert_seg_);
    return ok;
}

bool SessionStore::long_term_memory_ready() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    // ① 编译期真的带了 FTS5
    if (!sqlite3_compileoption_used("ENABLE_FTS5")) return false;

    // ② 三张长期记忆表可查询；③ FTS5 虚表可查询
    static const char* kProbes[] = {
        "SELECT count(*) FROM knowledge;",
        "SELECT count(*) FROM knowledge_history;",
        "SELECT count(*) FROM actions;",
        "SELECT count(*) FROM knowledge_fts;",
    };
    for (const char* sql : kProbes) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
        const int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_ROW && rc != SQLITE_DONE) return false;
    }
    return true;
}

std::vector<Segment> SessionStore::fetch_segments(long long session_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Segment> out;
    if (!db_) return out;
    const char* sql =
        "SELECT id, session_id, seq, ts, src_text, tgt_text, engine, ms, confidence "
        "FROM segments WHERE session_id = ? ORDER BY seq ASC;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;

    sqlite3_bind_int64(stmt, 1, session_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Segment s;
        s.id         = sqlite3_column_int64(stmt, 0);
        s.session_id = sqlite3_column_int64(stmt, 1);
        s.seq        = sqlite3_column_int(stmt, 2);
        s.ts         = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        s.src_text   = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        s.tgt_text   = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        s.engine     = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        s.ms         = sqlite3_column_int64(stmt, 7);
        s.confidence = sqlite3_column_double(stmt, 8);
        out.push_back(std::move(s));
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<SessionInfo> SessionStore::list_sessions(int limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SessionInfo> out;
    if (!db_) return out;

    const char* sql =
        "SELECT s.id, s.started_at, COALESCE(s.ended_at, ''), s.engine, s.note,"
        "       (SELECT COUNT(*) FROM segments g WHERE g.session_id = s.id) "
        "FROM sessions s ORDER BY s.id DESC LIMIT ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;

    sqlite3_bind_int(stmt, 1, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SessionInfo si;
        si.id            = sqlite3_column_int64(stmt, 0);
        si.started_at    = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        si.ended_at      = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        si.engine        = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        si.note          = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        si.segment_count = sqlite3_column_int(stmt, 5);
        out.push_back(std::move(si));
    }
    sqlite3_finalize(stmt);
    return out;
}

SessionStore::Stats SessionStore::stats(long long session_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    Stats st;
    if (!db_) return st;

    const char* sql = "SELECT COUNT(*), COALESCE(SUM(ms),0), COALESCE(AVG(ms),0) "
                      "FROM segments WHERE session_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return st;

    sqlite3_bind_int64(stmt, 1, session_id);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        st.count    = sqlite3_column_int(stmt, 0);
        st.total_ms = sqlite3_column_int64(stmt, 1);
        st.avg_ms   = static_cast<long long>(sqlite3_column_double(stmt, 2));
    }
    sqlite3_finalize(stmt);
    return st;
}

void SessionStore::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_session_ >= 0 && stmt_end_session_) {
        const std::string ts = now_string();
        sqlite3_bind_text(stmt_end_session_, 1, ts.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt_end_session_, 2, current_session_);
        sqlite3_step(stmt_end_session_);
        sqlite3_reset(stmt_end_session_);
        sqlite3_clear_bindings(stmt_end_session_);
        current_session_ = -1;
    }

    if (stmt_begin_session_) { sqlite3_finalize(stmt_begin_session_); stmt_begin_session_ = nullptr; }
    if (stmt_end_session_)   { sqlite3_finalize(stmt_end_session_);   stmt_end_session_   = nullptr; }
    if (stmt_insert_seg_)    { sqlite3_finalize(stmt_insert_seg_);    stmt_insert_seg_    = nullptr; }
    if (stmt_next_seq_)      { sqlite3_finalize(stmt_next_seq_);      stmt_next_seq_      = nullptr; }
    if (db_)                 { sqlite3_close(db_);                    db_ = nullptr; }
}
