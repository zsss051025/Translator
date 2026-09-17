#include <iostream>
#include <vector>
#include <string>
#include <conio.h>    // 用于 _kbhit() 和 _getch()
#include <io.h>       // _isatty / _fileno：判断 stdin 是不是终端（确认交互要据此让路）
#include <chrono>
#include <thread>
#include <cmath>
#include <memory>                    // std::unique_ptr

#include "ITranslator.h"
#include "HunyuanTranslator.h"
#include "DeepSeekTranslator.h"
#include "audio_capture.h"
#include "SpeechEngine.h"
#include "SessionStore.h"
#include "KnowledgeStore.h"
#include "KnowledgeGap.h"
#include "ConfirmGaps.h"
#include "KnowledgeExtract.h"
#include "Utf8.h"
#include "AgentTool.h"
#include "DeliverableWriter.h"
#include "LlmSummarizer.h"
#include "SubtitleWindow.h"
#include "TermFixer.h"
#include "AppConfig.h"
#include "WavReader.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>   // 确认交互的自检用 istringstream 喂脚本化回答

#include "json.hpp"  // Agent 工具层的自检要判断"结果是不是合法 JSON"

// 一段文本是不是**语法上合法的 JSON**。
//
// 【为什么自检要单独验这个】Agent 的工具结果会被塞进模型上下文，
// 模型要按字段读。一个拼歪的 JSON（少个引号、中文被 ensure_ascii 转义丢了）
// 在 C++ 里看不出任何异常 —— 只有解析时才炸，而那时问题已经到模型那边了。
// 所以"能解析"必须当成断言，而不是等它出问题。
static bool json_ok(const std::string& s) {
    if (s.empty()) return false;
    try {
        (void)nlohmann::json::parse(s);
        return true;
    } catch (...) {
        return false;
    }
}

// 仅对 ASCII 做小写化（中文不受影响）。
// 用途：黑名单匹配必须大小写不敏感——原实现用 result.find() 是敏感的，
// 而黑名单写的是 "yoyo"、Whisper 实际输出的是 "YoYo"，这条从未生效过。
static std::string to_lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// 按空白切词
static std::vector<std::string> split_words(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// 归一化一个词用于比较：去掉首尾非字母数字字符并转小写。
// 必须做这一步——实测里 "for today" vs "for today." 因为一个句号就匹配不上，
// 导致真实重复漏过。
static std::string norm_word(const std::string& w) {
    size_t b = 0, e = w.size();
    while (b < e && !std::isalnum(static_cast<unsigned char>(w[b]))) ++b;
    while (e > b && !std::isalnum(static_cast<unsigned char>(w[e - 1]))) --e;
    if (e <= b) return {};
    return to_lower_ascii(w.substr(b, e - b));
}

static bool words_equal(const std::string& a, const std::string& b) {
    const std::string na = norm_word(a);
    const std::string nb = norm_word(b);
    return !na.empty() && na == nb;
}

// 折叠段内立即重复："How are you Erikka? How are you Erikka? ..." -> 只留一次。
// 这是 Whisper 自身的退化循环（不是重叠造成的），靠解码参数很难完全压住，
// 所以在文本层做一次兜底折叠。
static std::string collapse_repeats(const std::string& s) {
    auto w = split_words(s);
    if (w.size() < 4) return s;

    bool changed = true;
    while (changed) {
        changed = false;
        const size_t n = w.size();
        // 从长到短找任意位置上的"连续重复片段"
        for (size_t len = n / 2; len >= 2 && !changed; --len) {
            for (size_t p = 0; p + 2 * len <= n; ++p) {
                bool same = true;
                for (size_t i = 0; i < len; ++i) {
                    if (!words_equal(w[p + i], w[p + len + i])) { same = false; break; }
                }
                if (same) {
                    w.erase(w.begin() + static_cast<long>(p + len),
                            w.begin() + static_cast<long>(p + 2 * len));
                    changed = true;
                    break;
                }
            }
        }
    }

    std::string out;
    for (const auto& x : w) {
        if (!out.empty()) out += ' ';
        out += x;
    }
    return out;
}

// 去掉"上一段尾部"与"本段头部"的重叠文本。
//
// 背景：滑动窗口相邻两段音频有重叠区，Whisper 会把同一句话识别两次。
// 实测中三次漏判的原因各不相同，所以这里做了两处放宽：
//   1) 比较前先归一化标点  —— "about." 与 "about" 应视为同一个词
//   2) 允许在本段开头跳过 0~2 个功能词 —— Whisper 常在切点处多吐一个 "to"/"a"
static std::string strip_overlap(const std::string& prev, const std::string& cur) {
    if (prev.empty() || cur.empty()) return cur;

    const auto pw = split_words(prev);
    const auto cw = split_words(cur);
    if (pw.empty() || cw.empty()) return cur;

    const size_t maxk = std::min<size_t>(std::min(pw.size(), cw.size()), 12);

    for (size_t off = 0; off <= 2 && off < cw.size(); ++off) {
        const size_t kmax = std::min(maxk, cw.size() - off);
        if (kmax < 1) continue;
        for (size_t k = kmax; k >= 1; --k) {
            bool match = true;
            for (size_t i = 0; i < k; ++i) {
                if (!words_equal(pw[pw.size() - k + i], cw[off + i])) { match = false; break; }
            }
            if (match) {
                std::string out;
                for (size_t i = off + k; i < cw.size(); ++i) {
                    if (!out.empty()) out += ' ';
                    out += cw[i];
                }
                return out;   // 完全重复时返回空串，调用方会跳过这一段
            }
        }
    }
    return cur;
}

// 读术语表文件（每行一个词或短语，# 开头为注释）。
// 返回词条列表——同一份列表有两个用途：
//   ① 作为 Whisper 的 initial_prompt（识别时偏向这些词）
//   ② 交给 TermFixer 做识别后的纠错替换（提示不够可靠时的兜底）
static std::vector<std::string> load_glossary_terms(const std::string& path) {
    std::vector<std::string> terms;
    if (path.empty()) return terms;

    std::ifstream f(path);
    if (!f) {
        std::cerr << "[Glossary] 打不开术语表: " << path << std::endl;
        return terms;
    }
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        const size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        line = line.substr(b);
        if (line.empty() || line[0] == '#') continue;
        terms.push_back(line);
    }
    if (!terms.empty()) {
        std::cout << "[Glossary] 已载入 " << terms.size() << " 个术语"
                     "（识别提示 + 识别后纠错）" << std::endl;
    }
    return terms;
}

// 把词条拼成 Whisper 的 initial_prompt。
// 实测提示的约束力有限（名字仍会乱跳），所以识别后纠错那一环不能省。
static std::string terms_to_prompt(const std::vector<std::string>& terms) {
    std::string out;
    for (const auto& t : terms) {
        if (!out.empty()) out += ", ";
        out += t;
    }
    return out;
}

// ---- 三条腿的输入：全部来自知识库里 **confirmed** 的条目（§7 步骤 2.5）----
//
// 三条腿指的是"同一份专名知识"的三个用法：
//   ① Whisper 的 initial_prompt —— 让识别偏向这些词
//   ② 翻译约束 —— 统一译文里的专名写法
//   ③ 摘要背景 —— 让纪要不与用户确认过的事实矛盾
//
// 【为什么要在这里统一取一次】原来三条腿各拿各的：① 和 ② 吃 --glossary 文件，
// ③ 什么都没有。于是"用得越久越懂你"完全没有落点 ——
// 用户确认过的知识躺在库里，一个字都影响不到识别和翻译。
//
// ⚠️ **必须经 KnowledgeStore::constraint_items()**，那是 §6.5 红线的唯一出口。
//    candidate（模型猜的）绝不能进来：它会污染整条链路，而用户看不出来。
struct ConstraintInputs {
    std::vector<std::string> terms;        // ①②③ 里的词条
    std::vector<std::string> background;   // ③ 的背景行
    size_t confirmed_total = 0;            // 库里 confirmed 的条目总数（用于日志说明）
    bool   store_ready     = false;
};

static ConstraintInputs load_constraints_from_knowledge() {
    ConstraintInputs ci;
    auto& ks = KnowledgeStore::instance();
    if (!SessionStore::instance().long_term_memory_ready()) {
        // 库还没建好（或 FTS5 没编进来）。**不是错误**：
        // 没有知识库时行为要跟以前完全一样，不能因此拒绝对话。
        return ci;
    }
    ci.store_ready = true;

    const auto usable = ks.constraint_items();
    ci.confirmed_total = usable.size();
    ci.terms      = knowledge::constraint_terms(usable);
    ci.background = knowledge::background_lines(usable);
    return ci;
}


// --export <session_id>：把指定会话导出为交付物
// （纪要 markdown / 行动项 csv / 单文件网页 / 双语字幕）
// 交付物生成结果
struct DeliverableOutcome {
    bool        ok = false;
    std::string dir;
    std::string html_path;      // 供自动打开
    std::string generator;      // 摘要来源
    int         segment_count = 0;
    int         action_count  = 0;
    long long   duration_sec  = 0;
    std::string error;
};

// --gaps：只打印知识缺口检测的结果。不加载模型、不采集音频，秒级。
//
// 【为什么需要它】缺口检测本身是纯函数、有 9 个单测，但那些用例是我构造的。
// 这个项目两次栽在"测试数据与真实数据形状不一致"上，所以留一条能对着**真库**跑的路——
// 2.4 的确认交互将来也走同一个 detect_gaps_from_store()。
static int run_show_gaps(const AppConfig& cfg) {
    if (!SessionStore::instance().init(cfg.db_path)) {
        std::cerr << "[Gaps] 打不开数据库: " << cfg.db_path << std::endl;
        return 1;
    }
    const auto all = KnowledgeStore::instance().list("", 10000);
    std::cout << "[Gaps] 库里共有 " << all.size() << " 条知识" << std::endl;
    for (const auto& k : all) {
        std::cout << "   #" << k.id << " [" << k.kind << "] " << k.key
                  << " = " << k.value
                  << "  status=" << k.status
                  << "  hits=" << k.hits
                  << "  conf=" << k.confidence << std::endl;
    }

    const auto qs = knowledge::detect_gaps_from_store();
    std::cout << "\n[Gaps] 检测到 " << qs.size() << " 个待确认的问题（最多 5 个）"
              << std::endl;
    if (qs.empty()) {
        std::cout << "   （没有需要问的 —— confirmed 且没变化的条目不会被问，这是有意的："
                     "§1.3「提问是稀缺资源」）" << std::endl;
    }
    int n = 0;
    for (const auto& q : qs) {
        std::cout << "   " << ++n << ". [" << knowledge::to_string(q.rule) << "] "
                  << q.question << std::endl;
    }
    SessionStore::instance().close();
    return 0;
}

// 用系统默认程序打开文件（Windows 的 start 命令）
static void open_with_default_app(const std::string& path) {
    if (path.empty()) return;
    const std::string cmd = "start \"\" \"" + path + "\"";
    std::system(cmd.c_str());
}

// 决定用哪个翻译后端：1 = 云端 DeepSeek，2 = 本地混元。
//
// 刻意做成**纯函数**（不碰 std::cin、不加载模型），这样能进 L1 自检——
// §8.2 的"把纯逻辑做成静态函数"就是为这种场合：决策规则要能被秒级验证，
// 而不是每次都得真启动一遍程序、手敲一遍。
//
// 关键规则：**有 --api-key 不代表要用云端翻译**。
// 用户完全可能只让摘要上云，翻译必须留本机（"数据不出本机"是卖点）。
// 所以云端翻译只能由 --translator cloud 显式选择。
static int choose_translator_backend(const AppConfig& cfg, std::string& why) {
    if (cfg.translator == "cloud") {
        if (cfg.deepseek_api_key.empty()) {
            why = "--translator cloud 需要 --api-key（或环境变量 DEEPSEEK_API_KEY），已回退本地";
            return 2;
        }
        why = "--translator cloud";
        return 1;
    }
    if (cfg.translator != "local") {
        why = "无法识别的 --translator 取值「" + cfg.translator + "」，已按 local 处理";
        return 2;
    }
    why = "--translator local（默认；云端翻译需显式指定 cloud）";
    return 2;
}

// 生成一场会话的交付物。
// 命令行（--export）和会话结束时的自动导出共用这一条路径，
// 保证两种入口产出的东西完全一致。
static DeliverableOutcome generate_deliverables(const AppConfig& cfg, long long sid) {
    DeliverableOutcome out;
    auto& store = SessionStore::instance();

    SessionInfo info;
    bool found = false;
    for (const auto& s : store.list_sessions(10000)) {
        if (s.id == sid) { info = s; found = true; break; }
    }
    if (!found) {
        out.error = "找不到会话 #" + std::to_string(sid);
        return out;
    }

    const auto segs = store.fetch_segments(sid);
    if (segs.empty()) {
        out.error = "会话 #" + std::to_string(sid) + " 没有任何段落";
        return out;
    }

    const auto st   = store.stats(sid);
    const auto meta = DeliverableWriter::make_meta(info, st.count, st.avg_ms, segs);

    // ---- 摘要：大模型优先，失败自动回退规则抽取 ----
    MeetingSummary summary;
    bool summarized = false;

    // 决定用哪条摘要后端。
    // 注意不能用翻译模型做摘要——实测 HY-MT1.5-1.8B 会把转录原样回显，
    // 完全不执行摘要指令。所以 local 必须由 --llm-model 指定一个指令模型。
    std::string mode = cfg.summarizer;
    if (mode == "auto") {
        if (!cfg.llm_model.empty())             mode = "local";
        else if (!cfg.deepseek_api_key.empty()) mode = "cloud";
        else                                    mode = "rules";
    }
    if (mode == "local" && cfg.llm_model.empty()) {
        std::cerr << "[摘要] 本地模式需要 --llm-model <指令模型.gguf>；"
                     "翻译模型不会做摘要，会原样回显转录。" << std::endl;
        mode = cfg.deepseek_api_key.empty() ? "rules" : "cloud";
        std::cerr << "[摘要] 已自动改用: " << mode << std::endl;
    }

    if (mode != "rules") {
        const bool use_cloud = (mode == "cloud");
        LlmSummarizer sm(use_cloud ? LlmSummarizer::Backend::Cloud
                                   : LlmSummarizer::Backend::Local);
        sm.set_api_key(cfg.deepseek_api_key);
        sm.set_target_language(cfg.target_lang);

        // ---- 第三条腿：把 confirmed 知识作为摘要背景（§7 步骤 2.5）----
        //
        // 【为什么在这里现取而不是从调用方传进来】`--export` 也要用得到 ——
        // 用户重出一场旧会话的纪要时，理应用**当前**的知识库，
        // 而且这正是"越用越懂你"最直观的体现：同一段录音，现在重出会比当时更准。
        // 传参的话两个入口都得记着传，迟早漏一个。
        {
            const auto kb = load_constraints_from_knowledge();
            sm.set_background(kb.background);
            if (!kb.background.empty()) {
                std::cout << "[摘要] 已注入 " << kb.background.size()
                          << " 条 confirmed 背景知识" << std::endl;
            }
        }

        // 本地后端需要一个 llama 实例；它只在本次导出期间存活
        std::unique_ptr<HunyuanTranslator> hy;
        bool backend_ready = true;
        if (!use_cloud) {
            hy = std::make_unique<HunyuanTranslator>(cfg.llm_model);
            if (!hy->init()) {
                std::cerr << "[摘要] 本地模型加载失败: " << cfg.llm_model << std::endl;
                backend_ready = false;
            } else {
                hy->set_target_language(cfg.target_lang);
                sm.set_generate_fn([&hy](const std::string& sys, const std::string& usr,
                                         std::string& o) {
                    return hy->generate_once(sys, usr, o, /*max_new=*/1024);
                });
            }
        }

        if (backend_ready) {
            std::cout << "[摘要] 正在用「" << sm.name() << "」生成纪要，请稍候..." << std::endl;
            std::string err;
            if (sm.summarize(segs, summary, err)) {
                summarized = true;
                std::cout << "[摘要] 成功：" << summary.topics.size() << " 个主题，"
                          << summary.actions.size() << " 条行动项" << std::endl;
            } else {
                std::cerr << "[摘要] 失败，回退规则抽取：" << err << std::endl;
            }
        }
    }

    if (!summarized) summary = DeliverableWriter::extract_by_rules(segs);

    // 行动项可信度校验。
    // 放在这里是刻意的：云端大模型、本地大模型、规则抽取三条路都汇到这一点，
    // 校验一次就能全覆盖。校验规则见 DeliverableWriter::sanitize_actions。
    {
        std::vector<std::string> notes;
        // 传整场转录进去：云端摘要走中文任务文本 + 英文转录时，
        // ActionItem.source 回填不上（LCS 跨语言匹配不了），
        // 只有拿整场转录当依据才能正确判断"这个日期是不是编的"。
        const int n_drop = DeliverableWriter::sanitize_actions(summary.actions, &notes, &segs);
        if (!notes.empty()) {
            // 措辞注意：n_drop 只数"被丢弃的条目"，而 notes 里还包含
            // "截止日期无依据已清空"这类**保留但被修正**的条目，两者数量不等。
            std::cout << "[摘要] 行动项校验：丢弃 " << n_drop << " 条，共 " << notes.size()
                      << " 处改动（宁缺勿滥）" << std::endl;
            for (const auto& d : notes) std::cout << "        - " << d << std::endl;
        }
    }

    const auto res = DeliverableWriter::write(sid, segs, meta, summary, cfg.deliverable_dir);
    if (!res.ok) {
        out.error = res.error;
        return out;
    }

    out.ok            = true;
    out.dir           = res.dir;
    out.generator     = summary.generator;
    out.segment_count = meta.count;
    out.action_count  = static_cast<int>(summary.actions.size());
    out.duration_sec  = meta.duration_sec;
    for (const auto& f : res.files) {
        if (f.size() > 5 && f.compare(f.size() - 5, 5, ".html") == 0) out.html_path = f;
    }
    return out;
}

// stdin 是不是一个真实终端。
//
// 【为什么用 _isatty 而不是别的】产品将来会有 GUI 壳（§7 第 4 阶段），
// 那时 stdin 不是终端 —— 确认交互必须**自动让路**，由 GUI 弹自己的对话框，
// 而不是在后台默默 getline 然后永远等不到输入。
// 管道/重定向（`Translator.exe | tee log.txt`）同理。
static bool stdin_is_tty() {
    return _isatty(_fileno(stdin)) != 0;
}

// 会话结束时的确认交互（§7 步骤 2.4）。**必须在交付物写完之后调用。**
//
// session_id：本场会话 id。它有两个用途：
//   ① <=0 表示这场没有内容，没什么可核对的，直接跳过
//   ② 传给 detect_gaps_from_store()，让 `NewlySeen` 规则能判断"这条是不是本场新听到的"
static void confirm_gaps_at_session_end(const AppConfig& cfg, long long session_id) {
    using namespace knowledge;

    if (session_id < 0) return;

    // 决定这次到底问不问，并把**原因**打出来。
    // 不打原因的话，"为什么这次没问"会变成一个只能靠读代码回答的问题。
    bool ask = false;
    const char* why = "";
    switch (cfg.ask_mode) {
    case AppConfig::AskMode::Never:
        ask = false; why = "--no-ask";
        break;
    case AppConfig::AskMode::Always:
        ask = true;  why = "--ask";
        break;
    case AppConfig::AskMode::Auto:
        if (!cfg.wav_path.empty()) {
            ask = false; why = "--wav 批处理模式（要交互加 --ask）";
        } else if (!stdin_is_tty()) {
            ask = false; why = "stdin 不是终端（管道/重定向/将来的 GUI 壳）";
        } else {
            ask = true;  why = "自动（stdin 是终端）";
        }
        break;
    }

    if (!ask) {
        std::cout << "[确认] 跳过（" << why << "）" << std::endl;
        return;
    }

    // 传 session_id：NewlySeen 规则靠它判断"这条是不是本场新听到的"。
    const auto questions = detect_gaps_from_store(kMaxQuestionsDefault, session_id);
    if (questions.empty()) {
        // 【为什么这里要说话，而不是按 §1.3 保持静默】
        // 真跑会话 #44 实测：结束日志里**一行 [确认] 都没有**，因为抽出的 3 个名字
        // 全都已在上一场确认过 → 没有可问的 → 静默返回。
        // 这行为**是对的**，但它和"抽取器坏了 / 库读不到"长得一模一样，
        // 排查时只能靠读代码。§1.3 说的是"别问没意义的问题"，
        // 不是"别告诉用户发生了什么" —— 打一行状态不算打扰。
        std::cout << "[确认] 没有需要确认的知识（本场抽出的条目都已在库里确认过）"
                  << std::endl;
        return;
    }

    const auto stats = run_confirmation(
        questions,
        // 读一行。返回 false = 输入结束。
        //
        // 【为什么用 getline 而不是 _getch 裸读】答案经常是中文专名的正确写法，
        // 裸读会把输入法打得七零八落。代价是没有超时；接受它，因为此时
        // **交付物已经落盘**，进程多等一会儿不丢任何东西（见 ConfirmGaps.h 的说明）。
        [](std::string& line) -> bool {
            if (!std::getline(std::cin, line)) return false;   // EOF
            return true;
        },
        std::cout,
        kMaxQuestionsDefault);

    if (stats.failed > 0) {
        std::cerr << "[确认] 有 " << stats.failed
                  << " 条没能写进知识库 —— 上面的 [失败] 行里有原因" << std::endl;
    }
}

