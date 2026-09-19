#include "ActionStore.h"

#include <algorithm>
#include <cctype>
#include <iterator>      // std::back_inserter（set_intersection/set_union 要用）
#include <mutex>

#include "SessionStore.h"    // 复用 db_ / mutex_ / now_string()
#include "Utf8.h"            // 中文按字符边界处理

#include "sqlite3.h"

namespace actions {

const std::vector<std::string>& valid_statuses() {
    static const std::vector<std::string> v = {"todo", "doing", "done"};
    return v;
}

ActionStore::Stats ActionStore::stats() {
    Stats s;
    SessionStore& ss = SessionStore::instance();
    std::lock_guard<std::mutex> lk(ss.mutex_);
    if (ss.db_ == nullptr) return s;

    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT COALESCE(sum(status='todo'),0), COALESCE(sum(status='doing'),0), "
        "       COALESCE(sum(status='done'),0), count(*) FROM actions;";
    if (sqlite3_prepare_v2(ss.db_, sql, -1, &st, nullptr) != SQLITE_OK) return s;
    if (sqlite3_step(st) == SQLITE_ROW) {
        s.todo  = sqlite3_column_int(st, 0);
        s.doing = sqlite3_column_int(st, 1);
        s.done  = sqlite3_column_int(st, 2);
        s.total = sqlite3_column_int(st, 3);
    }
    sqlite3_finalize(st);
    return s;
}

namespace {

bool in_list(const std::vector<std::string>& l, const std::string& x) {
    return std::find(l.begin(), l.end(), x) != l.end();
}

std::string lower_copy(const std::string& s) {
    std::string o = s;
    for (auto& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return o;
}

// ASCII 标点（含中英文常见标点）当分隔符。
//
// 【为什么这里可以按字节判断中文字符】下面只判"这个**字节**是不是标点"，
// 而 UTF-8 里所有中文标点的首字节都 ≥ 0xE0（不在 ASCII 范围），
// 所以按字节扫不会把中文的某个字节误判成标点而切坏字符 ——
// 判中的永远是那些**完整的 ASCII 标点字节**。
// （本项目在 `find_first_of(u8"：:")` 上栽过一次，那是拿中文标点去比对，
//   情况不同；但同一个教训适用：**按字节处理之前先想清楚字节的取值范围**。）
bool is_sep(unsigned char c) {
    switch (c) {
    case ' ':  case '\t': case '\n': case '\r':
    case '.':  case ',':  case ';':  case ':':
    case '!':  case '?':  case '"':  case '\'':
    case '(':  case ')':  case '[':  case ']':
    case '{':  case '}':  case '-':  case '_':
    case '/':  case '\\': case '*':  case '|':
    case '<':  case '>':  case '#':  case '@':
    case '~':  case '^':  case '=':  case '+':
        return true;
    default:
        return false;
    }
}

// 全角/中文标点的 UTF-8 序列 → 空格。
// 这些必须显式列出：它们不是单字节，按上面那种字节判断处理不了。
const char* kFullWidthSeps[] = {
    u8"，", u8"。", u8"、", u8"；", u8"：", u8"！", u8"？",
    u8"（", u8"）", u8"《", u8"》", u8"“", u8"”", u8"‘", u8"’",
    u8"…", u8"—", u8"·", u8"　",
};

std::vector<std::string> tokens_of(const std::string& s) {
    // 词集：中文整段会被当成一个"词"（unicode61 那套分词在这里不适用）。
    // 够用即可 —— 它的职责只是"值不值得问判断器"，不是最终判定。
    std::string t = s;
    for (const char* p : kFullWidthSeps) {
        const std::string pat = p;
        size_t pos = 0;
        while ((pos = t.find(pat, pos)) != std::string::npos) {
            t.replace(pos, pat.size(), " ");
            pos += 1;
        }
    }
    std::vector<std::string> out;
    std::string cur;
    for (unsigned char c : t) {
        if (is_sep(c)) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(static_cast<char>(c));
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

}  // namespace

namespace {

// 按 UTF-8 字符切（不是字节）。中文一个字 3 字节，用字节数当"长度"
// 会让同一个门槛在中文上松三倍 —— 而这条门槛是拿中文语料定的。
std::vector<std::string> utf8_chars(const std::string& s) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t n = 1;
        if      ((c & 0x80) == 0x00) n = 1;
        else if ((c & 0xE0) == 0xC0) n = 2;
        else if ((c & 0xF0) == 0xE0) n = 3;
        else if ((c & 0xF8) == 0xF0) n = 4;
        if (i + n > s.size()) break;          // 截断的尾巴：丢掉，不抛异常
        out.push_back(s.substr(i, n));
        i += n;
    }
    return out;
}

// 去掉**所有**空白（含全角空格）—— "重写 这块" 和 "重写这块" 要能对上。
std::string strip_spaces(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        if (s.compare(i, 3, u8"　") == 0) { i += 3; continue; }   // 全角空格
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++i; continue; }
        out.push_back(s[i]);
        ++i;
    }
    return out;
}

}  // namespace

