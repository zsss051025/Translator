#include "SpeechFilter.h"

#include <cctype>
#include <vector>

namespace speechfilter {

std::string normalize_for_compare(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            if (std::isalnum(c)) out += static_cast<char>(std::tolower(c));
            ++i;
        } else {
            size_t len = 1;
            if      ((c & 0xE0) == 0xC0) len = 2;
            else if ((c & 0xF0) == 0xE0) len = 3;
            else if ((c & 0xF8) == 0xF0) len = 4;
            out += s.substr(i, len);
            i += len;
        }
    }
    return out;
}

bool has_no_content(const std::string& s) {
    for (unsigned char c : s) {
        if (c < 0x80) {
            if (std::isalnum(c)) return false;      // 有字母/数字
        } else if (c >= 0xC3) {
            // 首字节 >= 0xC3 ⇒ U+00C0 以上 ⇒ 带音标的拉丁字母或 CJK 等，是"字"
            // （0xC2 开头的那一段是 ¶ · ° « » ¿ ¡ 这类符号，不算内容）
            return false;
        }
        // 0x80~0xBF 是续字节，0xC2 是拉丁-1 符号 —— 继续往后看
    }
    return true;
}

const std::vector<std::string>& boilerplate_patterns() {
    // 全部按 normalize_for_compare 处理后的形状写（小写、无标点空白）。
    // 加一条之前先问：**人在真会上会不会说出这三个词连在一起**？
    static const std::vector<std::string> v = {
        // ---- 字幕组 / 平台的署名（实测出现的那一类，多语言）----
        //
        // ⚠️ 这条**被我写错过一次**：原来写成 "soustritrage"（多了一个 r），
        // 而 "Sous-titrage" 去掉连字符归一化之后是 "soustitrage"。
        // 后果是它**永远不会命中** —— 黑名单里一条拼错的词，不报错、不告警，
        // 只是安静地不干活。是自检把它逼出来的（那个用例打印了归一化结果）。
        // 所以**加黑名单词必须配一条用真实原句的用例**，不能靠眼睛看。
        "soustitrage",               // 法语「字幕」：Sous-titrage Société Radio-Canada / ST' 501
        "amaraorg",                  // Amara.org（Whisper 最经典的幻觉之一）
        "mingpaocanada",
        "dramabay", "newasiantv", "mkvcinema", "kissasian",
        // ---- 视频平台的推广语（英文）----
        "thanksforwatching",
        "thankyouforwatching",
        "pleasesubscribe",
        "subscribetothechannel",
        "likethisvideo",
        // ---- 中文侧 ----
        "字幕由", "字幕组", "字幕制作", "本字幕", "字幕提供",
        "谢谢观看", "谢谢收看", "请订阅", "点赞订阅",
    };
    return v;
}

bool is_boilerplate_hallucination(const std::string& raw) {
    const std::string n = normalize_for_compare(raw);
    if (n.empty()) return false;
    for (const auto& p : boilerplate_patterns()) {
        if (n.find(p) != std::string::npos) return true;
    }
    return false;
}

}  // namespace speechfilter
