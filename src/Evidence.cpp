#include "Evidence.h"

#include <algorithm>
#include <cctype>
#include <mutex>

#include "SessionStore.h"   // resolve/verify 要查 segments

#include "sqlite3.h"

namespace evidence {

namespace {

// `·` 的 UTF-8 编码。用它当分隔符的理由见头文件 —— 这一段的重点是
// **它不能出现在中文正文里**，所以从散文里捞引用不会误抓。
const char* kSep = "\xC2\xB7";     // U+00B7 MIDDLE DOT

bool is_digit(unsigned char c) { return c >= '0' && c <= '9'; }

}  // namespace

std::string format(const Locator& loc) {
    if (!loc.valid()) return "";
    return "#" + std::to_string(loc.session_id) + kSep + std::to_string(loc.seq);
}

std::vector<Locator> extract(const std::string& text) {
    std::vector<Locator> out;
    const std::string sep = kSep;
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] != '#') { ++i; continue; }

        size_t p = i + 1;
        // 会话号：连续数字
        if (p >= text.size() || !is_digit(static_cast<unsigned char>(text[p]))) {
            ++i;
            continue;
        }
        long long sid = 0;
        size_t dig_start = p;
        while (p < text.size() && is_digit(static_cast<unsigned char>(text[p]))) {
            sid = sid * 10 + (text[p] - '0');
            ++p;
            if (p - dig_start > 15) break;      // 防溢出，不是防误抓
        }
        // 分隔符
        //
        // ⚠️ 这里**必须**要求分隔符：中文正文里 `#` 开头的数字串很常见
        // （`#3`、`#9002`、`#define`），少了这一条它们全会被当成引用，
        // 于是核对器会对一堆正常文字报「出处无效」—— 那比"漏抓"更糟：
        // 它会让**正确的报告显得有问题**，人就开始不信核对器了。
        // 自检里的反例就是钉这个的（我故意放松过一次，它立刻报
        // 「光有段号没有会话号也被抓了」）。
        if (text.compare(p, sep.size(), sep) != 0) { ++i; continue; }
        p += sep.size();
        // 段号：连续数字
        if (p >= text.size() || !is_digit(static_cast<unsigned char>(text[p]))) {
            ++i;
            continue;
        }
        int seq = 0;
        size_t sdig = p;
        while (p < text.size() && is_digit(static_cast<unsigned char>(text[p]))) {
            seq = seq * 10 + (text[p] - '0');
            ++p;
            if (p - sdig > 9) break;
        }

        Locator loc;
        loc.session_id = sid;
        loc.seq        = seq;
        if (loc.valid()) out.push_back(loc);
        i = p;
    }

    // 去重但保留首次出现的顺序（报告里同一处引用出现两次不算错，
    // 但统计"共用了几处出处"时不能重复计数）
    std::vector<Locator> uniq;
    for (const auto& l : out) {
        if (std::find(uniq.begin(), uniq.end(), l) == uniq.end()) uniq.push_back(l);
    }
    return uniq;
}

bool Evidence::resolve(const Locator& loc, Segment* out) {
    if (!loc.valid()) return false;
    SessionStore& ss = SessionStore::instance();
    std::lock_guard<std::mutex> lk(ss.mutex_);
    if (ss.db_ == nullptr) return false;

    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT id, session_id, seq, ts, src_text, tgt_text, engine, ms, confidence "
        "FROM segments WHERE session_id = ? AND seq = ? LIMIT 1;";
    if (sqlite3_prepare_v2(ss.db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, loc.session_id);
    sqlite3_bind_int(st, 2, loc.seq);

    bool found = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        found = true;
        if (out != nullptr) {
            auto text = [&](int i) -> std::string {
                const unsigned char* p = sqlite3_column_text(st, i);
                return p ? reinterpret_cast<const char*>(p) : "";
            };
            out->id         = sqlite3_column_int64(st, 0);
            out->session_id = sqlite3_column_int64(st, 1);
            out->seq        = sqlite3_column_int(st, 2);
            out->ts         = text(3);
            out->src_text   = text(4);
            out->tgt_text   = text(5);
            out->engine     = text(6);
            out->ms         = sqlite3_column_int(st, 7);
            out->confidence = sqlite3_column_double(st, 8);
        }
    }
    sqlite3_finalize(st);
    return found;
}

int Evidence::segment_count(long long session_id) {
    SessionStore& ss = SessionStore::instance();
    std::lock_guard<std::mutex> lk(ss.mutex_);
    if (ss.db_ == nullptr) return 0;

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(ss.db_, "SELECT count(*) FROM segments WHERE session_id = ?;",
                           -1, &st, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_int64(st, 1, session_id);
    int n = 0;
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

std::string Evidence::describe(const Locator& loc) {
    Segment s;
    if (!resolve(loc, &s)) {
        // **不假装它是对的。** 编造的出处必须在报告里当场露出来，
        // 否则"带出处的报告"会让人更信它，而不是更怀疑它。
        return u8"（⚠️ 出处无效 " + format(loc) + u8" —— 该位置不存在）";
    }
    // 时间只留 HH:MM:SS（ts 形如 2026-09-16 14:01:00.000）
    std::string t;
    const size_t sp = s.ts.find(' ');
    if (sp != std::string::npos && s.ts.size() >= sp + 9) t = s.ts.substr(sp + 1, 8);
    return format(loc) + (t.empty() ? "" : u8"（" + t + u8"）");
}

VerifyResult Evidence::verify(const std::string& text) {
    VerifyResult r;
    const auto locs = extract(text);
    r.total = static_cast<int>(locs.size());
    for (const auto& l : locs) {
        if (resolve(l, nullptr)) ++r.ok;
        else                     r.bad.push_back(l);
    }
    return r;
}

}  // namespace evidence
