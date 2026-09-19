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
    // 加一条之前先问：**人在真会上会不会说出这几个词连在一起**？
    //
    // ⚠️ 2026-09-19 合并：这张表原来是**两份**（这里 + `src/main.cpp` 的
    //    `static const std::vector<std::string> blacklist`）。两份都活着，
    //    改哪边都只改一半 —— 正是本项目栽过五次的那个模式。现在只有这一份。
    //
    // 合并时**删掉了原表里的三条**，因为它们会误杀真实内容
    //（这个方向比漏判坏：漏掉的幻觉用户读着别扭，删掉的真话用户看不见）：
    //    · `"翻译"`  —— 丢掉**任何含"翻译"二字的段落**。在讨论翻译工作的会上
    //                   这是必然误杀，而且它是全表最松的一条。
    //    · `"字幕"`  —— 同上；做字幕相关工作/产品评审时会整段消失。
    //                  表里保留的是**具体组合**（字幕由/字幕组/字幕制作/本字幕）。
    //    · `"订阅"`  —— 单独两个字太通用（"我们的订阅业务"）。保留请订阅/点赞订阅。
    // 保留的是"整类不变"的署名与推广语。`amara` / `yoyo` / `明镜与点点` 是
    // 具体专名（Whisper 最经典的几个幻觉源），不是普通词，所以原样保留。
    static const std::vector<std::string> v = {
        // ---- 字幕组 / 平台的署名（实测出现的那一类，多语言）----
        //
        // ⚠️ 这条**被我写错过一次**：原来写成 "soustritrage"（多了一个 r），
        // 而 "Sous-titrage" 去掉连字符归一化之后是 "soustitrage"。
        // 后果是它**永远不会命中** —— 黑名单里一条拼错的词，不报错、不告警，
        // 只是安静地不干活。是自检把它逼出来的（那个用例打印了归一化结果）。
        // 所以**加黑名单词必须配一条用真实原句的用例**，不能靠眼睛看。
        "soustitrage",               // 法语「字幕」：Sous-titrage Société Radio-Canada / ST' 501
        "amara",                     // Amara.org（Whisper 最经典的幻觉之一）
        "mingpaocanada", "明镜与点点",
        "dramabay", "newasiantv", "mkvcinema", "kissasian", "yoyo",
        // ---- 视频平台的推广语 ----
        "thanksforwatching",
        "thankyouforwatching",
        "pleasesubscribe",
        "subscribetothechannel",
        "likethisvideo",
        u8"请不吝点赞", u8"求打赏",
        // ---- 音乐标记（Whisper 在纯音乐上会吐这些）----
        u8"[音乐]", u8"(音乐)", u8"♪",
        // ---- 中文侧的字幕署名 ----
        "字幕由", "字幕组", "字幕制作", "本字幕", "字幕提供",
        "谢谢观看", "谢谢收看", "请订阅", "点赞订阅",
    };
    return v;
}

bool is_boilerplate_hallucination(const std::string& raw) {
    const std::string n = normalize_for_compare(raw);
    if (n.empty()) return false;
    for (const auto& p : boilerplate_patterns()) {
        if (p.empty()) continue;
        // ⚠️ **两遍匹配，缺一不可** —— 这是被自检逼出来的：
        //
        //   归一化匹配：对付大小写和标点差异（`Sous-titrage` 写作
        //     `soustitrage`，因为归一化会把连字符去掉）。表里像 `soustitrage`
        //     这种**按归一化形状写**的条目靠这一遍。
        //   原样匹配：对付**标点本身有意义**的条目，比如 `[音乐]` / `(音乐)` ——
        //     归一化会把方括号圆括号都去掉，于是"音乐"这两个字就和普通词一样了，
        //     那种条目在归一化那一遍里**永远不可能命中**（一条死条目 —— 不报错、
        //     不告警，只是安静地不干活）。
        //
        // 第一版只有归一化那一遍，`[音乐]`/`(音乐)` 两条就是这么死的
        //（自检里那条用例报了 `norm=[音乐] got=0`，而表里存的是 `[[音乐]]`）。
        if (n.find(p) != std::string::npos) return true;
        if (raw.find(p) != std::string::npos) return true;
    }
    return false;
}

}  // namespace speechfilter
