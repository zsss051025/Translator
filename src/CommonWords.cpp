#include "CommonWords.h"

#include <algorithm>
#include <cstring>
#include <unordered_set>

namespace commonwords {

namespace {

// ---- 一、真实会话里**实际被抽出来又问过**的通用词 --------------------------
//
// 注释里的会话号可以查：`Translator.exe --extract <id> --db <库>` 就能重现。
// **不要凭"我觉得这是常用词"往这里加** —— 加词的依据是"它真的冤枉过用户"。
const char* kObservedGeneral[] = {
    // #12（66 段英语播客）
    "froze", "people", "action", "learners",
    // #13（117 段播客，来自全大写小标题 `PUTTING IT TOGETHER`）
    "putting", "together",
    // #46
    "preview", "exactly",
    // #47（53 段播客）
    "down", "downs", "keep", "movies", "speaking", "midnight",
    // 用户三场演示（demo.db 会话 #1）
    "tv",
};

// ---- 二、通用缩写 ----------------------------------------------------------
//
// 判断依据只有一条：**它在任何行业里都指同一个东西**。
// 这一类不需要逐个找会话号 —— 没人需要被问"API 是什么"。
//
// ⚠️ 刻意**不收**的：KPI / OKR / MVP / ROI / SaaS 这种"每个公司定义都不一样"的。
// 它们在不同组织里含义会变（OKR 尤其），所以问一句是**有价值的**。
// 这个取舍写在注释里，免得以后有人"顺手补全"把它们塞进来。
const char* kGeneralAcronyms[] = {
    // 消费电子 / 计算机
    "tv", "pc", "usb", "cpu", "gpu", "ram", "rom", "ssd", "hdd", "os", "app",
    "id", "ip", "url", "http", "https", "html", "css", "sql", "json", "xml",
    "pdf", "faq", "dvd", "cd", "gps", "wifi", "hd", "uhd", "4k", "5g", "lte",
    // 软件开发里"在哪里都指同一个东西"的那些
    // （自检第一版就抓住了我漏掉 `api` —— 我注释里写了它却在表里没有）
    "api", "ui", "ux", "db", "sdk", "ide", "cli", "gui", "vm", "cdn",
    "ssl", "tcp", "udp", "dns", "ftp", "ssh", "exe", "sms", "gdp",
    // 常见文件/媒体格式
    "zip", "rar", "jpg", "jpeg", "png", "gif", "svg", "mp3", "mp4", "wav",
    "csv", "doc", "docx", "ppt", "pptx", "xls", "xlsx",
    // 存储 / 网络单位
    "kb", "mb", "gb", "tb", "pb", "hz", "khz", "mhz", "ghz",
    // 常见办公缩写（**含义不随公司变的那些**）
    "ceo", "cfo", "cto", "coo", "hr", "pr", "it", "asap", "fyi", "btw",
    "eta", "eod", "q1", "q2", "q3", "q4",
    // 通用人工智能/技术名词
    "ai", "ml", "nlp", "llm",
    // 地名/国家缩写
    "us", "uk", "eu", "un", "cn", "usa",
    // 【刻意**不**收 am / pm】2026-09-19 从自检脚手架里发现的：
    //   我原来把 am/pm 当"时间"（上午/下午）收进来了。但**在会议语境里**
    //   `PM` 几乎总是 **Product Manager / Project Manager**、`AM` 是 Account Manager ——
    //   也就是"人 / 角色"，恰恰是最该问的那一类。
    //   实测：一场写着"我们的 PM Penny 对过了"的会，`PM` 被**静默挡掉**、一次都没问。
    //   取舍：误问一次"PM 是重要概念吗"的代价 << 漏掉一个关键角色词的代价。
};

std::unordered_set<std::string> build_set() {
    std::unordered_set<std::string> s;
    for (const char* w : kObservedGeneral) s.insert(w);
    for (const char* w : kGeneralAcronyms) s.insert(w);
    return s;
}

}  // namespace

bool is_general(const std::string& key) {
    // 静态构造一次。表是只读的，线程安全由 C++11 的静态初始化保证。
    static const std::unordered_set<std::string> table = build_set();
    if (key.empty()) return false;

    // **自己再转一次小写**，不假设调用方给的一定是归一化键。
    //
    // 【为什么这道防线必须有】自检第一版的行为级用例传的是 `"TV"`（大写），
    // 而表里存的是 `"tv"` —— 于是**过滤静默失效**：库里的 TV 照样被问。
    // 生产路径上 `KnowledgeStore` 保证 key 是归一化的，所以这个 bug 没被暴露，
    // 但"依赖调用方替我归一化"就是典型的静默失败温床：
    // 哪天有人从别处拿了个 Value 当 key 传进来，过滤会**一声不吭地不生效**。
    //
    // ⚠️ 这里只做 ASCII 小写，**不做**完整的 normalize_key（不处理全角/标点）。
    // 表里全是 ASCII 词，够用；而完整归一化要依赖 KnowledgeStore，会造成循环依赖。
    // 调用方仍应传归一化键，这里只是兜底。
    std::string low;
    low.reserve(key.size());
    for (const char c : key) {
        low.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    }
    return table.find(low) != table.end();
}

size_t size() {
    static const std::unordered_set<std::string> table = build_set();
    return table.size();
}

}  // namespace commonwords
