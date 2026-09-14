#pragma once
#include <string>
#include <vector>

// 术语纠错。
//
// 为什么需要它：实测中同一个人的名字在连续片段里被识别成
// Marco / Marko / Erikka / Erin 四种写法。光靠 initial_prompt 提示是不够的
// （Whisper 会偏向，但不保证），必须做识别后的后处理替换。
//
// 设计取舍：**宁可漏改，不可误改**。
//   - 只替换术语表里已有的词，不做全局替换
//   - 只接受编辑距离恰好为 1 的候选（差一个字）
//   - ASCII 词要求长度 >= 4 且首字母大写（避免把动词 mark 改成人名 Mark）
//   - 中日韩词要求长度 >= 3 个字符
class TermFixer {
public:
    // 术语列表（每项一个词，通常是专名）
    void set_terms(const std::vector<std::string>& terms);

    bool   enabled() const { return !terms_.empty(); }
    size_t term_count() const { return terms_.size(); }

    // 返回修正后的文本。
    // replacements 非空时，会把每次替换记录成 "Erikka -> Erika" 这样的字符串。
    std::string fix(const std::string& text,
                    std::vector<std::string>* replacements = nullptr) const;

    // 统计与术语表匹配（距离为 0）的术语个数——用于自检与日志
    int count_exact_hits(const std::string& text) const;

private:
    std::vector<std::string> terms_;
};