std::string normalize_title(const std::string& raw) {
    const auto toks = tokens_of(raw);
    std::string out;
    for (const auto& t : toks) {
        if (!out.empty()) out.push_back(' ');
        out += lower_copy(t);
    }
    return out;
}

size_t longest_shared_run(const std::string& a, const std::string& b) {
    const auto ca = utf8_chars(strip_spaces(a));
    const auto cb = utf8_chars(strip_spaces(b));
    if (ca.empty() || cb.empty()) return 0;

    // 经典 DP，但只留上一行：几百字的标题用 O(n*m) 完全够，
    // 而滚动数组让它连内存都不用想。
    std::vector<size_t> prev(cb.size() + 1, 0), cur(cb.size() + 1, 0);
    size_t best = 0;
    for (size_t i = 1; i <= ca.size(); ++i) {
        for (size_t j = 1; j <= cb.size(); ++j) {
            cur[j] = (ca[i - 1] == cb[j - 1]) ? prev[j - 1] + 1 : 0;
            if (cur[j] > best) best = cur[j];
        }
        std::swap(prev, cur);
        std::fill(cur.begin(), cur.end(), 0);
    }
    return best;
}

bool has_boundary_aligned_shared_run(const std::string& a, const std::string& b,
                                     size_t min_len) {
    // ⚠️ 这里**不能**用 strip_spaces：分句边界正是靠标点和空白认出来的，
    // 把空白剥掉就等于把"分句在哪结束"这个信息扔了。
    // （`longest_shared_run` 要剥空白，是因为它只比长度、不关心边界 ——
    //   两个函数的预处理要求不同，这一点必须写在代码里，不然下一个人会顺手统一。）
    const auto ca = utf8_chars(a);
    const auto cb = utf8_chars(b);
    if (ca.empty() || cb.empty()) return false;

    // 单个 UTF-8 字符是不是"分句边界"。
    auto is_boundary_char = [](const std::string& ch) -> bool {
        if (ch.size() == 1) return is_sep(static_cast<unsigned char>(ch[0]));
        for (const char* p : kFullWidthSeps) {
            if (ch == p) return true;
        }
        return false;
    };
    // 位置 pos 是边界：要么已经到头，要么那一位就是标点/空白。
    auto ends_at_boundary = [&](const std::vector<std::string>& s, size_t pos) {
        return pos >= s.size() || is_boundary_char(s[pos]);
    };

    // O(n*m)：标题只有几十个字，够用；而且这里**必须显式双重循环**逐个起点试，
    // 不能只看"最长的那一段" —— 因为最长的可能恰好在分句中间断（重写/评审那种），
    // 而稍短一点的那段才是落在边界上的（前面带逗号的那种）。
    for (size_t i = 0; i < ca.size(); ++i) {
        for (size_t j = 0; j < cb.size(); ++j) {
            size_t k = 0;
            while (i + k < ca.size() && j + k < cb.size() && ca[i + k] == cb[j + k]) ++k;
            if (k < min_len) continue;
            // ⚠️ **必须回退着试每一个长度，不能只看最长的那一段。**
            // 实测：从 i=j=0 一路比下去，最长能到「…需要重写，」（**带那个逗号**，14 字），
            // 而它的下一位是「这」—— 不是分句边界，于是这一对会被漏判。
            // 真正落在边界上的是**去掉逗号**的那 13 字版本。
            // 从长到短试，取第一个满足的（最长的满足者），语义最稳。
            for (size_t len = k; len >= min_len; --len) {
                if (ends_at_boundary(ca, i + len) && ends_at_boundary(cb, j + len)) {
                    return true;
                }
                if (len == min_len) break;     // size_t 别减到回绕
            }
        }
    }
    return false;
}

