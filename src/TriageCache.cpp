#include "TriageCache.h"

#include <mutex>

#include "KnowledgeStore.h"   // 只为了 normalize_key()，见头文件说明
#include "SessionStore.h"     // 复用 db_ 与 now_string()

// SessionStore.h 里只做了前向声明，这里需要真正的 sqlite3 API。
#include "sqlite3.h"

namespace triage {

// 【不要在这里写"帮忙拿连接"的自由函数】
// 试过一版 `sqlite3* db_of(std::unique_lock<std::mutex>*)`，编译期直接 C2248：
// **friend 只授予 TriageCache 的成员函数**，匿名 namespace 里的自由函数不是成员，
// 拿不到 SessionStore 的私有 db_。这不是"加个 friend 就行"的问题 ——
// 授予一个自由函数等于把连接暴露给了一个谁都能调的名字，那正是 friend 想避免的事。
// 所以每个成员函数各自加锁取连接，重复三行换来"只有三个成员能碰这个连接"。

TriageCache& TriageCache::instance() {
    static TriageCache c;
    return c;
}

bool TriageCache::exec(const std::string& sql, std::string* err) const {
    SessionStore& ss = SessionStore::instance();
    std::lock_guard<std::mutex> lk(ss.mutex_);
    if (ss.db_ == nullptr) {
        if (err) *err = u8"数据库未打开";
        return false;
    }
    char* msg = nullptr;
    const int rc = sqlite3_exec(ss.db_, sql.c_str(), nullptr, nullptr, &msg);
    if (rc != SQLITE_OK) {
        if (err) *err = msg ? msg : sqlite3_errmsg(ss.db_);
        if (msg) sqlite3_free(msg);
        return false;
    }
    if (msg) sqlite3_free(msg);
    return true;
}

bool TriageCache::ready() const {
    SessionStore& ss = SessionStore::instance();
    std::lock_guard<std::mutex> lk(ss.mutex_);
    if (ss.db_ == nullptr) return false;

    // **用真正的查询探测，而不是"表应该存在"的假设。**
    // 老库（2.12 之前建的）schema 里没有这张表 —— 那种库必须退化成"没有缓存"，
    // 而不是每次分诊都报一次 SQL 错误。
    //
    // 【为什么不在这里顺手 CREATE TABLE IF NOT EXISTS】
    // 建表是 SessionStore::ensure_schema() 的职责（唯一 owner）。两处建表就会出现
    // "两边定义不一致、后建的赢"这种查不出来的事故 —— 本项目在 FTS 触发器上
    // 已经吃过一次同类的亏（PROJECT.md §8.8 反模式⑩）。
    sqlite3_stmt* st = nullptr;
    const char* sql = "SELECT 1 FROM triage_verdicts LIMIT 1;";
    if (sqlite3_prepare_v2(ss.db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_finalize(st);
    return true;
}

Cache TriageCache::hooks() {
    Cache c;
    if (!ready()) return c;       // 空 Cache → decide() 自动跳过第 ② 层

    c.lookup = [](const std::string& value, CachedVerdict* out) -> bool {
        if (!out) return false;
        SessionStore& ss = SessionStore::instance();
        std::lock_guard<std::mutex> lk(ss.mutex_);
        if (ss.db_ == nullptr) return false;

        // 归一化在**缓存层内部**做：CandidateTriage 不该知道知识库怎么归一化。
        const std::string key = knowledge::normalize_key(value);
        if (key.empty()) return false;

        sqlite3_stmt* st = nullptr;
        const char* sql =
            "SELECT verdict, kind, primer, why, judged_at FROM triage_verdicts "
            "WHERE key = ?;";
        if (sqlite3_prepare_v2(ss.db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_text(st, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        const bool hit = (sqlite3_step(st) == SQLITE_ROW);
        if (hit) {
            auto col = [&](int i) -> std::string {
                const unsigned char* p = sqlite3_column_text(st, i);
                return p ? reinterpret_cast<const char*>(p) : "";
            };
            const std::string v = col(0);
            // 只认 skip：即使表里将来出现别的判决，也不在这里采信。
            // 与写入侧（decide() 只写 skip）对称 —— 两边都收窄，中间没有缝隙。
            if (v != "skip") {
                sqlite3_finalize(st);
                return false;
            }
            out->verdict   = Verdict::Skip;
            out->kind      = col(1);
            out->primer    = col(2);
            out->why       = col(3);
            out->judged_at = col(4);
        }
        sqlite3_finalize(st);

        if (hit) {
            // 记一次"省下的调用"。这是"越用越省"唯一诚实的量化口径 ——
            // 它不是估算，就是命中次数本身。
            sqlite3_stmt* up = nullptr;
            if (sqlite3_prepare_v2(ss.db_,
                    "UPDATE triage_verdicts SET reused = reused + 1, "
                    "last_used_at = ? WHERE key = ?;", -1, &up, nullptr) == SQLITE_OK) {
                const std::string ts = SessionStore::now_string();
                sqlite3_bind_text(up, 1, ts.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(up, 2, key.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(up);
                sqlite3_finalize(up);
            }
        }
        return hit;
    };

    c.store = [](const std::string& value, const std::string& kind, const Decision& d) {
        SessionStore& ss = SessionStore::instance();
        std::lock_guard<std::mutex> lk(ss.mutex_);
        if (ss.db_ == nullptr) return;

        // 再收窄一次（纵深防御）：decide() 已经保证只有 Skip + model 会调到这里，
        // 但这条 SQL 是"把判决写进持久层"的最后一道门，在这里复述条件几乎零成本，
        // 而它挡住的是"某天有人从别处调 store 把 fallback 写进去"。
        if (d.verdict != Verdict::Skip || d.source != "model") return;

        const std::string key = knowledge::normalize_key(value);
        if (key.empty()) return;

        sqlite3_stmt* st = nullptr;
        const char* sql =
            "INSERT INTO triage_verdicts "
            "(key, value, verdict, kind, primer, why, source, reused, judged_at) "
            "VALUES (?,?,?,?,?,?,'model',0,?) "
            "ON CONFLICT(key) DO UPDATE SET "
            "  value=excluded.value, verdict=excluded.verdict, kind=excluded.kind, "
            "  primer=excluded.primer, why=excluded.why, judged_at=excluded.judged_at;";
        if (sqlite3_prepare_v2(ss.db_, sql, -1, &st, nullptr) != SQLITE_OK) return;
        const std::string ts = SessionStore::now_string();
        sqlite3_bind_text(st, 1, key.c_str(),    -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, value.c_str(),  -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, "skip",         -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, kind.c_str(),   -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 5, d.primer.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 6, d.reason.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 7, ts.c_str(),     -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
    };

    return c;
}

TriageCache::Stats TriageCache::stats() const {
    Stats s;
    SessionStore& ss = SessionStore::instance();
    std::lock_guard<std::mutex> lk(ss.mutex_);
    if (ss.db_ == nullptr) return s;

    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT count(*), COALESCE(sum(reused),0), "
        "       COALESCE(sum(CASE WHEN COALESCE(last_used_at, judged_at) < "
        "         datetime('now','localtime','-90 days') THEN 1 ELSE 0 END),0) "
        "FROM triage_verdicts;";
    if (sqlite3_prepare_v2(ss.db_, sql, -1, &st, nullptr) != SQLITE_OK) return s;
    if (sqlite3_step(st) == SQLITE_ROW) {
        s.total  = sqlite3_column_int(st, 0);
        s.reused = sqlite3_column_int(st, 1);
        s.stale  = sqlite3_column_int(st, 2);
    }
    sqlite3_finalize(st);
    return s;
}

std::vector<CacheEntry> TriageCache::list(int limit) const {
    std::vector<CacheEntry> out;
    SessionStore& ss = SessionStore::instance();
    std::lock_guard<std::mutex> lk(ss.mutex_);
    if (ss.db_ == nullptr) return out;

    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT key, value, verdict, COALESCE(kind,''), COALESCE(primer,''), "
        "       COALESCE(why,''), judged_at, COALESCE(last_used_at,''), reused "
        "FROM triage_verdicts ORDER BY judged_at DESC, key LIMIT ?;";
    if (sqlite3_prepare_v2(ss.db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_int(st, 1, limit > 0 ? limit : 200);
    while (sqlite3_step(st) == SQLITE_ROW) {
        CacheEntry e;
        auto col = [&](int i) -> std::string {
            const unsigned char* p = sqlite3_column_text(st, i);
            return p ? reinterpret_cast<const char*>(p) : "";
        };
        e.key          = col(0);
        e.value        = col(1);
        e.verdict      = col(2);
        e.kind         = col(3);
        e.primer       = col(4);
        e.why          = col(5);
        e.judged_at    = col(6);
        e.last_used_at = col(7);
        e.reused       = sqlite3_column_int(st, 8);
        out.push_back(std::move(e));
    }
    sqlite3_finalize(st);
    return out;
}

bool TriageCache::forget(const std::string& word, std::string* err) {
    SessionStore& ss = SessionStore::instance();
    std::lock_guard<std::mutex> lk(ss.mutex_);
    if (ss.db_ == nullptr) {
        if (err) *err = u8"数据库未打开";
        return false;
    }
    // 原样和归一化两种写法都试：用户会直接打他看到的那个词（可能是 `Penny`）。
    // 只认归一化键的话，`--forget Penny` 会静默地什么都不做 —— 那种"命令跑了、
    // 没报错、也没生效"是最难查的一类问题。
    const std::string key = knowledge::normalize_key(word);
    sqlite3_stmt* st = nullptr;
    const char* sql = "DELETE FROM triage_verdicts WHERE key = ? OR value = ?;";
    if (sqlite3_prepare_v2(ss.db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        if (err) *err = sqlite3_errmsg(ss.db_);
        return false;
    }
    sqlite3_bind_text(st, 1, key.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, word.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    const int changed = sqlite3_changes(ss.db_);
    sqlite3_finalize(st);
    if (changed == 0) {
        if (err) *err = u8"缓存里没有「" + word + u8"」（键 " + key + u8"）";
        return false;
    }
    return true;
}

int TriageCache::clear() {
    SessionStore& ss = SessionStore::instance();
    std::lock_guard<std::mutex> lk(ss.mutex_);
    if (ss.db_ == nullptr) return 0;
    if (sqlite3_exec(ss.db_, "DELETE FROM triage_verdicts;", nullptr, nullptr,
                     nullptr) != SQLITE_OK) {
        return 0;
    }
    return sqlite3_changes(ss.db_);
}

}  // namespace triage