// 会话结束时的知识处理：**先抽取，再问用户**（§6.4① + §6.7）。
//
// 【顺序不能反】抽取把本场听到的专名落成候选，确认交互才有东西可问。
// 反过来的话，第一场永远问不出任何问题 —— 这正是 2.6 之前的状态：
// knowledge 表跑完一场真实会话仍然是 0 行，一声都不问，三条腿也拿不到东西。
//
// 【必须在 write() 之后】交付物先落盘，用户在这里 Ctrl+C 或走开都不丢产出。
static void learn_from_session(const AppConfig& cfg, long long session_id, bool had_content) {
    using namespace knowledge;

    if (!had_content || session_id < 0) return;
    if (cfg.ask_mode == AppConfig::AskMode::Never) {
        // --no-ask 的意思是"别打扰我"。但它不该顺带关掉抽取 ——
        // 抽取是静默的、不打扰任何人的，而且关掉它等于知识库永远不增长。
        // 所以这里只记一笔、继续抽取。
    }

    // ---- 1) 自动抽取：全部落成 candidate（§6.4①）----
    {
        const auto segs = SessionStore::instance().fetch_segments(session_id);
        const auto cands = extract_candidates(segs, session_id);
        if (!cands.empty()) {
            std::string err;
            const int n = save_candidates(cands, &err);
            std::cout << "[知识] 本场抽出 " << n << " 个候选专名（未确认，不会被用作约束）"
                      << std::endl;
            // 只打印前几个，别刷屏
            constexpr size_t kShow = 6;
            for (size_t i = 0; i < cands.size() && i < kShow; ++i) {
                std::cout << "        " << cands[i].value
                          << "（" << cands[i].kind << "，出现 " << cands[i].hits
                          << " 次，依据：" << cands[i].why << "）" << std::endl;
            }
            if (cands.size() > kShow) {
                std::cout << "        ...（共 " << cands.size() << " 个）" << std::endl;
            }
            if (!err.empty()) std::cerr << "[知识] 部分条目写入失败：" << err << std::endl;
        } else {
            // 【为什么"0 个"也要打一行】不打的话，"抽取有没有跑"这件事
            // 在日志里完全看不出来 —— 而"诊断工具撒谎"这个项目已经栽过两次。
            // 一行字换一个可观测点，很值。
            std::cout << "[知识] 本场未抽到候选专名（没听到首字母大写的名字或缩写）"
                      << std::endl;
        }
    }

    // ---- 2) 确认交互 ----
    confirm_gaps_at_session_end(cfg, session_id);
}

// 把第二路音频混入主片段：逐样本相加并限幅。
// 两路都是 16kHz/mono/f32，不需要重采样。
// 长度不一致时以长的为准、短的按 0 补齐——两个设备时钟有微小漂移，
// 但主循环每轮都清空缓冲，所以漂移不会累积。
static void mix_audio(std::vector<float>& dst, const std::vector<float>& src) {
    if (src.empty()) return;
    if (dst.size() < src.size()) dst.resize(src.size(), 0.0f);
    for (size_t i = 0; i < src.size(); ++i) {
        const float v = dst[i] + src[i];
        dst[i] = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
    }
}

// --extract <会话id> [--apply]：对**已经存下来的**一场会话跑一遍自动抽取。
//
// 默认**只读**，只打印抽出什么。加 `--apply` 才真的写库，并接着跑确认交互。
//
// 【为什么要这个命令】
//   ① 抽取器的自检用的是手搓段落，而本项目栽过两次"测试数据与真实数据形状不一致"
//      （§8.8⑨）。有了它就能拿**真实会议转录**验证抽取器认出了什么、认错了什么，
//      不用重新录一遍音频。
//   ② `--apply` 是一个正当功能：从历史会话补学知识。
//      （2.6 之前的会话从没抽过；或者用户就是想拿几场旧会议喂一遍知识库。）
//
// 默认只读是刻意的：它同时是排查工具，反复跑不该改变库的状态
// （写库会让 hits 累加、把缺口检测的排序搅乱）。
static int run_extract(const AppConfig& cfg) {
    auto& store = SessionStore::instance();
    if (!store.init(cfg.db_path)) {
        std::cerr << "[Extract] 打不开数据库: " << cfg.db_path << std::endl;
        return 1;
    }

    const long long sid = cfg.extract_session;
    const auto segs = store.fetch_segments(sid);
    if (segs.empty()) {
        std::cerr << "[Extract] 会话 #" << sid << " 没有段落（或不存在）" << std::endl;
        return 1;
    }

    std::cout << "[Extract] 会话 #" << sid << " 共 " << segs.size() << " 段，开始抽取"
              << std::endl;
    const auto cands = knowledge::extract_candidates(segs, sid);
    std::cout << "[Extract] 抽出 " << cands.size() << " 个候选：" << std::endl;
    for (const auto& c : cands) {
        std::cout << "    [" << c.kind << "] " << c.value
                  << "  hits=" << c.hits
                  << "  conf=" << c.confidence
                  << "  seq=" << c.source_seq
                  << "  依据: " << c.why << std::endl;
        std::cout << "        证据: " << c.source_text << std::endl;
    }

    if (!cfg.extract_apply) {
        std::cout << "[Extract] （只读，未写库；要写入并确认请加 --apply）" << std::endl;
        store.close();
        return 0;
    }

    std::string err;
    const int n = knowledge::save_candidates(cands, &err);
    std::cout << "[Extract] 已写入 " << n << " 条候选（全部 status=candidate，"
                 "未被用作任何约束）" << std::endl;
    if (!err.empty()) std::cerr << "[Extract] 部分写入失败：" << err << std::endl;

    // 接着问用户 —— 和生产路径的顺序一致：先抽取落候选，再问。
    //
    // ⚠️ **不能在这之前 close()**：确认交互要从库里读候选（detect_gaps_from_store），
    // 库关了它只会返回空列表，然后**一声不吭**地结束 —— 看起来像"没有问题要问"。
    // 这个坑我踩了一次：先 close 再 ask，输出里连一行 [确认] 都没有，
    // 排查半天才发现是库已关。
    confirm_gaps_at_session_end(cfg, sid);
    store.close();
    return 0;
}

// --tools [名字] [--args '<json>']：看工具层，或者手动跑一个工具。
//
// 【为什么这是一个必需的命令，而不是"顺手加的"】
// Agent 出问题时，"是模型不会用工具"和"工具本身坏了"是两回事，
// 而它们在日志里长得一模一样（都表现为"最后没给出有用答案"）。
// 有了它就能把两者分开：
//   ① 只打印 → 确认**模型看到的工具定义**长什么样（description/schema 写歪了，模型就不会调）
//   ② 带 --args 跑 → 确认**工具本身**在真实数据上返回什么
// （这个项目因为"诊断工具和真实路径不一致"栽过三次，所以这里刻意只走一条路径：
//   命令行调用和 Agent 调用都走同一个 ToolRegistry::call）
static int run_tools(const AppConfig& cfg) {
    agent::ToolRegistry reg;
    agent::register_readonly_tools(reg);

    agent::ToolContext ctx;
    ctx.deliverable_root = cfg.deliverable_dir;

    if (cfg.tools_list_only) {
        std::cout << "[Tools] 已注册 " << reg.size() << " 个工具（5.1 全部只读）：" << std::endl;
        for (const auto& n : reg.names()) {
            const agent::Tool* t = reg.find(n);
            std::cout << "  · " << n << std::endl;
            std::cout << "      " << t->description << std::endl;
            std::cout << "      params: " << t->params_schema << std::endl;
        }
        std::cout << "\n[Tools] function calling 用的 tools 数组（" 
                  << reg.tools_json().size() << " 字节）：" << std::endl;
        std::cout << reg.tools_json() << std::endl;
        return 0;
    }

    // 指定了工具名 → 真跑一次。
    //
    // ⚠️ **刻意不在这里预检查"工具存在吗"**。
    // 第一版这里加了一句 `if (reg.find(name) == nullptr) { 报错并 return 1; }`，
    // 结果是 `ToolRegistry::call()` 里那段"未知工具就把它能用的工具列表回给模型"的
    // 逻辑**从命令行永远走不到** —— 于是命令行和 Agent 走了两条不同的路。
    // 这个项目因为"诊断工具和真实路径不一致"栽过三次（见 §8.8），
    // 而这次是在同一个文件的注释里刚警告完就又犯了一次。
    // 判断依据：**这里只负责打开库和打印，所有分支判断都留在 call() 里。**
    if (!SessionStore::instance().init(cfg.db_path)) {
        std::cerr << "[Tools] 打不开数据库: " << cfg.db_path << std::endl;
        return 1;
    }

    std::cout << "[Tools] 调用 " << cfg.tools_name
              << "  args=" << (cfg.tools_args.empty() ? "{}" : cfg.tools_args) << std::endl;
    const auto r = reg.call(cfg.tools_name, cfg.tools_args, ctx);
    if (!r.ok) {
        std::cerr << "[Tools] 失败：" << r.error << std::endl;
    } else {
        std::cout << "[Tools] 摘要：" << r.audit << std::endl;
    }
    std::cout << r.content << std::endl;
    SessionStore::instance().close();
    return r.ok ? 0 : 1;
}

// --export <id>：命令行方式生成交付物
static int run_export(const AppConfig& cfg) {
    auto& store = SessionStore::instance();
    if (!store.init(cfg.db_path)) {
        std::cerr << "[Export] 打不开数据库: " << cfg.db_path << std::endl;
        return 1;
    }

    const long long sid = cfg.export_session;
    const auto out = generate_deliverables(cfg, sid);

    if (!out.ok) {
        std::cerr << "[Export] " << out.error << std::endl;
        const auto all = store.list_sessions(10000);
        if (!all.empty()) {
            std::cerr << "[Export] 库中现有会话：" << std::endl;
            for (const auto& s : all) {
                std::cerr << "    #" << s.id << "  " << s.started_at
                          << "  engine=" << s.engine
                          << "  segments=" << s.segment_count << std::endl;
            }
        }
        return 1;
    }

    std::cout << "[Export] 会话 #" << sid << " 已导出（" << out.generator << "）" << std::endl;
    std::cout << "[Export] 输出目录: " << out.dir << std::endl;
    std::cout << "[Export] 段落 " << out.segment_count
              << " 条，时长 " << out.duration_sec << "s，"
              << "行动项 " << out.action_count << " 条" << std::endl;
    return 0;
}

// --demo-session：写入一场内容真实的演示会话。
// 用途是验证交付物导出（纪要/行动项/网页/字幕）这条链路，
// 不需要声卡和 GPU，也不需要在现场真开一场会。
static int run_demo_session(const AppConfig& cfg) {
    auto& store = SessionStore::instance();
    if (!store.init(cfg.db_path)) {
        std::cerr << "[Demo] 打不开数据库: " << cfg.db_path << std::endl;
        return 1;
    }

    const long long sid = store.begin_session("Hunyuan", u8"Q4 评审会（演示数据）");
    if (sid < 0) {
        std::cerr << "[Demo] 建立会话失败" << std::endl;
        return 1;
    }

    struct Pair { const char* src; const char* tgt; long long ms; double conf; };
    const Pair demo[] = {
        {"Hi everyone, thanks for joining the Q4 review.",
         u8"大家好，感谢参加第四季度评审。", 210, 0.93},
        {"Let's start with the Phoenix project status.",
         u8"我们先从 Phoenix 项目的状态开始。", 245, 0.90},
        {"We need to push the delivery to October 31st.",
         u8"我们需要把交付推迟到 10 月 31 日。", 268, 0.88},
        {"Could you send me the latest quotation?",
         u8"你能把最新的报价发给我吗？", 231, 0.54},
        {"Alice will prepare the updated roadmap by next Friday.",
         u8"Alice 将在下周五前准备更新后的路线图。", 289, 0.86},
        {"The SKU count for the new listing is around 240.",
         u8"新上架的商品 SKU 数量大约是 240 个。", 255, 0.91},
        {"I think we should confirm the pricing before the launch.",
         u8"我认为我们应该在发布前确认定价。", 276, 0.83},
        {"What's the budget for the marketing campaign?",
         u8"市场推广活动的预算是多少？", 219, 0.94},
        {"Let's follow up with the supplier next week.",
         u8"我们下周跟进一下供应商。", 238, 0.87},
        {"The logistics partner confirmed the shipping schedule.",
         u8"物流合作伙伴确认了发货时间表。", 262, 0.89},
        {"Please make sure the compliance documents are ready by the end of the month.",
         u8"请确保合规文件在月底前准备好。", 297, 0.81},
        {"Great, let's wrap up here. Thanks everyone.",
         u8"好的，我们到这里。谢谢大家。", 205, 0.95},
    };

    for (const auto& d : demo) {
        store.log_segment(d.src, d.tgt, "Hunyuan", d.ms, d.conf);
    }
    store.end_session();

    const auto st = store.stats(sid);
    std::cout << "[Demo] 已写入演示会话 #" << sid << "，共 " << st.count
              << " 条，平均 " << st.avg_ms << "ms" << std::endl;
    std::cout << "[Demo] 下一步: Translator.exe --export " << sid
              << " --db " << cfg.db_path << std::endl;
    store.close();
    return 0;
}

// --dump-prompt <文本>：只加载混元模型，打印 prompt 与 tokenize 结果后退出。
// 用途：不用声卡就能验证特殊 token（<｜hy_User｜> 等）是否被正确识别。
static int run_dump_prompt(const AppConfig& cfg) {
    HunyuanTranslator hy(cfg.hunyuan_model);
    if (!hy.init()) {
        std::cerr << "[Dump] 混元模型加载失败: " << cfg.hunyuan_model << std::endl;
        return 1;
    }
    hy.set_target_language(cfg.target_lang);

    // 【必须和真实路径一致】这个命令的**唯一价值**就是"让你看到实际送出的 prompt"。
    // 实测踩过两次：
    //   ① 加了术语约束之后 --dump-prompt 仍然不显示它，因为这条路径
    //      自己创建翻译器、从没调 set_glossary()；
    //   ② 2.5 把术语来源改成"知识库为主"之后，这里如果还只读 --glossary 文件，
    //      诊断工具会**第二次**开始撒谎，而且这次更难发现 ——
    //      因为文件路径是空的，prompt 里干干净净，看起来完全正常。
    // 【判断】以后凡是往翻译 prompt 里加东西，都要回来确认这里也加上了。
    //
    // 现在直接复用主路径那个"三条腿"的取数函数，两边不可能再走散。
    SessionStore::instance().init(cfg.db_path);   // 知识库要先打开
    const auto kb = load_constraints_from_knowledge();
    std::vector<std::string> glossary = kb.terms;
    for (const auto& t : load_glossary_terms(cfg.glossary_path)) {
        bool dup = false;
        for (const auto& e : glossary) {
            if (knowledge::normalize_key(e) == knowledge::normalize_key(t)) { dup = true; break; }
        }
        if (!dup) glossary.push_back(t);
    }
    hy.set_glossary(glossary);
    std::cout << "[Dump] 翻译约束共 " << glossary.size() << " 条"
              << "（知识库 confirmed " << kb.terms.size()
              << " 条 + --glossary 文件）" << std::endl;
    if (kb.store_ready) {
        std::cout << "[Dump] 知识库 confirmed 条目 " << kb.confirmed_total
                  << " 条，摘要背景 " << kb.background.size() << " 条" << std::endl;
    }

    hy.debug_dump_prompt(cfg.dump_prompt);

    // 顺便跑一次真实翻译，验证 prompt 修复后是否还有回显
    std::cout << "\n===== 真实翻译测试 =====" << std::endl;
    std::cout << "输入: " << cfg.dump_prompt << std::endl;
    std::string out;
    if (!hy.translate_once(cfg.dump_prompt, out)) {
        std::cerr << "[Dump] 翻译失败" << std::endl;
        return 1;
    }
    std::cout << "输出: " << out << std::endl;
    std::cout << "耗时: " << hy.get_last_api_ms() << "ms" << std::endl;

    // 回显检测：输出里不该出现任何 prompt 片段
    static const std::vector<std::string> kEchoSigns = {
        u8"将以下文本翻译", "Translate the following", "You are a professional",
        u8"翻译为中文", "Simplified Chinese",
    };
    bool echoed = false;
    for (const auto& s : kEchoSigns) {
        if (out.find(s) != std::string::npos) {
            std::cerr << "[Dump] ❌ 检测到 prompt 回显片段: " << s << std::endl;
            echoed = true;
        }
    }
    if (!echoed) std::cout << "[Dump] ✅ 未检测到 prompt 回显" << std::endl;
    return echoed ? 1 : 0;
}