bool same_thing(const Item& a, const Item& b) {
    if (a.key.empty() || b.key.empty()) return false;   // 空键不参与判同
    if (a.key == b.key) return true;                    // ① 完全相同

    // ② 有一段"两边都落在分句结尾"的长相同片段（语义见头文件的说明）。
    //
    // ⚠️ **只在"两边都够长"时才算** —— 否则一条 8 字的任务和一条 100 字的任务
    // 只要包含它就会被判同一件事，而那可能只是"顺便提了一句"。
    const size_t la = utf8_chars(strip_spaces(a.title)).size();
    const size_t lb = utf8_chars(strip_spaces(b.title)).size();
    if (la < kMinSharedRun || lb < kMinSharedRun) return false;
    if (has_boundary_aligned_shared_run(a.title, b.title, kMinSharedRun)) return true;

    // ③ 英文：8 个字母这个门槛太弱（"deadline" 就 8 个），所以走实词重合。
    //    要求"至少 3 个共同实词"是为了不让 the/and/for 这类词凑数 ——
    //    tokens_of 已经按标点切开了，短词（≤3 字母）在这里不算数。
    auto ta = tokens_of(a.title), tb = tokens_of(b.title);
    auto keep = [](std::vector<std::string>& v) {
        std::vector<std::string> o;
        for (auto& s : v) {
            std::string t = lower_copy(s);
            if (t.size() > 3) o.push_back(t);
        }
        std::sort(o.begin(), o.end());
        o.erase(std::unique(o.begin(), o.end()), o.end());
        v = o;
    };
    keep(ta); keep(tb);
    if (ta.size() < 3 || tb.size() < 3) return false;
    std::vector<std::string> inter;
    std::set_intersection(ta.begin(), ta.end(), tb.begin(), tb.end(),
                          std::back_inserter(inter));
    return inter.size() >= 3;
}

double title_similarity(const std::string& a, const std::string& b) {
    auto ta = tokens_of(a), tb = tokens_of(b);
    if (ta.empty() || tb.empty()) return 0.0;
    // 去重（同一个词出现两次不该加权）
    auto uniq = [](std::vector<std::string>& v) {
        for (auto& s : v) s = lower_copy(s);
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    };
    uniq(ta); uniq(tb);

    std::vector<std::string> inter;
    std::set_intersection(ta.begin(), ta.end(), tb.begin(), tb.end(),
                          std::back_inserter(inter));
    std::vector<std::string> uni;
    std::set_union(ta.begin(), ta.end(), tb.begin(), tb.end(),
                   std::back_inserter(uni));
    if (uni.empty()) return 0.0;
    return static_cast<double>(inter.size()) / static_cast<double>(uni.size());
}

namespace {

// 从一行 SELECT 里读出一个 Item。
// 用同一个 lambda 读所有查询，避免"两个查询的列顺序不一致"这种
// 只在某一条路径上发作的错（本项目栽过：两个实现迟早走散）。
void read_item(sqlite3_stmt* st, Item* out) {
    auto text = [&](int i) -> std::string {
        const unsigned char* p = sqlite3_column_text(st, i);
        return p ? reinterpret_cast<const char*>(p) : "";
    };
    out->id             = sqlite3_column_int64(st, 0);
    out->key            = text(1);
    out->title          = text(2);
    out->owner          = text(3);
    out->due            = text(4);
    out->status         = text(5);
    out->source_session = sqlite3_column_int64(st, 6);
    out->source_seq     = sqlite3_column_int(st, 7);
    out->confidence     = sqlite3_column_double(st, 8);
    out->origin         = text(9);
    out->evidence       = text(10);
    out->created_at     = text(11);
    out->updated_at     = text(12);
    out->seen_sessions  = sqlite3_column_int(st, 13);
    out->last_session   = sqlite3_column_int64(st, 14);
}

// 统一的列清单 + 派生列。所有查询都从这里拼，改一处就全改。
const char* kSelectCols =
    "a.id, a.key, a.title, a.owner, a.due, a.status, a.source_session, "
    "a.source_seq, a.confidence, a.origin, COALESCE(a.evidence,''), "
    "a.created_at, a.updated_at, "
    "(SELECT count(*) FROM action_sessions s WHERE s.action_id = a.id), "
    "COALESCE((SELECT max(s.session_id) FROM action_sessions s "
    "          WHERE s.action_id = a.id), COALESCE(a.source_session, 0))";

std::string sql_select(const std::string& where) {
    return std::string("SELECT ") + kSelectCols +
           " FROM actions a " + where;
}

// 按 id 读一条，**不加锁** —— 给已经持有锁的调用方（ingest）用。
//
// 【为什么不能直接调 ActionStore::get()】它自己会 `lock_guard` 同一把 mutex，
// 而 `std::mutex` **不可重入** —— 当场死锁，而且是那种"程序卡住、不报错、
// 看不出在哪"的死锁。所以这里明确分开：`get()` 是给外部用的加锁版本，
// 这个后缀 `_unlocked` 的是给内部持锁路径用的。
// **命名里带 `_unlocked` 是刻意的**：调用点一眼能看出"我现在是不是已经在锁里"。
bool read_by_id_unlocked(sqlite3* db, long long id, Item* out) {
    if (db == nullptr || out == nullptr) return false;
    const std::string sql = sql_select("WHERE a.id = ?;");
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, id);
    bool found = false;
    if (sqlite3_step(st) == SQLITE_ROW) { read_item(st, out); found = true; }
    sqlite3_finalize(st);
    return found;
}

}  // namespace

