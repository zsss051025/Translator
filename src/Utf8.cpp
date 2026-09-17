#include "Utf8.h"

namespace utf8 {

namespace {

// U+FFFD 的 UTF-8 编码
constexpr char kReplacement[] = "\xEF\xBF\xBD";

// 一个码点占几个字节、以及后续字节各自允许的范围。
//
// 【为什么要写得这么细】"首字节 & 0xE0 == 0xC0 就认为是 2 字节"这种宽松写法
// 会把**过长编码**（overlong，例如用 2 字节表示 ASCII）和**代理对**
// （U+D800..U+DFFF，UTF-8 里本来就不该出现）当成合法 ——
// 它们能让校验通过，却是真正会引发乱码/安全问题的东西。
//
// 返回 0 表示这第一个字节本身就是非法的（0x80..0xC1、0xF5..0xFF）。
int seq_len(unsigned char c) {
    if (c <= 0x7F) return 1;
    if (c <= 0xC1) return 0;          // 0x80..0xC1：孤立续字节 / 过长编码的首字节
    if (c <= 0xDF) return 2;
    if (c <= 0xEF) return 3;
    if (c <= 0xF4) return 4;
    return 0;                          // 0xF5..0xFF 越界
}

// 校验第 i 个后续字节（0-based 位置 i），返回它是否在合法范围
bool cont_ok(unsigned char lead, size_t index, unsigned char c) {
    // 第二个字节要单独判，因为 E0/ED/F0/F4 有额外约束
    if (index == 0) {
        switch (lead) {
        case 0xE0: return c >= 0xA0 && c <= 0xBF;   // 排除过长编码
        case 0xED: return c >= 0x80 && c <= 0x9F;   // 排除代理对
        case 0xF0: return c >= 0x90 && c <= 0xBF;   // 排除过长编码
        case 0xF4: return c >= 0x80 && c <= 0x8F;   // 排除 > U+10FFFF
        default:   return c >= 0x80 && c <= 0xBF;
        }
    }
    return c >= 0x80 && c <= 0xBF;
}

// 第 i 个位置开始的序列是否合法；len 回传序列长度（合法时 > 0，非法时为 1 表示"吃掉一个字节"）
bool valid_at(const std::string& s, size_t i, size_t* len) {
    const unsigned char lead = static_cast<unsigned char>(s[i]);
    const int n = seq_len(lead);
    if (n == 0) { *len = 1; return false; }
    if (n == 1) { *len = 1; return true; }
    if (i + static_cast<size_t>(n) > s.size()) { *len = 1; return false; }  // 被截断
    for (int k = 1; k < n; ++k) {
        const unsigned char c = static_cast<unsigned char>(s[i + k]);
        if (!cont_ok(lead, static_cast<size_t>(k - 1), c)) { *len = 1; return false; }
    }
    *len = static_cast<size_t>(n);
    return true;
}

}  // namespace

bool is_valid(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        size_t len = 0;
        if (!valid_at(s, i, &len)) return false;
        i += len;
    }
    return true;
}

std::string sanitize(const std::string& s, size_t* replaced) {
    size_t n_bad = 0;
    std::string out;
    out.reserve(s.size());

    for (size_t i = 0; i < s.size();) {
        size_t len = 0;
        if (valid_at(s, i, &len)) {
            out.append(s, i, len);
            i += len;
            continue;
        }
        // 非法：吃掉**一个**字节，吐一个替换字符。
        //
        // 【为什么不一次吃掉整段】无法判断"这段非法"到底是一个字符被截断
        // 还是好几个字符都坏了。一个字节换一个 U+FFFD 是最保守的做法：
        // 它不会把好字节一起吞掉，而坏字节本来也没有信息可救。
        out.append(kReplacement);
        ++n_bad;
        i += 1;
    }
    if (replaced) *replaced = n_bad;
    return out;
}

std::string truncate(const std::string& s, size_t max_bytes, const std::string& marker) {
    // 先净化再截断 —— 反过来会先切坏、再"净化"出一堆 U+FFFD
    size_t fixed = 0;
    const std::string clean = sanitize(s, &fixed);
    if (clean.size() <= max_bytes) return clean;

    // 预算不够时宁可少给内容，也别让 marker 把上限撑爆
    const bool room = max_bytes > marker.size();
    const size_t keep = room ? (max_bytes - marker.size()) : max_bytes;

    // 退到字符边界：UTF-8 的续字节长这样 10xxxxxx，一直退到不是续字节为止
    size_t cut = keep;
    while (cut > 0 && (static_cast<unsigned char>(clean[cut]) & 0xC0) == 0x80) --cut;

    std::string out = clean.substr(0, cut);
    if (room) out += marker;
    return out;
}

}  // namespace utf8