// --test-window：只弹出悬浮字幕窗并填入示例文字，用于快速检查外观与交互。
// 不加载模型、不采集音频，几十秒就能看出效果。
static int run_test_window(const AppConfig& cfg) {
    (void)cfg;
    SubtitleWindow w;
    if (!w.start()) {
        std::cerr << "[TestWindow] 悬浮窗创建失败" << std::endl;
        return 1;
    }

    // 按真实的"识别 → 翻译"时序播放几句，专门用来看两种效果：
    //   · 实时模式：换句时上一句译文向上滚一行，正文不出现占位文字
    //   · 回看模式：鼠标停在窗口上滚滚轮，能往回翻所有已翻过的句子
    // 所以这里要垫够句子（只垫 3 句的话滚一格就到头了，看不出效果）。
    struct Beat { const char* orig; const char* tran; };
    const Beat beats[] = {
        {"So the first thing I want to talk about is how we handle the deadline.",
         u8"首先我要讲的是我们怎么处理截止时间。"},
        {"We need to push the delivery to October 31st.",
         u8"我们需要把交付推迟到 10 月 31 日。"},
        {"That gives the firmware team two extra weeks for the audio pipeline.",
         u8"这样固件团队就多了两周时间来做音频链路。"},
        {"Could you send me the latest quotation before Friday?",
         u8"你能在周五之前把最新的报价发给我吗？"},
        {"Phoenix is our internal codename, please don't use it in the deck.",
         u8"Phoenix 是我们的内部代号，请不要写进演示文稿。"},
        {"The audio capture on Windows has to handle both loopback and microphone.",
         u8"Windows 上的音频采集必须同时处理环回和麦克风。"},
    };

    std::cout << "[TestWindow] 窗口已弹出，正在播放 "
              << (sizeof(beats) / sizeof(beats[0])) << " 句示例字幕……" << std::endl;
    std::cout << "[TestWindow] 检查要点：半透明、置顶、可拖动、不抢焦点、任务栏无图标"
              << std::endl;
    std::cout << "[TestWindow] 检查要点：换句时上一句译文向上滚一行，正文不出现占位文字"
              << std::endl;

    // 严格照抄真实时序，否则演示出来的画面和实际不一样：
    //   句子 N 的译文到达 → 只填正文，**不入历史**
    //   句子 N+1 的原文到达 → 这时才把 N 推进历史
    // 顶部行读的就是"历史里最新的一条"，所以顶部行永远是**上一句**。
    // （早先的写法是译文一到就推进历史，于是历史里最新的一条就是当前这句，
    //   顶部行和正文成了同一句话——看起来就像"上半个窗重播了一遍下半个窗"。）
    const size_t beat_count = sizeof(beats) / sizeof(beats[0]);
    for (size_t i = 0; i < beat_count; ++i) {
        if (w.quit_requested()) break;
        // 新句子顶掉正文：上一句这时才进历史
        if (i > 0) w.push_history(beats[i - 1].orig, beats[i - 1].tran);
        w.set_original(beats[i].orig);
        w.set_translation(std::string());            // 译文未就绪 → 正文留空
        w.set_status(u8"● 记录中   翻译中…   结束: Ctrl+Alt+Q");
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        if (w.quit_requested()) break;
        w.set_translation(beats[i].tran);            // 译文到达：只填正文
        w.set_status(u8"● 记录中   翻译 124ms   结束: Ctrl+Alt+Q");
        std::this_thread::sleep_for(std::chrono::milliseconds(900));
    }

    // 再留一段时间，方便从容检查外观和滚轮；按热键可立即关闭
    std::cout << "[TestWindow] 检查要点：鼠标停在窗口上滚滚轮 → 往回翻历史；"
                 "下滚到底 → 回到实时" << std::endl;
    std::cout << "[TestWindow] 检查要点：拖窗口边框/四角可以拉大拉小" << std::endl;
    std::cout << "[TestWindow] 保持 40 秒供检查；按 "
              << SubtitleWindow::hotkey_hint() << " 可立即关闭。" << std::endl;
    for (int i = 0; i < 400 && !w.quit_requested(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    w.stop();
    std::cout << "[TestWindow] 已关闭" << std::endl;
    return 0;
}

// --selftest：不加载任何模型，只验证数据层通路
// （建会话 → 写入段落 → 读回 → 统计 → 结束 → 列出历史）。
// 项目目前没有测试框架，这个是可长期保留的回归自检入口。
static int run_selftest(const AppConfig& cfg) {
    std::cout << "[SelfTest] 数据层自检开始（不需要模型）" << std::endl;

    // ---- 1) 重叠去重逻辑 ----
    // 用例全部取自真实会话日志。
    struct DedupCase { const char* prev; const char* cur; const char* want; };
    const DedupCase dedup_cases[] = {
        // 标点差异："about." 与 "about" 必须视为同一个词
        {"to give you some real English to talk about.",
         "about travel. Exactly. English that you would use",
         "travel. Exactly. English that you would use"},
        // 多一个功能词 "to"：允许本段开头跳过 0~2 个词
        {"about travel. Exactly. English that you would use",
         "to use in everyday life that you hear in movies andTV",
         "in everyday life that you hear in movies andTV"},
        // 标点差异 + 多词重叠
        {"exactly so why don't you tell us a little bit about this lesson for today",
         "for today. Okay, so today we are going to",
         "Okay, so today we are going to"},
        // 多一个功能词 "to"（另一处）
        {"talking about travel that's right we're gonna give you",
         "to give you some real English to talk about.",
         "some real English to talk about."},
        // 早期用例：真实重复
        {"Erika. How are you, Erika? Marco, I'm doing really well.",
         "Marco, I'm doing really well. How about you? I'm doing great.",
         "How about you? I'm doing great."},
        // 无重叠：必须原样返回
        {"Hello world", "Completely different text", "Completely different text"},
    };

    bool dedup_ok = true;
    int dedup_pass = 0;
    for (const auto& c : dedup_cases) {
        const std::string got = strip_overlap(c.prev, c.cur);
        if (got == c.want) { ++dedup_pass; continue; }
        dedup_ok = false;
        std::cerr << "[SelfTest] 去重失败\n"
                  << "    上一段: " << c.prev << "\n"
                  << "    本  段: " << c.cur << "\n"
                  << "    期望  : " << c.want << "\n"
                  << "    实际  : " << got << std::endl;
    }
    std::cout << "[SelfTest] 重叠去重: " << (dedup_ok ? "✅ " : "❌ ")
              << dedup_pass << "/" << (sizeof(dedup_cases) / sizeof(dedup_cases[0]))
              << " 通过" << std::endl;
    if (!dedup_ok) return 1;

    // ---- 2) 段内重复折叠 ----
    // Whisper 自己的退化循环，例如 "How are you Erikka?" 连说两遍。
    struct RepeatCase { const char* in; const char* want; };
    const RepeatCase repeat_cases[] = {
        {"How are you Erikka?  How are you Erikka?  Marko, I'm doing really well.",
         "How are you Erikka? Marko, I'm doing really well."},
        {"I give up on doing things my way I give up on doing things my way",
         "I give up on doing things my way"},
        {"Hello world", "Hello world"},   // 无重复，必须原样
    };

    bool repeat_ok = true;
    int repeat_pass = 0;
    for (const auto& c : repeat_cases) {
        const std::string got = collapse_repeats(c.in);
        if (got == c.want) { ++repeat_pass; continue; }
        repeat_ok = false;
        std::cerr << "[SelfTest] 重复折叠失败\n"
                  << "    输入: " << c.in << "\n"
                  << "    期望: " << c.want << "\n"
                  << "    实际: " << got << std::endl;
    }
    std::cout << "[SelfTest] 段内重复折叠: " << (repeat_ok ? "✅ " : "❌ ")
              << repeat_pass << "/" << (sizeof(repeat_cases) / sizeof(repeat_cases[0]))
              << " 通过" << std::endl;
    if (!repeat_ok) return 1;

    // ---- 3) 术语纠错 ----
    // 用例取自真实会话里出现过的误识别。
    {
        TermFixer tf;
        tf.set_terms({"Marko", "Erika", "EnglishPod", "Phoenix"});

        struct FixCase { const char* in; const char* want; };
        const FixCase fix_cases[] = {
            // 实测里同一场会议出现了四种拼法
            {"Hello, I'm Erikka. How are you, Erikka?",
             "Hello, I'm Erika. How are you, Erika?"},
            {"Marco, I'm doing really well.",
             "Marko, I'm doing really well."},
            // 标点必须保留
            {"Thanks, Marco.", "Thanks, Marko."},
            // 已经正确的不动
            {"Phoenix is our codename.", "Phoenix is our codename."},
            // 距离 2，不该改（Erin 本身是真实存在的名字）
            {"My name is Erin.", "My name is Erin."},
            // 小写动词不该被改成人名
            {"Please mark the checkbox.", "Please mark the checkbox."},
            // 无术语命中
            {"This has nothing to fix.", "This has nothing to fix."},
        };

        bool fix_ok = true;
        int  fix_pass = 0;
        for (const auto& c : fix_cases) {
            const std::string got = tf.fix(c.in);
            if (got == c.want) { ++fix_pass; continue; }
            fix_ok = false;
            std::cerr << "[SelfTest] 术语纠错失败\n"
                      << "    输入: " << c.in << "\n"
                      << "    期望: " << c.want << "\n"
                      << "    实际: " << got << std::endl;
        }
        std::cout << "[SelfTest] 术语纠错: " << (fix_ok ? "✅ " : "❌ ")
                  << fix_pass << "/" << (sizeof(fix_cases) / sizeof(fix_cases[0]))
                  << " 通过" << std::endl;
        if (!fix_ok) return 1;
    }

    // ---- 4) 幻觉过滤（术语提示回显 + 跨段重复）----
    // 两个用例都来自真实日志。
    {
        const std::string prompt = "EnglishPod, Phoenix, SKU, Q4, roadmap";
        struct EchoCase { bool want; const char* text; };
        const EchoCase echo_cases[] = {
            // 实测第 1 段就是这张词表被原样吐出，置信度还高达 0.99
            {true,  "EnglishPod, Phoenix, SKU, Q4, roadmap"},
            // 正常讲话撞上一两个术语，不该判为回显
            {false, "We need to talk about the Phoenix project status."},
            {false, "Let me show you the roadmap for next quarter."},
        };

        bool echo_ok = true;
        int  echo_pass = 0;
        for (const auto& c : echo_cases) {
            const bool got = SpeechEngine::looks_like_prompt_echo(c.text, prompt);
            if (got == c.want) { ++echo_pass; continue; }
            echo_ok = false;
            std::cerr << "[SelfTest] 术语提示回显判定失败\n"
                      << "    文本: " << c.text << "\n"
                      << "    期望: " << (c.want ? "回显" : "正常") << "\n"
                      << "    实际: " << (got ? "回显" : "正常") << std::endl;
        }
        std::cout << "[SelfTest] 术语提示回显: " << (echo_ok ? "✅ " : "❌ ")
                  << echo_pass << "/" << (sizeof(echo_cases) / sizeof(echo_cases[0]))
                  << " 通过" << std::endl;
        if (!echo_ok) return 1;

        // 跨段重复：实测 "and more." 在 23 段里出现 6 次、置信度高达 0.998
        struct RepCase { bool want; std::vector<std::string> window; const char* text; };
        const RepCase rep_cases[] = {
            {false, {},                     "and more."},
            {false, {"andmore"},            "and more."},
            {true,  {"andmore", "andmore"}, "and more."},
            // 太短的不做判定（"you" 这类可能真实重复）
            {false, {"you", "you", "you"},  "you"},
            // 太长的整句即使重复也更可能是真实内容
            {false, {"rafthasthreestatesfollowercandidateandleaderandtheelectiontimeoutisrandomized",
                     "rafthasthreestatesfollowercandidateandleaderandtheelectiontimeoutisrandomized"},
                    "Raft has three states Follower Candidate and Leader and the election timeout is randomized"},
        };

        bool rep_ok = true;
        int  rep_pass = 0;
        for (const auto& c : rep_cases) {
            const bool got = SpeechEngine::looks_repetitive(c.window, c.text);
            if (got == c.want) { ++rep_pass; continue; }
            rep_ok = false;
            std::cerr << "[SelfTest] 跨段重复判定失败\n"
                      << "    文本: " << c.text << "\n"
                      << "    期望: " << (c.want ? "重复" : "正常") << "\n"
                      << "    实际: " << (got ? "重复" : "正常") << std::endl;
        }
        std::cout << "[SelfTest] 跨段重复抑制: " << (rep_ok ? "✅ " : "❌ ")
                  << rep_pass << "/" << (sizeof(rep_cases) / sizeof(rep_cases[0]))
                  << " 通过" << std::endl;
        if (!rep_ok) return 1;
    }

    // ---- 5) 行动项可信度校验 ----
    // 第 1 条用例是**实测会话 #11 里真实出现的假作业**，不是编的。
    {
        struct ActCase { bool keep; const char* task; const char* due; const char* source; };
        const ActCase act_cases[] = {
            // ① 实测假条目：过场语 + 无动作词（触发词表里的"应该"命中了它）
            {false, u8"那么，您应该向我们介绍一下今天这堂课的内容。",
                    "today", "So, you should tell us a little bit about this lesson for today."},
            // ② 真待办：含"推迟"
            {true,  u8"我们需要把交付推迟到 10 月 31 日。",
                    "10 月 31 日", "We need to push the delivery to October 31st."},
            // ③ 问句形式但确实是待办（含"发给"），不能因为是问句就杀
            {true,  u8"你能在周五之前把最新的报价发给我吗？",
                    "周五", "Could you send me the latest quotation before Friday?"},
            // ④ 英文真待办
            {true,  "Please send me the latest quotation before Friday.",
                    "friday", "Please send me the latest quotation before Friday."},
            // ⑤ 主观评价，不是待办
            {false, u8"听起来有点复杂。",
                    "", "That sounds a little bit complicated."},
            // ⑥ 寒暄
            {false, u8"感谢大家参加今天的评审。",
                    "", "Thank you all for joining today's review."},
            // ⑦ 太短
            {false, u8"好的。", "", "Okay."},
            // ⑧ 词边界回归：裸 find("test") 会命中 "latest"，
            //    导致这句纯陈述句通过"必须有动作词"这一关（code review 发现）
            {false, "The latest results are in.", "", "The latest results are in."},
            // ⑨ 同理 "fix" 不能命中 "prefix"
            {false, "Use the prefix here.", "", "Use the prefix here."},
        };

        bool act_ok = true;
        int  act_pass = 0;
        for (const auto& c : act_cases) {
            std::vector<ActionItem> v;
            ActionItem a;
            a.task   = c.task;
            a.due    = c.due;
            a.source = c.source;
            v.push_back(a);

            DeliverableWriter::sanitize_actions(v);
            const bool kept = !v.empty();
            if (kept != c.keep) {
                act_ok = false;
                std::cerr << "[SelfTest] 行动项校验失败\n"
                          << "    任务: " << c.task << "\n"
                          << "    期望: " << (c.keep ? "保留" : "丢弃") << "\n"
                          << "    实际: " << (kept ? "保留" : "丢弃") << std::endl;
                continue;
            }
            ++act_pass;
        }
        std::cout << "[SelfTest] 行动项可信度校验: " << (act_ok ? "✅ " : "❌ ")
                  << act_pass << "/" << (sizeof(act_cases) / sizeof(act_cases[0]))
                  << " 通过" << std::endl;
        if (!act_ok) return 1;

        // 单独验"截止日期必须有依据"这一段。
        //
        // ⚠️ 用例必须反映**真实数据形状**。上一版的用例是我手工配好 source 的，
        // 而云端路径的 ActionItem.source 实际是**空的**（中文任务文本 vs 英文转录，
        // link_actions_to_segments 的 LCS 匹配不上），于是真实运行仍然清空正确日期，
        // 自检却全绿 —— 这就是"测试数据和真实路径分叉"，和 §8.8② 是同一类错误。
        {
            struct DueCase {
                const char* name;
                const char* task;
                const char* due;
                const char* source;          // 云端路径这里会是空串
                bool        has_transcript;  // 是否传整场转录
                bool        keep_due;
            };
            const DueCase due_cases[] = {
                // ① 【真实形状】云端路径：task 中文、source 空、日期在转录里
                //    → 必须保留（修的就是这条）
                {"云端形状/日期在转录里", u8"准备更新后的路线图", u8"下周五",
                 "", true, true},
                // ② 【真实形状】source 空、转录里也没有这个日期 → 真无依据，清空
                {"云端形状/转录里也没有", u8"准备更新后的路线图", u8"下周三",
                 "", true, false},
                // ③ 规则路径形状：source 有，日期在里面 → 保留
                {"规则形状/日期在 source", u8"准备更新后的路线图", u8"下周五",
                 "Alice will prepare the updated roadmap by next Friday.", false, true},
                // ④ 无 source、无转录 = **无法判断**，不能默认判负
                {"无依据可查时不判负", u8"准备更新后的路线图", u8"下周五",
                 "", false, true},
                // ⑤ 具体数字日期不判定
                {"数字日期不判定", u8"我们需要把交付推迟到 10 月 31 日。", u8"10 月 31 日",
                 "We need to push the delivery to October 31st.", false, true},
            };

            bool due_ok = true;
            int  due_pass = 0;
            for (const auto& c : due_cases) {
                std::vector<ActionItem> v;
                ActionItem a;
                a.task   = c.task;
                a.due    = c.due;
                a.source = c.source;
                v.push_back(a);

                // 造一场转录，里面含 "by next Friday"
                std::vector<Segment> tr;
                if (c.has_transcript) {
                    Segment s1;
                    s1.src_text = "Alice will prepare the updated roadmap by next Friday.";
                    s1.tgt_text = u8"Alice 将在下周五前准备更新后的路线图。";
                    tr.push_back(s1);
                    Segment s2;
                    s2.src_text = "Thanks everyone, let's wrap up.";
                    s2.tgt_text = u8"谢谢大家，今天就到这里。";
                    tr.push_back(s2);
                }

                DeliverableWriter::sanitize_actions(v, nullptr, c.has_transcript ? &tr : nullptr);

                const bool kept_due = (v.size() == 1) && !v[0].due.empty();
                if (kept_due == c.keep_due) { ++due_pass; continue; }
                due_ok = false;
                std::cerr << "[SelfTest] 截止日期依据判定失败: " << c.name
                          << "（期望 " << (c.keep_due ? "保留" : "清空") << "，实际 "
                          << (kept_due ? "保留" : "清空") << "）" << std::endl;
            }
            std::cout << "[SelfTest] 截止日期依据（真实数据形状）: " << (due_ok ? "✅ " : "❌ ")
                      << due_pass << "/" << (sizeof(due_cases) / sizeof(due_cases[0]))
                      << " 通过" << std::endl;
            if (!due_ok) return 1;
        }

        // 单独验"write() 自己会兜底校验"：直接把假条目塞进 summary 交给 write()，
        // 出来的 actions.csv 里必须没有它。防的是"以后有人绕过 main.cpp 直接调 write()"。
        {
            MeetingSummary dirty;
            dirty.content_type = u8"课程";
            ActionItem bad;
            bad.task   = u8"那么，您应该向我们介绍一下今天这堂课的内容。";
            bad.due    = "today";
            bad.source = "So, you should tell us a little bit about this lesson for today.";
            dirty.actions.push_back(bad);

            namespace tfs = std::filesystem;
            const tfs::path tmp = tfs::current_path() / "_selftest_write";
            std::error_code ec;
            tfs::remove_all(tmp, ec);

            const auto wr = DeliverableWriter::write(999999, {}, SessionMeta{}, dirty, tmp.string());
            std::string csv;
            if (wr.ok) {
                std::ifstream f(tfs::path(wr.dir) / "actions.csv", std::ios::binary);
                csv.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            }
            tfs::remove_all(tmp, ec);

            const bool ok = wr.ok && csv.find(u8"您应该") == std::string::npos;
            std::cout << "[SelfTest] write() 兜底校验（假条目不得落盘）: "
                      << (ok ? "✅ 通过" : "❌ 失败") << std::endl;
            if (!ok) {
                std::cerr << "    write.ok=" << wr.ok << " err=" << wr.error
                          << "\n    csv 内容: " << csv << std::endl;
                return 1;
            }
        }
    }

    // ---- 6) WAV 读取（--wav 测试入口的输入层）----
    // 用例现场造 WAV 字节：不依赖磁盘文件，也不依赖模型，秒级可跑。
    {
        std::vector<float> sine;
        sine.reserve(16000);
        for (int i = 0; i < 16000; ++i) {
            sine.push_back(0.5f * std::sin(2.0 * 3.14159265358979 * 440.0 * i / 16000.0));
        }

        // ① 16k 单声道：不做重采样，长度应原样
        {
            const auto bytes = WavReader::make_test_wav(sine, 16000, 1);
            const auto r = WavReader::parse(bytes, 16000);
            const bool ok = r.ok && r.samples.size() == sine.size() &&
                            r.src_rate == 16000 && r.src_channels == 1 && r.src_bits == 16;
            std::cout << "[SelfTest] WAV 16k 单声道直通: " << (ok ? "✅ 通过" : "❌ 失败");
            if (!ok) std::cout << "  ok=" << r.ok << " err=" << r.error
                               << " len=" << r.samples.size() << " 期望=" << sine.size();
            std::cout << std::endl;
            if (!ok) return 1;
        }

        // ② 立体声混单声道：长度不变（每帧取平均，不是相加——相加会削顶）
        {
            const auto bytes = WavReader::make_test_wav(sine, 16000, 2);
            const auto r = WavReader::parse(bytes, 16000);
            const bool ok = r.ok && r.samples.size() == sine.size() && r.src_channels == 2;
            std::cout << "[SelfTest] WAV 立体声混单声道: " << (ok ? "✅ 通过" : "❌ 失败");
            if (!ok) std::cout << "  ok=" << r.ok << " err=" << r.error
                               << " len=" << r.samples.size() << " 期望=" << sine.size();
            std::cout << std::endl;
            if (!ok) return 1;
        }

        // ③ 32k 源 → 16k：长度应减半。这条专门盯"忘了重采样"——
        //    da97d96 那份 wav_reader.h 就收下了 expected_rate 却从不使用。
        {
            std::vector<float> sine32k;
            sine32k.reserve(32000);
            for (int i = 0; i < 32000; ++i) {
                sine32k.push_back(0.5f * std::sin(2.0 * 3.14159265358979 * 440.0 * i / 32000.0));
            }
            const auto bytes = WavReader::make_test_wav(sine32k, 32000, 1);
            const auto r = WavReader::parse(bytes, 16000);
            const size_t want = 16000;
            const bool ok = r.ok && r.samples.size() == want && r.src_rate == 32000;
            std::cout << "[SelfTest] WAV 32k→16k 重采样: " << (ok ? "✅ 通过" : "❌ 失败");
            if (!ok) std::cout << "  ok=" << r.ok << " err=" << r.error
                               << " len=" << r.samples.size() << " 期望=" << want;
            std::cout << std::endl;
            if (!ok) return 1;
        }

        // ④ 错误路径必须**报错**，不能静默返回空样本
        {
            std::vector<std::pair<std::string, std::vector<unsigned char>>> errs;
            errs.emplace_back("非 RIFF", std::vector<unsigned char>(100, 'x'));
            errs.emplace_back("文件过短", std::vector<unsigned char>(10, 0));
            auto truncated = WavReader::make_test_wav(sine, 16000, 1);
            truncated.resize(20);
            errs.emplace_back("头部被截断", std::move(truncated));

            bool err_ok = true;
            for (const auto& c : errs) {
                const auto r = WavReader::parse(c.second, 16000);
                if (r.ok || r.error.empty()) {
                    err_ok = false;
                    std::cerr << "[SelfTest] WAV 错误路径失败: " << c.first
                              << " 应报错，实际 ok=" << r.ok << std::endl;
                }
            }
            std::cout << "[SelfTest] WAV 错误路径要报错(不静默): " << (err_ok ? "✅ " : "❌ ")
                      << errs.size() << " 例" << std::endl;
            if (!err_ok) return 1;
        }
    }

    // ---- 7) 翻译后端选择（纯函数，取代了启动时那道选择题）----
    {
        struct TCase { const char* name; const char* translator; const char* key; int want; };
        const TCase t_cases[] = {
            // 默认：不指定就是本地（离线、免费、数据不出本机）
            {"默认",                "local", "",     2},
            {"显式 local",          "local", "sk-x", 2},
            // 关键用例：**给了 key 也不能自动走云端翻译** ——
            // 用户可能只让"摘要"上云，翻译必须留本机
            {"有 key 但仍默认本地",  "local", "sk-x", 2},
            {"显式 cloud",          "cloud", "sk-x", 1},
            // cloud 但没 key：回退本地，而不是崩掉或原地死循环
            {"cloud 无 key",        "cloud", "",     2},
            // 拼错的取值：按 local 处理并给出原因，不要静默
            {"拼错的值",            "clud",  "sk-x", 2},
        };

        bool t_ok = true;
        int  t_pass = 0;
        for (const auto& c : t_cases) {
            AppConfig tmp;
            tmp.translator       = c.translator;
            tmp.deepseek_api_key = c.key;
            std::string why;
            const int got = choose_translator_backend(tmp, why);
            if (got == c.want && !why.empty()) { ++t_pass; continue; }
            t_ok = false;
            std::cerr << "[SelfTest] 翻译后端选择失败: " << c.name
                      << " 期望=" << c.want << " 实际=" << got
                      << " why为空=" << why.empty() << std::endl;
        }
        std::cout << "[SelfTest] 翻译后端选择规则: " << (t_ok ? "✅ " : "❌ ")
                  << t_pass << "/" << (sizeof(t_cases) / sizeof(t_cases[0]))
                  << " 通过" << std::endl;
        if (!t_ok) return 1;
    }

    // ---- 8) 术语约束（让译文里的专名保持同一写法）----
    {
        // 空表：不产生约束文本（拼上去是空串，不能凭空多一个换行）
        const std::string empty_c = ITranslator::glossary_constraint({});
        const bool c1 = empty_c.empty();

        // 正常：每个术语都要出现，并且明确写了"不要音译"
        const std::vector<std::string> terms = {"Marco", "Erica", "EnglishPod"};
        const std::string c = ITranslator::glossary_constraint(terms);
        const bool c2 = c.find("Marco") != std::string::npos &&
                        c.find("Erica") != std::string::npos &&
                        c.find("EnglishPod") != std::string::npos &&
                        c.find("transliterate") != std::string::npos;

        // 超长条目要被跳过（那多半不是专名，而是误入的词组）
        const std::vector<std::string> with_junk = {
            "Marco",
            "this is a whole sentence that accidentally ended up in the glossary list"
        };
        const std::string cj = ITranslator::glossary_constraint(with_junk);
        const bool c3 = cj.find("Marco") != std::string::npos &&
                        cj.find("accidentally") == std::string::npos;

        // 条数上限：不能把提示词撑爆
        std::vector<std::string> many;
        for (int i = 0; i < 100; ++i) many.push_back("Term" + std::to_string(i));
        const std::string cm = ITranslator::glossary_constraint(many);
        const bool c4 = cm.find("Term0") != std::string::npos &&
                        cm.find("Term50") == std::string::npos;

        const bool c_ok = c1 && c2 && c3 && c4;
        std::cout << "[SelfTest] 术语约束文本: " << (c_ok ? "✅ " : "❌ ")
                  << "空表/正常/超长剔除/条数上限 4 例" << std::endl;
        if (!c_ok) {
            std::cerr << "    空表='" << empty_c << "'\n"
                      << "    正常='" << c << "'\n"
                      << "    含垃圾='" << cj << "'\n"
                      << "    超量长度=" << cm.size() << std::endl;
            return 1;
        }

        // 术语约束必须**按段过滤**（只带这段里真出现过的术语）。
        //
        // 【为什么这条是硬需求，不是优化】实测对照实验（同一段原文，只改知识库）：
        //   `and wife get ready to go`  真值里没有任何专名
        //     无约束 → 「妻子也准备出发了」        ✅
        //     有约束 → 「埃丽卡和马可准备出发了」  ❌ 凭空编了两个名字
        //   `Marco, I'm doing really well. How about you?`
        //     无约束 → 「Marco，我过得很好。你呢？」 ✅
        //     有约束 → 「Erica, I'm doing really well. …」 ❌ 整句没翻译
        // 弱翻译模型看到"这几个词必须原样出现"、又发现本段没有，就会给它补上。
        {
            const std::vector<std::string> gterms = {"Erica", "Marco", "EnglishPod", "TV"};
            struct Fc { const char* text; size_t want; };
            const Fc fc[] = {
                {"How are you, Erica? Marco, I'm doing really well.", 2},  // Erica + Marco
                {"and wife get ready to go",                          0},  // 一个都不该带
                {"welcome to EnglishPod",                             1},
                {"watch it on tv",                                    1},  // 大小写不敏感
                {"erica and marco",                                   2},  // 全小写也要命中
                {"",                                                  0},
            };
            bool fc_ok = true;
            std::string why4;
            int fc_pass = 0;
            for (const auto& c : fc) {
                const auto got = ITranslator::glossary_for_text(gterms, c.text);
                if (got.size() == c.want) { ++fc_pass; continue; }
                fc_ok = false;
                why4 = std::string("按段过滤不对: '") + c.text + "' 期望 " +
                       std::to_string(c.want) + " 个，实际 " + std::to_string(got.size());
            }
            std::cout << "[SelfTest] 术语约束按段过滤（防凭空编造专名）: "
                      << (fc_ok ? "✅ " : "❌ ") << fc_pass << "/"
                      << (sizeof(fc) / sizeof(fc[0])) << " 通过" << std::endl;
            if (!fc_ok) {
                std::cerr << "    " << why4 << std::endl;
                return 1;
            }
        }

        // ---- UTF-8 净化（交付物写出边界 + 摘要解析前）----
        //
        // 【用例的来历】`meeting-42.md` 是**真实产物**：用 `--summarizer local`（混元）
        // 生成的整份文件不是合法 UTF-8，字节证据是 `### 询问你\xe7`
        // —— `\xe7` 是个没写完的三字节首字节，下面一行还有孤立的续字节 `\x84`。
        // 所以第一个用例直接抄那两个真实字节，而不是我编一个"看起来非法"的串。
        {
            bool u_ok = true;
            std::string why5;

            // ① 真实事故字节：截断的三字节首字节 + 孤立续字节
            {
                const std::string bad = "### \xe8\xaf\xa2\xe9\x97\xae\xe4\xbd\xa0\xe7"
                                        "\n- \x84\xe5\x9b\xbd\xe5\xae\xb6";
                if (utf8::is_valid(bad)) { u_ok = false; why5 = "真实事故字节被判成合法了"; }
                size_t n = 0;
                const std::string fixed = utf8::sanitize(bad, &n);
                if (u_ok) {
                    if (n != 2) { u_ok = false; why5 = "应替换 2 处，实际 " + std::to_string(n); }
                    else if (!utf8::is_valid(fixed)) { u_ok = false; why5 = "净化后仍不合法"; }
                }
            }

            // ② 合法输入必须**逐字节不变**
            {
                const std::string good = u8"正常的 UTF-8：中文、English、emoji 😀、"
                                         u8"「引号」、ASCII abc123";
                if (u_ok && !utf8::is_valid(good)) { u_ok = false; why5 = "合法串被判成非法"; }
                size_t n = 0;
                const std::string same = utf8::sanitize(good, &n);
                if (u_ok && (n != 0 || same != good)) {
                    u_ok = false; why5 = "合法输入被改动了（应逐字节不变）";
                }
            }

            // ③ 严格性：这些都必须判**非法**。宽松写法会全部放过它们。
            {
                struct Uc { const char* name; std::string bytes; };
                const Uc cases[] = {
                    {u8"过长编码（2 字节表示 'A'）",  std::string("\xC1\x81", 2)},
                    {u8"过长编码（3 字节表示 '/'）",  std::string("\xE0\x80\xAF", 3)},
                    {u8"过长的 4 字节",               std::string("\xF0\x80\x80\xAF", 4)},
                    {u8"UTF-16 代理对（U+D800）",     std::string("\xED\xA0\x80", 3)},
                    {u8"超出 U+10FFFF",               std::string("\xF4\x90\x80\x80", 4)},
                    {u8"孤立续字节",                  std::string("\x80", 1)},
                    {u8"被截断的 2 字节序列",         std::string("\xC3", 1)},
                    {u8"被截断的 3 字节序列",         std::string("\xE4\xB8", 2)},
                    {u8"0xFF 不是合法首字节",         std::string("\xFF", 1)},
                };
                for (const auto& c : cases) {
                    if (!u_ok) break;
                    if (utf8::is_valid(c.bytes)) {
                        u_ok = false;
                        why5 = std::string("应判非法却放过了：") + c.name;
                    }
                }
            }

            // ④ 边界码点必须放过
            {
                struct Vc { const char* name; std::string bytes; };
                const Vc cases[] = {
                    {u8"U+0000",   std::string("\x00", 1)},
                    {u8"U+007F",   std::string("\x7F", 1)},
                    {u8"U+0080",   std::string("\xC2\x80", 2)},
                    {u8"U+07FF",   std::string("\xDF\xBF", 2)},
                    {u8"U+0800",   std::string("\xE0\xA0\x80", 3)},
                    {u8"U+D7FF",   std::string("\xED\x9F\xBF", 3)},
                    {u8"U+E000",   std::string("\xEE\x80\x80", 3)},
                    {u8"U+FFFF",   std::string("\xEF\xBF\xBF", 3)},
                    {u8"U+10000",  std::string("\xF0\x90\x80\x80", 4)},
                    {u8"U+10FFFF", std::string("\xF4\x8F\xBF\xBF", 4)},
                };
                for (const auto& c : cases) {
                    if (!u_ok) break;
                    if (!utf8::is_valid(c.bytes)) {
                        u_ok = false;
                        why5 = std::string("应判合法却拒了：") + c.name;
                    }
                }
            }

            // ⑤ 坏字节不能把后面的好字节一起吞掉
            {
                const std::string mix = std::string("A") + "\xE7" + "B" + "\x84" + "C";
                size_t n = 0;
                const std::string fixed = utf8::sanitize(mix, &n);
                if (u_ok) {
                    if (n != 2) { u_ok = false; why5 = "应替换 2 处"; }
                    else if (fixed.find('A') == std::string::npos ||
                             fixed.find('B') == std::string::npos ||
                             fixed.find('C') == std::string::npos) {
                        u_ok = false; why5 = "净化把好字节一起吞了";
                    }
                }
            }

            std::cout << "[SelfTest] UTF-8 净化（严格判定/合法不动/坏字节不吞好字节）: "
                      << (u_ok ? "✅ 通过" : "❌ 失败") << std::endl;
            if (!u_ok) {
                std::cerr << "    " << why5 << std::endl;
                return 1;
            }

            // ---- 多字节分隔符不能被按单字节比较 ----
            //
            // 【这是 meeting-42.md 真正的病根，比上面那组更要紧】
            // 原代码：`item.find_first_of(u8"：:")` —— 按**单字节**比较，
            // 而 u8"：" 是 EF BC 9A，于是它在找「0xEF 或 0xBC 或 0x9A 或 ':'」。
            // **「的」= E7 9A 84，第二个字节就是 0x9A** → 被当成冒号：
            //     "询问你的想法：要点" 里，它找到位置 10（「的」中间），真正的冒号在 18
            //     substr(0,10) = "询问你" + 0xE7  ← 切在字符中间
            // 结果整份交付物不再是合法 UTF-8。
            //
            // 【为什么这一条必须用「的」当用例】0x9A / 0xBC 是**极常见**的汉字续字节
            // （的/地/得/等…），所以这个 bug 在实际输出里几乎必然命中 ——
            // 用别的字测反而可能测不出来。
            {
                bool mb_ok = true;
                std::string why6;

                LlmSummarizer sm(LlmSummarizer::Backend::Local);
                sm.set_target_language("zh");

                // 本地后端的结构化纯文本格式
                const std::string reply =
                    u8"【类型】\n会议\n\n"
                    u8"【概述】\n一段概述。\n\n"
                    u8"【要点】\n"
                    u8"- 询问你的想法：要点一；要点二\n"
                    u8"- 讨论的进展：第三点\n\n"
                    u8"【行动项】\n- 无\n";

                MeetingSummary sum;
                std::string perr;
                if (!sm.parse_reply(reply, sum, perr)) {
                    mb_ok = false; why6 = "解析失败: " + perr;
                } else {
                    if (sum.topics.size() != 2) {
                        mb_ok = false;
                        why6 = "应解析出 2 个主题，实际 " + std::to_string(sum.topics.size());
                    } else {
                        // 标题必须完整，不能被切在「的」中间
                        if (sum.topics[0].title != u8"询问你的想法") {
                            mb_ok = false;
                            why6 = u8"标题被切坏了：'" + sum.topics[0].title +
                                   u8"'（应为「询问你的想法」）";
                        } else if (sum.topics[0].points.size() != 2 ||
                                   sum.topics[0].points[0] != u8"要点一" ||
                                   sum.topics[0].points[1] != u8"要点二") {
                            mb_ok = false;
                            why6 = u8"要点切分不对（全角冒号应只占一个分隔位）";
                        } else if (sum.topics[1].title != u8"讨论的进展") {
                            mb_ok = false;
                            why6 = u8"第二行标题也错了：'" + sum.topics[1].title + u8"'";
                        }
                    }
                    // 解析出来的每个字段都必须是合法 UTF-8
                    if (mb_ok) {
                        for (const auto& t : sum.topics) {
                            if (!utf8::is_valid(t.title)) {
                                mb_ok = false; why6 = "解析出的标题不是合法 UTF-8"; break;
                            }
                            for (const auto& pt : t.points) {
                                if (!utf8::is_valid(pt)) {
                                    mb_ok = false; why6 = "解析出的要点不是合法 UTF-8"; break;
                                }
                            }
                        }
                    }
                }
                // 半角冒号也得能切（模型偶尔吐 ASCII 冒号）
                if (mb_ok) {
                    const std::string reply2 =
                        u8"【要点】\n- Ask about the plan: point one; point two\n";
                    MeetingSummary s2;
                    std::string e2;
                    if (!sm.parse_reply(reply2, s2, e2) || s2.topics.size() != 1 ||
                        s2.topics[0].title != "Ask about the plan") {
                        mb_ok = false;
                        why6 = "半角冒号的分隔没处理对";
                    }
                }

                std::cout << "[SelfTest] 多字节分隔符（「的」不能被当成全角冒号）: "
                          << (mb_ok ? "✅ 通过" : "❌ 失败") << std::endl;
                if (!mb_ok) {
                    std::cerr << "    " << why6 << std::endl;
                    return 1;
                }
            }
        }
    }

    auto& store = SessionStore::instance();
    if (!store.init(cfg.db_path)) {
        std::cerr << "[SelfTest] init 失败" << std::endl;
        return 1;
    }

    const long long sid = store.begin_session("SelfTest", "自检会话");
    if (sid < 0) {
        std::cerr << "[SelfTest] begin_session 失败" << std::endl;
        return 1;
    }
    std::cout << "[SelfTest] 会话已开启 session_id=" << sid << std::endl;

    store.log_segment("We need to push the delivery to October 31st.",
                      "我们需要把交付推迟到 10 月 31 日。", "SelfTest", 240, 0.88);
    store.log_segment("Could you send me the latest quotation?",
                      "你能把最新的报价发给我吗？", "SelfTest", 310, 0.54);
    store.log_segment("Phoenix is our internal codename.",
                      "Phoenix 是我们的内部代号。", "SelfTest", 198, 0.91);

    const auto segs = store.fetch_segments(sid);
    std::cout << "[SelfTest] 读回 " << segs.size() << " 条：" << std::endl;
    for (const auto& s : segs) {
        std::cout << "   #" << s.seq << " [" << s.engine << " " << s.ms << "ms conf="
                  << s.confidence << "] " << s.src_text << " => " << s.tgt_text << std::endl;
    }
    if (segs.size() != 3) {
        std::cerr << "[SelfTest] 期望 3 条，实际 " << segs.size() << " 条 ❌" << std::endl;
        return 1;
    }

    const auto st = store.stats(sid);
    std::cout << "[SelfTest] 统计: count=" << st.count
              << " total=" << st.total_ms << "ms avg=" << st.avg_ms << "ms" << std::endl;

    store.end_session();

    // ---- 9) 长期记忆：三张表 + FTS5（§7 步骤 2.1 的验收）----
    {
        const bool mem_ok = SessionStore::instance().long_term_memory_ready();
        std::cout << "[SelfTest] 长期记忆表 + FTS5: " << (mem_ok ? "✅ 通过" : "❌ 失败")
                  << "（knowledge / knowledge_history / actions / knowledge_fts）" << std::endl;
        if (!mem_ok) {
            std::cerr << "    提示：FTS5 需要 CMakeLists.txt 里的 SQLITE_ENABLE_FTS5；"
                         "若刚改过该行，务必重新 configure（只 build 不会生效）" << std::endl;
            return 1;
        }
    }

    // ---- 10) 长期记忆：纯规则（归一化 + §6.5 红线）----
    {
        // 归一化：同一个专名的不同写法必须落到同一个键
        struct NormCase { const char* in; const char* want; };
        const NormCase norm_cases[] = {
            {"Erika",              "erika"},
            {"  ERIKA  ",          "erika"},
            {"Erika.",             "erika"},
            // 全角字母 → 半角：识别结果里偶尔出现全角
            {u8"Ｅｒｉｋａ",       "erika"},
            {"Q4, 2024",           "q4 2024"},
            {"English   Pod",      "english pod"},
            // 中文不该被插空格、不该被拆
            {u8"埃里卡",           u8"埃里卡"},
            {u8"埃里卡。",         u8"埃里卡"},
        };

        bool norm_ok = true;
        int  norm_pass = 0;
        for (const auto& c : norm_cases) {
            const std::string got = knowledge::normalize_key(c.in);
            if (got == c.want) { ++norm_pass; continue; }
            norm_ok = false;
            std::cerr << "[SelfTest] 键归一化失败: '" << c.in << "' 期望 '" << c.want
                      << "' 实际 '" << got << "'" << std::endl;
        }
        std::cout << "[SelfTest] 知识键归一化: " << (norm_ok ? "✅ " : "❌ ")
                  << norm_pass << "/" << (sizeof(norm_cases) / sizeof(norm_cases[0]))
                  << " 通过" << std::endl;
        if (!norm_ok) return 1;

        // §6.5 红线：只有 confirmed 能进识别提示 / 翻译约束
        struct ConstraintCase { const char* status; const char* value; bool want; };
        const ConstraintCase cc[] = {
            {"confirmed", "Erika",  true},
            // 这三条是红线本体：模型猜的、归档的、空的，一律不能当约束
            {"candidate", "Erika",  false},
            {"archived",  "Erika",  false},
            {"confirmed", "",       false},
        };
        bool cc_ok = true;
        int  cc_pass = 0;
        for (const auto& c : cc) {
            KnowledgeItem k;
            k.status = c.status;
            k.value  = c.value;
            if (knowledge::usable_as_constraint(k) == c.want) { ++cc_pass; continue; }
            cc_ok = false;
            std::cerr << "[SelfTest] 约束资格判定失败: status=" << c.status
                      << " value='" << c.value << "' 期望 " << c.want << std::endl;
        }
        std::cout << "[SelfTest] 约束资格（§6.5 红线：Candidate 不能进提示/约束）: "
                  << (cc_ok ? "✅ " : "❌ ") << cc_pass << "/"
                  << (sizeof(cc) / sizeof(cc[0])) << " 通过" << std::endl;
        if (!cc_ok) return 1;

        // 检索词清洗：用户打的是自然文本，不是 FTS5 查询表达式。
        // 这一组专门钉住"打标点不会让查询报错"—— 搜索框崩掉是最没面子的一类 bug。
        struct QueryCase { const char* in; const char* want; };
        const QueryCase qc[] = {
            // 【每个词后面都带 `*`】FTS5 默认整词匹配，"eric" 查不到 "Erica"。
            // 而输入这个查询的通常是人打的半截词、或 Agent 从用户话里截的词 ——
            // 整词匹配会让他们得到"库里没这条知识"这个错误结论。
            // 实测：`"eric"` → 0 条；`"eric"*` → 命中 Erica。
            {"phoenix",            "\"phoenix\"*"},
            {"Q4, 2024",           "\"Q4\"* AND \"2024\"*"},
            // 这三个是操作符，必须被中性化，否则 MATCH 直接语法错
            {"AND",                "\"AND\"*"},
            {"-foo",               "\"foo\"*"},
            {"a\"b",               "\"a\"* AND \"b\"*"},
            {"(unclosed",          "\"unclosed\"*"},
            // 纯标点 → 空串（调用方据此返回空结果，不发查询）
            {"???",                ""},
            {"   ",                ""},
            // 中文必须整字保留，不能被当标点切碎
            {u8"埃里卡",           u8"\"埃里卡\"*"},
            {u8"埃里卡，你好",     u8"\"埃里卡\"* AND \"你好\"*"},
        };
        bool q_ok = true;
        int  q_pass = 0;
        for (const auto& c : qc) {
            const std::string got = knowledge::fts_query_from_user_text(c.in);
            if (got == c.want) { ++q_pass; continue; }
            q_ok = false;
            std::cerr << "[SelfTest] 检索词清洗失败: '" << c.in << "' 期望 '"
                      << c.want << "' 实际 '" << got << "'" << std::endl;
        }
        std::cout << "[SelfTest] 检索词清洗（标点/操作符/中文）: " << (q_ok ? "✅ " : "❌ ")
                  << q_pass << "/" << (sizeof(qc) / sizeof(qc[0])) << " 通过" << std::endl;
        if (!q_ok) return 1;

        // ---- 三条腿的输入（§7 步骤 2.5）：红线必须守在三处，不只守一处 ----
        //
        // 【为什么这一组比"断言 usable_as_constraint 返回 false"更重要】
        // 只断言守卫函数本身，等于只验"锁是好的、门没关"。
        // 真正会出的事故是：某条腿绕过了守卫自己按 status 过滤，
        // 于是 candidate 摸进了识别提示 / 翻译约束 / 摘要背景。
        // 所以这里**逐个腿断言它的实际输入里没有 candidate**。
        {
            using namespace knowledge;
            bool leg_ok = true;
            std::string why;

            auto item = [](const char* kind, const char* value, const char* status,
                           int hits) {
                KnowledgeItem k;
                k.kind   = kind;
                k.key    = value;
                k.value  = value;
                k.status = status;
                k.hits   = hits;
                return k;
            };

            const std::vector<KnowledgeItem> items = {
                item("person",  "Erika",     "confirmed", 9),
                item("term",    "Phoenix",   "confirmed", 5),
                item("project", "Apollo",    "confirmed", 3),
                // ↓ 这三条是本组的全部意义：模型猜的、被否掉的、空的
                item("person",  "Marko",     "candidate", 7),
                item("term",    "GhostTerm", "archived",  7),
                item("person",  "EmptyVal",  "confirmed", 7),
                // ↓ fact 的值是句子，不该进"词条"，但**该**进摘要背景
                item("fact",    "ASR 从 Whisper large-v3 换成了 Qwen ASR",
                     "confirmed", 4),
            };
            // 空值那条要真的空
            auto items2 = items;
            items2[5].value.clear();

            const auto terms = constraint_terms(items2);
            const auto bg    = background_lines(items2);

            auto contains = [](const std::vector<std::string>& hay, const std::string& needle) {
                for (const auto& s : hay) if (s.find(needle) != std::string::npos) return true;
                return false;
            };

            // 腿① 和腿② 共用 terms。
            //
            // 刻意写成 contains() 而不是 lacks() —— 第一版用了个"找不到才为真"的辅助函数，
            // 于是"不该有"和"该有"两组断言混在一起时我把两处写反了，
            // 自检报的是"candidate 摸进了识别提示"，而真相是断言自己错了。
            // 双否定在断言里是纯负债。
            if (contains(terms, "Marko"))      { leg_ok = false; why = "candidate 摸进了识别提示/翻译约束"; }
            else if (contains(terms, "GhostTerm")) { leg_ok = false; why = "archived 摸进了识别提示/翻译约束"; }
            else if (contains(terms, "EmptyVal"))  { leg_ok = false; why = "空值不该进词条"; }
            else if (!contains(terms, "Erika"))    { leg_ok = false; why = "confirmed 的词条反而漏了"; }
            else if (contains(terms, "Whisper"))   { leg_ok = false; why = "fact 的句子不该进词条（会诱发提示回显）"; }

            // 腿③ 摘要背景
            if (!leg_ok) {}
            else if (contains(bg, "Marko"))    { leg_ok = false; why = "candidate 摸进了摘要背景"; }
            else if (contains(bg, "GhostTerm")){ leg_ok = false; why = "archived 摸进了摘要背景"; }
            // fact 必须**在**背景里 —— 它是"越用越懂你"在摘要上的落点
            else if (!contains(bg, "Whisper large-v3")) {
                leg_ok = false;
                why = "fact/decision 类知识必须进摘要背景（它们只适合当背景）";
            }

            // 词条上限与长度保护
            {
                std::vector<KnowledgeItem> many;
                for (int i = 0; i < 100; ++i)
                    many.push_back(item("term", ("T" + std::to_string(i)).c_str(),
                                        "confirmed", i));
                if (constraint_terms(many, 40).size() != 40)
                    { leg_ok = false; why = "词条数应被截断到上限"; }
                // 超过 40 字的值不是词条（整句话塞进 initial_prompt 没有意义）
                std::vector<KnowledgeItem> longv = {
                    item("term",
                         "this is a whole sentence that accidentally ended up in knowledge",
                         "confirmed", 9)
                };
                if (!constraint_terms(longv).empty())
                    { leg_ok = false; why = "超长值不该当词条"; }
            }

            // 大小写重复的词条要去掉：'Erika' 和 'erika' 是同一个
            {
                std::vector<KnowledgeItem> dup = {
                    item("person", "Erika", "confirmed", 9),
                    item("person", "erika", "confirmed", 5),
                };
                if (constraint_terms(dup).size() != 1)
                    { leg_ok = false; why = "大小写不同的同一个词条应去重"; }
            }

            std::cout << "[SelfTest] 知识复用三腿（candidate/archived 绝不能进）: "
                      << (leg_ok ? "✅ 通过" : "❌ 失败") << std::endl;
            if (!leg_ok) {
                std::cerr << "    " << why << std::endl;
                return 1;
            }

            // 腿③ 的真正落点：背景知识有没有**进到送给模型的 user 内容里**。
            //
            // 只断言 set_background() 被调用过是没有意义的 ——
            // 那和"配置项设了"一样，证明不了模型看得到。
            // 这里直接断言拼出来的 user 内容。
            {
                using knowledge::GapRule;
                LlmSummarizer sm(LlmSummarizer::Backend::Local);
                sm.set_target_language("zh");

                std::vector<Segment> segs;
                Segment s0;
                s0.seq = 1;
                s0.src_text = "Welcome to EnglishPod.";
                s0.tgt_text = u8"欢迎来到 EnglishPod。";
                segs.push_back(s0);

                const std::string without = sm.build_user_content(segs);
                if (without.find(u8"欢迎来到") == std::string::npos) {
                    std::cerr << "[SelfTest] 摘要 user 内容里没有转录" << std::endl;
                    return 1;
                }
                if (without.find(u8"背景知识") != std::string::npos) {
                    std::cerr << "[SelfTest] 没有背景时不该出现背景段落" << std::endl;
                    return 1;
                }

                sm.set_background({u8"- Erica（人名）", u8"- Marco（人名）"});
                const std::string with = sm.build_user_content(segs);
                bool ok = true;
                std::string why2;
                if (with.find("Erica") == std::string::npos) { ok = false; why2 = "背景知识没进 user 内容"; }
                else if (with.find(u8"欢迎来到") == std::string::npos) { ok = false; why2 = "加了背景之后转录丢了"; }
                // 背景必须出现在转录**之前**，并且明确标注"不是转录内容"——
                // 否则小模型会把背景当成本场发生的事写进纪要
                else if (with.find("Erica") > with.find(u8"欢迎来到")) {
                    ok = false; why2 = "背景应排在转录之前";
                }
                else if (with.find(u8"不要把它当成发生过的事") == std::string::npos) {
                    ok = false; why2 = "缺少'背景不是转录内容'的显式标注";
                }
                std::cout << "[SelfTest] 摘要背景注入（进 user 内容且标注清楚）: "
                          << (ok ? "✅ 通过" : "❌ 失败") << std::endl;
                if (!ok) {
                    std::cerr << "    " << why2 << std::endl;
                    return 1;
                }
            }

            // ---- 2.6 抽取器：从转录里认出专名候选 ----
            //
            // 【这一组的用例全部来自真实数据，不是我编的】
            // 两个会话的转录都是真跑出来的：
            //   #43  EnglishPod 播客 → 真专名 Marco / Erica / EnglishPod / TV
            //   #42  jfk.wav        → **一个专名都没有**，但每句首词都大写（Ask/What/…）
            // 第二句是这一组最要紧的用例：句首大写是**语法**不是**专名**证据，
            // 认不出来就等于每句话都抽出一个假专名。
            {
                using namespace knowledge;
                bool ex_ok = true;
                std::string why3;

                auto mkseg = [](int seq, const char* src, double conf = 0.85) {
                    Segment s;
                    s.seq        = seq;
                    s.src_text   = src;
                    s.confidence = conf;
                    return s;
                };
                auto has = [](const std::vector<ExtractedCandidate>& v, const char* key) {
                    for (const auto& c : v) if (c.key == key) return true;
                    return false;
                };
                auto get = [](const std::vector<ExtractedCandidate>& v, const char* key)
                             -> const ExtractedCandidate* {
                    for (const auto& c : v) if (c.key == key) return &c;
                    return nullptr;
                };

                // ① 句首大写不算证据（jfk 的形状：每句首词都大写，但没有专名）
                {
                    std::vector<Segment> segs = {
                        mkseg(1, "Ask not what your country can do for you"),
                        mkseg(2, "What your country can do for you"),
                        mkseg(3, "And so my fellow Americans"),
                    };
                    const auto c = extract_candidates(segs, 1);
                    if (has(c, "ask"))      { ex_ok = false; why3 = "句首词 Ask 被当成了专名"; }
                    else if (has(c, "what")){ ex_ok = false; why3 = "句首词 What 被当成了专名"; }
                    else if (has(c, "and")) { ex_ok = false; why3 = "句首词 And 被当成了专名"; }
                }

                // ② 非句首的大写词要认出来，而且要认成 person（人名句式）
                {
                    std::vector<Segment> segs = {
                        mkseg(1, "EnglishPod. My name is Marco. And I'm Erica."),
                        mkseg(2, "How are you, Erica? Marco, I'm doing really well."),
                    };
                    const auto c = extract_candidates(segs, 1);
                    const auto* marco = get(c, "marco");
                    const auto* erica = get(c, "erica");
                    if (!marco || !erica) {
                        ex_ok = false; why3 = "真专名 Marco / Erica 没被抽出来";
                    } else {
                        if (marco->kind != "person") { ex_ok = false; why3 = "Marco 应是 person（人称句式）"; }
                        // 【次数必须准】第一版把"证据"和"计数"混在一起 ——
                        // 既漏掉句首那次，又把同一处两条路径各算一次，
                        // 于是界面显示"已经听到 3 次"而实际只有 2 次。
                        else if (marco->hits != 2) { ex_ok = false; why3 = "Marco 出现次数应为 2"; }
                        else if (erica->hits != 2) { ex_ok = false; why3 = "Erica 出现次数应为 2"; }
                    }
                    // CamelCase 在句首也要认（EnglishPod 正好在句首）
                    if (ex_ok && !has(c, "englishpod")) {
                        ex_ok = false; why3 = "句首的 CamelCase 词 EnglishPod 漏了";
                    }
                    // 缩写 TV
                    if (ex_ok) {
                        std::vector<Segment> t = {mkseg(1, "you hear in movies and TV shows")};
                        if (!has(extract_candidates(t, 1), "tv")) {
                            ex_ok = false; why3 = "缩写 TV 没被认出来";
                        }
                    }
                }

                // ③ 收缩式绝不能被当专名（实测 `I'm` 报过 hits=4）
                {
                    std::vector<Segment> segs = {
                        mkseg(1, "And I'm Erica."),
                        mkseg(2, "That's right. It's really good."),
                        mkseg(3, "Don't worry, We're fine."),
                    };
                    const auto c = extract_candidates(segs, 1);
                    for (const char* bad : {"i'm", "that's", "it's", "don't", "we're"}) {
                        if (has(c, bad)) { ex_ok = false; why3 = std::string("收缩式被当成专名: ") + bad; }
                    }
                    // 但 O'Brien 这种"词中间有大写"的必须留下。
                    //
                    // 键是 `o brien`（**带空格**）：normalize_key 把撇号当**分隔符**处理
                    // （和逗号一个待遇），所以 O'Brien → "o brien"。
                    // 两版断言都写错了（先写 "o'brien"，再写 "obrien"），是我没先看
                    // normalize_key 的行为就写断言 —— 这也是**用例写错不是代码错**。
                    // 记一笔限制：`O'Brien` / `OBrien` / `O Brien` 会归成三个不同的键，
                    // 已有的 8 条归一化用例没覆盖这种情况，本轮不动它（属遗留）。
                    std::vector<Segment> ob = {mkseg(1, "I met O'Brien yesterday.")};
                    if (ex_ok && !has(extract_candidates(ob, 1), "o brien")) {
                        ex_ok = false; why3 = "O'Brien 被撇号规则误杀（R2 应该放行）";
                    }
                }

                // ④ 人称句式里的假阳性：I'm doing / I'm really 不是人名
                {
                    std::vector<Segment> segs = {
                        mkseg(1, "I'm doing really well."),
                        mkseg(2, "I'm really excited because"),
                    };
                    const auto c = extract_candidates(segs, 1);
                    if (has(c, "doing") || has(c, "really")) {
                        ex_ok = false; why3 = "「I'm X」里的动词/副词被当成了人名（实测发生过）";
                    }
                }

                // ⑤ 引号里的词：最强的信号，大小写不管
                {
                    std::vector<Segment> segs = {mkseg(1, "we call it \xe3\x80\x8c" "EchoMind" "\xe3\x80\x8d internally")};
                    const auto c = extract_candidates(segs, 1);
                    if (!has(c, "echomind")) { ex_ok = false; why3 = "引号里的词没被抽出来"; }
                }

                // ⑥ 中文人名（只能靠人称句式 —— 中文没有大小写）
                {
                    std::vector<Segment> segs = {mkseg(1, "\xe6\x88\x91\xe5\x8f\xab" "\xe5\xbc\xa0\xe4\xb8\x89" "\xef\xbc\x8c\xe4\xbd\xa0\xe5\xa5\xbd")};
                    const auto c = extract_candidates(segs, 1);
                    bool found_cn = false;
                    for (const auto& x : c) if (x.kind == "person") found_cn = true;
                    if (!found_cn) { ex_ok = false; why3 = "「我叫张三」里的中文人名没被抽出来"; }
                }

                // ⑦ 抽出来的东西**必须**是 candidate，绝不能是 confirmed（§6.5 红线）
                {
                    std::vector<Segment> segs = {mkseg(1, "My name is Marco.")};
                    const auto c = extract_candidates(segs, 1);
                    if (c.empty()) { ex_ok = false; why3 = "用例数据没抽出东西，后面的写入断言没意义"; }
                    else {
                        const std::string PRE2 = "__selftest_extract_";
                        auto& ks2 = KnowledgeStore::instance();
                        ks2.purge_key_prefix(PRE2);
                        std::vector<ExtractedCandidate> shifted = c;
                        for (auto& x : shifted) x.key = PRE2 + x.key;
                        const int n = save_candidates(shifted);
                        if (n <= 0) { ex_ok = false; why3 = "候选写入失败"; }
                        else {
                            KnowledgeItem got;
                            if (ks2.get("person", PRE2 + "marco", &got)) {
                                if (got.status != "candidate") {
                                    ex_ok = false;
                                    why3 = "自动抽取写进去的必须是 candidate，实际 " + got.status;
                                }
                                // 【次数必须一路传到库里】抽取器算出的 hits 不能在
                                // save_candidates 那一步丢掉 —— 真数据上抓到过：
                                // 抽取器说 2 次、库里查到 1 次（save_candidates 没传 item.hits）。
                                else if (got.hits != 1) {
                                    // 这个用例只有一段、Marco 只出现 1 次
                                    ex_ok = false;
                                    why3 = "hits 没传到库里（应 1，实际 "
                                           + std::to_string(got.hits) + "）";
                                }
                                else if (!knowledge::usable_as_constraint(got)) {
                                    // 这条应当成立：candidate 不能当约束
                                } else {
                                    ex_ok = false;
                                    why3 = "candidate 竟然通过了 usable_as_constraint（红线破了）";
                                }
                            } else {
                                ex_ok = false; why3 = "抽取的候选没写进库";
                            }
                        }
                        ks2.purge_key_prefix(PRE2);
                    }
                }

                std::cout << "[SelfTest] 自动抽取（句首不算/收缩式排除/次数准确/一律候选）: "
                          << (ex_ok ? "✅ 通过" : "❌ 失败") << std::endl;
                if (!ex_ok) {
                    std::cerr << "    " << why3 << std::endl;
                    return 1;
                }

                // ---- ⑨ 中文专名抽取（R6/R7）----
                //
                // 【为什么必须有这一组】中文没有大小写、没有词边界 —— 英文那套规则
                // 在中文里一条都用不上。结果就是中文会议里「张伟负责下周的报价」
                // 一个候选都抽不出来 → 知识库不长 → 没问题可问 → 三条腿没输入
                // → "越用越懂你"在中文场景下完全不成立。
                //
                // ⚠️ **这些用例是我写的，不是真实 ASR 输出**（§8.8⑨ 的教训要求用真数据）。
                // 中文侧现在没有真实录音可用，所以这一组只验"规则在中文会议语言形状上
                // 认不认得出"。**真实 ASR 输出上的准确率还要靠录一场中文会议来验**
                // （录完跑 `--extract <id>`）。这一点在 STATE.md 里标了 ⚠️。
                {
                    bool z_ok = true;
                    std::string whyz;
                    auto mkz = [](int seq, const char* src) {
                        Segment s;
                        s.seq        = seq;
                        s.src_text   = src;
                        s.confidence = 0.85;
                        return s;
                    };
                    auto hasz = [](const std::vector<ExtractedCandidate>& v, const char* k) {
                        for (const auto& c : v) if (c.key == k) return true;
                        return false;
                    };
                    auto kindz = [](const std::vector<ExtractedCandidate>& v, const char* k)
                                    -> std::string {
                        for (const auto& c : v) if (c.key == k) return c.kind;
                        return {};
                    };

                    // 一场"像样"的中文周会片段
                    const std::vector<Segment> segs = {
                        mkz(1, u8"大家好，今天这个会我们主要讨论三件事。"),
                        mkz(2, u8"张伟负责下周的报价，李经理跟进客户那边的反馈。"),
                        mkz(3, u8"凤凰项目的排期由王工来定，星辰科技那边也在等我们的回复。"),
                        mkz(4, u8"另外李小明提出，交付计划要不要往后挪一周。"),
                        mkz(5, u8"这个项目需要延期，于是我们就成为了负责人。"),
                        mkz(6, u8"张总说下周五之前必须给出结论。"),
                        mkz(7, u8"星辰科技的对接人是王老师，凤凰项目下周启动。"),
                        mkz(8, u8"好处是我们不用重新做，他说的对。"),
                        mkz(9, u8"施工队那边今天进不了场，工具还没到位。"),
                    };
                    const auto c = extract_candidates(segs, 1);

                    // 该抽到的（8 个）
                    struct Want { const char* key; const char* kind; };
                    const Want want[] = {
                        {u8"张伟",     "person"},
                        {u8"李经理",   "person"},
                        {u8"李小明",   "person"},
                        {u8"张总",     "person"},
                        {u8"王工",     "person"},
                        {u8"王老师",   "person"},
                        {u8"凤凰项目", "term"},
                        {u8"星辰科技", "term"},
                    };
                    for (const auto& w : want) {
                        if (!z_ok) break;
                        const std::string k = knowledge::normalize_key(w.key);
                        if (!hasz(c, k.c_str())) {
                            z_ok = false;
                            whyz = std::string(u8"该抽到却漏了：") + w.key;
                        } else if (kindz(c, k.c_str()) != w.kind) {
                            z_ok = false;
                            whyz = std::string(u8"kind 不对：") + w.key + u8"（应为 "
                                   + w.kind + u8"，实际 " + kindz(c, k.c_str()) + u8"）";
                        }
                    }

                    // 绝不能抽到的
                    const char* never[] = {
                        u8"交付计划",   // 后缀表里刻意没收"计划"
                        u8"这个项目",   // 泛指前缀 + 弱后缀
                        u8"于是",       // 黑名单（于 是姓氏）
                        u8"成为",       // 黑名单（成 是姓氏）
                        u8"边的",       // 人名里不能含虚词（"那边 的 反馈"）
                        u8"施工",       // 施 是姓氏、工 是称谓 → 必须被黑名单挡住
                        u8"工作", u8"工具", u8"好处",
                    };
                    for (const char* bad : never) {
                        if (!z_ok) break;
                        const std::string k = knowledge::normalize_key(bad);
                        if (hasz(c, k.c_str())) {
                            z_ok = false;
                            whyz = std::string(u8"不该抽到却抽了：") + bad;
                        }
                    }

                    // 次数必须准：凤凰项目/星辰科技 各出现 2 次
                    if (z_ok) {
                        for (const auto& x : c) {
                            if (x.key == knowledge::normalize_key(u8"星辰科技") && x.hits != 2) {
                                z_ok = false;
                                whyz = u8"星辰科技 出现次数应为 2，实际 " + std::to_string(x.hits);
                            }
                        }
                    }

                    std::cout << "[SelfTest] 中文专名抽取（姓氏+佐证 / 后缀 / 假阳性拦截）: "
                              << (z_ok ? "✅ 通过" : "❌ 失败") << std::endl;
                    if (!z_ok) {
                        std::cerr << "    " << whyz << std::endl;
                        return 1;
                    }
                }

                // ---- ⑩ Agent 工具层（§7 步骤 5.1）----
                //
                // 【这一组守的是什么】工具层是 Agent 唯一能"动手"的地方。
                // 它出问题有两种表现，而且在日志里长得一样（"最后没给出有用答案"）：
                //   ① 模型不会用 —— schema/description 拼歪了，模型根本不会调用
                //   2 工具本身坏了 —— 参数解析、限长、错误路径
                // 所以这里把两边都钉住：schema 必须能被解析、每个工具必须真跑通、
                // **出错必须变成"回一句话"而不是异常**（模型会猜错名字、会吐半截 JSON，
                // 这两件事在循环里一定会发生，不能让整个任务崩掉）。
                {
                    bool a_ok = true;
                    std::string whya;
                    agent::ToolRegistry reg;
                    agent::register_readonly_tools(reg);

                    // ① 注册清单
                    const char* must_have[] = {"search_knowledge", "knowledge_history",
                                               "list_sessions", "get_session",
                                               "get_session_report"};
                    if (reg.size() != 5) {
                        a_ok = false;
                        whya = "应注册 5 个工具，实际 " + std::to_string(reg.size());
                    }
                    for (const char* n : must_have) {
                        if (!a_ok) break;
                        if (reg.find(n) == nullptr) {
                            a_ok = false;
                            whya = std::string("缺少工具：") + n;
                        }
                    }

                    // ② 重名 / 空名 / 无实现必须被拒（宁可注册失败，也不能让
                    //    "调用哪个"取决于注册顺序 —— 那是不可复现的行为）
                    if (a_ok) {
                        std::string e;
                        if (reg.add({"search_knowledge", "dup", "{}", nullptr}, &e)) {
                            a_ok = false; whya = "重名工具应被拒绝";
                        }
                        if (a_ok && reg.add({"", "empty", "{}", [](auto, auto, auto) {
                                return agent::ToolResult{}; }}, &e)) {
                            a_ok = false; whya = "空名工具应被拒绝";
                        }
                        if (a_ok && reg.add({"no_impl", "x", "{}", nullptr}, &e)) {
                            a_ok = false; whya = "没有实现的工具应被拒绝";
                        }
                    }

                    // ③ function calling 用的 tools 数组必须合法、字段齐全
                    if (a_ok) {
                        const std::string tj = reg.tools_json();
                        if (!utf8::is_valid(tj)) { a_ok = false; whya = "tools_json 不是合法 UTF-8"; }
                        else {
                            if (!json_ok(tj)) { a_ok = false; whya = "tools_json 不是合法 JSON"; }
                        }
                        if (a_ok) {
                            for (const auto& n : reg.names()) {
                                const auto* t = reg.find(n);
                                if (t->description.empty() || t->params_schema.empty()) {
                                    a_ok = false;
                                    whya = "工具 " + n + " 缺 description 或 params_schema";
                                    break;
                                }
                            }
                        }
                    }

                    // ④ 出错路径：未知工具必须把"能用的工具"回给模型
                    agent::ToolContext ctx;
                    ctx.deliverable_root = cfg.deliverable_dir;
                    if (a_ok) {
                        const auto r = reg.call("definitely_not_a_tool", "{}", ctx);
                        if (r.ok) { a_ok = false; whya = "未知工具应返回失败"; }
                        else if (r.content.find("available_tools") == std::string::npos ||
                                 r.content.find("search_knowledge") == std::string::npos) {
                            a_ok = false;
                            whya = "未知工具时没把可用工具列表回给模型（模型就无从改口）";
                        }
                    }
                    // ⑤ 参数不是合法 JSON（模型吐半截 JSON 是常态）
                    if (a_ok) {
                        const auto r = reg.call("get_session", "{\"session_id\":", ctx);
                        if (r.ok) { a_ok = false; whya = "坏参数应返回失败"; }
                        else if (r.error.find("JSON") == std::string::npos) {
                            a_ok = false; whya = "坏参数应明确说'不是合法 JSON'";
                        }
                    }

                    // ⑥ 每个只读工具在**真实库**上跑通，且结果是合法 JSON
                    //
                    // 【必须自己造数据】第一版直接查 "erica" —— 那等于**依赖库里已经有知识**，
                    // 换个空库跑就红。项目自己的规矩是"断言必须与真实数据隔离"（§8.8⑬）：
                    // 用 `__selftest_` 前缀插一条探针，跑完按前缀清掉。
                    if (a_ok) {
                        KnowledgeItem tmp;
                        tmp.kind        = "person";
                        tmp.key         = "__selftest_tool_probe";
                        tmp.value       = "EricaToolProbe";
                        tmp.status      = "candidate";
                        tmp.confidence  = 0.8;
                        tmp.source_text = u8"自检造的探针（工具层用例）";
                        std::string e;
                        if (KnowledgeStore::instance().upsert(tmp, &e) <= 0) {
                            a_ok = false;
                            whya = "工具层用例的探针知识写不进去：" + e;
                        }
                    }
                    if (a_ok) {
                        const std::string sid_s = std::to_string(sid);   // 自检会话，一定存在
                        struct Tc { const char* name; std::string args; };
                        const Tc tcs[] = {
                            {"search_knowledge",   "{\"query\":\"EricaToolProbe\"}"},
                            {"knowledge_history",  "{\"key\":\"__selftest_tool_probe\"}"},
                            {"list_sessions",      "{\"days\":3650,\"limit\":3}"},
                            {"get_session",        "{\"session_id\":" + sid_s + ",\"max_segments\":2}"},
                            {"get_session_report", "{\"session_id\":0}"},
                        };
                        for (const auto& c : tcs) {
                            if (!a_ok) break;
                            const auto r = reg.call(c.name, c.args, ctx);
                            // get_session_report 对"没生成过纪要的会话"**应当**失败 —— 那是对的
                            if (!r.ok && std::string(c.name) != "get_session_report") {
                                a_ok = false;
                                whya = std::string(c.name) + " 在真实库上跑失败：" + r.error;
                                break;
                            }
                            if (!utf8::is_valid(r.content)) {
                                a_ok = false; whya = std::string(c.name) + " 的结果不是合法 UTF-8";
                                break;
                            }
                            if (!json_ok(r.content)) {
                                a_ok = false; whya = std::string(c.name) + " 的结果不是合法 JSON";
                                break;
                            }
                        }
                        // 自检自己造的数据自己擦
                        KnowledgeStore::instance().purge_key_prefix("__selftest_tool_");
                    }

                    // ⑦ 限长必须真的生效（不生效就会一次撑爆模型上下文）
                    if (a_ok) {
                        const std::string base = "{\"session_id\":" + std::to_string(sid);
                        const auto r = reg.call("get_session", base + ",\"max_segments\":1}", ctx);
                        if (r.ok && r.content.size() > 13000) {
                            a_ok = false; whya = "工具结果超过硬上限";
                        }
                        // 参数越界要被夹住，而不是照单全收
                        const auto r2 = reg.call("get_session", base + ",\"max_segments\":99999}", ctx);
                        if (r2.ok && r2.content.size() > 13000) {
                            a_ok = false; whya = "max_segments 越界没有被夹住";
                        }
                    }

                    std::cout << "[SelfTest] Agent 工具层（注册/schema/真跑/错误路径/限长）: "
                              << (a_ok ? "✅ 通过" : "❌ 失败") << std::endl;
                    if (!a_ok) {
                        std::cerr << "    " << whya << std::endl;
                        return 1;
                    }
                }

                // ⑧ **调用方给的 hits 必须被采信**（不是固定算 1）
                //
                // 【这条是被真数据抓出来的】抽取器算出「Erica 这场出现 2 次」，
                // 但 upsert 的 INSERT 把 hits 硬编码成 1 —— 于是：
                //   ① 问用户时显示"已经听到 1 次"，是假证据
                //   ② `hits >= 3` 那条规则在一场会话内永远不可能为真
                // 真实会话 #43 上跑出来才发现（`--extract 43` 说 2 次，`--gaps` 说 1 次）。
                {
                    const std::string PK = "__selftest_hits_";
                    auto& ks3 = KnowledgeStore::instance();
                    ks3.purge_key_prefix(PK);

                    KnowledgeItem h;
                    h.kind       = "term";
                    h.key        = PK + "phoenix";
                    h.value      = "Phoenix";
                    h.status     = "candidate";
                    h.confidence = 0.8;
                    h.hits       = 5;          // 抽取器说：这场听到 5 次
                    std::string e;
                    if (ks3.upsert(h, &e) <= 0) {
                        std::cerr << "[SelfTest] hits 用例写入失败: " << e << std::endl;
                        return 1;
                    }
                    KnowledgeItem got3;
                    if (!ks3.get("term", PK + "phoenix", &got3) || got3.hits != 5) {
                        std::cerr << "[SelfTest] upsert 丢了调用方给的 hits（应为 5，实际 "
                                  << got3.hits << "）—— 这会让「已经听到 N 次」显示假数字"
                                  << std::endl;
                        return 1;
                    }
                    // 值相同时累加的也应当是"本次听到的次数"，不是固定 1
                    ks3.upsert(h, &e);
                    if (!ks3.get("term", PK + "phoenix", &got3) || got3.hits != 10) {
                        std::cerr << "[SelfTest] 值相同时 hits 应累加 5（10），实际 "
                                  << got3.hits << std::endl;
                        return 1;
                    }
                    ks3.purge_key_prefix(PK);
                    std::cout << "[SelfTest] hits 采信（调用方给的次数不被丢掉）: ✅ 通过"
                              << std::endl;
                }
            }
        }
    }

    // ---- 11) 长期记忆：落库（upsert 三条语义 + 降级 + 历史）----
    //
    // 隔离方式：用**合法的 kind**（term）+ `__selftest_` 前缀的 key，跑完按前缀清理。
    // 第一版用例拿 kind="__selftest" 做隔离，被 kind 校验直接拒了 ——
    // 这反过来说明校验是有效的；而隔离只能靠 key 前缀（按 kind 删会误删用户真数据）。
    {
        const std::string TK   = "term";
        const std::string TKEY = "__selftest_erika";
        const std::string TPRE = "__selftest_";

        auto& ks = KnowledgeStore::instance();
        ks.purge_key_prefix(TPRE);   // 清掉上次跑的残留

        bool db_ok = true;
        std::string last_err;
        auto fail = [&](const char* what) {
            db_ok = false;
            std::cerr << "[SelfTest] 知识落库失败: " << what
                      << (last_err.empty() ? "" : ("  <- " + last_err)) << std::endl;
            last_err.clear();
        };

        KnowledgeItem k;
        k.kind           = TK;
        k.key            = TKEY;
        k.value          = "Erika";
        k.status         = "confirmed";
        k.confidence     = 0.9;
        k.source_session = 999;
        k.source_seq     = 7;
        k.source_text    = "Hello, I'm Erika.";

        const long long id1 = ks.upsert(k, &last_err);
        if (id1 <= 0) fail("新建应返回正数 id");

        KnowledgeItem got;
        last_err.clear();
        if (!ks.get(TK, TKEY, &got)) fail("按归一化后的键应能查到");
        else {
            if (got.value  != "Erika")     fail("value 不对");
            if (got.status != "confirmed") fail("status 不对");
            if (got.hits   != 1)           fail("新建 hits 应为 1");
            if (got.source_seq != 7)       fail("证据段落号丢了");
        }

        // 语义一：值相同 → 只累加 hits，不写历史
        last_err.clear();
        const long long id2 = ks.upsert(k, &last_err);
        if (id2 != id1) fail("同键应更新同一行，不能新建");
        if (ks.get(TK, TKEY, &got) && got.hits != 2) fail("hits 应累加到 2");
        if (!ks.history(id1).empty()) fail("值没变时不该写历史");

        // 语义二：值变了 → 更新 + 写历史 + **confirmed 降回 candidate**
        k.value = u8"埃里卡";
        last_err.clear();
        ks.upsert(k, &last_err);
        if (ks.get(TK, TKEY, &got)) {
            if (got.value  != u8"埃里卡")   fail("value 应更新");
            if (got.status != "candidate") fail("值变了必须降回 candidate（§6.5）");
        }
        const auto hist = ks.history(id1);
        if (hist.size() != 1) fail("值变化应写 1 条历史");
        else if (hist[0].old_value != "Erika" || hist[0].new_value != u8"埃里卡")
            fail("历史里应记下旧值与新值");

        // 语义三：用户确认 → 写历史且状态变 confirmed
        last_err.clear();
        if (!ks.set_status(id1, "confirmed", "user_confirmed", &last_err)) fail("确认应成功");
        if (ks.get(TK, TKEY, &got) && got.status != "confirmed") fail("状态应变为 confirmed");
        if (ks.history(id1).size() != 2) fail("状态变化也应进历史");

        // 非法输入必须被拒绝，而不是悄悄写进去
        {
            KnowledgeItem bad = k;
            bad.kind = "not_a_kind";
            last_err.clear();
            if (ks.upsert(bad, &last_err) > 0) fail("非法 kind 应被拒绝");
            bad = k;
            bad.value.clear();
            last_err.clear();
            if (ks.upsert(bad, &last_err) > 0) fail("空 value 应被拒绝");
        }

        // ---- 全文检索往返：走**真实写入路径**，索引必须自己跟上 ----
        //
        // 【为什么单列这一组】2.3 之前自检只验了"四张表在不在"，于是
        // "knowledge_fts 的索引其实一条都没建起来、MATCH 啥也查不到"
        // 这个 bug 藏了两轮。弱断言的代价就是假绿灯。
        //
        // 这里一句 FTS 语句都不写：upsert 插进去，就必须能被 search() 查回来。
        // 全靠 knowledge 表上的触发器维护外部内容表的索引。
        //
        // 【检索词必须是这个用例独有的】第一版用 "phoenix" 检索并断言"恰好 1 条"，
        // 结果真实数据里存在 `__demo_phoenix` 时自检就红了 ——
        // **用户知识库里只要有一条叫 Phoenix 的真知识，自检就会失败。**
        // 用一个不可能撞车的 token，断言才只反映本用例干了什么。
        const std::string UNIQ = "ZqFtsRoundTrip9";
        {
            last_err.clear();
            KnowledgeItem s;
            s.kind        = TK;
            s.key         = "__selftest_ftsroundtrip";
            s.value       = UNIQ;
            s.status      = "candidate";
            s.confidence  = 0.8;
            s.source_text = "The " + UNIQ + " project ships in Q4.";
            if (ks.upsert(s, &last_err) <= 0) fail("检索用例写入失败");

            // 把 err 一并报出去：搜索的失败模式里最坑的是"SQL 没编译过"，
            // 那种情况下返回的是空结果，看起来和"真的没搜到"一模一样。
            last_err.clear();
            const auto found = ks.search(UNIQ, 20, &last_err);
            if (found.size() != 1 || found[0].key != "__selftest_ftsroundtrip")
                fail("触发器没把新写入的行放进 FTS 索引（search 查不到）");

            // 中文也必须能查到（unicode61 分词器对 CJK 的行为要在这里钉住）。
            // 这一条只断言"非空" —— 库里本来就有中文条目时也成立。
            const auto cn = ks.search(u8"埃里卡");
            if (cn.empty()) fail("中文全文检索查不到（分词器或索引有问题）");

            // 大小写不同的检索词也要命中：FTS 检索**不该**被大小写绊住
            if (ks.search("zqftsroundtrip9").size() != 1) fail("检索应忽略大小写");

            // 全是标点 → 不该是错误，返回空即可
            std::string punct_err;
            if (!ks.search("???", 20, &punct_err).empty() || !punct_err.empty())
                fail("无有效词的查询应安静返回空");

            // 删掉之后必须查不到，否则检索会返回"幽灵记录"
            ks.purge_key_prefix("__selftest_ftsroundtrip");
            if (!ks.search(UNIQ).empty()) fail("删除没有同步到 FTS 索引（幽灵记录）");
        }

        last_err.clear();
        const int removed = ks.purge_key_prefix(TPRE);
        if (removed < 1) fail("清理应至少删掉 1 条");

        std::cout << "[SelfTest] 知识落库（新建/累加/变更降级/确认/拒绝非法/FTS 往返/清理）: "
                  << (db_ok ? "✅ 通过" : "❌ 失败") << std::endl;
        if (!db_ok) return 1;
    }

    // ---- 12) 知识缺口检测四条规则（§6.6 / §6.7）----
    // 全内存构造，不碰数据库、不碰模型。
    {
        using knowledge::GapRule;
        using knowledge::KnowledgeWithHistory;

        auto mk = [](long long id, const char* status, const char* value,
                     int hits, double conf, int asked = 0) {
            KnowledgeWithHistory e;
            e.item.id         = id;
            e.item.kind       = "term";
            e.item.key        = value;
            e.item.value      = value;
            e.item.status     = status;
            e.item.hits       = hits;
            e.item.confidence = conf;
            e.item.asked_count = asked;
            return e;
        };
        auto add_hist = [](KnowledgeWithHistory& e, const char* oldv, const char* newv,
                           const char* reason) {
            KnowledgeStore::HistoryRow h;
            h.old_value  = oldv;
            h.new_value  = newv;
            h.changed_at = "2026-09-16 00:00:00.000";
            h.reason     = reason;
            e.history.push_back(h);
        };

        bool gap_ok = true;
        std::string why;

        // ① confirmed 且没变化 → **一个字都不问**（§1.3 提问稀缺，这是最重要的一条）
        {
            std::vector<KnowledgeWithHistory> v = {mk(1, "confirmed", "Erika", 9, 0.9)};
            const auto q = knowledge::detect_gaps(v);
            if (!q.empty()) { gap_ok = false; why = "confirmed 且无变化的不该被问"; }
        }

        // ② archived → 不问（用户已经否掉过）
        {
            std::vector<KnowledgeWithHistory> v = {mk(2, "archived", "Phoenix", 9, 0.9)};
            const auto q = knowledge::detect_gaps(v);
            if (!q.empty()) { gap_ok = false; why = "archived 不该被问"; }
        }

        // ③ 高频未确认：hits >= 3 触发，=2 不触发
        {
            std::vector<KnowledgeWithHistory> v = {
                mk(3, "candidate", "Phoenix", 3, 0.9),
                mk(4, "candidate", "Q4",      2, 0.9),
            };
            const auto q = knowledge::detect_gaps(v);
            if (q.size() != 1 || q[0].rule != GapRule::HighFreqUnconfirmed || q[0].value != "Phoenix") {
                gap_ok = false; why = "高频未确认的阈值判定不对";
            }
        }

        // ④ 低置信度专名：0.4 触发，0.6 不触发
        {
            std::vector<KnowledgeWithHistory> v = {
                mk(5, "candidate", "Marko",  1, 0.40),
                mk(6, "candidate", "EnglishPod", 1, 0.60),
            };
            const auto q = knowledge::detect_gaps(v);
            if (q.size() != 1 || q[0].rule != GapRule::LowConfidenceName) {
                gap_ok = false; why = "低置信度的阈值判定不对";
            }
        }

        // ⑤ 旧值≠新值：优先级最高（库里可能已经是错的）
        {
            std::vector<KnowledgeWithHistory> v = {
                mk(7, "candidate", u8"埃里卡", 1, 0.9),
                mk(8, "candidate", "Phoenix",  5, 0.9),
            };
            add_hist(v[0], "Erika", u8"埃里卡", "value_changed_demoted");
            const auto q = knowledge::detect_gaps(v);
            // 两条都该出，但"值冲突"必须排在前面
            if (q.size() != 2 || q[0].rule != GapRule::ValueChanged || q[0].old_value != "Erika") {
                gap_ok = false; why = "值冲突的优先级或旧值没取对";
            }
        }

        // ⑥ 同一个键命中多条规则 → **只出一条**（否则用户看到三条问同一件事的题）
        {
            std::vector<KnowledgeWithHistory> v = {
                mk(9, "candidate", u8"埃里卡", 9, 0.30),   // 同时满足三条规则
            };
            add_hist(v[0], "Erika", u8"埃里卡", "value_changed_demoted");
            const auto q = knowledge::detect_gaps(v);
            if (q.size() != 1 || q[0].rule != GapRule::ValueChanged) {
                gap_ok = false; why = "同键应只出一条且取优先级最高的规则";
            }
        }

        // ⑦ 译法不一致：值变过但没被降级过
        {
            std::vector<KnowledgeWithHistory> v = {mk(10, "candidate", u8"埃里卡", 1, 0.9)};
            add_hist(v[0], "Erika",   u8"埃里卡", "model_extracted");
            add_hist(v[0], u8"埃里卡", u8"艾瑞卡", "model_extracted");
            const auto q = knowledge::detect_gaps(v);
            if (q.size() != 1 || q[0].rule != GapRule::InconsistentRendering) {
                gap_ok = false; why = "译法不一致没被识别";
            }
        }

        // ⑧ 上限：问不超过 max_questions 个（§1.3 红线）
        {
            std::vector<KnowledgeWithHistory> v;
            for (int i = 0; i < 20; ++i) v.push_back(mk(100 + i, "candidate", "T", 9, 0.9));
            const auto q = knowledge::detect_gaps(v, 5);
            if (q.size() != 5) { gap_ok = false; why = "问问题数应被截断到 5"; }
            if (!knowledge::detect_gaps(v, 0).empty()) { gap_ok = false; why = "max=0 时应一个问题都不出"; }
        }

        // ⑨ 空输入
        {
            if (!knowledge::detect_gaps({}).empty()) { gap_ok = false; why = "空输入不该出问题"; }
        }

        // ⑩ 问够了就不再问 —— **"提问稀缺"真正落地的那一条**
        //
        // 没有它，用户每次按回车跳过，下一场会话同样的问题原样再来一遍。
        // 用 asked_count 计数：问过 1 次还问（用户可能只是没想好），
        // 问过 kMaxAsks 次就沉底。
        {
            std::vector<KnowledgeWithHistory> v = {
                mk(200, "candidate", "Phoenix", 9, 0.9, 0),                 // 没问过 → 问
                mk(201, "candidate", "Marko",   9, 0.9, 1),                 // 问过 1 次 → 还问
                mk(202, "candidate", "Erika",   9, 0.9, knowledge::kMaxAsks), // 问够了 → 不问
                mk(203, "candidate", "Q4",      9, 0.9, knowledge::kMaxAsks + 3),
            };
            const auto q = knowledge::detect_gaps(v);
            if (q.size() != 2) {
                gap_ok = false;
                why = "asked_count 达到 kMaxAsks 的条目不该再被问（实际出了 "
                      + std::to_string(q.size()) + " 条）";
            }
        }

        // ⑪ **已经确认过的，即使历史里还留着降级记录也不再问**
        //
        // 这是实跑抓到的：用户答完"以后都用 Qwen ASR"之后那条已经是 confirmed，
        // 但历史里的 value_changed_demoted 还在，于是它下一场会话又是第 1 问 ——
        // 直接违反 §1.3「confirmed 且无变化一个字都不问」。
        {
            std::vector<KnowledgeWithHistory> v = {
                mk(210, "confirmed", "Qwen ASR", 2, 1.0, 0),   // 用户确认过 → 不该问
                mk(211, "candidate", "Qwen ASR", 2, 0.9, 0),   // 同形状但没确认 → 该问
            };
            add_hist(v[0], "Whisper large-v3", "Qwen ASR", "value_changed_demoted");
            add_hist(v[1], "Whisper large-v3", "Qwen ASR", "value_changed_demoted");
            const auto q = knowledge::detect_gaps(v);
            if (q.size() != 1 || q[0].knowledge_id != 211) {
                gap_ok = false;
                why = "confirmed 的条目不该因为历史里有降级记录而被再问";
            }
        }

        std::cout << "[SelfTest] 知识缺口检测（四规则/去重/优先级/上限/问过不再问）: "
                  << (gap_ok ? "✅ 11 例通过" : "❌ 失败") << std::endl;
        if (!gap_ok) {
            std::cerr << "    " << why << std::endl;
            return 1;
        }
    }

    // ---- 13) 会话结束的确认交互（§7 步骤 2.4）----
    //
    // 【这一组为什么用 stringstream 而不是手搓数据结构】
    // §8.8⑨ 的教训：用例的输入必须来自**真实路径**。
    // 所以这里不构造"理想的"问答对象，而是把回答**当用户敲的那样喂进
    // run_confirmation 的真实循环**，让 interpret_answer / apply_answer /
    // run_confirmation 全部真跑一遍，改动**真的落到数据库**上，再查回来断言。
    // 唯一被替换的是"从哪读一行"—— 而那正是为了可测而刻意抽出来的接缝。
    {
        using namespace knowledge;

        const std::string PRE = "__selftest_confirm_";
        const std::string CKEY = PRE + "marko";
        auto& ks = KnowledgeStore::instance();
        ks.purge_key_prefix(PRE);

        bool cf_ok = true;
        std::string why;

        // ---- 13a) 纯函数：一行输入 → 动作 ----
        struct AnsCase { const char* in; AnswerKind want; const char* value; };
        const AnsCase ac[] = {
            {"",            AnswerKind::Skip,      ""},
            {"   ",         AnswerKind::Skip,      ""},
            {"y",           AnswerKind::Affirm,    ""},
            {"YES",         AnswerKind::Affirm,    ""},
            {u8"是",        AnswerKind::Affirm,    ""},
            {u8"对",        AnswerKind::Affirm,    ""},
            // 【最关键的两条】"n"/"no" 必须被认成"否"，否则字面量 "no"
            // 会被当成用户给的新写法写进知识库，而且是 confirmed —— 它会进翻译约束。
            {"n",           AnswerKind::Reject,    ""},
            {"no",          AnswerKind::Reject,    ""},
            {u8"不是",      AnswerKind::Reject,    ""},
            // 新写法：两边空白要去掉
            {"  Marco  ",   AnswerKind::NewValue,  "Marco"},
            {u8"埃里卡",    AnswerKind::NewValue,  u8"埃里卡"},
            {"??",          AnswerKind::NewValue,  "??"},
            // 【真实路径抓到的 bug】PowerShell 往管道写第一行会带 UTF-8 BOM，
            // "\uFEFFy" 当时掉进了"其余一律当新值"分支 —— 于是字面量 "y"
            // 被当成专名写进库，而且是 confirmed（会进翻译约束）。
            // 这些用例全部来自那次实跑，不是我编的。
            {"\xEF\xBB\xBFy",      AnswerKind::Affirm,   ""},
            {"\xEF\xBB\xBFMarco",  AnswerKind::NewValue, "Marco"},
            {"\xEF\xBB\xBF",       AnswerKind::Skip,     ""},
            {"\xE3\x80\x80y\xE3\x80\x80", AnswerKind::Affirm, ""},   // 全角空格
            {"\xE2\x80\x8BMarco",  AnswerKind::NewValue, "Marco"},   // 零宽空格
            // 粘进来一整句话 → 退化成 Skip（宁可不记，不可记错）
            {"I think it should be Marco, the previous one was misheard I believe",
             AnswerKind::Skip, ""},
        };
        int ac_pass = 0;
        for (const auto& c : ac) {
            const Answer a = interpret_answer(c.in);
            if (a.kind == c.want && a.value == c.value) { ++ac_pass; continue; }
            cf_ok = false;
            why = std::string("输入解释不对: '") + c.in + "'";
        }
        std::cout << "[SelfTest] 确认交互·输入解释（是/否/新值/跳过）: "
                  << (cf_ok ? "✅ " : "❌ ") << ac_pass << "/"
                  << (sizeof(ac) / sizeof(ac[0])) << " 通过" << std::endl;

        // ---- 13b) 循环 + 落库：走真实路径 ----
        if (cf_ok) {
            auto seed = [&](const std::string& key, const char* value,
                            const char* status, int hits, double conf) -> long long {
                KnowledgeItem it;
                it.kind        = "person";
                it.key         = key;
                it.value       = value;
                it.status      = status;
                it.confidence  = conf;
                it.source_text = u8"自检造的候选";
                std::string e;
                const long long id = ks.upsert(it, &e);
                // hits 直接写到位（走 SQL，不改库的语义）
                for (int i = 1; i < hits; ++i) ks.upsert(it, &e);
                return id;
            };

            seed(CKEY,               "Marko",   "candidate", 1, 0.35);  // 低置信度 → 该问
            seed(PRE + "phoenix",    "Phoenix", "candidate", 5, 0.90);  // 高频未确认 → 该问
            seed(PRE + "keep",       "EnglishPod", "confirmed", 9, 0.95); // confirmed → 不该问

            auto qs = detect_gaps_from_store(kMaxQuestionsDefault);
            // 只留本组造的（库里可能有别的组留下的数据）
            std::vector<GapQuestion> mine;
            for (const auto& q : qs) {
                if (q.key.rfind(PRE, 0) == 0) mine.push_back(q);
            }
            if (mine.size() != 2) {
                cf_ok = false;
                why = "本组应恰好出 2 个问题（confirmed 的那条不该出），实际 "
                      + std::to_string(mine.size());
            } else {
                // ① 高频未确认那条 → 直接给新写法（Phoenix → Fenix）
                // ② 低置信度那条   → 答 y，认可当前值
                //
                // 【为什么不写死 "y\nFenix\n"】第一版就是这么写的，结果挂在
                // "答新写法之后值应变 Fenix，实际 Phoenix" —— 因为出题是按优先级排的，
                // 高频未确认(3) 排在低置信度(4) 前面，于是 "y" 答给了 Phoenix、
                // "Fenix" 答给了 Marko。**断言没错，是我把顺序想当然了。**
                // 改成按规则生成回答，顺序怎么变都不影响这一组。
                std::string script;
                for (const auto& q : mine) {
                    script += (q.rule == GapRule::HighFreqUnconfirmed) ? "Fenix\n" : "y\n";
                }
                std::istringstream scripted(script);
                std::ostringstream sink;
                const auto st = run_confirmation(
                    mine,
                    [&](std::string& line) -> bool {
                        if (!std::getline(scripted, line)) return false;
                        return true;
                    },
                    sink, kMaxQuestionsDefault);

                if (st.asked != 2 || st.confirmed != 2 || st.failed != 0) {
                    cf_ok = false;
                    why = "问答统计不对: asked=" + std::to_string(st.asked) +
                          " confirmed=" + std::to_string(st.confirmed) +
                          " failed=" + std::to_string(st.failed);
                }

                KnowledgeItem got;
                // 认可用当前值 → confirmed，值不变
                if (!ks.get("person", PRE + "marko", &got) || got.status != "confirmed") {
                    cf_ok = false;
                    why = "答 y 之后 Marko 应变 confirmed";
                } else if (got.value != "Marko") {
                    cf_ok = false;
                    why = "答 y 不该改值";
                }

                // 给了新写法 → confirmed 且值是新的
                if (!ks.get("person", PRE + "phoenix", &got) || got.status != "confirmed") {
                    cf_ok = false;
                    why = "答新写法之后 Phoenix 应变 confirmed";
                } else if (got.value != "Fenix") {
                    cf_ok = false;
                    why = "答新写法之后值应变 Fenix，实际 " + got.value;
                }

                // 【必须验的一步】答过的、以及 confirmed 的，**下一轮不能再问**
                // —— 这是 §1.3 提问稀缺的回归线：问过就沉底。
                auto again = detect_gaps_from_store(kMaxQuestionsDefault);
                for (const auto& q : again) {
                    if (q.key.rfind(PRE, 0) != 0) continue;
                    // 只有在"问过一次"的额度还没用完时才允许再出现；
                    // 已 confirmed 的一律不允许（confirmed 且无变化 = 一个字都不问）
                    cf_ok = false;
                    why = "已确认的条目第二次仍被问：" + q.key;
                }
            }
        }

        // ---- 13c) 跳过也要计数（问过就沉底）----
        //
        // 用户连按回车的意思就是"别再问了"。如果只在"答了"才计数，
        // 同一个问题会永远排在候选里 —— 稀缺性直接失效。
        if (cf_ok) {
            std::istringstream scripted("\n\n");
            std::ostringstream sink;
            auto qs = detect_gaps_from_store(kMaxQuestionsDefault);
            std::vector<GapQuestion> mine;
            for (const auto& q : qs) if (q.key.rfind(PRE, 0) == 0) mine.push_back(q);
            if (!mine.empty()) {
                run_confirmation(mine, [&](std::string& l) {
                    return static_cast<bool>(std::getline(scripted, l));
                }, sink, kMaxQuestionsDefault);

                KnowledgeItem got;
                if (ks.get("person", PRE + "marko", &got) && got.asked_count < 1) {
                    cf_ok = false;
                    why = "跳过也必须记 asked_count（否则同样的问题永远再问）";
                }
            }
        }

        ks.purge_key_prefix(PRE);

        std::cout << "[SelfTest] 确认交互·循环与落库（认可/新值/跳过计数）: "
                  << (cf_ok ? "✅ 通过" : "❌ 失败") << std::endl;
        if (!cf_ok) {
            std::cerr << "    " << why << std::endl;
            return 1;
        }
    }

    std::cout << "[SelfTest] 最近会话：" << std::endl;
    for (const auto& s : store.list_sessions(5)) {
        std::cout << "   #" << s.id << "  " << s.started_at << " ~ "
                  << (s.ended_at.empty() ? "(未结束)" : s.ended_at)
                  << "  engine=" << s.engine
                  << "  note=" << s.note
                  << "  segments=" << s.segment_count << std::endl;
    }

    store.close();
    std::cout << "[SelfTest] ✅ 全部通过" << std::endl;
    return 0;
}


int main(int argc, char** argv) {
    //  设置控制台为 UTF-8 编码，防止中文乱码
    system("chcp 65001");

    AppConfig cfg = AppConfig::from(argc, argv);
    cfg.dump();
    if (cfg.list_only) return 0;
    if (cfg.selftest)  return run_selftest(cfg);
    if (cfg.demo_session) return run_demo_session(cfg);
    if (cfg.test_window)  return run_test_window(cfg);
    if (cfg.show_gaps)    return run_show_gaps(cfg);
    if (cfg.extract_session >= 0) return run_extract(cfg);
    if (cfg.tools_list_only || !cfg.tools_name.empty()) return run_tools(cfg);
    if (!cfg.dump_prompt.empty()) return run_dump_prompt(cfg);
    if (cfg.export_session >= 0) return run_export(cfg);

    if (!cfg.whisper_ready()) {
        std::cerr << "[Error] Whisper 模型不存在: " << cfg.whisper_model << "\n"
                  << "        可用 --whisper <路径> 或环境变量 AT_WHISPER_MODEL 指定。" << std::endl;
        return -1;
    }

    SessionStore::instance().init(cfg.db_path);

    std::cout << "[System] 系统启动中，正在预热基础引擎..." << std::endl;

            // 初始化 Whisper 语音引擎
    //
    // 计时是**产品指标**，不是调试信息：这段时间用户是听不到任何东西的。
    // 实测（RTX 4060 Laptop + large-v3）加载要 ~100 秒，比一开始估的"20~40 秒"长得多，
    // 也就是"打开就听"在加载完成前是不成立的。把这个数字打在屏幕上，
    // 既是给用户的解释，也是以后做优化时的基线。
    const auto t_load0 = std::chrono::steady_clock::now();
    SpeechEngine engine;
    if (!engine.init(cfg.whisper_model)) {
        std::cerr << "[Error] Whisper模型加载失败！" << std::endl;
        return -1;
    }
    const auto load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t_load0).count();
    std::cout << "[System] Whisper 加载完成，耗时 " << load_ms << " ms"
              << "（这段时间还在预热，尚未开始记录）" << std::endl;
    // 语言策略：默认"首次检测后锁定"，不再每段重新检测。
    // 实测日志显示 auto 在 3 秒碎片上经常只有 20% 把握，判错一次整段识别就废了。
    engine.set_language_policy(cfg.source_lang, cfg.lang_recheck_sec);

    // 术语：一份列表，三个用途——
    //   ① 喂给 Whisper 作为 initial_prompt（识别时偏向专名）
    //   ② 交给 TermFixer 做识别后纠错（实测提示的约束力不够，名字仍会乱跳）
    //   ③ 作为翻译约束下发给翻译器（统一译文里的专名写法）
    //
    // 【来源】**知识库里 confirmed 的条目为主，--glossary 文件为辅。**
    // 2.5 之前这里只读文件 —— 也就是说用户确认过的知识一个字都影响不到识别和翻译，
    // 第二句承诺（用得越久越懂你）没有落点。
    // --glossary 保留：它是"我知道我要说什么、先手工喂给你"的显式入口，仍然有用。
    const auto kb = load_constraints_from_knowledge();
    std::vector<std::string> glossary = kb.terms;
    {
        const auto from_file = load_glossary_terms(cfg.glossary_path);
        for (const auto& t : from_file) {
            bool dup = false;
            for (const auto& e : glossary) {
                if (knowledge::normalize_key(e) == knowledge::normalize_key(t)) { dup = true; break; }
            }
            if (!dup) glossary.push_back(t);
        }
    }
    if (kb.store_ready) {
        std::cout << "[知识库] confirmed 条目 " << kb.confirmed_total << " 条，"
                  << "其中可用作识别提示/翻译约束的词条 " << kb.terms.size() << " 条";
        if (kb.terms.size() < kb.confirmed_total) {
            std::cout << "（其余是事实/决定类，只作摘要背景）";
        }
        std::cout << std::endl;
    }
    // 把**实际送进引擎的那串 prompt** 打出来，而不是只报"取到了几条"。
    // 区别很重要：报条数只能说明"我们打算喂什么"，打印字符串才说明"引擎真收到了什么"。
    // （诊断工具撒谎的坑这个项目踩过两次，见 run_dump_prompt 的注释。）
    {
        const std::string prompt = terms_to_prompt(glossary);
        if (!prompt.empty()) {
            constexpr size_t kShow = 160;
            std::cout << "[识别提示] initial_prompt = "
                      << prompt.substr(0, kShow)
                      << (prompt.size() > kShow ? " ...(截断)" : "") << std::endl;
        }
        engine.set_initial_prompt(prompt);
    }

    TermFixer term_fixer;
    term_fixer.set_terms(glossary);

    engine.start(); // 让推理线程在后台长亮待机

    while(1) {
        // 1. 选择翻译后端。
        //
        // **不再问用户**：产品的承诺是"打开就听"，开始记录之前先答一道选择题
        // 直接违背它。选择改由 --translator 决定，默认 local（离线、免费、数据不出本机）。
        //
        // 【曾经的现象】无参启动后被一道选择题挡住，必须敲回车才开始录
        // 【为什么必须去掉】① 违背"打开就听"；② 它阻塞在 getline 上，
        //   自动化（--wav / 脚本 / 批处理）没法应答；
        //   而且实测 process 内的 system("chcp 65001") 在 stdin 被重定向时
        //   会把管道里的输入吃掉，getline 直接拿到 EOF 退出
        // 【判断】若又出现"等你输入"的行为，先查这里是不是把 getline 加回来了
        std::string why;
        const int choice = choose_translator_backend(cfg, why);
        std::cout << "\n[翻译后端] " << (choice == 1 ? "DeepSeek 云端" : "本地混元")
                  << " —— " << why << std::endl;

        std::unique_ptr<ITranslator> translator;
        if(choice == 2) {
            auto hy = std::make_unique<HunyuanTranslator>(cfg.hunyuan_model);
            if(!hy->init()) {
                std::cerr << "[Error] Hunyuan翻译器初始化失败!" << std::endl;
                return -1;
            }
            translator = std::move(hy);
        } else {
            // choose_translator_backend 保证走到这里时 key 一定非空。
            // 万一不成立也**不能 continue** —— 选择题已经删掉，下一轮会得到完全相同的
            // 决策，continue 就是原地死循环。宁可报错退出。
            if (cfg.deepseek_api_key.empty()) {
                std::cerr << "[Error] 云端后端缺少 API Key（内部逻辑异常）" << std::endl;
                return -1;
            }
            translator = std::make_unique<DeepSeekTranslator>(cfg.deepseek_api_key);
        }

        // 术语约束：让译文里的专名保持同一写法。
        //
        // 【为什么必须加】术语表现在只喂 Whisper（识别时偏向这些词）
        // 和 TermFixer（识别后纠错），**对译文输出零约束**。
        // 实测会话 #11：同一个 Erica，第 1 句保留成 "Erica"、第 2 句被音译成「埃里卡」。
        // 这两条腿只保护"听得对"，不保护"译得一致"。
        //
        // ⚠️ **必须在 start() 之前设置**：这两个字段是 worker 线程构造 prompt 时读的。
        // 放到 start() 之后虽然实际也来得及（第一条文本更晚才 push），
        // 但那是"靠时序侥幸"，一旦以后有人改成启动即预热就会变成数据竞争。
        translator->set_target_language(cfg.target_lang);   // 译文语言
        translator->set_glossary(glossary);
        if (!glossary.empty()) {
            std::cout << "[术语] 已作为翻译约束下发（" << glossary.size()
                      << " 条），用于统一译文里的专名写法" << std::endl;
        }

        translator->start();   // 统一由接口启动工作线程，不再需要向下转型

        // 开启一场会话：此后所有翻译记录都归入这场会话，
        // 作为纪要/行动项等交付物的数据基础。
        const long long session_id = SessionStore::instance().begin_session(translator->name());
        if (session_id < 0) {
            std::cerr << "[Warn] 会话创建失败，本次记录将不会被保存" << std::endl;
        } else {
            std::cout << "[Session] 已开启会话 #" << session_id
                      << "（引擎: " << translator->name() << "）" << std::endl;
        }


        std::cout << "\n>>> 已开始记录 <<<" << std::endl;
        std::cout << ">>> 结束并生成纪要: " << SubtitleWindow::hotkey_hint()
                  << "   (在控制台按 Q 也可以) <<<" << std::endl;
        std::cout << "------------------------------------------------" << std::endl;

        // 半透明悬浮字幕窗。
        // 看视频/上课时需要"边看画面边看字幕"，控制台窗口会把画面挡住。
        SubtitleWindow subtitle;
        const bool have_window = subtitle.start();
        if (have_window) {
            subtitle.set_status(std::string(u8"● 记录中   结束: ") + SubtitleWindow::hotkey_hint());
        } else {
            std::cout << "[提示] 悬浮字幕窗创建失败，字幕将只显示在控制台" << std::endl;
        }
        int last_translation_count = 0;
        // 正文里当前显示的原文与译文。
        // 用来判断到达的译文是否属于这一句——识别与翻译是两条独立异步流，
        // 翻译总滞后一句，用 get_last_source() 比对才能配对。
        std::string displayed_source;
        std::string displayed_translation;
        // 已经离开正文、但译文还在路上的那一句的原文。
        // 译文迟到时靠它把"原文→译文"补进历史，否则这一句就整条丢了。
        std::string pending_source;

        // 状态行文案统一在这里拼，避免多处拼装导致口径不一致。
        // 译文没出来之前，这里显示"翻译中…"，而不是把它塞进正文——
        // 正文里放占位文字会先显示"翻译中…"再被译文替换，就是一次明显的闪烁。
        auto status_line = [&](const std::string& tail) {
            return std::string(u8"● 记录中   ")
                 + std::to_string(engine.get_inference_count()) + u8" 句   "
                 + tail
                 + u8"   结束: " + SubtitleWindow::hotkey_hint();
        };
        // 音频来源：系统音频环回（对方/视频），可选叠加麦克风（你/房间）。
        //
        // --wav 模式下**完全不初始化音频设备**：测试入口不该依赖声卡，
        // 否则"没有音频输出设备的机器上跑不了回归"——那就失去可脚本化的意义了。
        std::unique_ptr<AudioCapture> capture_mic;
        AudioCapture capture_sys(CaptureSource::SystemLoopback);
        if (!cfg.wav_path.empty()) {
            // --wav 下麦克风通道无意义（音频来自文件）。显式告知，不要静默忽略 ——
            // 用户传了 --mic 却听不到自己的声音，会以为麦克风坏了。
            if (cfg.enable_mic) {
                std::cout << "[提示] --mic 在 --wav 模式下不生效（音频来源是文件，不是声卡）"
                          << std::endl;
            }
            std::cout << "[Audio] --wav 模式：不打开任何音频设备，只回放文件" << std::endl;
        } else {
            if (!capture_sys.init()) {
                std::cerr << "[Error] 系统音频设备初始化失败！" << std::endl;
                return -1;
            }
            capture_sys.start();

            if (cfg.enable_mic) {
                capture_mic = std::make_unique<AudioCapture>(CaptureSource::Microphone);
                if (!capture_mic->init()) {
                    std::cerr << "[警告] 麦克风初始化失败，本次只采集系统音频" << std::endl;
                    capture_mic.reset();
                } else {
                    capture_mic->start();
                    std::cout << "[Audio] 双路采集已启用（系统音频 + 麦克风）" << std::endl;
                }
            }
        }

        // --- 核心逻辑变量 ---
        std::vector<float> audio_accumulator;      // 音频累加缓冲区(后续换成环形缓冲区)
        const int sample_rate = 16000;             // Whisper 标准采样率
        const float trigger_seconds = 3.0f;        // 攒够3秒音频再进行一次推理
        const size_t trigger_size = static_cast<size_t>(sample_rate * trigger_seconds);

        // ---- WAV 回放模式（--wav）----
        //
        // 关键设计：**只替换"音频从哪来"，循环体其余部分一行不改**。
        // 如果为测试另写一条管线，它迟早会和真实路径分叉——
        // 这正是 §8.8② 踩过的坑（--test-window 自己编时序，结果演示通过、真实路径是坏的）。
        //
        // 分块大小取 160 采样 = 10ms，与 WASAPI 回调的典型周期一致。
        // 这一点必须对齐：循环里的静音计数是"多少个连续静音块"，块变大会让
        // 分句边界整体变长，测出来的东西就和实时路径不是一回事了。
        std::vector<float> wav_pcm;
        size_t             wav_pos = 0;
        size_t             wav_silence_left = 0;    // 文件放完后还要喂多久静音
        bool               wav_flushing = false;    // 是否已进入收尾阶段
        // 分块取 1600 采样 = 100ms。**这个数字不是随便定的**：
        // 实时路径每轮结尾有 sleep(100ms)，而 miniaudio 回调每 ~10ms 往里填一次，
        // 所以主循环一次 get_buffer_and_clear() 实际拿到的是 ~100ms 的音频。
        // 回放必须对齐这个量，否则循环里"多少块连续静音"的计数语义会差 10 倍：
        // 实测按 10ms 喂时 silence_count>20 只等于 200ms 静音（实时是 2 秒），
        // 分句边界和生产完全不同，A/B 对照就不成立了。
        const size_t       wav_block = static_cast<size_t>(sample_rate / 10);   // 100ms
        if (!cfg.wav_path.empty()) {
            const WavReader::Result wr = WavReader::read_file(cfg.wav_path, sample_rate);
            if (!wr.ok) {
                std::cerr << "[WAV] 读取失败：" << wr.error << std::endl;
                return -1;
            }
            wav_pcm = wr.samples;
            std::cout << "[WAV] " << cfg.wav_path << " -> "
                      << wr.src_rate << "Hz " << wr.src_channels << "ch "
                      << wr.src_bits << "bit，共 " << wr.src_seconds << " 秒；"
                      << "已转为 16kHz 单声道 " << wav_pcm.size() << " 个采样点" << std::endl;
            if (wr.src_rate != sample_rate) {
                std::cout << "[WAV] 注意：源采样率不是 16kHz，已线性插值重采样"
                             "（未做抗混叠滤波，人声内容影响可忽略）" << std::endl;
            }
        }


        float silence_threshold = 0.001f;
        int silence_count = 0;
        bool has_speech = false;

        // 用推理序号判断"是否有新结果"，而不是比较文本。
        // 原实现用 result != last_displayed_text，当两段识别出同样文字时会漏掉。
        int last_inference_count = engine.get_inference_count();
        std::string last_raw_text;   // 上一段的原始识别结果，用于重叠去重

        while (true) {
            // A. 退出检测：全局热键优先（看全屏视频时控制台没有焦点，
            //    只有系统级热键才收得到），控制台 Q 作为备用
            if (subtitle.quit_requested()) {
                std::cout << "\n[System] 收到结束热键 " << SubtitleWindow::hotkey_hint() << std::endl;
                break;
            }
            if (_kbhit()) {
                char ch = static_cast<char>(_getch());
                if (ch == 'q' || ch == 'Q') break;
            }

            // B. 取得音频片段
            //
            // 这里是**两条路径唯一的交汇点**：
            //   实时：从系统环回取，可选叠加麦克风
            //   回放（--wav）：从文件里按 10ms 一块取
            // 往下的 VAD、推理投递、文本清洗、翻译配对、落库、字幕推送全部共用。
            std::vector<float> pcm_chunk;
            if (!wav_pcm.empty()) {
                if (wav_pos < wav_pcm.size()) {
                    // 背压：队列满时 push_audio 会**丢弃最旧音频段**，
                    // 那样测试会静默丢内容。所以这里等一等，别把引擎喂爆。
                    while (engine.get_queue_depth() >= 5) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    const size_t n = std::min(wav_block, wav_pcm.size() - wav_pos);
                    pcm_chunk.assign(wav_pcm.begin() + static_cast<ptrdiff_t>(wav_pos),
                                     wav_pcm.begin() + static_cast<ptrdiff_t>(wav_pos + n));
                    wav_pos += n;
                    // 刻意不用 sleep 模拟实时：分句只取决于"多少采样点、多少块"，
                    // 不取决于墙上时钟；块大小已按实时路径对齐（100ms），
                    // 所以快速回放的切分结果与实时一致。
                } else {
                    // 文件放完了 —— **不能直接 break**。
                    // stop() 里 run_inference_loop 是"先判 is_running_ 再取队列"，
                    // 直接退出会把队列里还没推理的段全部丢掉，
                    // 表现为"WAV 最后 1~2 句没进纪要"。所以改喂静音：
                    //   ① 静音触发 VAD 把最后一句正常收尾（silence_count > 20）
                    //   ② 循环继续跑，识别结果照常被翻译、落库、推字幕
                    //   ③ 队列排空后才退出
                    if (!wav_flushing) {
                        wav_flushing = true;
                        wav_silence_left = static_cast<size_t>(sample_rate) * 5;   // 5 秒
                        std::cout << "\n[WAV] 回放完毕，喂 5 秒静音让最后一句收尾并排空队列..."
                                  << std::endl;
                    }
                    if (wav_silence_left > 0) {
                        const size_t n = std::min(wav_block, wav_silence_left);
                        pcm_chunk.assign(n, 0.0f);
                        wav_silence_left -= n;
                    } else if (engine.get_queue_depth() == 0) {
                        std::cout << "[WAV] 队列已排空，结束回放" << std::endl;
                        break;
                    } else {
                        pcm_chunk.assign(wav_block, 0.0f);   // 继续喂静音，等队列排空
                    }
                }
            } else {
                pcm_chunk = capture_sys.get_buffer_and_clear();
                if (capture_mic) {
                    mix_audio(pcm_chunk, capture_mic->get_buffer_and_clear());
                }
            }


            if (pcm_chunk.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            // 将当前采集到的一丁点声音放入“大池子”
            audio_accumulator.insert(audio_accumulator.end(), pcm_chunk.begin(), pcm_chunk.end());

            float sum_squares = 0.0f;
            for (float sample : pcm_chunk) {
                sum_squares += sample * sample;
            }
            float rms = std::sqrt(sum_squares / pcm_chunk.size());

            // 
            //拦截部分
                if (rms <= silence_threshold) {//小于设计的阈值说明没有说话
                    silence_count++;
                    // 池子里有超过 1 秒的声音，就说明一句话说完了
                    if (silence_count > 20 && audio_accumulator.size() > sample_rate * 1 && has_speech) {
                        engine.push_audio(audio_accumulator);
                        audio_accumulator.clear();
                        silence_count = 0;
                        has_speech = false;
                        continue;
                    }
                }
                else {
                    has_speech = true;
                    silence_count = 0;
                }

            // C. 检查是否达到推理长度阈值
            if (audio_accumulator.size() >= trigger_size && has_speech) {
                // 将攒够的 3 秒音频通过生产者接口塞入 SpeechEngine
                // 这里使用的是 std::move 来减少一次内存拷贝，
                engine.push_audio(audio_accumulator);

                // 发送后保留一段尾巴作为上下文。
                //
                // 这个值试过两头：
                //   1.5s —— 整句被相邻两段各识别一次，出现大段重复
                //   0.3s —— 够去重了，但词被切在边界两侧（"to talk" | "about"），
                //           每段都是残句，翻译质量下降
                // 现在取 0.7s：既能补全被切断的词，多出来的重复交给
                // strip_overlap() 处理（它已经能处理标点差异和功能词错位）。
                const float keep_seconds = 0.7f;
                const size_t keep_size = static_cast<size_t>(sample_rate * keep_seconds);

                if (audio_accumulator.size() > keep_size) {
                    // 用 vector 的迭代器，把最后 keep_size 长度的数据切下来
                    std::vector<float> tail(audio_accumulator.end() - keep_size, audio_accumulator.end());
                    // 把大池子替换成这个尾巴，剩下的丢弃
                    audio_accumulator = std::move(tail);
                }
                else {
                    audio_accumulator.clear(); // 兜底：如果不足 keep_size（极少发生），才全清
                }
            }

            const int count_now = engine.get_inference_count();

            // D0. 轮询译文，推给悬浮窗。
            //
            // ⚠️ 这一步必须**每轮都执行**。之前它被放在下面"有新识别结果"的判断之后，
            // 而那个分支里有个 continue，导致轮询只在刚换原文的那一刻跑一次——
            // 那时译文还没出来，来源必然不匹配，之后就再也不检查了，
            // 表现为窗口永远停在"翻译中…"。
            if (have_window) {
                const int tc = translator->get_translation_count();
                if (tc != last_translation_count) {
                    last_translation_count = tc;
                    const std::string tsrc = translator->get_last_source();
                    const std::string ttxt = translator->get_last_translation();
                    // 只有「译文的来源 == 当前显示的原文」才更新正文；
                    // 不匹配说明这是上一句的译文，显示到正文就会错配。
                    if (!displayed_source.empty() && tsrc == displayed_source) {
                        displayed_translation = ttxt;
                        subtitle.set_translation(ttxt);
                        subtitle.set_status(status_line(
                            u8"翻译 " + std::to_string(translator->get_last_api_ms()) + u8"ms"));
                        // ⚠️ 这里**不要** push_history。
                        // 一入历史，历史里最新的一条就是"当前这句"，
                        // 而顶部行显示的正是历史里最新的一条 →
                        // 顶部行和正文变成同一句话，屏幕上就是"重复了一遍"。
                        // 入历史的时机是下一句顶掉正文的时候（见下面"新句子"处）。
                    } else if (!ttxt.empty() && !pending_source.empty() && tsrc == pending_source) {
                        // 上一句的译文迟到了：它的原文早被下一句顶掉，正文里没有它的位置，
                        // 直接补进历史（顶部行显示的就是它）。
                        subtitle.push_history(tsrc, ttxt);
                        pending_source.clear();
                    }
                }
            }

            // D. 获取并显示最新文字结果
            if (count_now == last_inference_count) {
                // 没有新的识别结果，直接进入下一轮（译文轮询已在上面做过）
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            last_inference_count = count_now;

            std::string raw = engine.get_last_text();
            const SpeechQuality quality = engine.get_last_quality();

            // 两级文本清洗：
            //   ① 折叠 Whisper 自身的段内重复循环
            //   ② 裁掉与上一段重叠的开头
            const std::string collapsed = collapse_repeats(raw);
            std::string result = strip_overlap(last_raw_text, collapsed);
            last_raw_text = collapsed;

            // ③ 术语纠错：把与已知专名只差一个字的识别结果替换掉
            //    （Marco -> Marko、Erikka -> Erika）
            std::vector<std::string> term_fixes;
            if (term_fixer.enabled()) {
                result = term_fixer.fix(result, &term_fixes);
            }

            bool is_hallucination = result.empty();
            if (result.length() < 4) {
                is_hallucination = true;
            }

            // 黑名单是 no_speech_prob 之外的兜底。
            // 表项一律小写，配合 to_lower_ascii(result) 做大小写不敏感匹配。
            static const std::vector<std::string> blacklist = {
                "谢谢观看", "请不吝点赞", "订阅", "打赏", "明镜与点点",
                "字幕", "amara", "[音乐]", "(音乐)", "翻译", "yoyo", "♪"
            };
            const std::string result_lower = to_lower_ascii(result);
            for (const auto& bad_word : blacklist) {
                // std::string::npos 意思是“没找到”
                if (result_lower.find(bad_word) != std::string::npos) {
                    is_hallucination = true;
                    break;
                }
            }

            if (!result.empty() && !is_hallucination) {
                const std::string src_lang = engine.get_language();

                std::cout << "\r[第" << count_now << "] " << result
                << " | 推理:" << engine.get_last_inference_ms() << "ms"
                << " 置信:" << (quality.confidence >= 0 ? quality.confidence : 0.0)
                << " 源:" << (src_lang.empty() ? "?" : src_lang)
                << "     " << std::flush;

                if (have_window) {
                    // 上一句被顶出正文 → 现在才进历史。
                    // 顶部行显示的正是"历史里最新的一条"，所以这一步就是
                    // 屏幕上"上一句译文向上滚一行"的来源。
                    // 反过来，如果一拿到译文就入历史，历史里最新的一条会变成
                    // 当前这句，顶部行和正文就成了同一句话（看起来像重复了一遍）。
                    if (!displayed_source.empty()) {
                        if (!displayed_translation.empty()) {
                            subtitle.push_history(displayed_source, displayed_translation);
                            pending_source.clear();
                        } else {
                            // 译文还没到，先记着原文，等它到了再补进历史
                            pending_source = displayed_source;
                        }
                    }
                    displayed_translation.clear();
                    // 正文换成新原文、译文清空（等这一句的译文）
                    subtitle.set_original(result);
                    subtitle.set_translation(std::string());
                    subtitle.set_status(status_line(u8"翻译中…"));
                }
                displayed_source = result;           // 记下当前显示的原文，供译文比对

                // 源语言与目标语言相同时跳过翻译：
                // 中文内容再"翻译成中文"纯属浪费，而且小模型在这时会乱改内容。
                if (!src_lang.empty() && src_lang == cfg.target_lang) {
                    SessionStore::instance().log_segment(result, result, "passthrough",
                                                         0, quality.confidence);
                    if (have_window) {
                        displayed_translation = result;   // 立即就有了译文，下次换句时入历史
                        subtitle.set_translation(result);
                        subtitle.set_status(status_line(u8"无需翻译"));
                    }
                } else {
                    // 把识别置信度一并传下去，最终落库
                    translator->push_text(result, quality.confidence);
                }
                // \r 会让光标回到行首，实现原地刷新的效果
                // 后面加一些空格是为了覆盖掉之前可能更长的文字
            }

            // 术语纠错日志：让用户看得见"它把 Erikka 改成了 Erika"，
            // 而不是悄悄改掉——出问题时也能一眼看出改了哪些
            for (const auto& r : term_fixes) {
                std::cout << "\n[术语] 已纠正 " << r << std::endl;
            }

            // E. 适当休眠，避免主线程空转占满 CPU
            //
            // 【为什么 --wav 模式不能睡】实测：11 秒的素材有 1100 块，
            // 每轮睡 100ms 就是 110 秒 —— 回放被拖成 0.1 倍速，
            // 一段 5 分钟的测试素材要跑 50 分钟，"可脚本化"就名存实亡了。
            // 这条 sleep 只对实时路径有意义（没声音时别空转），
            // 回放的节奏由背压（get_queue_depth）控制。
            if (wav_pcm.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }

        // 5. 资源清理
        std::cout << "\n\n[System] 正在结束本次记录..." << std::endl;
        subtitle.stop();
        if (capture_mic) capture_mic->stop();
        capture_sys.stop();
        translator->stop();
        SessionStore::instance().end_session();

        if (session_id >= 0) {
            const auto st = SessionStore::instance().stats(session_id);
            std::cout << "[Session] 会话 #" << session_id << " 已结束：共 "
                      << st.count << " 条记录，平均耗时 " << st.avg_ms << "ms" << std::endl;

            // ---- 产品的核心承诺：结束就给总结 ----
            if (st.count > 0) {
                std::cout << "\n[纪要] 正在生成..." << std::endl;
                const auto out = generate_deliverables(cfg, session_id);
                if (out.ok) {
                    std::cout << "[纪要] 完成：" << out.segment_count << " 条转录，"
                              << out.action_count << " 条行动项" << std::endl;
                    std::cout << "[纪要] 目录: " << out.dir << std::endl;
                    if (!out.html_path.empty() && cfg.wav_path.empty()) {
                        // --wav 是自动化测试入口，不要弹浏览器打断脚本
                        std::cout << "[纪要] 正在打开网页..." << std::endl;
                        open_with_default_app(out.html_path);
                    }
                } else {
                    std::cerr << "[纪要] 生成失败: " << out.error << std::endl;
                }
            } else {
                std::cout << "[纪要] 本次没有记录到内容，跳过生成" << std::endl;
            }

            // ---- 2.6 自动抽取 + 2.4 确认交互（顺序固定：先抽后问）----
            //
            // 【为什么放在 write() 之后】这是 §6.7 的硬约束，不是风格偏好：
            // 交付物已经落盘，用户在这里 Ctrl+C、或者干脆走开，
            // 会议纪要一个字都不会少。反过来就是"问了半天结果没生成纪要"——
            // 那是最不可原谅的失败方式，因为这个产品的核心承诺就是"结束就给总结"。
            //
            // 第二句承诺（用得越久越懂你）就是在这个函数里闭环的：
            // 抽取（落候选）→ 询问（用户确认）→ 下一场三条腿才有东西可用。
            learn_from_session(cfg, session_id, st.count > 0);
        }

        // 一次运行 = 一场会话：**结束就退出**。
        //
        // 【为什么不再循环回开头】外层 while(1) 原本的唯一用途是"结束后让用户换后端再开一场"。
        // 选择题删掉之后，回开头就是**立刻用同样的配置自动开下一场**——
        // 用户刚按完 Ctrl+Alt+Q 说"结束"，程序马上又开始录，那是坏行为
        // （何况再按一次热键会结束一场空会话，产生"本次没有记录到内容"）。
        // 【判断】若看到会话 #2 自动开起来，先查这个 break 被删了没有。
        // 代价：想连做两场要重新启动，模型需重新加载（实测见 §2.5）。
        std::cout << "\n[System] 本次会话已结束。要再记一场请重新运行程序。" << std::endl;
        break;
    }
    std::cout << "[System] 正在彻底关闭系统..." << std::endl;
    engine.stop();
    SessionStore::instance().close();
    return 0;
}