std::vector<Item> ActionStore::list(const std::string& status, int limit) {
    std::vector<Item> out;
    if (!status.empty() && !in_list(valid_statuses(), status)) return out;

    SessionStore& ss = SessionStore::instance();
    std::lock_guard<std::mutex> lk(ss.mutex_);
    if (ss.db_ == nullptr) return out;

    // 排序：todo → doing → done（待办在最上面），同组按"最近提到"倒序。
    // 用 CASE 而不是按字母序 —— 'doing' < 'done' < 'todo' 的字母序
    // 会把已经做完的排在最前面，那正好是用户最不想看的顺序。
    std::string sql = sql_select(status.empty() ? "" : "WHERE a.status = ?") +
        " ORDER BY CASE a.status WHEN 'todo' THEN 0 WHEN 'doing' THEN 1 ELSE 2 END, "
        "a.updated_at DESC, a.id DESC LIMIT ?;";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(ss.db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return out;
    int idx = 1;
    if (!status.empty()) sqlite3_bind_text(st, idx++, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, idx, limit > 0 ? limit : 200);
    while (sqlite3_step(st) == SQLITE_ROW) {
        Item it;
        read_item(st, &it);
        out.push_back(std::move(it));
    }
    sqlite3_finalize(st);
    return out;
}

std::vector<Item> ActionStore::for_session(long long session_id) {
    std::vector<Item> out;
    SessionStore& ss = SessionStore::instance();
    std::lock_guard<std::mutex> lk(ss.mutex_);
    if (ss.db_ == nullptr) return out;

    const std::string sql = sql_select(
        "JOIN action_sessions s ON s.action_id = a.id WHERE s.session_id = ? "
        "ORDER BY a.id;");
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(ss.db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_int64(st, 1, session_id);
    while (sqlite3_step(st) == SQLITE_ROW) {
        Item it;
        read_item(st, &it);
        out.push_back(std::move(it));
    }
    sqlite3_finalize(st);
    return out;
}

bool ActionStore::get(long long id, Item* out) {
    if (out == nullptr) return false;
    SessionStore& ss = SessionStore::instance();
    std::lock_guard<std::mutex> lk(ss.mutex_);
    if (ss.db_ == nullptr) return false;

    const std::string sql = sql_select("WHERE a.id = ?;");
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(ss.db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, id);
    bool found = false;
    if (sqlite3_step(st) == SQLITE_ROW) { read_item(st, out); found = true; }
    sqlite3_finalize(st);
    return found;
}

bool ActionStore::set_status(long long id, const std::string& status, std::string* err) {
    auto set_err = [&](const std::string& m) { if (err) *err = m; };
    if (!in_list(valid_statuses(), status)) {
        set_err(u8"非法状态「" + status + u8"」（只能是 todo / doing / done）");
        return false;
    }
    SessionStore& ss = SessionStore::instance();
    std::lock_guard<std::mutex> lk(ss.mutex_);
    if (ss.db_ == nullptr) { set_err(u8"数据库未打开"); return false; }

    sqlite3_stmt* st = nullptr;
    const char* sql = "UPDATE actions SET status = ?, updated_at = ? WHERE id = ?;";
    if (sqlite3_prepare_v2(ss.db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        set_err(sqlite3_errmsg(ss.db_));
        return false;
    }
    const std::string ts = SessionStore::now_string();
    sqlite3_bind_text(st, 1, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, ts.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, id);
    sqlite3_step(st);
    sqlite3_finalize(st);

    if (sqlite3_changes(ss.db_) == 0) {
        // 【为什么不报"状态没变"为失败】用户把一条本来就在 todo 的任务再标一次
        // todo 是很自然的事。§2.6d 已经栽过一次同类问题
        // （`from == to` 被报成 `[失败] 状态不允许从 confirmed 变为 confirmed`）：
        // **"无需变更"不是错误。** 但 id 不存在是真错，要区分开。
        sqlite3_stmt* cs = nullptr;
        bool exists = false;
        if (sqlite3_prepare_v2(ss.db_, "SELECT 1 FROM actions WHERE id = ?;",
                               -1, &cs, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(cs, 1, id);
            exists = (sqlite3_step(cs) == SQLITE_ROW);
            sqlite3_finalize(cs);
        }
        if (!exists) {
            set_err(u8"没有 id = " + std::to_string(id) + u8" 的行动项");
            return false;
        }
    }
    return true;
}

Outcome ActionStore::ingest(long long session_id, const std::vector<Incoming>& items,
                            const SameJudge& judge, const std::string& origin,
                            double confidence) {
    Outcome oc;
    SessionStore& ss = SessionStore::instance();
    std::lock_guard<std::mutex> lk(ss.mutex_);
    if (ss.db_ == nullptr) { oc.err = u8"数据库未打开"; return oc; }

    const std::string ts = SessionStore::now_string();

    for (const auto& in : items) {
        const std::string key = normalize_title(in.title);
        if (key.empty()) continue;      // 空标题不进库（它没法被聚合，也没法被检索）

        // ---- ① 确定性层：key 完全相同，**或者有足够长的连续相同片段** ----
        //
        // 【为什么必须在库里按 key 查一遍之外，还要在内存里比一遍】
        // 第一版只按 `key = ?` 精确匹配，实测三场演示会攒出 8 条重复任务、
        // 聚合一条都没生效 —— 因为抽取器给的是**整句**，第二场多说一句补充
        // 就再也不是同一个 key 了。所以这一层要拿候选行出来逐条做
        // `same_thing()`（语义见头文件 kMinSharedRun 的说明）。
        //
        // 代价：每次都把未完成的行动项读一遍（上限 200 条）。这个量级无所谓，
        // 而它换来的是"聚合真的会生效"。
        long long hit_id = 0;
        {
            // 精确 key 优先（最确定、也最常见）
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(ss.db_, "SELECT id FROM actions WHERE key = ? LIMIT 1;",
                                   -1, &st, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(st, 1, key.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(st) == SQLITE_ROW) hit_id = sqlite3_column_int64(st, 0);
                sqlite3_finalize(st);
            }
        }

        Item probe;                 // 拿来和库里已有条目比
        probe.key   = key;
        probe.title = in.title;
        probe.owner = in.owner;

        long long best_id = 0;      // 模糊层要用的"最像的那一条"（只看未完成）
        double    best_sim = 0.0;
        if (hit_id == 0) {
            // ⚠️ **这一遍扫描不排除 done**，而下面的模糊层排除。
            //
            // 【为什么这个区别是必需的】第一版两处都排除了 done，实测后果：
            // 用户把 #9 标成 done 之后**再导出一次同一场会** → 那条任务被当成
            // 新任务又建了一行（`新建 1 条`），于是"已完成"和"待办"里
            // 各有一条一模一样的事。**用户的一个操作（标完成）导致了重复行**，
            // 而重复正是 5.5 要消灭的东西。
            //
            // 判据：`same_thing` 成立（key 相同 / 有足够长连续片段）意味着
            // "这就是同一件事"，与做没做完无关 —— 标了 done 也还是它。
            // 而**模糊层**不同：那是"看起来像"，把一条新的相似任务
            // 吞进一条已完成的旧任务里，用户就再也看不到它了。所以模糊层只比未完成的。
            sqlite3_stmt* st = nullptr;
            const std::string sql = sql_select(
                "ORDER BY a.updated_at DESC LIMIT 200;");
            if (sqlite3_prepare_v2(ss.db_, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
                while (sqlite3_step(st) == SQLITE_ROW) {
                    Item c;
                    read_item(st, &c);
                    if (same_thing(c, probe)) {
                        hit_id = c.id;      // ② 连续相同片段 → 直接算同一件事
                        break;
                    }
                    if (c.status != "done") {   // 模糊层只考虑未完成的
                        const double sim = title_similarity(in.title, c.title);
                        if (sim > best_sim) { best_sim = sim; best_id = c.id; }
                    }
                }
                sqlite3_finalize(st);
            }
        }

        // ---- ③ 模糊层：key 不同、也不够"连续相同"，但很像 → 问判断器 ----
        if (hit_id == 0 && judge && best_id != 0 && best_sim >= kAskJudgeAbove) {
            Item exist;
            if (read_by_id_unlocked(ss.db_, best_id, &exist)) {
                Item incoming;
                incoming.key   = key;
                incoming.title = in.title;
                incoming.owner = in.owner;
                ++oc.judged;
                // 判断器**不返回"失败"**：判不出来就当新任务（见头文件）
                if (judge(exist, incoming)) hit_id = best_id;
            }
        }

        if (hit_id != 0) {
            // ---- 并入已有条目 ----
            ++oc.merged;
            // 【空字段才填，有值不覆盖】"接口文档重写"上周负责人写的是张伟，
            // 这周抽取器没抽出负责人 —— 那是**这次没抽出来**，不是"负责人变成了空"。
            // 覆盖的话，用户看到的是信息**变少**了。
            sqlite3_stmt* up = nullptr;
            const char* sql =
                "UPDATE actions SET "
                "  owner    = CASE WHEN owner    = '' THEN ? ELSE owner    END,"
                "  due      = CASE WHEN due      = '' THEN ? ELSE due      END,"
                "  updated_at = ? "
                "WHERE id = ?;";
            if (sqlite3_prepare_v2(ss.db_, sql, -1, &up, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(up, 1, in.owner.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(up, 2, in.due.c_str(),   -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(up, 3, ts.c_str(),       -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(up, 4, hit_id);
                sqlite3_step(up);
                sqlite3_finalize(up);
            }
        } else {
            // ---- 新建 ----
            sqlite3_stmt* st = nullptr;
            // ⚠️ 列与值的个数必须逐个数一遍：这里手写了 12 列 / 12 个值 / 10 个占位符
            // （due 和 status 走字面量）。第一版写错了 placeholders 的个数，
            // 而 SQLite 对"占位符比 bind 少"**不报错** —— 多绑的那个被静默丢掉，
            // 于是 due 永远是空、而且没有任何提示。（自检里有断言钉住 due 能写进去。）
            const char* sql =
                "INSERT INTO actions "
                "(key,title,owner,due,status,source_session,source_seq,confidence,"
                " origin,evidence,created_at,updated_at) "
                "VALUES (?,?,?,?,'todo',?,?,?,?,?,?,?);";
            if (sqlite3_prepare_v2(ss.db_, sql, -1, &st, nullptr) != SQLITE_OK) {
                if (oc.err.empty()) oc.err = sqlite3_errmsg(ss.db_);
                continue;
            }
            sqlite3_bind_text(st, 1,  key.c_str(),          -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2,  in.title.c_str(),     -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 3,  in.owner.c_str(),     -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 4,  in.due.c_str(),       -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 5, session_id);
            sqlite3_bind_int(st, 6,   in.source_seq);
            sqlite3_bind_double(st, 7, confidence);
            sqlite3_bind_text(st, 8,  origin.c_str(),       -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 9,  in.evidence.c_str(),  -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 10, ts.c_str(),           -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 11, ts.c_str(),           -1, SQLITE_TRANSIENT);
            if (sqlite3_step(st) != SQLITE_DONE) {
                if (oc.err.empty()) oc.err = sqlite3_errmsg(ss.db_);
                sqlite3_finalize(st);
                continue;
            }
            hit_id = sqlite3_last_insert_rowid(ss.db_);
            sqlite3_finalize(st);
            ++oc.inserted;
        }

        // ---- 台账：INSERT OR IGNORE，这就是幂等的落点 ----
        if (session_id >= 0 && hit_id != 0) {
            sqlite3_stmt* st = nullptr;
            const char* sql =
                "INSERT OR IGNORE INTO action_sessions "
                "(action_id,session_id,first_seq,evidence,first_seen_at) VALUES (?,?,?,?,?);";
            if (sqlite3_prepare_v2(ss.db_, sql, -1, &st, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(st, 1, hit_id);
                sqlite3_bind_int64(st, 2, session_id);
                sqlite3_bind_int(st, 3, in.source_seq);
                sqlite3_bind_text(st, 4, in.evidence.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 5, ts.c_str(),          -1, SQLITE_TRANSIENT);
                sqlite3_step(st);
                sqlite3_finalize(st);
                if (sqlite3_changes(ss.db_) > 0) ++oc.linked;
            }
        }
    }
    return oc;
}

}  // namespace actions
