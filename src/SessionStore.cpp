#include "SessionStore.h"
#include "sqlite3.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace {

// 当前本地时间，格式 "YYYY-MM-DD HH:MM:SS.mmm"
// 带毫秒是为了让导出层能算出字幕（SRT）所需的相对时间轴。
std::string now_string() {
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

const char* kSchema =
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
    "CREATE INDEX IF NOT EXISTS idx_segments_session ON segments(session_id, seq);";

}  // namespace

SessionStore& SessionStore::instance() {
    static SessionStore store;   // C++11 保证多线程安全，只构造一次
    return store;
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
    rc = sqlite3_exec(db_, kSchema, nullptr, nullptr, &err);
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
