#include <iostream>
#include <vector>
#include <string>
#include <conio.h>    // 用于 _kbhit() 和 _getch()
#include <io.h>       // _isatty / _fileno：判断 stdin 是不是终端（确认交互要据此让路）
#include <chrono>
#include <thread>
#include <cmath>
#include <memory>                    // std::unique_ptr
#include <map>                       // 自检里的内存判断缓存

#include "ITranslator.h"
#include "HunyuanTranslator.h"
#include "DeepSeekTranslator.h"
#include "audio_capture.h"
#include "SpeechEngine.h"
#include "CandidateTriage.h"
#include "TriageCache.h"
#include "ActionStore.h"
#include "Evidence.h"
#include "CommonWords.h"
#include "ModelLog.h"
#include "SessionStore.h"
#include "KnowledgeStore.h"
#include "KnowledgeGap.h"
#include "ConfirmGaps.h"
#include "KnowledgeExtract.h"
#include "Utf8.h"
#include "AgentTool.h"
#include "AgentLoop.h"
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

// 术语清单的**唯一装配点**。
//
// 这段"知识库 confirmed + --glossary 文件、按归一化键去重合并"的逻辑
// 原本在三个地方各写了一遍（主路径 / run_dump_prompt / 别处）。
// 而本项目"诊断工具和真实路径走散"已经踩过三次（见 run_dump_prompt 的注释），
// 每一次都是因为同一段逻辑存在第二份拷贝。只要还有第二份，第四次就是时间问题。
//
// out_kb 可以为 nullptr；传入时会带回知识库侧的统计（条数/背景），
// 让调用方不必为了拿统计而再查一次库（再查一次就可能查出不同的结果）。
static std::vector<std::string> build_glossary(const AppConfig& cfg, ConstraintInputs* out_kb) {
    const auto kb = load_constraints_from_knowledge();
    if (out_kb) *out_kb = kb;

    std::vector<std::string> glossary = kb.terms;
    for (const auto& t : load_glossary_terms(cfg.glossary_path)) {
        bool dup = false;
        for (const auto& e : glossary) {
            if (knowledge::normalize_key(e) == knowledge::normalize_key(t)) { dup = true; break; }
        }
        if (!dup) glossary.push_back(t);
    }
    return glossary;
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
    size_t n_general = 0;
    for (const auto& k : all) {
        // 通用词要**标出来**，不能悄悄不问了事。
        // 用户看到"TV 在库里但从来没被问过"时，得有个地方能确认
        // "这是有意的"，而不是以为漏了。
        const bool gen = commonwords::is_general(k.key);
        if (gen) ++n_general;
        std::cout << "   #" << k.id << " [" << k.kind << "] " << k.key
                  << " = " << k.value
                  << "  status=" << k.status
                  << "  hits=" << k.hits
                  << "  conf=" << k.confidence
                  << (gen ? "   [通用词，不问]" : "") << std::endl;
    }
    if (n_general > 0) {
        std::cout << "   （通用词过滤表共 " << commonwords::size() << " 条，"
                  << "本库命中 " << n_general << " 条 —— 人人皆知的东西不占用你的注意力）"
                  << std::endl;
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
        // 走**同一条**渲染路径：诊断打出来的问句必须和真跑时一模一样。
        // 这里以前直接打 q.question，而那个字段已经删掉了 —— 就是为了不让
        // "该问什么"的代码有机会拼文案（拼了就会把内部状态漏给用户）。
        const auto prompt = knowledge::to_prompt(q);
        std::cout << "   " << ++n << ". [" << knowledge::to_string(q.rule) << "] "
                  << interaction::question(prompt) << std::endl
                  << "      " << interaction::hint(prompt) << std::endl;
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

    // ---- 行动项落库 + 跨会话聚合（5.5）----
    //
    // 【为什么正好在这里】上面那个 `sanitize_actions` 是**三条摘要后端唯一的汇合点**
    // （云端 / 本地 / 规则都要过它），所以校验完的行动项在这里就是"已经可信"的。
    // 在这之后落库，意味着库里永远不会有"被校验器否掉的句子"。
    //
    // 【为什么不在 DeliverableWriter::write 里写】它刻意是**纯函数**
    // （头文件写着"不访问数据库，便于单独测试"）。为了省一次调用破坏那个性质，
    // 换来的是"交付物渲染需要数据库"，以后单独测渲染就必须先建库。
    {
        std::vector<actions::Incoming> inc;
        inc.reserve(summary.actions.size());
        for (const auto& a : summary.actions) {
            actions::Incoming in;
            in.title      = a.task;
            in.owner      = a.owner;
            in.due        = a.due;
            in.evidence   = a.source;      // 来源原话 —— 5.7 Evidence 的落点
            in.source_seq = a.seq;
            inc.push_back(std::move(in));
        }
        if (!inc.empty()) {
            // 判同器暂时不接模型：① 确定性层（同一个 key）已经能覆盖
            // "上周说过的这周又提一次"这个主场景；② 接模型就要联网，
            // 而这一步跑在**会话结束的收尾路径**上 —— 收尾路径必须离线可用。
            // 留了接口（ingest 的 judge 参数），要接随时能接。
            const auto oc = actions::ActionStore::ingest(sid, inc, nullptr, "summary");
            if (oc.inserted || oc.merged || oc.linked) {
                std::cout << "[行动项] 新建 " << oc.inserted << " 条，"
                          << "并入已有 " << oc.merged << " 条"
                          << "（本轮新增关联 " << oc.linked << " 条）" << std::endl;
            }
            if (!oc.err.empty()) {
                // 落库失败**不能**让交付物生成失败：文件已经写好了，
                // 行动项表只是附加的长期记忆。说清楚但继续。
                std::cerr << "[行动项] 落库有问题: " << oc.err << std::endl;
            }
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
    const auto raw_questions = detect_gaps_from_store(kMaxQuestionsDefault, session_id);

    // ---- 分诊：把"人人皆知"的候选挡在提问之外（步骤 2.10）----
    //
    // 【放在这里而不是 detect_gaps 里】`detect_gaps` 是**纯函数**，必须确定性、
    // 能进 L1。而分诊的第三层是联网的模型 —— 不确定的东西不能进纯函数。
    // 所以分诊放在流程层，判断器是可注入的回调（自检喂脚本化判决）。
    //
    // 【判据只发候选词 + 一句证据，不发整场转录】
    // 这是这一层和"摘要上云"最大的区别，隐私面小得多。
    std::vector<GapQuestion> questions;
    {
        triage::Judge judge;
        std::string judge_state;
        if (cfg.deepseek_api_key.empty()) {
            judge_state = u8"没有 DeepSeek Key → 只用本地通用词表";
        } else {
            judge = triage::make_cloud_judge(cfg.deepseek_api_key);
            judge_state = u8"本地通用词表 + 云端判断（DeepSeek）";
        }

        // 第 ② 层：判断缓存（步骤 2.12）。
        //
        // `hooks()` 内部自己判断表在不在 —— 老库/库没打开时返回空 Cache，
        // decide() 会自动跳过这一层，会话照常。
        // **缓存是可选加速层，它的缺席绝不能影响主流程。**
        const triage::Cache cache = triage::TriageCache::instance().hooks();
        if (cache) judge_state += u8" + 判断缓存";

        size_t skipped = 0;
        size_t reclassified = 0;
        size_t from_cache = 0;
        for (const auto& q : raw_questions) {
            // **证据 = 它在转录里的那句原话**（2026-09-19 修正）。
            //
            // 原来这里传的是 `q.value`（词本身），我还在注释里写"人名/项目名的判断
            // 不依赖长上下文" —— **那句是错的**，后果是**静默挡掉用户最想被问的那类词**：
            //     用户的花名 `Penny`，孤立看是英文单词"便士" → 模型判"通用词" → 不问。
            // 而「我们的 PM Penny 说…」里的 `Penny` 一眼就是人名。
            // 这一步是"按类型问对的问题"能不能成立的前提。
            const std::string evidence = q.evidence.empty() ? q.value : q.evidence;

            const auto d = triage::decide(q.value, q.kind, evidence, judge, cache);
            if (d.source == "cache") ++from_cache;

            // **逐个候选打判决**，不是只报一个总数。
            // 验证"模型到底判了什么"必须能看见每一条 —— 报总数的话，
            // "模型判通用"和"模型调用失败"在输出上长得一模一样。
            std::cout << "[分诊] 「" << q.value << "」"
                      << (d.verdict == triage::Verdict::Skip ? u8"不问" : u8"问")
                      << u8" —— " << d.reason << u8"（" << d.source << u8"）";
            if (!d.kind.empty() && d.kind != q.kind) {
                std::cout << u8"  类型: " << q.kind << u8" → " << d.kind;
            }
            if (!d.primer.empty()) {
                std::cout << u8"  猜: " << d.primer;
            }
            std::cout << std::endl;

            if (d.verdict == triage::Verdict::Skip) {
                ++skipped;
                continue;
            }

            GapQuestion q2 = q;

            // 模型判出了更准的类型 → 改标签 + 问题按新类型走。
            //
            // 【为什么敢改】kind 只影响：① 显示用的 label ② 走哪条问法
            // ③ `is_name_like_kind`（person/project/product/term 四种都过）。
            // 它**不碰** status / hits / confidence —— 所以判错了代价只是
            // "标签和问法有点怪"，不会污染任何约束（见 KnowledgeStore::set_kind）。
            if (!d.kind.empty() && d.kind != q2.kind) {
                std::string ke;
                if (KnowledgeStore::instance().set_kind(q2.kind, q2.key, d.kind, &ke)) {
                    q2.kind = d.kind;
                    ++reclassified;
                } else {
                    std::cout << u8"      （类型没改： " << ke << u8"）" << std::endl;
                }
            }

            // 引子挂到问题上 → 交互层会把"它指什么"变成"我猜…。对吗？"
            q2.suggested_meaning = d.primer;
            questions.push_back(std::move(q2));
        }
        std::cout << "[分诊] " << raw_questions.size() << u8" 个候选 → 问 "
                  << questions.size() << u8" 个（挡掉 " << skipped;
        if (reclassified > 0) std::cout << u8"，重判类型 " << reclassified << u8" 个";
        // 单独报"其中几个是缓存命中的"：这是"第二场开始就不用联网了"的**现场证据**。
        // 混在"挡掉 N 个"里就看不出来了 —— 而这两件事的意义完全不同：
        // 一个是"这个判断本来就不必问"，一个是"这个判断这次没联网"。
        if (from_cache > 0) std::cout << u8"，其中 " << from_cache << u8" 个走缓存（未联网）";
        std::cout << u8"）。判据：" << judge_state << std::endl;
    }

    if (questions.empty()) {
        // 【为什么这里要说话，而不是按 §1.3 保持静默】
        // 真跑会话 #44 实测：结束日志里**一行 [确认] 都没有**，因为抽出的 3 个名字
        // 全都已在上一场确认过 → 没有可问的 → 静默返回。
        // 这行为**是对的**，但它和"抽取器坏了 / 库读不到"长得一模一样，
        // 排查时只能靠读代码。§1.3 说的是"别问没意义的问题"，
        // 不是"别告诉用户发生了什么" —— 打一行状态不算打扰。
        std::cout << "[确认] 没有需要确认的知识（本场抽出的条目都已在库里确认过，"
                     "或已被分诊挡掉）" << std::endl;
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
// --report "<任务>"：把一件事**派给 agent** —— 它自己决定调哪些工具、调几次，
// 然后给出带出处的结论（§7 步骤 5.6，依赖 5.2 的规划循环）。
//
// 【为什么这是整个第 5 阶段的落点】在它之前，产品的四份交付物都是
// "一场会话 → 一份文件"，**没有任何东西能跨会话回答一个问题**。
// 而用户真正想要的是：「整理过去一周关于凤凰项目的工作」这种**派活**。
//
// 【失败模式是本命令最要紧的部分】资料不足时它必须说"我查不到"，
// 绝不能编一份看起来完整的报告 —— 这直接决定赛题"结果交付与可验收性"
// 是加分还是负分。所以：
//   · 每次工具调用与结果都逐步打印（`--audit` 打的就是它）
//   · 全程没调过工具就给结论 → 明确标出来
//   · 预算耗尽 → 交半成品 + 说清为什么停
static int run_report(const AppConfig& cfg) {
    auto& store = SessionStore::instance();
    if (!store.init(cfg.db_path)) {
        std::cerr << "[报告] 打不开数据库: " << cfg.db_path << std::endl;
        return 1;
    }

    agent::ToolRegistry reg;
    agent::register_readonly_tools(reg);
    // 派活这条路**额外**给它"提议"能力（5.4）。正常会话路径不注册写工具 ——
    // 调用点一眼能看出"这一步给了写权限"。而且它只能写 candidate：
    // 见 AgentTool.h 里"闸门怎么保证"的三层说明。
    agent::register_write_tools(reg);

    agent::ToolContext ctx;
    ctx.deliverable_root = cfg.deliverable_dir;

    auto planner = agent::make_deepseek_planner(cfg.deepseek_api_key);
    if (!planner) {
        // **能力降级，不是崩溃** —— 说清楚，并指出退路。
        std::cout << "[报告] 没有配置 DeepSeek API Key，做不了多步查证。\n"
                     "        可以用 --search \"<关键词>\" 做单次检索（不需要 Key、不走网络）。"
                  << std::endl;
        SessionStore::instance().close();
        return 1;
    }

    std::cout << "[报告] 任务：" << cfg.report_goal << std::endl;
    std::cout << "[报告] 可用工具 " << reg.size() << " 个：";
    for (const auto& n : reg.names()) std::cout << n << " ";
    std::cout << "\n-------- 执行过程 --------" << std::endl;

    const auto r = agent::run(cfg.report_goal, reg, ctx, planner, agent::Budget(),
                              &std::cout);

    std::cout << "-------- 结果 --------" << std::endl;
    std::cout << r.answer << std::endl;
    std::cout << "\n[报告] " << r.steps_used << " 步 / " << r.tool_calls << " 次工具调用";
    if (r.partial) std::cout << "（未完成）";
    if (r.no_tools_used) std::cout << "  ⚠️ 全程没调用任何工具";
    std::cout << "\n[报告] 停止原因：" << r.stop_reason << std::endl;

    // ---- 出处核对（5.7）----
    //
    // 【为什么每次出报告都要跑一遍，而不是"信任模型"】
    // 系统提示里要求它给每句结论标出处，但**要求不等于做到**，
    // 而编出来的出处（`#9002·7` 当那一场只有 4 段）看起来和真的完全一样。
    // 所以这里当场核对，并且把结果**写在报告后面** —— 让人一眼看到
    // "这份东西有几句是可验证的、有没有编的"，而不是自己去抽查。
    //
    // ⚠️ 核对失败**不改变退出码**：报告本身是产出，出处有问题不等于任务失败。
    //    但它必须显眼 —— 所以用 ⚠️ 而不是静默。
    {
        const auto vr = evidence::Evidence::verify(r.answer);
        std::cout << "\n[报告] 出处核对：";
        if (vr.total == 0) {
            std::cout << "⚠️ 这份报告**一处出处都没标** —— 每句结论都无法回溯到原话。\n"
                         "        系统提示里要求过标出处，所以这通常意味着模型忽略了它；\n"
                         "        也可能这次任务本来就没有可引用的具体段落。"
                      << std::endl;
        } else {
            std::cout << "共 " << vr.total << " 处，有效 " << vr.ok
                      << " 处，对不上 " << vr.bad.size() << " 处" << std::endl;
            for (const auto& l : vr.bad) {
                std::cerr << "        ❌ " << evidence::format(l)
                          << " 指向的位置不存在（编造或写错）" << std::endl;
            }
            if (vr.bad.empty()) {
                std::cout << "        ✅ 每一处都可以回到库里查证" << std::endl;
            }
            std::cout << "        （复核：--verify-report <把报告存成的文件>）"
                      << std::endl;
        }
    }

    SessionStore::instance().close();
    return r.ok ? 0 : 1;
}

// --search "<关键词>"：单次检索长期记忆（§7 步骤 5.3）。
//
// ⚠️ **不能叫 `--ask`**：那个名字已经被"会话结束的确认交互开关"占了
// （`--ask` / `--no-ask`，见 AppConfig::AskMode）。计划文档里写的是
// "5.3 `--ask` 检索"，落地时必须改名 —— 否则两个语义撞在一个参数上，
// 谁先解析谁生效，行为取决于参数顺序。
//
// 【它是 agent 的**退化情形**，不是另一个功能】一次工具调用、零规划。
// 两个好处：① 不需要 Key、不联网也能用 ② 两条路径共用同一个工具实现，
// 不会出现"agent 查到的东西和 --search 查到的不同"这种走散。
static int run_search(const AppConfig& cfg) {
    auto& store = SessionStore::instance();
    if (!store.init(cfg.db_path)) {
        std::cerr << "[检索] 打不开数据库: " << cfg.db_path << std::endl;
        return 1;
    }

    agent::ToolRegistry reg;
    agent::register_readonly_tools(reg);
    agent::ToolContext ctx;
    ctx.deliverable_root = cfg.deliverable_dir;

    nlohmann::json args;
    args["query"] = cfg.search_query;
    const auto tr = reg.call("search_knowledge", args.dump(), ctx);
    if (!tr.ok) {
        std::cerr << "[检索] 失败：" << tr.error << std::endl;
        SessionStore::instance().close();
        return 1;
    }
    std::cout << "[检索] " << cfg.search_query << " —— " << tr.audit << std::endl;
    std::cout << tr.content << std::endl;
    SessionStore::instance().close();
    return 0;
}

// --extract <会话id> [--apply]：对**已经存下来的**一场会话跑一遍自动抽取。
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

    // 写工具（5.4）也注册进来，但**要说清它只在哪条路上有效**。
    //
    // 【为什么诊断里也要有它】项目的规矩是"诊断必须显示真实的东西"。
    // 真实情况是**两条路的工具集不一样**：正常会话只有只读，
    // `--report` 派活时额外有"提议"。诊断里只显示一半，会让人以为
    // agent 不能写；显示出来但不说明，又会让人以为会话路径也能写。
    // 所以：注册 + 明确标注生效范围。
    agent::register_write_tools(reg);

    agent::ToolContext ctx;
    ctx.deliverable_root = cfg.deliverable_dir;

    if (cfg.tools_list_only) {
        // 【不要把这个数字写死】原来写的是"5 个只读 + 1 个「提议」"，
        // 加了两个行动项工具之后它还那么打 —— 又是一处**会撒谎的硬编码**。
        // 总数用 reg.size()，两个分类从名字数出来，写死的只有"哪些算提议"这件事。
        int n_write = 0;
        for (const auto& n : reg.names()) {
            if (n.rfind("propose_", 0) == 0) ++n_write;
        }
        std::cout << "[Tools] 已注册 " << reg.size() << " 个工具"
                     "（" << (reg.size() - n_write) << " 个只读 + " << n_write
                  << " 个「提议」；**提议只在 --report 派活时可用**）："
                  << std::endl;
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
// --terms：不加载任何模型，打印"这次启动会把什么喂给识别和翻译"，然后退出。
//
// 存在的理由（这是它区别于"我读了一遍代码"的地方）：
// §6.5 红线的可观测面就是**那串 initial_prompt**。以前只有真跑一场会、
// 在启动日志里翻 `[识别提示]` 那一行才能看到它 —— 要录音、要等 Whisper 加载
// 上百秒，验一次的成本高到没人会验。所以库被污染了没人发现，
// 直到用户自己看日志问"这些词哪来的"。
//
// 这条命令几秒钟给同一个答案，而且走的是同一个 build_glossary()，
// 不可能出现"诊断看到的和真跑时不一样"。
static int run_dump_terms(const AppConfig& cfg) {
    SessionStore::instance().init(cfg.db_path);

    ConstraintInputs kb;
    const std::vector<std::string> glossary = build_glossary(cfg, &kb);

    std::cout << "库: " << cfg.db_path << std::endl;
    if (!kb.store_ready) {
        std::cout << "知识库未就绪（库不存在 / 未建表 / FTS5 未编入）。" << std::endl;
    } else {
        std::cout << "confirmed: " << kb.confirmed_total << " 条"
                  << "（其中可作识别提示/翻译约束 " << kb.terms.size() << " 条，"
                  << "摘要背景 " << kb.background.size() << " 条）" << std::endl;
    }

    std::cout << "\n--- ① 识别提示（Whisper initial_prompt）---" << std::endl;
    const std::string prompt = terms_to_prompt(glossary);
    if (prompt.empty()) {
        std::cout << "(空) —— 引擎不会收到任何 initial_prompt" << std::endl;
    } else {
        std::cout << prompt << std::endl;
    }

    std::cout << "\n--- ② 翻译约束（逐条下发）---" << std::endl;
    if (glossary.empty()) {
        std::cout << "(空)" << std::endl;
    } else {
        for (size_t i = 0; i < glossary.size(); ++i) {
            std::cout << "  [" << (i + 1) << "] " << glossary[i] << std::endl;
        }
    }

    std::cout << "\n--- ③ 摘要背景（进 user 内容，不参与约束）---" << std::endl;
    if (kb.background.empty()) {
        std::cout << "(空)" << std::endl;
    } else {
        for (const auto& b : kb.background) std::cout << "  " << b << std::endl;
    }

    // 结论行：给脚本一个可以 grep 的锚点（人看的是上面，脚本看的是这行）
    std::cout << "\n[terms] prompt_chars=" << prompt.size()
              << " glossary=" << glossary.size()
              << " background=" << kb.background.size() << std::endl;
    return 0;
}

// --triage-cache [--forget <词> | --clear]：看 / 撤销判断缓存（步骤 2.12）
//
// 【为什么这个命令是判断缓存能不能上线的**前提**，而不是附赠品】
// 缓存的行为是"某个词以后不再问模型"。它一旦判错，症状是**那个词再也不被问** ——
// 没有任何报错，用户只会觉得"系统怎么不学这个词"。所以一个会静默改变行为的
// 缓存，必须配一个能被看见、能被撤销的入口；否则它不是优化，是隐患。
//
// 和 `--terms` 一样，这条命令**不加载任何模型**，几秒钟出结果。
static int run_triage_cache(const AppConfig& cfg) {
    SessionStore::instance().init(cfg.db_path);
    auto& tc = triage::TriageCache::instance();

    if (!tc.ready()) {
        std::cout << "判断缓存不可用（库未打开，或这个库是 2.12 之前建的、"
                     "没有 triage_verdicts 表）。" << std::endl;
        std::cout << "【这不是错误】没有缓存时行为退化成："
                     "本地词表命中就不问，其余一律问模型。" << std::endl;
        std::cout << "要让老库长出这张表：把库删掉重建，或用任意一次会话启动它都会建表 —— "
                     "但**已存在的库不会补建**（CREATE TABLE IF NOT EXISTS 对"
                     "已建好的 schema 是空操作）。" << std::endl;
        return 1;
    }

    if (cfg.triage_clear) {
        const int n = tc.clear();
        std::cout << "已清空判断缓存：" << n << " 条。" << std::endl;
        std::cout << "（下次分诊会重新联网判断；判断结论会重新缓存。）" << std::endl;
        return 0;
    }

    if (!cfg.triage_forget.empty()) {
        std::string err;
        if (tc.forget(cfg.triage_forget, &err)) {
            std::cout << "已忘掉「" << cfg.triage_forget << "」。"
                      << "下次遇到它会重新联网判断。" << std::endl;
            return 0;
        }
        std::cerr << "忘掉失败：" << err << std::endl;
        std::cerr << "用 `--triage-cache` 看看里面到底存了哪些词。" << std::endl;
        return 1;
    }

    const auto st = tc.stats();
    std::cout << "库: " << cfg.db_path << std::endl;
    std::cout << "判断缓存: " << st.total << " 条，"
              << "累计复用 " << st.reused << " 次" << std::endl;

    // 【"省了多少次调用"是这一层唯一诚实的量化口径】
    // 它不是估算出来的，就是命中次数本身 —— 每次命中都少一次联网判断。
    if (st.reused > 0) {
        std::cout << "   也就是说：有 " << st.reused
                  << " 次判断没有联网（这些词以前判过）。" << std::endl;
    }
    if (st.stale > 0) {
        std::cout << "   其中 " << st.stale << " 条超过 90 天没被用过，"
                     "可以考虑 `--triage-cache --clear`。" << std::endl;
    }

    const auto rows = tc.list(200);
    if (rows.empty()) {
        std::cout << "\n（空的）—— 还没有任何词被模型判过「不必问」。" << std::endl;
    } else {
        std::cout << "\n存下来的判决（只存「不问」；「该问」不缓存，"
                     "由「问过不再问」负责）:" << std::endl;
        for (const auto& e : rows) {
            std::cout << "  · " << e.value;
            if (!e.kind.empty()) std::cout << "（" << e.kind << "）";
            std::cout << "  判于 " << e.judged_at;
            if (e.reused > 0) std::cout << "，复用 " << e.reused << " 次";
            if (!e.why.empty()) std::cout << "\n      理由: " << e.why;
            std::cout << std::endl;
        }
    }

    // 红线核对：判决不是知识，这张表里**一行都不该出现在识别提示/翻译约束里**。
    // 顺手打出来，因为"缓存有没有偷偷影响约束"是这个模块最值得怀疑的地方。
    std::cout << "\n--- 红线核对：判断缓存能不能影响识别/翻译 ---" << std::endl;
    {
        ConstraintInputs kb;
        const std::vector<std::string> glossary = build_glossary(cfg, &kb);
        std::cout << "当前可作识别提示/翻译约束的词条: " << glossary.size() << " 条" << std::endl;
        bool leaked = false;
        for (const auto& e : rows) {
            for (const auto& g : glossary) {
                if (knowledge::normalize_key(g) == knowledge::normalize_key(e.value)) {
                    std::cout << "   ⚠️ 缓存里的「" << e.value
                              << "」出现在约束里 —— 但它必然是**另有来源**"
                                 "（confirmed 知识），不是从这个缓存来的。" << std::endl;
                    leaked = true;
                }
            }
        }
        if (!leaked) {
            std::cout << "   ✅ 缓存里的 " << rows.size()
                      << " 个词，一个都没进识别提示/翻译约束（判决不是知识）" << std::endl;
        }
    }

    std::cout << "\n[triage-cache] total=" << st.total
              << " reused=" << st.reused << std::endl;
    return 0;
}

// --actions [--status X] [--done|--doing|--todo <id>]：行动项总表与状态流转（5.5）
//
// 【为什么需要一个"看"的入口】5.5 之前 `actions` 表**一行都没有过** ——
// 行动项只在生成交付物时在内存里活一次就没了。所以"跨会话追踪"从来没有落点：
// 上周会上定的事，这周没人记得。现在它落库了，就必须能被看见；
// 否则"你的待办里有 3 件事一直挂着"这句话没有任何人能验证。
//
// 不加载任何模型，几秒出结果。
static int run_actions(const AppConfig& cfg) {
    using actions::ActionStore;
    SessionStore::instance().init(cfg.db_path);

    // ---- 状态流转（用户的操作）----
    if (cfg.action_set_id >= 0 || !cfg.action_set_to.empty()) {
        if (cfg.action_set_id < 0) {
            std::cerr << "要改状态得给一个 id：--" << cfg.action_set_to
                      << " <id>。先用 `--actions` 看 id。" << std::endl;
            return 1;
        }
        std::string err;
        if (!ActionStore::set_status(cfg.action_set_id, cfg.action_set_to, &err)) {
            std::cerr << "改状态失败：" << err << std::endl;
            return 1;
        }
        actions::Item it;
        if (ActionStore::get(cfg.action_set_id, &it)) {
            std::cout << "已把 #" << it.id << " 标成 " << it.status
                      << "：「" << it.title << "」" << std::endl;
        } else {
            std::cout << "已把 #" << cfg.action_set_id << " 标成 "
                      << cfg.action_set_to << "。" << std::endl;
        }
        return 0;
    }

    // ---- 列表 ----
    const std::string filter = cfg.actions_status;
    if (!filter.empty() &&
        std::find(actions::valid_statuses().begin(), actions::valid_statuses().end(),
                  filter) == actions::valid_statuses().end()) {
        std::cerr << "未知状态「" << filter << "」（只能是 todo / doing / done）"
                  << std::endl;
        return 1;
    }

    const auto st = ActionStore::stats();
    std::cout << "库: " << cfg.db_path << std::endl;
    std::cout << "行动项: 共 " << st.total << " 条 —— "
              << "待办 " << st.todo << " / 进行中 " << st.doing
              << " / 已完成 " << st.done << std::endl;

    if (st.total == 0) {
        std::cout << "\n（空的）" << std::endl;
        // 空的时候要解释为什么空 —— 否则用户分不清"没有行动项"
        // 和"这个命令坏了 / 读错库了"。这正是 §1.3 说的"说话要有理由"。
        std::cout << "行动项是在**导出交付物**时落库的（会话结束时自动导出）。" << std::endl;
        std::cout << "所以：还没导出过任何一场会 → 这张表就是空的。" << std::endl;
        std::cout << "可以试：Translator.exe --export <会话id> --summarizer rules --db "
                  << cfg.db_path << std::endl;
        return 0;
    }

    const auto rows = ActionStore::list(filter, 500);
    std::cout << "\n" << (filter.empty() ? "全部" : filter) << "：" << rows.size()
              << " 条" << std::endl;
    for (const auto& a : rows) {
        std::cout << "  #" << a.id << "  [" << a.status << "]  " << a.title;
        if (!a.owner.empty()) std::cout << "   负责人: " << a.owner;
        if (!a.due.empty())   std::cout << "   截止: " << a.due;
        // "在几场会话里被提到"是跨会话聚合**唯一诚实的量化口径** ——
        // 它来自台账（action_sessions），不是估算。
        if (a.seen_sessions > 1) {
            std::cout << "   （在 " << a.seen_sessions << " 场会话里被提到";
            if (a.last_session > 0) std::cout << "，最近 #" << a.last_session;
            std::cout << "）";
        }
        std::cout << std::endl;
        if (!a.origin.empty() && a.origin != "summary") {
            std::cout << "        来源: " << a.origin << std::endl;
        }
        if (!a.evidence.empty()) {
            std::cout << "        原话: " << a.evidence << std::endl;
        }
    }

    std::cout << "\n改状态：--actions --doing <id> / --done <id> / --todo <id>" << std::endl;
    std::cout << "[actions] total=" << st.total << " todo=" << st.todo
              << " doing=" << st.doing << " done=" << st.done << std::endl;
    return 0;
}

// --verify-report <文件>：核对一份报告里**每一处出处**是否真的存在（5.7）
//
// 【这个命令是"可验收"能不能成立的关键】
// 一份"带出处的报告"看着比不带出处可信得多 —— 而这正是危险所在：
// **模型编出来的出处，长得和真的一模一样**（`#9002·3` 和 `#9002·8` 没有区别，
// 而那一场可能只有 6 段）。所以"带出处"如果不配一个**机器可执行的核对**，
// 它带来的不是可信度，而是**更难被发现的可信度假象**。
//
// 这条命令就是那个核对：把报告当普通文本读，捞出所有 `#会话·段`，
// 逐个回库里查。编造的、越界的、写错会话号的，全部点出来。
//
// 不加载任何模型，纯查库 + 正则，毫秒级。
static int run_verify_report(const AppConfig& cfg) {
    SessionStore::instance().init(cfg.db_path);

    std::ifstream in(cfg.verify_report, std::ios::binary);
    if (!in) {
        std::cerr << "读不到文件：" << cfg.verify_report << std::endl;
        return 1;
    }
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    if (text.empty()) {
        std::cerr << "文件是空的：" << cfg.verify_report << std::endl;
        return 1;
    }

    const auto locs = evidence::extract(text);
    const auto vr   = evidence::Evidence::verify(text);

    std::cout << "文件: " << cfg.verify_report << "（" << text.size() << " 字节）"
              << std::endl;
    std::cout << "库  : " << cfg.db_path << std::endl;

    if (locs.empty()) {
        // ⚠️ **"零引用"不能报成通过。** 一份没有任何出处的报告
        // 是"没做这件事"，不是"做对了" —— 把它算成 100% 通过，
        // 等于给最该被质疑的那种报告发绿灯。
        std::cout << "\n⚠️ 这份报告里**一处出处都没有**。" << std::endl;
        std::cout << "   这不等于通过：不能核对的结论，读者只能选择信或不信。" << std::endl;
        std::cout << "   出处格式是 #会话号·段号，例如 #9002·3。" << std::endl;
        std::cout << "[verify-report] citations=0 ok=0 bad=0" << std::endl;
        return 2;
    }

    std::cout << "\n核对 " << vr.total << " 处出处：" << std::endl;
    for (const auto& l : locs) {
        const int n = evidence::Evidence::segment_count(l.session_id);
        Segment s;
        const bool found = evidence::Evidence::resolve(l, &s);
        std::cout << "  " << (found ? "✅" : "❌") << " "
                  << evidence::format(l);
        if (found) {
            // 把那段原文开头摘出来 —— 让核对的人**当场看到**它确实在那儿，
            // 而不是只看到一个绿勾（绿勾是程序说的，原话是自己的眼睛看到的）
            std::cout << "  " << utf8::truncate(s.src_text, 40);
        } else {
            std::cout << "  ← 该位置不存在";
            if (n == 0) std::cout << "（会话 #" << l.session_id << " 在库里没有段落）";
            else        std::cout << "（会话 #" << l.session_id << " 只有 " << n << " 段）";
        }
        std::cout << std::endl;
    }

    std::cout << "\n[verify-report] citations=" << vr.total
              << " ok=" << vr.ok << " bad=" << vr.bad.size() << std::endl;
    if (!vr.bad.empty()) {
        std::cerr << "\n❌ 有 " << vr.bad.size() << " 处出处对不上 —— "
                     "**报告里有编造或写错的位置**。" << std::endl;
        for (const auto& l : vr.bad) {
            std::cerr << "   " << evidence::format(l) << std::endl;
        }
        return 1;
    }
    std::cout << "✅ 全部 " << vr.ok << " 处出处都指向真实存在的段落。" << std::endl;
    return 0;
}

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
    // 现在直接复用**唯一**的装配函数，两边不可能再走散。
    SessionStore::instance().init(cfg.db_path);   // 知识库要先打开
    ConstraintInputs kb;
    const std::vector<std::string> glossary = build_glossary(cfg, &kb);
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

    // 自检会**真的往库里写会话**（它要验落库往返）。所以没显式给 --db 时，
    // 自己开一个临时库、跑完删掉 —— 绝不往用户的默认 `translations.db` 里塞假会话。
    //
    // 【为什么必须有这道闸】默认库路径是相对于**当前目录**的 `translations.db`。
    // 用户从 exe 所在目录（`build\RelWithDebInfo\`）跑一次 `--selftest`，
    // 历史库里就多出几场 `engine=SelfTest` 的假会话：它们会出现在"最近会话"里，
    // 将来还会被 agent 的 `list_sessions` / 交付物当成真会话读进去。
    // 这个坑实测踩过 —— 在项目根目录跑了几次不带 --db 的 `--selftest`，
    // 根目录就留下一个只装着 11 场自检会话的 `translations.db`。
    //
    // 显式给了 --db 时照旧（`--selftest --db t.db` 是文档里的用法，
    // 也是"让新版程序打开某个库一次以补齐触发器/迁移"的入口，那条路径必须保留）。
    bool scratch_db = !cfg.db_path_explicit;
    AppConfig scfg = cfg;
    if (scratch_db) {
        std::error_code ec;
        const auto tmp = std::filesystem::temp_directory_path(ec);
        const auto dir = ec ? std::filesystem::path(".") : tmp;
        scfg.db_path = (dir / "at_selftest_scratch.db").string();
        // 上次崩溃可能留下残骸；先删，否则自检结果会被旧数据影响
        std::error_code rmec;
        std::filesystem::remove(scfg.db_path, rmec);
        std::cout << "[SelfTest] 未指定 --db，使用一次性临时库: " << scfg.db_path
                  << "\n           （不会碰你的 translations.db；要验指定库请加 --db <路径>）"
                  << std::endl;
    } else {
        std::cout << "[SelfTest] 使用数据库: " << scfg.db_path << std::endl;
    }

    // 收尾时删掉临时库。必须在 SessionStore 关掉**之后**删 ——
    // Windows 下文件被 sqlite 打开着是删不掉的。
    struct ScratchCleanup {
        bool active = false;
        std::string path;
        ~ScratchCleanup() {
            if (!active) return;
            SessionStore::instance().close();
            std::error_code ec;
            std::filesystem::remove(path, ec);
            if (ec) {
                std::cerr << "[SelfTest] 临时库删除失败（不影响自检结论）: "
                          << path << " —— " << ec.message() << std::endl;
            } else {
                std::cout << "[SelfTest] 已删除一次性临时库: " << path << std::endl;
            }
        }
    } cleanup;
    cleanup.active = scratch_db;
    cleanup.path   = scfg.db_path;

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
    if (!store.init(scfg.db_path)) {
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

            // ---- 同一 key 两种 kind 不得在背景里出现两次 ----
            //
            // 【真实来源】2026-xx 清理污染库时 `--terms` 打出来的是：
            //     - Marco（人名）
            //     - Marco（术语）
            // 因为条目身份是 (kind,key)：`Marco` 一场会话抽成 person，
            // 另一场用户确认成 term，就成了两行。
            // 存储层这样设计没错，但背景是给模型看的**陈述**，
            // 同一个名字说两遍、还给了两个互相矛盾的标签，是纯噪声。
            {
                using knowledge::background_lines;
                KnowledgeItem a;
                a.kind = "person"; a.key = "marco"; a.value = "Marco";
                a.status = "confirmed"; a.hits = 3;
                KnowledgeItem b;
                b.kind = "term";   b.key = "marco"; b.value = "Marco";
                b.status = "confirmed"; b.hits = 1;
                KnowledgeItem c;
                c.kind = "term";   c.key = "tv";    c.value = "TV";
                c.status = "confirmed"; c.hits = 2;

                const auto lines = background_lines({a, b, c}, 32);
                bool ok = true;
                std::string why;
                int marco = 0;
                for (const auto& l : lines) {
                    if (l.find("Marco") != std::string::npos) marco += 1;
                }
                if (marco != 1) {
                    ok = false;
                    why = "同一个 key 的两种 kind 在背景里出现了 " +
                          std::to_string(marco) + " 次，应为 1 次";
                } else if (lines.size() != 2) {
                    ok = false;
                    why = "背景行数应为 2（Marco + TV），实际 " +
                          std::to_string(lines.size());
                } else if (lines[0].find(u8"人名") == std::string::npos) {
                    // hits 降序：保留的应是证据更强的 person 那一行
                    ok = false;
                    why = "应保留 hits 更高的 person 行，实际: " + lines[0];
                }
                std::cout << "[SelfTest] 背景去重（同 key 两种 kind 只出一行）: "
                          << (ok ? "✅ 通过" : "❌ 失败") << std::endl;
                if (!ok) {
                    std::cerr << "    " << why << std::endl;
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

                // ⑧ 整段大写的**标题行**里，一个词都不算专名证据
                //
                // 【真实数据（用户真跑会话 #13，段 98）】
                // 播客的小标题是 `PUTTING IT TOGETHER`。整行大写里 8 个字母的
                // `TOGETHER` 满足"连续 ≥2 个大写"，于是被判成缩写抽了出来，
                // 还被问了一句「第一次听到「TOGETHER」。这个词的写法对吗？」
                //
                // 这条同时验两个修法，缺一个都会漏：
                //   · 全大写当缩写**必须限长**（真缩写都在 5 个字母内）
                //   · 整行大写是**排版**，这段在"大写"上零信息量，应当整段作废
                {
                    std::vector<Segment> segs = {
                        mkseg(1, "PUTTING IT TOGETHER"),
                        mkseg(2, "Welcome to EnglishPod."),
                    };
                    const auto c = extract_candidates(segs, 1);
                    for (const char* bad : {"together", "putting"}) {
                        if (has(c, bad)) {
                            ex_ok = false;
                            why3 = std::string("全大写标题行里的普通词被当成专名: ") + bad;
                        }
                    }
                    // 但真正的缩写（≤5 字母）仍然要认出来 —— 别把 R3 修死
                    if (ex_ok) {
                        std::vector<Segment> t =
                            {mkseg(1, "our KPI and the CRM system")};
                        const auto ct = extract_candidates(t, 1);
                        if (!has(ct, "kpi") || !has(ct, "crm")) {
                            ex_ok = false;
                            why3 = "真缩写 KPI/CRM 被全大写限长误杀了";
                        }
                    }
                    // 单个词的段不整段作废（信息不足，不据此下结论）
                    if (ex_ok) {
                        std::vector<Segment> t = {mkseg(1, "OKAY")};
                        (void)extract_candidates(t, 1);   // 只要求不崩、不误伤别的判据
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

                    // ---- ③b **报告动词后面的项目名**（真实 bug，2026-09-19）----
                    //
                    // 【现象】这两句**一个候选都抽不出来**，而同一句话写成
                    // 「凤凰项目的接口文档要重写。」就能抽到。
                    // 【根因】R7 从左往右扫时，「项目」往左走到 `凰 → 凤 → 说`，
                    // 而 `说` 不在虚词表里 → 继续走到 `他` 才停 →
                    // 拼出 **「说凤凰项目」**（5 字怪名字）。
                    // 「项目」是**弱后缀**（有出现次数门槛），那个怪名字只出现一次 →
                    // 被丢弃，**真正的「凤凰项目」跟着一起没了**。
                    // 【修法】给 R7 左扫单独一张"报告动词"停止字表
                    // （见 `is_report_verb_char`）—— 不能塞进 `is_function_char`，
                    // 因为那个函数还被 R6 的"人名里不能含虚词"用着，而动词不是虚词。
                    //
                    // ⚠️ 用例必须**放在句中**：我第一次把名字写在段首，
                    // 结果"修复前也能通过"——因为段首根本不触发这个 bug。
                    if (z_ok) {
                        std::vector<Segment> v = {
                            mkz(1, u8"他说凤凰项目的接口文档要重写，所以我又确认了一遍。"),
                            mkz(2, u8"他觉得迁移成本太高，凤凰项目下一期再说。"),
                        };
                        const auto cc = extract_candidates(v, 1);
                        if (!hasz(cc, knowledge::normalize_key(u8"凤凰项目").c_str())) {
                            z_ok = false;
                            whyz = u8"报告动词后面的项目名漏了"
                                   u8"（会先拼出「说凤凰项目」这种怪名字，再把真名一起丢掉）";
                        }
                        // 而且**不许**把动词/代词粘进名字
                        if (z_ok) {
                            for (const auto& x : cc) {
                                if (x.value.rfind(u8"说", 0) == 0 ||
                                    x.value.rfind(u8"他", 0) == 0) {
                                    z_ok = false;
                                    whyz = u8"动词/代词被粘进了名字： " + x.value;
                                    break;
                                }
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

                // ---- ⑨b 真实数据抽取回归：会话 #47 的假阳性 ----
                //
                // 【这一组是整组里最值钱的】它用的**不是手写语料** ——
                // 是用户真实录的一场 EnglishPod 播客（53 段）里的原文。
                // 光靠 R1（首字母大写 + 非句首）时，这一场抽出了 8 个假阳性：
                //   down(6) Keep(4) Midnight movies(3) Speaking Learners Exactly preview
                // 全是普通词 —— 大写只是因为被当作**词汇标题**写（`Keep It Down.`）、
                // 标题式大写（`Speaking of Movies`）、或者称呼语（`Hello English Learners`）。
                //
                // 而 4 个真专名（Erica / EnglishPod / Marco / TV）**必须同时保住** ——
                // 所以两个方向都要断言，只测一边等于没测。
                {
                    using namespace knowledge;
                    bool r_ok = true;
                    std::string whyr;

                    struct RCase {
                        const char* name;
                        std::vector<const char*> segs;
                        std::vector<const char*> must_have;
                        std::vector<const char*> must_not_have;
                    };
                    const RCase cases[] = {
                        {
                            u8"#47 真实播客关键句",
                            {
                                u8"Hello English Learners and welcome to EnglishPod.",
                                u8"My name is Marco. And I'm Erica.",
                                u8"Erica. Today we are really excited, right?",
                                u8"that you hear in movies and TV shows.",
                                u8"Yeah, Speaking of Movies,",
                                u8"at the movies. Exactly.",
                                u8"So Erica, what is it when someone is inconsistent?",
                                u8"Keep It Down.",
                                u8"examples on how we use Keep It Downs.",
                                u8"Keep it down so we can understand.",
                                u8"do you mind keeping it down?",
                                u8"It's After Midnight.",
                                u8"preview.",
                            },
                            {"erica", "marco", "englishpod", "tv"},
                            {"down", "downs", "keep", "midnight", "movies",
                             "speaking", "learners", "exactly", "preview"},
                        },
                        {
                            u8"#43 真实播客关键句",
                            {
                                u8"EnglishPod. My name is Marco. And I'm Erica.",
                                u8"How are you, Erica? Marco, I'm doing really well.",
                                u8"you hear in movies and TV shows",
                            },
                            {"erica", "marco", "englishpod", "tv"},
                            {"movies", "doing", "really"},
                        },
                        {
                            u8"jfk.wav（每句首词都大写，但一个专名都没有）",
                            {
                                u8"Ask not what your country can do for you",
                                u8"What your country can do for you",
                                u8"And so my fellow Americans",
                            },
                            {},
                            {"ask", "what", "and", "country", "americans"},
                        },
                    };

                    for (const auto& c : cases) {
                        if (!r_ok) break;
                        std::vector<Segment> segs;
                        int seq = 1;
                        for (const char* t : c.segs) {
                            Segment s;
                            s.seq        = seq++;
                            s.src_text   = t;
                            s.confidence = 0.8;
                            segs.push_back(s);
                        }
                        const auto got = extract_candidates(segs, 1);
                        auto has = [&](const char* k) {
                            const std::string nk = normalize_key(k);
                            for (const auto& g : got) if (g.key == nk) return true;
                            return false;
                        };
                        for (const char* k : c.must_have) {
                            if (!has(k)) {
                                r_ok = false;
                                whyr = std::string(c.name) + u8"：该抽到却漏了「" + k + u8"」";
                                break;
                            }
                        }
                        if (!r_ok) break;
                        for (const char* k : c.must_not_have) {
                            if (has(k)) {
                                r_ok = false;
                                whyr = std::string(c.name) + u8"：不该抽到却抽了「" + k
                                       + u8"」—— 假阳性会进识别提示和翻译约束";
                                break;
                            }
                        }
                    }

                    std::cout << "[SelfTest] 真实数据抽取回归（#47 八个假阳性 / #43 四个真名 / jfk 零）: "
                              << (r_ok ? "✅ 通过" : "❌ 失败") << std::endl;
                    if (!r_ok) {
                        std::cerr << "    " << whyr << std::endl;
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
                                               "get_session_report", "list_actions"};
                    // 数目跟着 must_have 走，不写死 —— 写死的数字在加工具时
                    // 会以"测试失败"的形式提醒你，但更常见的是有人顺手把数字改大
                    // 而不去想"新工具是不是也该有断言"。用数组长度则两者必然同步。
                    const int want = static_cast<int>(sizeof(must_have) / sizeof(*must_have));
                    if (reg.size() != want) {
                        a_ok = false;
                        whya = "应注册 " + std::to_string(want) + " 个只读工具，实际 "
                               + std::to_string(reg.size())
                               + "（加了工具就往 must_have 里加一条）";
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

        // ⑨ **同一实体不能因为 kind 被改过就变成两行**（2.12）
        //
        // 【这一条守的是什么】表的身份是 `(kind, key)`，而 kind 是可修正的属性：
        // 抽取器只会说"首字母大写的词 → term"，分诊层的模型会把它改成
        // product/person/project（`set_kind`）。改完之后下一次抽取同一个词，
        // `(kind,key)` 就对不上了 —— upsert 找不到旧行，**又建一行**。
        // 实测后果（Nimbus，端到端跑出来的）：
        //     用户在一场里被问了两遍同一个东西；库里两行；kMaxAsks 各算各的。
        // 修法是写入前先 `find_by_key()` 认旧行并沿用它的 kind。
        //
        // 故意**走真实的写入路径**（upsert → set_kind → save_candidates），
        // 而不是直接构造两行数据 —— 这个 bug 的本质就在"写入路径"上，
        // 手搓数据反而绕过了它。
        {
            const std::string DK = "__selftest_dupshape";
            ks.purge_key_prefix(DK);

            KnowledgeItem a;
            a.kind = "term"; a.key = DK; a.value = "ZZDupProbe";
            a.status = "candidate"; a.confidence = 0.7; a.hits = 3;
            if (ks.upsert(a) <= 0) fail("重复行用例：初次写入失败");

            // 分诊层把它改成 product（真实路径就是这个）
            if (!ks.set_kind("term", DK, "product", &last_err))
                fail(("重复行用例：set_kind 失败 " + last_err).c_str());

            // 下一次抽取：抽取器**仍然**只会给 term（它只会看形状）
            std::vector<knowledge::ExtractedCandidate> again;
            knowledge::ExtractedCandidate c;
            c.kind = "term"; c.key = DK; c.value = "ZZDupProbe";
            c.confidence = 0.7; c.hits = 2;
            knowledge::save_candidates(again, nullptr);   // 空集合：不该报错
            again.push_back(c);
            knowledge::save_candidates(again, nullptr);

            // ① 必须仍然只有一行（同一个 key 不同 kind 的两行就是 bug）
            int rows = 0;
            for (const auto& it : ks.list("", 10000)) {
                if (knowledge::normalize_key(it.key) == DK) ++rows;
            }
            if (rows != 1) {
                fail(("同一个实体变成了 " + std::to_string(rows) + " 行"
                      "（kind 被改过之后又按旧 kind 建了新行）").c_str());
            }
            // ② 那唯一一行的 kind 必须是**更知情的那个**（分诊改出来的 product），
            //    不能被抽取器的形状猜测 term 覆盖回去
            KnowledgeItem got;
            if (!ks.find_by_key(DK, &got) || got.kind != "product") {
                fail("沿用 kind 失败：库里已有的 product 被抽取器的 term 盖掉了");
            }
            // ③ hits 要**累加到同一行**上，而不是落到新行里（那会让 hits 变小、
            //    HighFreqUnconfirmed 那条规则永远不触发）
            if (got.hits != 5) {
                fail(("hits 没累加到同一行（应为 3+2=5，实际 "
                      + std::to_string(got.hits) + "）").c_str());
            }
            ks.purge_key_prefix(DK);
        }

        last_err.clear();
        const int removed = ks.purge_key_prefix(TPRE);
        if (removed < 1) fail("清理应至少删掉 1 条");

        std::cout << "[SelfTest] 知识落库（新建/累加/变更降级/确认/拒绝非法/FTS 往返/清理）: "
                  << (db_ok ? "✅ 通过" : "❌ 失败") << std::endl;
        if (!db_ok) return 1;
    }

    // ---- 22) Agent 规划循环（§7 步骤 5.2）----
    //
    // 【为什么这一组能存在，本身就是架构的证据】
    // 规划是**联网 + 不确定**的：直接写进循环就永远进不了 L1（自检跑不了、
    // 行为随模型变、回归无从谈起）。把"调 LLM"抽成可注入回调之后，
    // 这里喂**脚本化的工具调用序列**，走的是**真实循环逻辑** ——
    // 预算、审计留痕、工具失败处理、失败模式，全都能验。
    {
        using namespace agent;
        bool a_ok = true;
        std::string awhy;

        auto mk_reg = []() {
            ToolRegistry reg;
            Tool t;
            t.name        = "echo";
            t.description = u8"回显参数（自检用）";
            t.params_schema = "{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"string\"}}}";
            t.run = [](const std::string& args, const ToolContext&, std::string*) {
                ToolResult r;
                r.ok      = true;
                r.content = "{\"got\":" + args + "}";
                r.audit   = u8"回显了一次";
                return r;
            };
            std::string e;
            reg.add(t, &e);

            Tool bad;
            bad.name        = "boom";
            bad.description = u8"总是失败（自检用）";
            bad.params_schema = "{\"type\":\"object\",\"properties\":{}}";
            bad.run = [](const std::string&, const ToolContext&, std::string*) {
                ToolResult r;
                r.ok    = false;
                r.error = u8"故意的失败";
                return r;
            };
            reg.add(bad, &e);
            return reg;
        };

        // ① 正常多步：查两次 → 给结论；审计要留下两次调用的痕迹
        {
            const auto reg = mk_reg();
            std::vector<Step> script;
            {
                Step s; s.calls.push_back({"c1", "echo", "{\"x\":\"a\"}"}); script.push_back(s);
                Step s2; s2.calls.push_back({"c2", "echo", "{\"x\":\"b\"}"}); script.push_back(s2);
                Step s3; s3.content = u8"查到了：a 和 b"; script.push_back(s3);
            }
            size_t i = 0;
            Planner p = [&](const std::vector<Message>&, const std::string&, Step* out,
                            std::string*) {
                if (i >= script.size()) return false;
                *out = script[i++];
                return true;
            };
            std::ostringstream sink;
            const auto r = run(u8"查一下 a 和 b", reg, ToolContext{}, p, Budget(), &sink);
            if (!r.ok || r.answer.find("a 和 b") == std::string::npos) {
                a_ok = false; awhy = u8"正常多步没给出结论";
            } else if (r.tool_calls != 2) {
                a_ok = false; awhy = u8"工具调用次数应为 2，实际 " + std::to_string(r.tool_calls);
            } else if (r.audit.size() != 2) {
                // **审计是评审判断"它真在做事"的唯一依据**，缺了等于没有证据
                a_ok = false; awhy = u8"审计留痕条数不对：" + std::to_string(r.audit.size());
            } else if (sink.str().find("调用 echo") == std::string::npos) {
                a_ok = false; awhy = u8"--audit 没有逐步打印出工具调用";
            }
        }

        // ② **全程没调工具就给结论 → 必须标出来**（"不能编"那条唯一的机械抓手）
        {
            const auto reg = mk_reg();
            Planner p = [](const std::vector<Message>&, const std::string&, Step* out,
                           std::string*) {
                out->content = u8"我猜大概是这样的……";   // 一次都没查
                return true;
            };
            const auto r = run(u8"查一下", reg, ToolContext{}, p);
            if (!r.no_tools_used) {
                a_ok = false;
                awhy = u8"全程没调工具却给了结论，没有被标出来 —— 那是编报告的信号";
            }
        }

        // ③ 单个工具失败 → **不许让任务崩掉**，错误要作为观察回给模型
        {
            const auto reg = mk_reg();
            size_t i = 0;
            bool saw_error_obs = false;
            std::vector<Step> script;
            { Step s; s.calls.push_back({"c1", "boom", "{}"}); script.push_back(s); }
            { Step s2; s2.calls.push_back({"c2", "echo", "{}"}); script.push_back(s2); }
            { Step s3; s3.content = u8"我换了办法，查到了"; script.push_back(s3); }
            Planner p = [&](const std::vector<Message>& h, const std::string&, Step* out,
                            std::string*) {
                for (const auto& m : h) {
                    if (m.role == "tool" && m.content.find(u8"故意的失败") != std::string::npos) {
                        saw_error_obs = true;
                    }
                }
                if (i >= script.size()) return false;
                *out = script[i++];
                return true;
            };
            const auto r = run(u8"试一下", reg, ToolContext{}, p);
            if (!r.ok) {
                a_ok = false; awhy = u8"一次工具失败就把整个任务搞崩了";
            } else if (!saw_error_obs) {
                a_ok = false; awhy = u8"工具失败的原因没有回给模型（它没法换个办法）";
            } else if (r.audit.size() != 2 || r.audit[0].tool_ok) {
                a_ok = false; awhy = u8"审计没记下那一次失败";
            }
        }

        // ④ **预算耗尽 → 交半成品 + 说清为什么停**，不是报错
        {
            const auto reg = mk_reg();
            int n = 0;
            Planner p = [&](const std::vector<Message>&, const std::string&, Step* out,
                            std::string*) {
                // 永远要求继续查 —— 模拟"停不下来"
                out->calls.push_back({"c" + std::to_string(++n), "echo", "{}"});
                return true;
            };
            Budget b; b.max_steps = 3;
            const auto r = run(u8"查个没完", reg, ToolContext{}, p, b);
            if (!r.partial) {
                a_ok = false; awhy = u8"步数用完了却没标成 partial";
            } else if (r.answer.find(u8"步数用完") == std::string::npos) {
                // 用户看到"半成品 + 一句实话"是有用的；
                // 看到 "Error: budget exceeded" 是没用的
                a_ok = false; awhy = u8"预算耗尽时没有说清为什么停：" + r.answer;
            } else if (r.tool_calls != 3) {
                a_ok = false; awhy = u8"应恰好执行 3 步，实际 " + std::to_string(r.tool_calls);
            }
        }

        // ⑤ 没有规划器（没配 Key）→ **能力降级，不是崩溃**，而且要说清退路
        {
            const auto reg = mk_reg();
            const auto r = run(u8"随便", reg, ToolContext{}, Planner());
            if (r.ok) {
                a_ok = false; awhy = u8"没有规划器却报了 ok";
            } else if (r.answer.find("--search") == std::string::npos) {
                a_ok = false;
                awhy = u8"没配 Key 时没告诉用户退路（--search）";
            }
        }

        // ⑥ 没有工具 → 也要说人话
        {
            ToolRegistry empty_reg;
            Planner p = [](const std::vector<Message>&, const std::string&, Step* out,
                           std::string*) { out->content = "x"; return true; };
            const auto r = run(u8"随便", empty_reg, ToolContext{}, p);
            if (r.ok || r.answer.find(u8"没有任何可用的工具") == std::string::npos) {
                a_ok = false; awhy = u8"没有工具时没说清楚";
            }
        }

        // ⑦ 规划器返回失败 → 交已查到的部分 + 说清断线
        {
            const auto reg = mk_reg();
            size_t i = 0;
            Planner p = [&](const std::vector<Message>&, const std::string&, Step* out,
                            std::string*) {
                if (i++ == 0) { out->calls.push_back({"c1", "echo", "{}"}); return true; }
                return false;   // 断线
            };
            const auto r = run(u8"查一下", reg, ToolContext{}, p);
            if (!r.partial) {
                a_ok = false; awhy = u8"规划器失败却没标成 partial";
            } else if (r.stop_reason.find(u8"断线") == std::string::npos &&
                       r.answer.find(u8"断线") == std::string::npos) {
                a_ok = false; awhy = u8"断线时没说清楚";
            }
        }

        // ⑧ assistant 消息必须带上**原始 arguments**（协议要求，反推是有损的）
        {
            const auto reg = mk_reg();
            size_t i = 0;
            std::string seen_args;
            Planner p = [&](const std::vector<Message>& h, const std::string&, Step* out,
                            std::string*) {
                for (const auto& m : h) {
                    if (m.role == "assistant" && !m.calls.empty()) {
                        seen_args = m.calls[0].args_json;
                    }
                }
                if (i++ == 0) { out->calls.push_back({"c1", "echo", "{\"x\":\"KEEP_ME\"}"}); return true; }
                out->content = u8"好了";
                return true;
            };
            (void)run(u8"查", reg, ToolContext{}, p);
            if (seen_args.find("KEEP_ME") == std::string::npos) {
                a_ok = false;
                awhy = u8"历史里的工具调用参数丢了（我第一版从后续消息反推，arguments 会变成 {}）";
            }
        }

        // ⑨ **写工具的闸**（步骤 5.4）—— agent 只能提议，永远不能确认
        //
        // 【为什么这条必须有可执行证据】§6.5 红线是"模型猜的东西永远不能自动
        // 变成约束"。写工具是**唯一可能破这条线的东西**（它真的往库里写）。
        // 只在注释里承诺"我们只写 candidate"是不够的 —— 那和"注意不要泄漏
        // 内部状态"是同一类靠不住的保证。所以这里真调一次写工具，然后**断言**：
        //   ① 落库状态必须是 candidate
        //   ② `usable_as_constraint()` 必须拒绝它（不进识别提示/翻译约束）
        //   ③ 未知/非法 kind 必须被拒（不许借 kind 绕过）
        {
            const std::string PK = "__selftest_agentprop_";
            auto& ksx = KnowledgeStore::instance();
            ksx.purge_key_prefix(PK);

            ToolRegistry wreg;
            register_write_tools(wreg);
            if (wreg.size() == 0) {
                a_ok = false; awhy = u8"写工具没注册上";
            } else if (wreg.find("propose_knowledge") == nullptr) {
                a_ok = false; awhy = u8"propose_knowledge 没注册";
            }

            if (a_ok) {
                nlohmann::json args;
                args["value"] = PK + u8"凤凰项目";
                args["kind"]  = "project";
                args["evidence"] = u8"这周我们在推凤凰项目";
                const auto tr = wreg.call("propose_knowledge", args.dump(), ToolContext{});
                if (!tr.ok) {
                    a_ok = false; awhy = u8"写工具调用失败：" + tr.error;
                } else if (tr.content.find("\"status\":\"candidate\"") == std::string::npos) {
                    a_ok = false; awhy = u8"写工具的结果没说明它是候选";
                } else if (tr.audit.find(u8"待用户确认") == std::string::npos) {
                    a_ok = false; awhy = u8"审计里没说清它是待确认的候选";
                }

                KnowledgeItem got;
                const std::string nk = knowledge::normalize_key(PK + u8"凤凰项目");
                if (a_ok && !ksx.get("project", nk, &got)) {
                    a_ok = false; awhy = u8"agent 提议的东西没落库";
                } else if (a_ok && got.status != "candidate") {
                    // **这一条是本组的核心**：红线破了就是这里报
                    a_ok = false;
                    awhy = u8"agent 提议的东西落库状态不是 candidate，而是 " + got.status +
                           u8" —— §6.5 红线破了";
                } else if (a_ok && knowledge::usable_as_constraint(got)) {
                    a_ok = false;
                    awhy = u8"candidate 通过了 usable_as_constraint（它会进识别提示/翻译约束）";
                }
            }

            // **非法 kind 必须被拒** —— 不许借 kind 字段绕过名字类的限制
            if (a_ok) {
                nlohmann::json bad;
                bad["value"] = PK + u8"随便";
                bad["kind"]  = "decision";       // 值是整句话的那类，刻意不收
                const auto tr = wreg.call("propose_knowledge", bad.dump(), ToolContext{});
                if (tr.ok) {
                    a_ok = false;
                    awhy = u8"写工具接受了 decision 这类 kind（值可以是整句话，风险大）";
                }
            }

            // **空 value 必须被拒**
            if (a_ok) {
                nlohmann::json empty;
                empty["kind"] = "term";
                const auto tr = wreg.call("propose_knowledge", empty.dump(), ToolContext{});
                if (tr.ok) { a_ok = false; awhy = u8"写工具接受了空的 value"; }
            }

            ksx.purge_key_prefix(PK);
        }

        std::cout << "[SelfTest] Agent 规划循环（多步/审计/失败不崩/预算降级/没Key说退路）: "
                  << (a_ok ? "✅ 通过" : "❌ 失败") << std::endl;
        if (!a_ok) { std::cerr << "    " << awhy << std::endl; return 1; }
    }

    // ---- 12) 知识缺口检测六条规则（§6.6 / §6.7）----
    // 全内存构造，不碰数据库、不碰模型。
    {
        using knowledge::GapRule;
        using knowledge::KnowledgeWithHistory;

        auto mk = [](long long id, const char* status, const char* value,
                     int hits, double conf, int asked = 0,
                     // 默认给一个非空含义 —— 否则 2.7 新加的"问含义"规则
                     // 会在**每一**条 term 上都命中，把别的用例的期望条数全打乱，
                     // 于是每条用例都不再在测它原本要测的东西。
                     // 要测"没有含义"的用例请显式传空串。
                     const char* def = u8"（用例占位含义）") {
            KnowledgeWithHistory e;
            e.item.id         = id;
            e.item.kind       = "term";
            e.item.key        = value;
            e.item.value      = value;
            e.item.status     = status;
            e.item.hits       = hits;
            e.item.confidence = conf;
            e.item.asked_count = asked;
            e.item.definition = def;
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

        // ⑧ 上限：问不超过 max_questions 个
        //
        // 【2026-09-18 改了框架】这原来是按"§1.3 红线：提问是稀缺资源"写的。
        // 用户否掉了那个说法 —— 提问量会**自然衰减**，不需要靠上限压。
        // 所以这个上限现在是**安全网**（防提取器失控吐 50 个候选），
        // 措辞也改了：它约束的是我们的提取器，不是用户。
        // 下面同时断言"正常规模不该被截断"—— 那是这条闸真正该守的东西。
        {
            // ⚠️ **一条实体只能算一个**：这里原来写的是 20 条 value 全为 `"T"` 的记录，
            // 也就是**同一个实体的 20 份拷贝**。去重（2.12）一上来就把它们并成 1 条，
            // 于是"应被截断到 5"当场失败。
            //
            // 这不是去重太狠，是**用例本身没意义**：库里不可能真有 20 个都叫 `T`
            // 的实体 —— 那正是去重要防的东西。要测上限就得给 20 个**不同的**实体。
            // （`mk()` 这类测试数据踩坑这是第四次了，见下面那段的三条记录。）
            std::vector<KnowledgeWithHistory> v;
            for (int i = 0; i < 20; ++i) {
                v.push_back(mk(100 + i, "candidate", ("T" + std::to_string(i)).c_str(),
                               9, 0.9));
            }
            const auto q = knowledge::detect_gaps(v, 5);
            if (q.size() != 5) { gap_ok = false; why = "问问题数应被截断到 5"; }
            if (!knowledge::detect_gaps(v, 0).empty()) { gap_ok = false; why = "max=0 时应一个问题都不出"; }

            // **同一实体两行 → 只问一次**（2.12）
            //
            // 【为什么这条必须有】真实库里 `Erika` 就是 person / term 两行，
            // 那是"分诊改过 kind、下一次抽取又按老 kind 建了新行"攒下来的。
            // 不修的话用户**每次**都会看到同一个东西被问两遍 ——
            // 那给人的印象恰恰是"这东西不认识我"，而它其实认识。
            {
                std::vector<KnowledgeWithHistory> dup;
                KnowledgeWithHistory a = mk(800, "candidate", "Erika", 5, 0.9);
                KnowledgeWithHistory b = mk(801, "candidate", "Erika", 2, 0.9);
                b.item.kind = "person";        // 同一个 key，不同 kind
                a.item.kind = "term";
                dup.push_back(a);
                dup.push_back(b);
                const auto qd = knowledge::detect_gaps(dup);
                if (qd.size() != 1) {
                    gap_ok = false;
                    why = "同一个实体两行时问了 " + std::to_string(qd.size())
                          + " 次（应该只问 1 次）";
                } else if (qd[0].hits != 5) {
                    // 保留的必须是**证据更强**的那条（hits 5 而不是 2）——
                    // 去重在排序之后做，就是为了这个
                    gap_ok = false;
                    why = "去重留下了 hits 较小的那条（应留证据更强的）";
                }
            }

            // **正常规模（每场几个新名字）绝不能被默认上限截断。**
            // 这条守的是"别再用稀缺性的名义把闸收紧" —— 那次收紧的代价是
            // 用户的演示被迫一场只学一个概念（kMaxDefinitionAsksPerSession 原来是 1）。
            //
            // ⚠️ 用例构造踩了三次坑，每次都值得记：
            //   ① 6 个 `kind=term` 且无含义 → 全走开放式问题，撞上含义题上限（4）→ 出 4 个
            //   ② 换成 `ProjA/ProjB/ProjC` → 编辑距离 1、同首字母，被写法冲突规则
            //      **正确地**合并成 1 问 → 出 1 个
            //   ③ person 类型的候选在默认 `current_session = -1` 下**一条规则都不触发**
            //      （NewlySeen 要求本场、HighFreq 要求 hits≥3）→ 出 0 个
            // 结论：**造"正常规模"的用例必须同时理解全部七条规则怎么互相作用**，
            // 否则测的是别的东西。下面这组是逐条核过的：首字母全不同（不会被合并）、
            // hits=3（触发 HighFreqUnconfirmed）、有含义（不触发 AskDefinition）。
            {
                std::vector<KnowledgeWithHistory> normal;
                const char* vals[] = {"Alpha", "Bravo", "Charlie",
                                      "Delta", "Echo", "Foxtrot"};
                for (int i = 0; i < 6; ++i) {
                    normal.push_back(mk(700 + i, "candidate", vals[i], 3, 0.9, 0,
                                        u8"（占位含义）"));
                }
                const auto qd = knowledge::detect_gaps(normal);
                if (qd.size() != 6) {
                    gap_ok = false;
                    why = "6 个互不相似的候选是正常规模，不该被默认上限截断（实际 "
                          + std::to_string(qd.size()) + " 个）";
                }
            }
            // 开放式问题（"它指什么"）的独立上限也不该卡在 1 —— 那正是
            // 用户三场演示里"一场只学一个概念"的原因。
            if (knowledge::kMaxDefinitionAsksPerSession < 3) {
                gap_ok = false;
                why = "含义问题的每场上限被收回去了（那会让学习速度被人为放慢）";
            }
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
            // 【2.7 之后这条断言的边界要收窄】原来断言"confirmed 的一条问题都不出"，
            // 但"问含义"是**另一个**问题：前者是"同一件事反复问"（§1.3 要防的），
            // 后者是"这个概念我还没学过"（用户第一次教它就是靠它）。
            // 所以现在只断言：confirmed 的那条不能再被问**值**的问题。
            for (const auto& x : q) {
                if (x.knowledge_id == 210 && x.rule == GapRule::ValueChanged) {
                    gap_ok = false;
                    why = "confirmed 的条目不该因为历史里有降级记录而被再问";
                }
            }
            bool has_211 = false;
            for (const auto& x : q) if (x.knowledge_id == 211) has_211 = true;
            if (gap_ok && !has_211) {
                gap_ok = false; why = "没确认过的那条反而没被问";
            }
        }

        // ⑫ **同一个实体的两种写法必须合并成一条问题**（用户真跑会话 #13 的形态）
        //
        // 真实经过：库里本来有 `Erica`（#43 听到，3 次），#13 这一场 Whisper
        // 听成了 `Erika`。旧代码把它们当两条互不相干的知识，问了两个独立问题：
        //     3. 已经听到 3 次「Erica」，一直没确认过。它是对的说法吗？  → 跳过
        //     4. 第一次听到「Erika」。这个词的写法对吗？                 → 按了 y
        // 用户的两个回答互相矛盾，而他**无从知道**这两个写法指的是同一个人
        // —— 两个问题都没提到对方。结果 `Erika` 成了 confirmed，
        // 下一场的 `initial_prompt` 变成 `Marco, Erika`（正确的是 Erica）。
        //
        // 所以这一组断言的是"**只出一条**"，而不是"能识别出冲突" ——
        // 分开问两条也算"识别出来了"，但那正是出问题的地方。
        {
            std::vector<KnowledgeWithHistory> v = {
                mk(220, "confirmed", "Erika", 2, 0.84, 0),   // 用户按过 y 的（其实是听错）
                mk(221, "candidate", "Erica", 3, 0.80, 0),   // 证据更多的那个（对）
            };
            const auto q = knowledge::detect_gaps(v);
            if (q.size() != 1) {
                gap_ok = false;
                why = "两种写法应合并成**一条**问题，实际出了 " +
                      std::to_string(q.size()) + " 条";
            } else if (q[0].rule != GapRule::ConflictingSpellings) {
                gap_ok = false; why = "写法冲突没走 ConflictingSpellings 规则";
            } else if (q[0].alternatives.size() != 2) {
                gap_ok = false; why = "选项数应为 2";
            } else if (q[0].alternatives[0].value != "Erica") {
                // hits 降序 —— **刻意不按 confirmed 排前面**：
                // 这个真实案例里 confirmed 的恰恰是错的那个，
                // 按状态排序等于拿错误答案引导用户。
                gap_ok = false;
                why = "选项应按 hits 降序（证据多的在前），实际：";
                for (const auto& a : q[0].alternatives) {
                    why += a.value + "(" + std::to_string(a.hits) + ") ";
                }
            // 【断言改成打在**渲染之后的文案**上】
            // 2.8 之后 GapQuestion 里已经没有 question 字段（措辞搬去了交互层），
            // 所以这里改成走真实渲染路径再看结果 —— 这比打在一个中间字段上更有意义：
            // 它验的是"用户到底会看到什么"。
            } else if (q[0].alternatives[0].knowledge_id == 0) {
                gap_ok = false; why = "选项没带上 knowledge_id，落库时选不了";
            } else {
                const auto shown = interaction::question(knowledge::to_prompt(q[0]));
                if (shown.find("Erika") == std::string::npos) {
                    // 冲突必须**摆出来**，这是这个问题全部的价值
                    gap_ok = false; why = "问题里没提到另一种写法 —— 用户还是只能瞎猜";
                } else if (shown.find(u8"现在用的是") == std::string::npos) {
                    gap_ok = false;
                    why = "没说清当前在用哪个写法（用户不知道改动影响什么）";
                } else if (shown.find("Erika") != std::string::npos &&
                           shown.find("Erica") == std::string::npos) {
                    gap_ok = false; why = "两种写法没有都列出来";
                }
            }
        }

        // ⑬ 相近但**不相干**的两个专名不能被误合并
        //
        // 合并错比不合并更糟：那会让用户在两个无关的词之间做选择。
        {
            std::vector<KnowledgeWithHistory> v = {
                mk(230, "candidate", "Phoenix", 3, 0.9, 0),
                mk(231, "candidate", "Marco",   3, 0.9, 0),
            };
            const auto q = knowledge::detect_gaps(v);
            if (q.size() != 2) {
                gap_ok = false;
                why = "不相干的专名被误合并了（应各出一条，实际 " +
                      std::to_string(q.size()) + " 条）";
            }
            for (const auto& x : q) {
                if (x.rule == GapRule::ConflictingSpellings) {
                    gap_ok = false; why = "不相干的专名走了写法冲突规则";
                }
            }
        }

        // ⑭ 两字母词不能靠编辑距离合并（PC / PB 这种太容易撞上）
        {
            std::vector<KnowledgeWithHistory> v = {
                mk(240, "candidate", "PC", 3, 0.9, 0),
                mk(241, "candidate", "PB", 3, 0.9, 0),
            };
            const auto q = knowledge::detect_gaps(v);
            for (const auto& x : q) {
                if (x.rule == GapRule::ConflictingSpellings) {
                    gap_ok = false; why = "两字母词被误判成同一个实体";
                }
            }
        }

        // ⑮ **没有含义的术语要问"它指什么"** —— 这是闭环的核心那一问（步骤 2.7）
        //
        // 【为什么必须有】在它之前，一条知识能表达的全部内容是"这个词该写成什么样"。
        // 系统能做的只有"把 Erica 认成 Erica"，**永远做不到知道 CO-RE 是什么**。
        // 而用户能教给系统最有价值的东西恰恰是后者。
        // 没有这条规则，`knowledge.definition` 那一列永远是空的。
        {
            std::vector<KnowledgeWithHistory> v = {
                mk(300, "candidate", "CO-RE", 1, 0.9, 0, ""),   // 没含义 → 该问
                mk(301, "candidate", "Phoenix", 1, 0.9, 0, u8"内部代号"),  // 有含义 → 不问
            };
            const auto q = knowledge::detect_gaps(v);
            bool found = false;
            for (const auto& x : q) {
                if (x.rule == GapRule::AskDefinition) {
                    if (found) { gap_ok = false; why = "含义问题出了不止一条"; }
                    found = true;
                    if (x.knowledge_id != 300) { gap_ok = false; why = "问错了条目"; }
                }
            }
            if (!found) { gap_ok = false; why = "没有含义的术语没被问「它指什么」"; }
        }

        // ⑯ **每场最多问一个含义问题** —— 用户要打一整句话，问三个他就不答了
        //
        // 这条和"最多问 5 个问题"是两条独立的闸：5 个是总数（§1.3），
        // 1 个是**开放式**问题的数（选择题按个 y 就行，开放式要打字）。
        {
            std::vector<KnowledgeWithHistory> v;
            for (int i = 0; i < 6; ++i) {
                v.push_back(mk(400 + i, "candidate", "术语T", 1, 0.9, 0, ""));
            }
            const auto q = knowledge::detect_gaps(v, 10);
            size_t n_def = 0;
            for (const auto& x : q) if (x.rule == GapRule::AskDefinition) ++n_def;
            if (n_def > knowledge::kMaxDefinitionAsksPerSession) {
                gap_ok = false;
                why = "含义问题超过了每场上限（" + std::to_string(n_def) + " 条）";
            }
        }

        // ⑰ **含义绝不进识别提示 / 翻译约束**（它只该进摘要背景和检索）
        //
        // 这条是 2.7 最容易写错、而且写错了最难发现的地方：
        // 一句中文描述混进 Whisper 的 initial_prompt，Whisper 不会报错，
        // 只会**识别得更差**；而 `constraint_terms` 只检查长度上限。
        {
            KnowledgeItem with_def;
            with_def.kind       = "term";
            with_def.key        = "co-re";
            with_def.value      = "CO-RE";
            with_def.status     = "confirmed";
            with_def.confidence = 1.0;
            with_def.definition = u8"Compile Once – Run Everywhere，eBPF 项目的核心方案";

            const auto terms = knowledge::constraint_terms({with_def}, 32);
            if (terms.size() != 1 || terms[0] != "CO-RE") {
                gap_ok = false;
                why = "有含义的术语应只把「写法」交给约束，实际: " +
                      (terms.empty() ? std::string("(空)") : terms[0]);
            }
            for (const auto& t : terms) {
                if (t.find(u8"Compile") != std::string::npos ||
                    t.find(u8"eBPF") != std::string::npos) {
                    gap_ok = false; why = "含义漏进了翻译约束/识别提示";
                }
            }
            // 但它必须进**摘要背景** —— 否则"学了没用上"，闭环在最后一步断掉
            const auto lines = knowledge::background_lines({with_def}, 8);
            bool in_bg = false;
            for (const auto& l : lines) {
                if (l.find(u8"Compile Once") != std::string::npos) in_bg = true;
            }
            if (!in_bg) { gap_ok = false; why = "含义没进摘要背景（用户教了但没用上）"; }
        }

        // ⑱ **交互层独立自检**（步骤 2.8）
        //
        // 【为什么这一组能存在，本身就是架构的证据】它不需要数据库、不需要缺口检测、
        // 不需要 KnowledgeItem —— 只喂 interaction::Prompt 就能验全部文案。
        // 以前文案长在 KnowledgeGap.cpp 里，验它就得先把知识库和检测规则都跑起来。
        //
        // 【这一组守的是产品体验，不是"字符串对不对"】
        // 用户原话：「感觉自己是在给数据库做标注，而不是在教 AI 学习」。
        // 所以下面每一条断言都在守同一件事：**不许把内部状态漏给用户**。
        {
            using interaction::Familiarity;
            using interaction::Kind;
            using interaction::Outcome;
            using interaction::Prompt;
            bool ix_ok = true;
            std::string iwhy;

            auto mkp = [](Kind k, const char* subject) {
                Prompt p;
                p.kind    = k;
                p.subject = subject;
                return p;
            };

            // ① 人名：要说清"我为什么觉得它值得记住"（提到过 = 用户自己的经历，
            //    不是我们的计数器）
            {
                Prompt p = mkp(Kind::ConfirmPersonName, "Marco");
                p.familiarity = Familiarity::ManyTimes;
                const std::string s = interaction::question(p);
                if (s.find("Marco") == std::string::npos) {
                    ix_ok = false; iwhy = "人名问题里没有那个名字";
                } else if (s.find(u8"多次提到") == std::string::npos) {
                    ix_ok = false; iwhy = "没说清为什么值得记（少了'多次提到'这个理由）";
                } else if (s.find(u8"人名") == std::string::npos) {
                    ix_ok = false; iwhy = "没告诉用户这是个什么人名";
                }
            }

            // ② 技术术语：要问"这是不是术语"，而且要问含义
            {
                Prompt p = mkp(Kind::AskTermMeaning, "CO-RE");
                const std::string s = interaction::question(p);
                if (s.find("CO-RE") == std::string::npos ||
                    s.find(u8"概念") == std::string::npos ||
                    s.find(u8"指什么") == std::string::npos) {
                    ix_ok = false; iwhy = "术语问题没同时问到'是不是重要概念'和'它指什么'";
                }
            }

            // ③ 值变化：**必须把"我原来记的是什么"摆出来**
            //    —— 否则用户不知道自己在同意什么，而这一问正是"我在学习"的证据
            {
                Prompt p = mkp(Kind::UpdateChangedValue, "凤凰项目");
                p.previous = u8"10 月上线";
                p.current  = u8"11 月上线";
                const std::string s = interaction::question(p);
                if (s.find(u8"10 月上线") == std::string::npos ||
                    s.find(u8"11 月上线") == std::string::npos) {
                    ix_ok = false; iwhy = "值变化的问题里没有同时出现旧值和新值";
                }
            }

            // ④ 回执：不许是"已确认"，要说明**以后我会拿它做什么**
            {
                Prompt p = mkp(Kind::ConfirmPersonName, "Marco");
                p.answer = "Marco";
                const std::string s = interaction::acknowledgment(p, Outcome::Accepted);
                if (s.find("Marco") == std::string::npos) {
                    ix_ok = false; iwhy = "回执里没有那个名字";
                } else if (s.find(u8"以后") == std::string::npos) {
                    ix_ok = false; iwhy = "回执没说清'以后会怎么用它'（这是闭环感的落点）";
                }
            }

            // ⑤ 教了含义的回执：要复述那份含义 —— 让用户看见"我教的东西被记住了"
            {
                Prompt p = mkp(Kind::AskTermMeaning, "CO-RE");
                p.answer = u8"Compile Once – Run Everywhere";
                const std::string s = interaction::acknowledgment(p, Outcome::MeaningLearned);
                if (s.find(u8"Compile Once") == std::string::npos) {
                    ix_ok = false; iwhy = "教了含义之后回执里没有那份含义";
                }
            }

            // ⑤b 用户的答案自带系词时**不许出现「指的是指的是」**
            //
            // 【真实数据】用户答「指的是电视节目的缩写」，回执成了
            //     「「TV」是指指的是电视节目的缩写」
            // 这是"我记住了"那一句，全篇最该干净的地方。
            {
                Prompt p = mkp(Kind::AskTermMeaning, "TV");
                p.answer = u8"指的是电视节目的缩写";
                const std::string s = interaction::acknowledgment(p, Outcome::MeaningLearned);
                if (s.find(u8"是指指的是") != std::string::npos ||
                    s.find(u8"指的是指的是") != std::string::npos) {
                    ix_ok = false; iwhy = "回执里系词重复了（指的是指的是）";
                } else if (s.find(u8"电视节目的缩写") == std::string::npos) {
                    ix_ok = false; iwhy = "剥系词时把用户答案本身弄丢了";
                }
                // 不带系词的答案必须原样保留
                Prompt q2 = mkp(Kind::AskTermMeaning, "CarsPacked");
                q2.answer = u8"装车，将行李或物品装上车";
                const std::string s2 = interaction::acknowledgment(q2, Outcome::MeaningLearned);
                if (s2.find(u8"装车，将行李或物品装上车") == std::string::npos) {
                    ix_ok = false; iwhy = "不带系词的答案被改动了";
                }
            }

            // ⑤c 开工语：**让复用看得见**（真实数据暴露的问题 —— 复用是静默的）
            {
                interaction::ReuseBrief b;
                const std::string s = interaction::reuse_intro(b);
                if (s.empty()) {
                    ix_ok = false; iwhy = "没有知识时开工语是空的（用户分不清'没记忆功能'和'记忆是空的'）";
                } else if (s.find(u8"从零开始") == std::string::npos) {
                    ix_ok = false; iwhy = "空库的开工语没说清'从零开始'";
                }

                b.items.push_back({"EnglishPod", u8"指的是这个英语博客的名字"});
                b.items.push_back({"不会念这个词", ""});
                b.background_only = 2;
                const std::string s2 = interaction::reuse_intro(b);
                if (s2.find("EnglishPod") == std::string::npos) {
                    ix_ok = false; iwhy = "开工语里没有列出学到的词";
                } else if (s2.find(u8"这个英语博客的名字") == std::string::npos) {
                    ix_ok = false; iwhy = "开工语没带上用户教过的含义";
                } else if (s2.find(u8"带着之前学到的 2 条") == std::string::npos) {
                    ix_ok = false; iwhy = "开工语没报条数";
                } else if (s2.find(u8"不参与识别") == std::string::npos) {
                    ix_ok = false; iwhy = "开工语没说清背景知识不参与识别";
                }
            }

            // ⑥ **内部状态一个字都不许出现**（这一组最重要的一条）
            //
            // 把内部才会有的说法全列出来，任何一种出现在**任何**一条文案里都算失败。
            // 为什么用"禁用词表"这种笨办法：漏一个字段的代价是用户看到
            // 「已经听到 4 次」「识别置信度只有 0.40」这种莫名其妙的东西，
            // 而这类文案改一次很难被发现（没人会去读自己的输出）。
            {
                const char* kForbidden[] = {
                    u8"已经听到", u8"听到 ", u8"次「", u8"一直没确认", u8"最多再问",
                    u8"置信度", u8"candidate", u8"Candidate", u8"confirmed",
                    u8"已确认：", u8"标记为", u8"hits", u8"状态", u8"约束",
                    u8"知识库", u8"入库", u8"跳过（以后",
                };
                // 把各种组合都渲染一遍
                std::vector<Prompt> all;
                for (Kind k : {Kind::ConfirmPersonName, Kind::ConfirmTerm,
                               Kind::AskTermMeaning, Kind::ConfirmProjectName,
                               Kind::UpdateChangedValue, Kind::PickSpelling,
                               Kind::UnifySpelling, Kind::SuggestCorrectSpelling}) {
                    Prompt p = mkp(k, "X");
                    p.previous = "A";
                    p.current  = "B";
                    p.options  = {"X", "Y"};
                    p.answer   = "C";
                    all.push_back(p);
                }
                for (const auto& p : all) {
                    std::vector<std::string> texts = {
                        interaction::question(p),
                        interaction::hint(p),
                        interaction::acknowledgment(p, Outcome::Accepted),
                        interaction::acknowledgment(p, Outcome::Corrected),
                        interaction::acknowledgment(p, Outcome::MeaningLearned),
                        interaction::acknowledgment(p, Outcome::Skipped),
                        interaction::acknowledgment(p, Outcome::Declined),
                    };
                    for (const auto& t : texts) {
                        for (const char* bad : kForbidden) {
                            if (t.find(bad) != std::string::npos) {
                                ix_ok = false;
                                iwhy = std::string("内部状态漏进了文案：命中禁用词「") +
                                       bad + u8"」—— " + t;
                            }
                        }
                    }
                }
                // 开场和收尾也一样要过这一关
                for (const char* bad : kForbidden) {
                    interaction::ReuseBrief rb;
                    rb.items.push_back({"X", "Y"});
                    if (interaction::intro(3).find(bad) != std::string::npos ||
                        interaction::intro(1).find(bad) != std::string::npos ||
                        interaction::reuse_intro(rb).find(bad) != std::string::npos ||
                        interaction::reuse_intro(interaction::ReuseBrief{}).find(bad) != std::string::npos ||
                        interaction::summary(2, 3, 0).find(bad) != std::string::npos ||
                        interaction::no_input_note().find(bad) != std::string::npos) {
                        ix_ok = false;
                        iwhy = std::string("开场/收尾里有内部状态：") + bad;
                    }
                }
            }

            // ⑦ 失败是唯一允许说技术原因的地方 —— 那时候藏着原因更坏
            {
                Prompt p = mkp(Kind::ConfirmTerm, "X");
                p.failure = u8"数据库被锁";
                const std::string s = interaction::acknowledgment(p, Outcome::Failed);
                if (s.find(u8"数据库被锁") == std::string::npos) {
                    ix_ok = false; iwhy = "失败原因没告诉用户";
                }
            }

            std::cout << "[SelfTest] 交互层文案（类型化提问/理由/回执/内部状态不外泄）: "
                      << (ix_ok ? "✅ 通过" : "❌ 失败") << std::endl;
            if (!ix_ok) { std::cerr << "    " << iwhy << std::endl; return 1; }
        }

        // ⑲ **教含义的那条历史不能被当成"值变化过"**
        //
        // 2.8 把"用户教了含义"也记进了 knowledge_history（否则时间线里看不见这一步，
        // 表现为"库里突然有个 definition，不知道谁写的"）。
        // 但 `distinct_values()` 是数"出现过多少个不同的值"来判
        // 「译法不一致」的 —— 含义不是写法，必须排除。
        // 不排除的话，教完含义下一场就会问用户
        // 「「EnglishPod」的写法变过好几次，固定成哪一种？」—— 他完全不知道在说什么。
        {
            std::vector<KnowledgeWithHistory> v = {
                mk(500, "candidate", "EnglishPod", 1, 0.9, 0, ""),
            };
            add_hist(v[0], "EnglishPod", u8"英语播客节目", "definition_defined");
            const auto q = knowledge::detect_gaps(v);
            for (const auto& x : q) {
                if (x.rule == GapRule::InconsistentRendering) {
                    gap_ok = false;
                    why = "教含义的历史被当成了「值变化过好几次」";
                }
            }
            // 但真正的值变化仍然要触发（别把规则修死）
            std::vector<KnowledgeWithHistory> v2 = {
                mk(501, "candidate", "EnglishPod", 1, 0.9, 0, u8"已有含义"),
            };
            add_hist(v2[0], "EnglishPod", "English Pod", "model_extracted");
            add_hist(v2[0], "English Pod", "EnglishPod", "model_extracted");
            const auto q2 = knowledge::detect_gaps(v2);
            bool found = false;
            for (const auto& x : q2) {
                if (x.rule == GapRule::InconsistentRendering) found = true;
            }
            if (!found) {
                gap_ok = false; why = "真正的多次值变化反而没被识别";
            }
        }

        // ⑳ **通用概念不问用户**（用户需求："只问可能只会在我会话里出现的名词"）
        //
        // 【用例全部来自真实会话】下面"该问"和"不该问"两组的键，
        // 都是**真实会话里真的被抽出来过**的候选 —— 可以在库里查回来：
        //   #12（66 段）  EnglishPod / Marco / Erica / TV / PC / people / action / froze
        //   #13（117 段） EnglishPod / Marco / Erika / Marko / TV / putting / together
        //   #47（53 段）  Erica / EnglishPod / Marco / TV / down / Keep / movies / Speaking
        //   demo.db #1    TV / EnglishPod / CarsPacked / WGBH / Marco / Erica
        //
        // 这一组守的是**两个方向**，缺一个都不算对：
        //   ① 通用词不许问（问了就是浪费用户注意力，实测发生过 15 次以上）
        //   ② 真专名**必须还会问** —— 把过滤表做宽到"什么都不问"是最容易犯的错，
        //      而且它会静默地把整个闭环掐死
        {
            struct WCase { const char* key; bool should_ask; };
            const WCase wc[] = {
                // ---- 通用词：不问（都来自真实会话的冤枉提问）----
                {"tv", false},      {"pc", false},      {"down", false},
                {"keep", false},    {"movies", false},  {"speaking", false},
                {"learners", false},{"exactly", false}, {"preview", false},
                {"midnight", false},{"people", false},  {"action", false},
                {"froze", false},   {"putting", false}, {"together", false},
                // 通用缩写（同类推广：没人需要被问"API 是什么"）
                {"api", false},     {"cpu", false},     {"url", false},
                {"json", false},    {"ceo", false},     {"usb", false},
                {"ui", false},      {"sdk", false},     {"csv", false},
                {"pdf", false},     {"png", false},     {"ssh", false},
                // ---- 真专名：必须还是会问 ----
                {"marco", true},    {"erica", true},    {"erika", true},
                {"marko", true},    {"englishpod", true},
                {"carspacked", true}, {"wgbh", true},   {"samuel", true},
                {"co-re", true},    {"phoenix", true},  {"凤凰项目", true},
                // ---- 刻意**不**收的一类：含义随组织而变的缩写 ----
                {"okr", true},      {"kpi", true},      {"mvp", true},
                // 会议里的**角色**缩写。pm/am 曾经被我当"时间"收进通用表，
                // 结果"我们的 PM Penny"里的 PM 被静默挡掉（详见 CommonWords.cpp）
                {"pm", true},       {"am", true},       {"po", true},
                {"em", true},       {"tl", true},
            };
            bool w_ok = true;
            std::string wwhy;
            int w_pass = 0;
            for (const auto& c : wc) {
                const bool gen = commonwords::is_general(c.key);
                const bool asks = !gen;          // is_general → 不会问
                if (asks == c.should_ask) { ++w_pass; continue; }
                w_ok = false;
                wwhy = std::string(c.should_ask ? "该问的却被过滤掉了: " : "不该问的还会问: ")
                       + c.key;
                break;
            }
            std::cout << "[SelfTest] 通用概念过滤（真实会话的冤枉提问 / 真专名不许误杀）: "
                      << (w_ok ? "✅ " + std::to_string(w_pass) + "/" +
                                 std::to_string(sizeof(wc) / sizeof(wc[0])) + " 通过"
                               : "❌ 失败")
                      << std::endl;
            if (!w_ok) { std::cerr << "    " << wwhy << std::endl; return 1; }

            // 表本身不能是空的（手写计数会撒谎那一课：这里报的是真实值）
            if (commonwords::size() < 20) {
                std::cerr << "[SelfTest] 通用词表只有 " << commonwords::size()
                          << " 条 —— 表空了过滤器就等于没有" << std::endl;
                return 1;
            }

            // 行为级：真的喂进 detect_gaps，确认通用词不产生问题
            {
                // ⚠️ **刻意用大写的键**。第一版这几条用例传的是 `"TV"`，
                // 而当时的 `is_general` 只认小写 —— 过滤静默失效、TV 照样被问。
                // 保留大写形式当用例，是为了让"不假设调用方归一化"这道防线
                // 一直被守住（生产路径传的是小写，这里故意传大写）。
                std::vector<KnowledgeWithHistory> v = {
                    mk(600, "candidate", "TV", 5, 0.9, 0, ""),
                    mk(601, "candidate", "down", 9, 0.9, 0, ""),
                };
                if (!knowledge::detect_gaps(v).empty()) {
                    std::cerr << "[SelfTest] 通用词仍然被问出来了"
                                 "（检查 is_general 是否在大小写上失效）" << std::endl;
                    return 1;
                }
                // 但真专名照旧
                std::vector<KnowledgeWithHistory> v2 = {
                    mk(602, "candidate", "EnglishPod", 5, 0.9, 0, ""),
                };
                bool asked = !knowledge::detect_gaps(v2).empty();
                if (!asked) {
                    std::cerr << "[SelfTest] 真专名 EnglishPod 被通用词过滤误杀了" << std::endl;
                    return 1;
                }
            }
        }

        // ㉑ **候选分诊**（步骤 2.10）—— 用脚本化判断器验编排，**不联网**
        //
        // 【为什么这一组必须是脚本化的】第三层判断是联网的模型：不确定、不可复现。
        // 所以判断器做成可注入回调（和确认交互的 `LineReader` 同一招），
        // 这一组喂一段**写死的判决序列**，走真实编排逻辑。
        // 云端实现只是另一个回调 —— 换掉它不影响这里的任何断言。
        {
            using triage::Decision;
            using triage::Verdict;
            bool t_ok = true;
            std::string twhy;

            // 【缓存那一段的 why 单独放，打印时优先】`twhy` 是**覆盖式**的：
            // 后面失败的用例会把前面那条理由盖掉。实测过一次：故意把"判断失败"
            // 写进缓存，报出来的却是后面用例的「要问被写进了缓存」——
            // **症状指错方向，比不报还费时间**。
            // 判断缓存是全组唯一会**静默改变行为**的东西（写错了的症状是
            // "某个词从此再也不被问"，没有任何报错），所以它的理由优先报。
            std::string cache_why;

            // ① 本地通用词表优先级最高：**judge 不该被调用**
            {
                int calls = 0;
                auto judge = [&](const std::string&, const std::string&,
                                 const std::string&, Decision* out) {
                    ++calls;
                    out->verdict = Verdict::Ask;
                    return true;
                };
                const auto d = triage::decide("TV", "term", "", judge);
                if (d.verdict != Verdict::Skip) {
                    t_ok = false; twhy = "本地通用词表命中的词没被挡掉";
                } else if (calls != 0) {
                    // 本地表能定的，**一次网络都不该发** —— 那既是成本也是稳定性
                    t_ok = false; twhy = "本地表已能判定，却还是调了判断器";
                } else if (d.source != "common_words") {
                    t_ok = false; twhy = "来源没标成 common_words（审计时看不出是谁挡的）";
                }
            }

            // ② 模型说通用 → 不问；并带上引子
            {
                auto judge = [](const std::string&, const std::string&,
                                const std::string&, Decision* out) {
                    out->verdict = Verdict::Skip;
                    out->reason  = u8"通用缩写";
                    return true;
                };
                const auto d = triage::decide("WGBH", "term", "", judge);
                if (d.verdict != Verdict::Skip || d.source != "model") {
                    t_ok = false; twhy = "模型判通用时没挡住，或来源没标成 model";
                }
            }

            // ③ 模型说值得问 → 问，而且**引子要带回来**
            {
                auto judge = [](const std::string&, const std::string&,
                                const std::string&, Decision* out) {
                    out->verdict = Verdict::Ask;
                    out->primer  = u8"Compile Once – Run Everywhere";
                    return true;
                };
                const auto d = triage::decide("CO-RE", "term", "", judge);
                if (d.verdict != Verdict::Ask) {
                    t_ok = false; twhy = "模型说值得问却被挡了";
                } else if (d.primer.find("Compile Once") == std::string::npos) {
                    // 引子是"把开放式问题降到接近选择题"的关键，丢了就等于没做
                    t_ok = false; twhy = "引子没传回来（问了也还是开放式，用户要打一整句）";
                }
            }

            // ④ **判断失败必须退化成"问"** —— 这一组最重要的一条
            //
            // 反过来（失败就不问）会造成最坏的一种失败：网络断了 →
            // 系统**悄悄停止学习** → 用户完全看不出来，只会觉得"它好像没在记东西"。
            // 而"多问一次"的代价只是按个回车。一个坏掉的判断器
            // 不该有能力关掉整个闭环。
            {
                auto failing = [](const std::string&, const std::string&,
                                  const std::string&, Decision*) {
                    return false;      // 网络/超时/解析失败
                };
                const auto d = triage::decide("SomePrivateThing", "term", "", failing);
                if (d.verdict != Verdict::Ask) {
                    t_ok = false;
                    twhy = "判断失败时没有退化成「问」—— 那会静默关掉整个学习闭环";
                } else if (d.source != "fallback") {
                    t_ok = false; twhy = "降级路径没标成 fallback";
                }
            }

            // ⑤ 完全没有判断器（没配 Key）→ 也要退化成"问"，而不是不问
            {
                const auto d = triage::decide("SomePrivateThing", "term", "", nullptr);
                if (d.verdict != Verdict::Ask) {
                    t_ok = false; twhy = "没有判断器时没有退化成「问」";
                }
                // 但本地表仍然生效 —— 没 Key 不等于连通用词都挡不住
                const auto d2 = triage::decide("down", "term", "", nullptr);
                if (d2.verdict != Verdict::Skip) {
                    t_ok = false; twhy = "没有判断器时本地通用词表也失效了";
                }
            }

            // ⑥ 没配 Key 时 make_cloud_judge 必须返回**空**判断器
            //    （返回一个"永远说不值得问"的判断器是最坏的做法：
            //      那会让闭环在没配 Key 的时候静默停摆）
            {
                const auto j = triage::make_cloud_judge("");
                if (j) {
                    t_ok = false; twhy = "没有 Key 时不该造出可用的判断器";
                }
            }

            // ⑦ 分诊层**不许**写知识库 —— 结构层面已经在头文件里保证了
            //    （它没有 include KnowledgeStore.h），这里再做一次行为级确认：
            //    上面所有 decide 调用前后，库里的条目数不变。
            {
                auto& ksx = KnowledgeStore::instance();
                const int before = static_cast<int>(ksx.list("", 10000).size());
                auto judge = [](const std::string&, const std::string&,
                                const std::string&, Decision* out) {
                    out->verdict = Verdict::Ask;
                    out->primer  = u8"模型猜的东西";
                    return true;
                };
                (void)triage::decide("InventedThing", "term", "", judge);
                const int after = static_cast<int>(ksx.list("", 10000).size());
                if (before != after) {
                    t_ok = false;
                    twhy = "分诊层往知识库里写了东西 —— §6.5 红线要求模型猜的东西绝不能入库";
                }
            }

            // ⑧ **引子 + 用户认可 → 含义落库**；没有引子时**不许编含义**
            //
            // 【为什么这条必须离线可验】它碰的是 §6.5 红线附近的东西：
            // "模型猜的内容"经用户确认后入库。这条路径不能只靠"某次用真 Key
            // 跑通了"来保证 —— 那不可复现，而且换个人接手就断了。
            {
                auto& ksx = KnowledgeStore::instance();
                const std::string PK = "__selftest_primer_";
                ksx.purge_key_prefix(PK);

                KnowledgeItem it;
                it.kind       = "term";
                it.key        = PK + "core";
                it.value      = "CO-RE";   // ⚠️ 刻意让 key ≠ normalize_key(value)：
                                           // 用例要守住"apply_answer 用 q.key，
                                           // 而不是从展示形反推"这条（曾经真的推错过）
                it.status     = "candidate";
                it.confidence = 0.9;
                std::string e;
                const long long id = ksx.upsert(it, &e);
                if (id <= 0) { t_ok = false; twhy = "自检造候选失败"; }

                // --- 有引子：答 y 应该把**引子**写成含义 ---
                if (t_ok) {
                    knowledge::GapQuestion gq;
                    gq.rule               = GapRule::AskDefinition;
                    gq.knowledge_id       = id;
                    gq.kind               = "term";
                    gq.key                = PK + "core";
                    gq.value              = "CO-RE";
                    gq.suggested_meaning  = u8"Compile Once – Run Everywhere";

                    knowledge::Answer a;
                    a.kind = knowledge::AnswerKind::Affirm;
                    const auto r = knowledge::apply_answer(gq, a, &e);

                    KnowledgeItem got;
                    ksx.get("term", PK + "core", &got);
                    if (r.outcome != knowledge::ConfirmOutcome::ConfirmedNewValue) {
                        t_ok = false;
                        twhy = "用户认可了引子，却没有被当成「学到了含义」";
                    } else if (got.definition.find("Compile Once") == std::string::npos) {
                        // 用户明明确认了一份含义，库里却什么都没存 —— 这一环断了
                        t_ok = false;
                        twhy = "引子没有落库（用户点头认可的含义丢了）";
                    } else if (got.status != "confirmed") {
                        t_ok = false; twhy = "认可引子之后状态没变 confirmed";
                    }
                }

                // --- 没有引子：答 y 只表示"这是个重要概念"，**不许编含义** ---
                if (t_ok) {
                    KnowledgeItem it2;
                    it2.kind       = "term";
                    it2.key        = PK + "unknown";
                    it2.value      = "SomeTerm";
                    it2.status     = "candidate";
                    it2.confidence = 0.9;
                    const long long id2 = ksx.upsert(it2, &e);

                    knowledge::GapQuestion gq;
                    gq.rule         = GapRule::AskDefinition;
                    gq.knowledge_id = id2;
                    gq.kind         = "term";
                    gq.key          = PK + "unknown";
                    gq.value        = "SomeTerm";
                    // suggested_meaning 故意留空

                    knowledge::Answer a;
                    a.kind = knowledge::AnswerKind::Affirm;
                    const auto r = knowledge::apply_answer(gq, a, &e);

                    KnowledgeItem got;
                    ksx.get("term", PK + "unknown", &got);
                    if (!got.definition.empty()) {
                        // 编一句含义出来会静默污染摘要背景 —— 那是这类 bug 里最难查的
                        t_ok = false;
                        twhy = "没有引子时凭空编了一条含义出来";
                    } else if (r.outcome != knowledge::ConfirmOutcome::ConfirmedExisting) {
                        t_ok = false; twhy = "没有引子时的确认结果不对";
                    }
                }

                ksx.purge_key_prefix(PK);
            }

            // ⑨ **按类型换问法**（2026-09-19，用户明确要的效果）
            //
            // 【为什么这条必须离线可验】它靠真 Key 那一次跑是能看见的，
            // 但"能看见"不等于"有回归保护" —— 下次谁改了 kind 的路由，
            // 没有人会发现问法退回了"这是重要概念吗"。
            //
            // 用户的需求原话：「陌生的人的花名例如 penny → 问我这似乎是一个人名，
            // **具体是什么身份**；陌生的产品名类似询问；新的项目名字也询问」
            {
                struct RouteCase {
                    const char* kind;        // 候选的 kind
                    const char* must_contain; // 问句里必须出现
                };
                const RouteCase rc[] = {
                    {"person",  u8"人名"},      // 问身份
                    {"project", u8"项目"},      // 问用途
                    {"product", u8"产品"},      // 问定位
                    {"term",    u8"概念"},      // 问含义
                };
                for (const auto& c : rc) {
                    knowledge::GapQuestion gq;
                    gq.rule         = GapRule::AskDefinition;
                    gq.knowledge_id = 1;
                    gq.kind         = c.kind;
                    gq.key          = "x";
                    gq.value        = "Penny";
                    const auto pr = knowledge::to_prompt(gq);
                    const std::string qtext = interaction::question(pr);
                    if (qtext.find(c.must_contain) == std::string::npos) {
                        t_ok = false;
                        twhy = std::string("kind=") + c.kind +
                               u8" 的问法没问到点子上（应含「" + c.must_contain +
                               u8"」），实际: " + qtext;
                        break;
                    }
                }
                // 人名那一问必须问**身份**，不能退化成问"含义"
                if (t_ok) {
                    knowledge::GapQuestion gq;
                    gq.rule = GapRule::AskDefinition;
                    gq.knowledge_id = 1;
                    gq.kind = "person";
                    gq.key = "x";
                    gq.value = "Penny";
                    const std::string qtext =
                        interaction::question(knowledge::to_prompt(gq));
                    if (qtext.find(u8"他是谁") == std::string::npos) {
                        t_ok = false;
                        twhy = u8"人名那一问没问「他是谁」—— 那正是用户要的效果";
                    }
                }
                // 产品那一问不能退化成"技术术语"
                if (t_ok) {
                    knowledge::GapQuestion gq;
                    gq.rule = GapRule::AskDefinition;
                    gq.knowledge_id = 1;
                    gq.kind = "product";
                    gq.key = "x";
                    gq.value = "Gecko";
                    const std::string qtext =
                        interaction::question(knowledge::to_prompt(gq));
                    if (qtext.find(u8"技术术语") != std::string::npos) {
                        t_ok = false;
                        twhy = u8"产品名被问成了「技术术语」—— 用户会觉得系统在胡乱归类";
                    }
                }
                // person / project / product 都必须是"名字类"（否则进不了三条腿）
                if (t_ok) {
                    for (const char* k : {"person", "project", "product", "term"}) {
                        if (!knowledge::is_name_like_kind(k)) {
                            t_ok = false;
                            twhy = std::string(k) + u8" 不被当成名字类 —— 它进不了识别提示";
                            break;
                        }
                    }
                }
                // 四种类型都必须是**合法 kind**（否则写库会被拒）
                if (t_ok) {
                    for (const char* k : {"person", "project", "product", "term"}) {
                        if (!knowledge::is_valid_kind(k)) {
                            t_ok = false;
                            twhy = std::string("kind 白名单里没有 ") + k;
                            break;
                        }
                    }
                }
            }

            // ⑩ **判断缓存**（步骤 2.12）—— 用内存 map 当缓存，验的是**闸**
            //
            // 【这一组要证明的不是"缓存能命中"，而是"缓存不会乱命中"】
            // 命中是显而易见的；真正会出事的是**不该写的时候写了**：
            // 把一次网络抖动固化成"这个词永远不问"，而用户完全看不出来。
            // 所以下面四条里有三条是在证明"没写进去"。
            {
                // 内存缓存：和 SQLite 版实现同一对回调，但完全不碰数据库 ——
                // 这正是"可注入回调"这个模式的价值：编排逻辑离线可验。
                struct MemCache {
                    std::map<std::string, triage::CachedVerdict> m;
                    std::vector<std::string> writes;
                } mc;

                // 【缓存这一组用独立的 why（上面声明的 cache_why）】这段是本组唯一
                // 涉及"静默改变行为"的用例：缓存写错了，症状是**某个词从此再也不被问**，
                // 用户看不到任何报错。而 `twhy` 是覆盖式的，后面的失败会盖掉它 ——
                // 刚刚实测就是这样。所以这里用第一条失败锁定的 `cfail`。
                auto cfail = [&](const std::string& m) {
                    t_ok = false;
                    if (cache_why.empty()) cache_why = m;   // 第一条失败锁定
                };

                triage::Cache cache;
                cache.lookup = [&mc](const std::string& v, triage::CachedVerdict* out) {
                    auto it = mc.m.find(v);
                    if (it == mc.m.end()) return false;
                    *out = it->second;
                    return true;
                };
                cache.store = [&mc](const std::string& v, const std::string&,
                                    const triage::Decision& d) {
                    mc.writes.push_back(v);
                    triage::CachedVerdict cv;
                    cv.verdict = d.verdict;
                    cv.primer  = d.primer;
                    cv.why     = d.reason;
                    mc.m[v] = cv;
                };

                // (a) 模型判"不问" → 落缓存
                {
                    int calls = 0;
                    auto judge = [&calls](const std::string&, const std::string&,
                                          const std::string&, Decision* out) {
                        ++calls;
                        out->verdict = Verdict::Skip;
                        out->reason  = u8"通用缩写";
                        return true;
                    };
                    const auto d1 = triage::decide("ZZTestA", "term", "", judge, cache);
                    if (d1.verdict != Verdict::Skip || d1.source != "model") {
                        cfail("模型判不问时没挡住");
                    } else if (mc.writes.size() != 1) {
                        cfail("模型判「不问」没落缓存（缓存等于没做）");
                    }

                    // (b) **第二次必须走缓存、且不再调判断器** —— 这才是省下来的那次调用
                    if (t_ok) {
                        const auto d2 = triage::decide("ZZTestA", "term", "", judge, cache);
                        if (d2.source != "cache") {
                            cfail("第二次没走缓存（source=" + d2.source + "）");
                        } else if (calls != 1) {
                            cfail("缓存命中了却还是联网问了一次 —— 白缓存");
                        } else if (d2.verdict != Verdict::Skip) {
                            cfail("缓存里的「不问」没被采信");
                        } else if (d2.reason.find("缓存") == std::string::npos) {
                            // 看日志的人必须能分辨"模型刚判的"和"缓存里的旧结论"，
                            // 否则会误以为网络是通的
                            cfail("缓存命中的理由里看不出这是缓存的旧结论");
                        }
                    }
                }

                // (c) **判断失败绝不能落缓存** —— 本层唯一的严重风险
                //
                // 反例的具体后果：某次网络抖动 → 模型调用失败 →
                // 如果把这次失败当"不问"存下来，那个词**以后永远不再被问**，
                // 而用户看不到任何报错。所以这条闸必须验。
                {
                    const size_t before = mc.writes.size();
                    auto failing = [](const std::string&, const std::string&,
                                      const std::string&, Decision*) { return false; };
                    const auto d = triage::decide("ZZTestB", "term", "", failing, cache);
                    if (d.verdict != Verdict::Ask) {
                        cfail("判断失败时没退化成「问」");
                    } else if (mc.writes.size() != before) {
                        cfail("判断失败被写进了缓存 —— 那会把一次网络抖动"
                               "固化成「这个词永远不问」");
                    }
                }

                // (d) **模型判"要问"也不落缓存**
                //     Ask 由 KnowledgeGap 的「问过不再问」负责；缓存它等于同一件事做两遍，
                //     而且会把那一刻模型猜的引子冻住（以后越猜越准的能力就没了）。
                {
                    const size_t before = mc.writes.size();
                    auto judge = [](const std::string&, const std::string&,
                                    const std::string&, Decision* out) {
                        out->verdict = Verdict::Ask;
                        out->primer  = u8"模型猜的";
                        return true;
                    };
                    const auto d = triage::decide("ZZTestC", "term", "", judge, cache);
                    if (d.verdict != Verdict::Ask) {
                        cfail("模型说值得问却被挡了");
                    } else if (mc.writes.size() != before) {
                        cfail("「要问」被写进了缓存");
                    }
                }

                // (e) **本地词表命中时不读缓存**：本地表的结论是确定的，
                //     而缓存是"以前的模型判决"。让可能过时的缓存盖住确定结论，
                //     等于拿确定性换历史偶然。
                {
                    auto judge = [](const std::string&, const std::string&,
                                    const std::string&, Decision* out) {
                        out->verdict = Verdict::Ask;
                        return true;
                    };
                    // 先让模型把一个词判成"不问"，制造出与本地表冲突的缓存
                    (void)triage::decide("down", "term", "", judge, cache);
                    const auto d = triage::decide("down", "term", "", judge, cache);
                    if (d.source != "common_words") {
                        cfail("本地通用词表的结论被缓存盖住了（source=" + d.source + "）");
                    }
                }

                // (f) 空缓存 / 只给 store 不给 lookup → **必须等于没有缓存**，
                //     否则会出现"一直在写、从来读不到"这种看起来在工作的假象
                {
                    triage::Cache half;
                    half.store = [](const std::string&, const std::string&,
                                    const triage::Decision&) {};
                    if (static_cast<bool>(half)) {
                        cfail("只给 store 的 Cache 被当成了「有缓存」");
                    }
                }
            }

            std::cout << "[SelfTest] 候选分诊（本地表优先/缓存不误写/模型判决/引子/失败降级/不写库）: "
                      << (t_ok ? "✅ 通过" : "❌ 失败") << std::endl;
            if (!t_ok) {
                std::cerr << "    "
                          << (cache_why.empty() ? twhy : cache_why) << std::endl;
                return 1;
            }
        }

        // ㉒ **判断缓存落库**（步骤 2.12）—— 用**真实的** TriageCache（SQLite）跑
        //
        // 【为什么 ⑩ 之外还要再验一遍】⑩ 用的是内存 map，它证明的是**编排**对；
        // 缓存真正的实现是 SQLite 那张表，而它完全没被碰过。
        // 这是本项目栽过三次的同一个坑：**验了逻辑没验落点** ——
        // "编排调用了一个 store 回调"和"那一行真的写进了 triage_verdicts"
        // 是两件事，中间可以整段是坏的（SQL 拼错、表名错、绑定错）而 ⑩ 照样全绿。
        //
        // 这一组走的是**真实实现**：真表、真 SQL、真往返。用的是一次性 scratch 库，
        // 全程不联网（判断器是脚本化的）。
        {
            using triage::Decision;      // ㉑ 的 using 在它自己的块里，这里要重新引入
            using triage::Verdict;
            auto& tc = triage::TriageCache::instance();
            bool c_ok = true;
            std::string cwhy;
            const std::string KW = "zzcacheprobe";

            tc.forget(KW, nullptr);        // 先清干净（上一次自检可能留下）

            if (!tc.ready()) {
                c_ok = false;
                cwhy = "自检的 scratch 库里没有 triage_verdicts 表 —— "
                       "schema 没建这张表，线上会静默退化成「没有缓存」";
            }

            const triage::Cache real = tc.hooks();
            if (c_ok && !real) {
                c_ok = false;
                cwhy = "ready() 说表在，hooks() 却没给出可用缓存";
            }

            // (a) 走一遍真实编排：模型判"不问" → 应该真的落进表里
            if (c_ok) {
                auto skip_judge = [](const std::string&, const std::string&,
                                     const std::string&, Decision* out) {
                    out->verdict = Verdict::Skip;
                    out->reason  = u8"自检：通用缩写";
                    return true;
                };
                (void)triage::decide(KW, "term", "", skip_judge, real);
                if (tc.list(500).empty()) {
                    c_ok = false;
                    cwhy = "走了真实编排，但 triage_verdicts 里一行都没有";
                }
            }

            // (b) 换一个**会说"要问"**的判断器：命中缓存就必须仍然是"不问"。
            //
            // 这一条才是"缓存真的生效了"的证据：不是"表里有行"，
            // 而是**这一行改变了后续的判决**。同时它也证明了命中时不联网 ——
            // 这个判断器一次都不该被调到。
            if (c_ok) {
                int calls = 0;
                auto ask_judge = [&calls](const std::string&, const std::string&,
                                          const std::string&, Decision* out) {
                    ++calls;
                    out->verdict = Verdict::Ask;
                    return true;
                };
                const auto d = triage::decide(KW, "term", "", ask_judge, real);
                if (d.source != "cache") {
                    c_ok = false;
                    cwhy = "第二次没命中缓存（source=" + d.source + "）—— "
                           "可能是 key 归一化和写入时不一致";
                } else if (calls != 0) {
                    c_ok = false;
                    cwhy = "命中缓存却还是调了判断器（等于没缓存）";
                } else if (d.verdict != Verdict::Skip) {
                    c_ok = false; cwhy = "缓存里的「不问」没被采信";
                }
                // 复用计数必须真的涨了 —— 它是"省了多少次调用"的唯一口径，
                // 不涨的话 `--triage-cache` 报出来的数字就是假的
                if (c_ok) {
                    int reused = 0;
                    for (const auto& e : tc.list(500)) {
                        if (knowledge::normalize_key(e.value) == KW) reused = e.reused;
                    }
                    if (reused < 1) {
                        c_ok = false;
                        cwhy = "命中了但 reused 没累加 —— --triage-cache 报的"
                               "「省了多少次调用」会是假的";
                    }
                }
            }

            // (c) 撤销入口必须真的能撤：忘掉之后，同一个词必须重新走判断器
            if (c_ok) {
                std::string e;
                if (!tc.forget(KW, &e)) {
                    c_ok = false; cwhy = "forget() 失败：" + e;
                } else {
                    int calls = 0;
                    auto skip2 = [&calls](const std::string&, const std::string&,
                                          const std::string&, Decision* out) {
                        ++calls;
                        out->verdict = Verdict::Skip;
                        return true;
                    };
                    (void)triage::decide(KW, "term", "", skip2, real);
                    if (calls != 1) {
                        c_ok = false;
                        cwhy = "forget 之后没有重新判断 —— 撤销入口是假的，"
                               "判错的词将永远无法恢复";
                    }
                }
                tc.forget(KW, nullptr);    // 收尾，别把探针留在库里
            }

            std::cout << "[SelfTest] 判断缓存落库（真实 SQLite 往返/命中不联网/撤销/reused）: "
                      << (c_ok ? "✅ 通过" : "❌ 失败") << std::endl;
            if (!c_ok) { std::cerr << "    " << cwhy << std::endl; return 1; }
        }

        // ㉓ **行动项落库 + 跨会话聚合**（§7 步骤 5.5）
        //
        // 【为什么这一组必须存在】5.5 之前 `actions` 表**一行都没被写过**，
        // 所以"落库"这条路从来没被任何测试或真跑碰过 —— 里面写错什么都不会有人发现。
        // 而它有三个"错了会静默失败"的点：
        //   ① 聚合判同（错了 → 同一件事攒成 N 条，或者两件事并成一条）
        //   ② 幂等（错了 → 数字凭空涨，而那个数字是给用户看的）
        //   ③ 状态不被重跑打回（错了 → 用户标好的 done 被下周的会打回 todo）
        {
            using actions::ActionStore;
            using actions::Item;
            bool a_ok = true;
            std::string a_why;
            auto afail = [&](const std::string& m) {
                a_ok = false;
                if (a_why.empty()) a_why = m;      // 第一条失败锁定
            };

            // ---- ① 纯函数：判同。**用的是演示数据里那三对真实句子** ----
            //
            // 需求原话要的就是"上周说的事这周还记得"，而这件事能不能成立
            // 全押在这个判同函数上。所以用例直接用当时端到端跑出来的句子。
            {
                struct Pair { const char* a; const char* b; bool same; const char* why; };
                const Pair ps[] = {
                    // 同一件事，第二场多说了一句补充 → **必须合并**
                    //（实测：第一版只有"key 完全相同"时这一对没合上，
                    //   三场会攒出 8 条重复任务，聚合等于没做）
                    {"凤凰项目的接口文档需要重写，这块张伟负责跟进。",
                     "凤凰项目的接口文档需要重写，这个上周就说了，张伟负责跟进，这周五之前完成。",
                     true, "同一任务加了一句补充，没被判成同一件事"},
                    // 换了个说法、差异在**分句内部**的同一件事 → **不合并**。
                    //
                    // ⚠️ 这一条断言的是**已知的漏判**，不是"正确行为"。
                    //    「另外李经理需要提交一份极光平台的压测报告。」
                    //    「李经理需要提交一份压测报告，负责极光平台那部分。」
                    //    这是同一件事，但相同片段「李经理需要提交一份」在两条里
                    //    都不是分句结尾 —— 确定性规则认不出来。
                    //    它交给可注入的判断器（模型）那一档。
                    //
                    // 【为什么断言 false 而不是把规则改松直到它变成 true】
                    //    因为把规则改松就会顺手把上面「重写 vs 评审」也合掉，
                    //    而那是**一条真待办从列表里消失**。两害相权：
                    //    不合并 = 多一行（用户看得见）；错合并 = 少一条（看不见）。
                    //    所以这条断言的作用是**把这个取舍钉住**：
                    //    哪天有人想放松规则，他会先看到这条用例在说什么。
                    {"另外李经理需要提交一份极光平台的压测报告。",
                     "李经理需要提交一份压测报告，负责极光平台那部分。",
                     false, "已知漏判：差异在分句内部的换说法没被合并 —— "
                            "如果这条变成失败，说明有人放松了判同规则，"
                            "请先确认「重写 vs 评审」那条还守得住"},
                    // ⚠️ **这一对是最危险的一对**：用词极像，但是两件事。
                    // 相似度阈值在这里必错（它一定判"很像"）；
                    // 最初"连续相同 ≥8 字"的写法也错了 —— 这两条有 11 个字相同，
                    // 自检当场报了出来。能分开它们的是"相同片段要在两边都
                    // 落在分句结尾"：这里差异（重写/评审）落在**分句的谓语上**，
                    // 也就是主干被改了，那是两件事。
                    {"凤凰项目的接口文档需要重写。",
                     "凤凰项目的接口文档需要评审。",
                     false, "两个不同任务（重写 vs 评审）被错误合并了"},
                    {"需要在周五之前提交压测报告。",
                     "需要在周五之前安排安全评审。",
                     false, "同前缀的两个不同任务被错误合并了"},
                    // 空键不参与判同（不能因为"两条都算不出键"就把它们并起来）
                    {"", "随便什么内容", false, "空标题被当成了同一件事"},
                };
                for (const auto& p : ps) {
                    Item ia, ib;
                    ia.key = actions::normalize_title(p.a); ia.title = p.a;
                    ib.key = actions::normalize_title(p.b); ib.title = p.b;
                    const bool got = actions::same_thing(ia, ib);
                    if (got != p.same) afail(p.why);
                }
            }

            // ---- ② 归一化要忽略大小写/标点/空白 ----
            {
                if (actions::normalize_title("Rewrite  the  API docs!") !=
                    actions::normalize_title("rewrite the api docs")) {
                    afail("归一化没有忽略大小写/多余空白/末尾标点");
                }
                if (actions::normalize_title(u8"重写接口文档。") !=
                    actions::normalize_title(u8"重写接口文档")) {
                    afail("归一化没有把中文句号当分隔符处理");
                }
                // 全角逗号也要当分隔符（中文会议里最常见的标点）
                if (actions::normalize_title(u8"重写，接口文档") !=
                    actions::normalize_title(u8"重写 接口文档")) {
                    afail("归一化没有把全角逗号当分隔符处理");
                }
            }

            // ---- ③ 真库往返 + 幂等 + 状态（走真实 ActionStore）----
            const long long SID = 900001;      // 一次性探针会话号，刻意避开真实段
            {
                // 先清干净（上一次自检可能留了）
                auto purge = [&]() {
                    for (const auto& it : ActionStore::list("", 500)) {
                        if (it.title.find("ZZActionProbe") != std::string::npos) {
                            std::string e;
                            ActionStore::set_status(it.id, "done", &e);  // 先移出未完成集合
                        }
                    }
                };
                (void)purge;

                std::vector<actions::Incoming> batch;
                {
                    actions::Incoming i1;
                    i1.title = u8"ZZActionProbe 提交凤凰项目的接口文档";
                    i1.owner = u8"张伟";
                    i1.due   = u8"周五";
                    i1.evidence = u8"张伟说他会在这周五之前完成。";
                    i1.source_seq = 3;
                    batch.push_back(i1);
                    // 同一个任务的换说法版本（应该并进去，不该新建）
                    actions::Incoming i2;
                    i2.title = u8"ZZActionProbe 提交凤凰项目的接口文档，这周五之前";
                    batch.push_back(i2);
                }

                const auto o1 = ActionStore::ingest(SID, batch, nullptr, "summary");
                if (!o1.err.empty()) afail("落库报错：" + o1.err);
                if (o1.inserted != 1 || o1.merged != 1) {
                    afail("第一轮应为「新建 1 / 并入 1」，实际 新建 "
                          + std::to_string(o1.inserted) + " / 并入 "
                          + std::to_string(o1.merged));
                }

                // **幂等**：同一场再 ingest 一次，一条都不该多
                const auto o2 = ActionStore::ingest(SID, batch, nullptr, "summary");
                if (o2.inserted != 0 || o2.linked != 0) {
                    afail("重复 ingest 不幂等：新建 " + std::to_string(o2.inserted)
                          + " 条、台账新增 " + std::to_string(o2.linked)
                          + " 条（都应为 0）—— 会把「提到过几次」这个数字弄假");
                }

                // 找到那条，核对派生字段
                long long pid = 0;
                for (const auto& it : ActionStore::for_session(SID)) {
                    if (it.title.find("ZZActionProbe") != std::string::npos) pid = it.id;
                }
                if (pid == 0) {
                    afail("台账里查不到刚写进去的行动项（for_session 失效）");
                } else {
                    Item got;
                    if (!ActionStore::get(pid, &got)) {
                        afail("get() 读不回刚写的行动项");
                    } else {
                        if (got.owner != u8"张伟") {
                            // 并入时"空字段才填"：i2 没带 owner，不能把 i1 的覆盖成空
                            afail("并入时把已有的负责人覆盖成空了（应为 张伟，实际「"
                                  + got.owner + "」）");
                        }
                        if (got.status != "todo") afail("新建的行动项状态不是 todo");
                        if (got.seen_sessions != 1) {
                            afail("seen_sessions 应为 1，实际 "
                                  + std::to_string(got.seen_sessions));
                        }
                    }

                    // ---- 状态流转 ----
                    std::string se;
                    if (!ActionStore::set_status(pid, "doing", &se)) {
                        afail("标 doing 失败：" + se);
                    }
                    if (!ActionStore::set_status(pid, "done", &se)) {
                        afail("标 done 失败：" + se);
                    }
                    // 【关键】标成 done 之后再 ingest 同一场：
                    // 不许新建一行（这是修过的真 bug —— 原来排除 done 之后
                    // "用户标完成"这个动作本身就会导致重复行）
                    const auto o3 = ActionStore::ingest(SID, batch, nullptr, "summary");
                    if (o3.inserted != 0) {
                        afail("标成 done 之后再 ingest 又新建了 "
                              + std::to_string(o3.inserted)
                              + " 行 —— 用户的一个操作导致了重复待办");
                    }
                    Item after;
                    if (ActionStore::get(pid, &after) && after.status != "done") {
                        afail("新一场的 ingest 把用户标好的 done 打回了 "
                              + after.status + "（用户做完的事不该被重跑复活）");
                    }

                    // ---- 非法状态必须被拒 ----
                    if (ActionStore::set_status(pid, "finished", &se)) {
                        afail("非法状态 finished 被接受了");
                    }
                    // 不存在的 id 要报错，而**"状态没变"不算错**（§2.6d 同类）
                    if (ActionStore::set_status(999999999, "todo", &se)) {
                        afail("不存在的 id 却报成功");
                    }
                    if (!ActionStore::set_status(pid, "done", &se)) {
                        afail("把已经是 done 的再标一次 done 报了失败 —— "
                              "「无需变更」不是错误");
                    }
                }
            }

            std::cout << "[SelfTest] 行动项落库（判同/幂等/状态不打回/空字段不覆盖/台账）: "
                      << (a_ok ? "✅ 通过" : "❌ 失败") << std::endl;
            if (!a_ok) { std::cerr << "    " << a_why << std::endl; return 1; }
        }

        // ㉔ **出处（5.7）**：格式、从散文里捞引用、以及**证伪**
        //
        // 【为什么"证伪"要和"打印"一起验】这一节的价值全在证伪那一半。
        // 编出来的出处（`#9002·7` 当那一场只有 4 段）和真的长得一模一样，
        // 所以"能标出处"这件事本身**不构成任何保证**；
        // 只有"能当场抓出对不上的"才是。
        {
            using evidence::Locator;
            bool e_ok = true;
            std::string e_why;
            auto efail = [&](const std::string& m) {
                e_ok = false;
                if (e_why.empty()) e_why = m;
            };

            // ---- ① 格式：唯一形状 ----
            {
                Locator l; l.session_id = 9002; l.seq = 3;
                if (evidence::format(l) != u8"#9002·3") {
                    efail("出处格式不对：应为 #9002·3，实际 " + evidence::format(l));
                }
                Locator bad;                    // session_id=-1, seq=0
                if (!evidence::format(bad).empty()) {
                    efail("无效位置却格式化出了字符串（会让人以为引用是有效的）");
                }
            }

            // ---- ② 从散文里捞引用 ----
            {
                const std::string text =
                    u8"接口文档需要重写（#9002·2）。压测报告由李经理负责（#9002·3）。"
                    u8"另外 #9002·2 又提了一次。";
                const auto got = evidence::extract(text);
                if (got.size() != 2) {
                    // 去重后应是 2（第三个是重复的同一处）
                    efail("从散文里捞引用应为 2 处（去重后），实际 "
                          + std::to_string(got.size()));
                } else if (got[0].session_id != 9002 || got[0].seq != 2 ||
                           got[1].seq != 3) {
                    efail("捞出来的引用顺序/内容不对（应保持出现顺序）");
                }

                // ⚠️ **不该被误抓的几种**（正文里到处都是这些形状）
                struct Neg { const char* text; const char* why; };
                const Neg negs[] = {
                    {u8"这个问题 #3 很关键", u8"光有段号没有会话号也被抓了"},
                    {u8"会议定在 14:01 · 三点开始", u8"正文里的时间被当成引用了"},
                    {u8"#9002 这一场整体不错", u8"只有会话号也被抓了"},
                    {u8"第 3 段说了这件事", u8"「第 3 段」这种说法被当成引用了"},
                    {u8"C++ 里 #define 是预处理指令", u8"代码片段被当成引用了"},
                };
                for (const auto& n : negs) {
                    if (!evidence::extract(n.text).empty()) efail(n.why);
                }
            }

            // ---- ③ 证伪：这是本组最重要的一条 ----
            {
                // 真库（自检的一次性 scratch 库）里造一场 3 段的会话
                auto& store = SessionStore::instance();
                const long long sid2 = store.begin_session("SelfTest", "出处用例");
                if (sid2 <= 0) {
                    efail("开不了自检会话");
                } else {
                    store.log_segment(u8"第一段：接口文档需要重写", u8"第一段", "SelfTest", 0);
                    store.log_segment(u8"第二段：压测报告由李经理负责", u8"第二段", "SelfTest", 0);
                    store.log_segment(u8"第三段：安全评审定在下周三", u8"第三段", "SelfTest", 0);
                    store.end_session();

                    Locator good; good.session_id = sid2; good.seq = 2;
                    Segment s;
                    if (!evidence::Evidence::resolve(good, &s)) {
                        efail("真实存在的段落却 resolve 失败（引用全会被误判成编造）");
                    } else if (s.src_text.find(u8"压测报告") == std::string::npos) {
                        efail("resolve 回来的不是那一段（段号对错了行）");
                    }

                    // 越界：那一场只有 3 段
                    Locator oob; oob.session_id = sid2; oob.seq = 99;
                    if (evidence::Evidence::resolve(oob, nullptr)) {
                        efail("越界的段号被判成存在 —— 编造的出处抓不出来");
                    }
                    // 不存在的会话
                    Locator nos; nos.session_id = 987654321; nos.seq = 1;
                    if (evidence::Evidence::resolve(nos, nullptr)) {
                        efail("不存在的会话被判成存在");
                    }
                    if (evidence::Evidence::segment_count(sid2) != 3) {
                        efail("segment_count 不对（应为 3，实际 "
                              + std::to_string(evidence::Evidence::segment_count(sid2)) + "）");
                    }

                    // 整段文本核对：1 真 + 2 假 → 必须报 ok=1 bad=2
                    const std::string report =
                        u8"结论一（" + evidence::format(good) + u8"）。"
                        u8"结论二（" + evidence::format(oob) + u8"）。"
                        u8"结论三（" + evidence::format(nos) + u8"）。";
                    const auto vr = evidence::Evidence::verify(report);
                    if (vr.total != 3) {
                        efail("核对应识别 3 处，实际 " + std::to_string(vr.total));
                    } else if (vr.ok != 1) {
                        efail("应只有 1 处有效，实际 " + std::to_string(vr.ok));
                    } else if (vr.bad.size() != 2) {
                        efail("应报 2 处对不上（编造/越界），实际 "
                              + std::to_string(vr.bad.size()));
                    }

                    // 一份**没有出处**的报告：不能报错，但也不能算"通过"
                    // （这一条对应 --verify-report 的 exit 2 —— 零引用不是成功）
                    const auto vr0 = evidence::Evidence::verify(u8"这次会开了三个主题。");
                    if (vr0.total != 0 || vr0.ok != 0 || !vr0.bad.empty()) {
                        efail(u8"没有出处的文本被算成了有问题（应该只是「零引用」）");
                    }
                }
            }

            std::cout << "[SelfTest] 出处（格式/从散文捞引用/证伪编造与越界/零引用）: "
                      << (e_ok ? "✅ 通过" : "❌ 失败") << std::endl;
            if (!e_ok) { std::cerr << "    " << e_why << std::endl; return 1; }
        }

        // ㉕ **负责人抽取**：宁可留空，也不许写出一个不存在的人
        //
        // 【为什么值得单开一组】交付物里"负责人"那一栏是**评审照着去找人**的字段，
        // 写错比留空坏得多。而旧实现是"从 负责/跟进 往前数 4 个汉字"：
        //     「…说了，张伟负责跟进」→ 「了，张伟」   ← 把标点数进去了
        //     「这块张伟负责跟进」    → 「这块张伟」   ← 多带了修饰语
        //     「我们需要跟进一下」    → 「我们需要」   ← 压根不是人名
        // 用户真跑时那一栏就是脏的，agent 还专门在周报里替我们道歉
        // （"原始记录里的负责人字段是脏的，建议核对一遍"）——
        // 一个字段需要模型替我们解释，那就该修，而且该有回归。
        {
            bool o_ok = true;
            std::string o_why;
            auto ofail = [&](const std::string& m) {
                o_ok = false;
                if (o_why.empty()) o_why = m;
            };

            // 谓词层（和抽取器 R6 共用同一套姓氏表）
            struct PCase { const char* s; bool want; const char* why; };
            const PCase pcs[] = {
                {u8"张伟",   true,  "张伟 应该被认成人名"},
                {u8"李经理", true,  "李经理（姓氏+称谓）应该被认成人名"},
                {u8"王工",   true,  "王工（姓氏+单字称谓）应该被认成人名"},
                {u8"了，张伟", false, "带标点的片段不是人名"},
                {u8"这块张伟", false, "带修饰语的片段本身不是人名（应取其中「张伟」）"},
                {u8"我们需要", false, "我们需要 不是人名"},
                {u8"测报告，", false, "测报告， 不是人名"},
                {u8"报告",    false, "报告 不是人名"},
                {u8"",        false, "空串不是人名"},
            };
            for (const auto& c : pcs) {
                const bool got = knowledge::looks_like_person_name(c.s);
                if (got != c.want) ofail(c.why);
            }

            // 端到端：走**真实**的规则抽取路径，用出问题的那几句话
            {
                const std::pair<const char*, const char*> cases[] = {
                    // 句子, 期望的负责人（空 = 必须留空）
                    {u8"凤凰项目的接口文档需要重写，这个上周就说了，张伟负责跟进，这周五之前完成。",
                     u8"张伟"},
                    {u8"凤凰项目的接口文档需要重写，这块张伟负责跟进。", u8"张伟"},
                    {u8"另外李经理需要提交一份极光平台的压测报告。", u8""},
                    {u8"我们需要跟进一下 Gecko 模块的遗留问题。", u8""},
                };
                for (const auto& cs : cases) {
                    std::vector<Segment> segs;
                    Segment s;
                    s.id = 1; s.session_id = 1; s.seq = 1;
                    s.ts = "2026-09-19 10:00:00.000";
                    s.src_text = cs.first; s.tgt_text = cs.first;
                    s.engine = "SelfTest"; s.ms = 0; s.confidence = -1.0;
                    segs.push_back(s);

                    const auto sum = DeliverableWriter::extract_by_rules(segs);
                    std::string got;
                    for (const auto& a : sum.actions) {
                        if (!a.owner.empty()) { got = a.owner; break; }
                    }
                    if (got != cs.second) {
                        ofail(std::string(u8"「") + cs.first + u8"」的负责人应为「"
                              + cs.second + u8"」，实际「" + got + u8"」");
                    }
                }
            }

            std::cout << "[SelfTest] 负责人抽取（留空好过写错人 / 标点与修饰语不误入）: "
                      << (o_ok ? "✅ 通过" : "❌ 失败") << std::endl;
            if (!o_ok) { std::cerr << "    " << o_why << std::endl; return 1; }
        }

        // 【这里刻意不写"共 N 例"】原来写死了 `"✅ 11 例通过"`，
        // 而加用例的人（我）不会记得回来改数字 —— 本轮加了 ⑫⑬⑭ 三条之后，
        // 它照样打"11 例通过"，**在骗人**。手写计数就是这个下场。
        // 想报真实数字就得让计数跟着用例走（像上面输入解释那组用数组长度），
        // 而这组的用例是十四个代码块、不是一张表，所以宁可不报数字：
        // 失败时 `why` 会指名道姓说是哪一条，那才是真正有用的信息。
        std::cout << "[SelfTest] 知识缺口检测（六规则/去重/优先级/上限/问过不再问）: "
                  << (gap_ok ? "✅ 通过" : "❌ 失败") << std::endl;
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
            // 【真实数据事故】用户答了「是的」，而表里只有「是」「对的」「好的」——
            // 于是它掉进"其余内容一律当新值"分支，把两个人名（Marco/Erica）
            // 覆盖成了「是的」，而且是 confirmed：`initial_prompt` 变成
            // "EnglishPod, CarsPacked, 是的, TV"，正确的人名整个消失。
            // 「核心词 + 句尾语气词」这一整类一起解决（见 strip_trailing_particles）。
            {u8"是的",      AnswerKind::Affirm,    ""},
            {u8"是啊",      AnswerKind::Affirm,    ""},
            {u8"是呀",      AnswerKind::Affirm,    ""},
            {u8"对的",      AnswerKind::Affirm,    ""},
            {u8"对啊",      AnswerKind::Affirm,    ""},
            {u8"好的",      AnswerKind::Affirm,    ""},
            {u8"好的呀",    AnswerKind::Affirm,    ""},
            {u8"嗯嗯",      AnswerKind::Affirm,    ""},
            {u8"没错的",    AnswerKind::Affirm,    ""},
            {u8"可以的",    AnswerKind::Affirm,    ""},
            {u8"行",        AnswerKind::Affirm,    ""},
            {u8"行吧",      AnswerKind::Affirm,    ""},
            {u8"没问题",    AnswerKind::Affirm,    ""},
            {u8"是这样的",  AnswerKind::Affirm,    ""},
            {u8"是的。",    AnswerKind::Affirm,    ""},
            {"YES",         AnswerKind::Affirm,    ""},
            // 【最关键的两条】"n"/"no" 必须被认成"否"，否则字面量 "no"
            // 会被当成用户给的新写法写进知识库，而且是 confirmed —— 它会进翻译约束。
            {"n",           AnswerKind::Reject,    ""},
            {"no",          AnswerKind::Reject,    ""},
            {u8"不是",      AnswerKind::Reject,    ""},
            {u8"没有",      AnswerKind::Reject,    ""},
            {u8"不用了",    AnswerKind::Reject,    ""},
            {u8"算了",      AnswerKind::Reject,    ""},
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

        // ---- 13a2) 选择题解析（仅写法冲突用）----
        //
        // 【为什么必须和 interpret_answer 分开】后者**刻意**把纯数字判成 Skip
        // （真实事故：用户答 `1`，库里多出一条 `term preview = 1 status=confirmed`）。
        // 那个判定对"是/否 + 请给正确写法"仍然正确，但"这儿有两种写法选哪个"这类问题里，
        // **数字就是最自然的回答**。两边共用一条解析必然牺牲一边：
        // 要么允许数字当值（重新引入事故），要么用户没法选。
        {
            struct ChCase { const char* in; int choice; bool not_same; AnswerKind kind; };
            const ChCase cc[] = {
                {"1",     1, false, AnswerKind::Skip},
                {"2",     2, false, AnswerKind::Skip},
                {"3",    -1, false, AnswerKind::Skip},   // 越界 → 不当选择，退化成常规解释
                {"12",   -1, false, AnswerKind::Skip},   // 多位数不认（选项最多 5 个）
                {"n",    -1, true,  AnswerKind::Skip},
                {u8"不是", -1, true, AnswerKind::Skip},
                {"",     -1, false, AnswerKind::Skip},
                // 「y」在选择题里没有确定含义（选哪个？）—— 不替用户猜，退化成 Skip
                {"y",    -1, false, AnswerKind::Skip},
                // 直接打出正确写法也认
                {"Erica", -1, false, AnswerKind::NewValue},
            };
            int cc_pass = 0;
            for (const auto& c : cc) {
                const ChoiceAnswer ca = interpret_choice(c.in, 2);
                if (ca.choice == c.choice && ca.not_same == c.not_same &&
                    ca.answer.kind == c.kind) { ++cc_pass; continue; }
                cf_ok = false;
                why = std::string("选择题解析不对: ") + c.in +
                      "（choice=" + std::to_string(ca.choice) +
                      " not_same=" + (ca.not_same ? "1" : "0") + "）";
                break;
            }
            std::cout << "[SelfTest] 选择题解析（编号/不是同一个/越界/y 不猜）: "
                      << (cf_ok ? "✅ " + std::to_string(cc_pass) + "/" +
                                  std::to_string(sizeof(cc) / sizeof(cc[0])) + " 通过"
                                : "❌ 失败")
                      << std::endl;
            if (!cf_ok) { std::cerr << "    " << why << std::endl; return 1; }
        }

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
                // 【占位说明，和 mk() 同一招、同一个理由】2026-09-19 起
                // "这条名字还没有说明"对**四种名字类**都会触发（原来只对 term），
                // 而本组的三条种子全是 `kind=person` —— 不给说明的话它们
                // 会各自多出一问，本组的断言就从"2 个问题"变成别的数字，
                // 于是**测的就不是它原本要测的东西了**。
                it.definition  = u8"（用例占位说明）";
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


// 真正的程序主体。参数已经是**合法 UTF-8**（由下面的 wmain/main 保证）。
static int run_app(int argc, char** argv) {
    //  设置控制台为 UTF-8 编码，防止中文乱码
    system("chcp 65001");

    AppConfig cfg = AppConfig::from(argc, argv);

    // **必须在任何模型加载之前**装好日志过滤。
    //
    // 实测一次真实启动，模型加载阶段会打几百行 control token / tensor / KV cache，
    // 把 `>>> 已开始记录 <<<` 那两行埋在里面 —— 用户不知道自己该什么时候开始放视频。
    // 放在 cfg.dump() 之前，是为了让 `--list` 这类不加载模型的路径也保持一致行为。
    modellog::install_silencer(cfg.verbose);

    cfg.dump();
    if (cfg.list_only) return 0;
    if (cfg.selftest)  return run_selftest(cfg);
    if (cfg.demo_session) return run_demo_session(cfg);
    if (cfg.test_window)  return run_test_window(cfg);
    if (cfg.show_gaps)    return run_show_gaps(cfg);
    if (cfg.extract_session >= 0) return run_extract(cfg);
    if (cfg.tools_list_only || !cfg.tools_name.empty()) return run_tools(cfg);
    if (!cfg.report_goal.empty()) return run_report(cfg);
    if (!cfg.search_query.empty()) return run_search(cfg);
    if (cfg.dump_terms) return run_dump_terms(cfg);
    if (cfg.triage_cache) return run_triage_cache(cfg);
    if (cfg.show_actions || cfg.action_set_id >= 0) return run_actions(cfg);
    if (!cfg.verify_report.empty()) return run_verify_report(cfg);
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
    ConstraintInputs kb;
    const std::vector<std::string> glossary = build_glossary(cfg, &kb);
    // 把**给用户看的那句话**放在最前面，然后才是内部计数、最后是实际送进引擎的字符串。
    //
    // 【顺序是刻意的，从"人"到"机器"】用户先要知道"它记不记得我"，
    // 再（如果想深究）看条数，最后才是那串 initial_prompt。
    //
    // 【为什么必须有第一句 —— 真实数据暴露的】用户跑完三场演示后，
    // "第二次知道"这一拍在屏幕上**完全看不见**：系统确实把上一场的词带进了
    // 本场的识别提示，但没有任何一句话说出来。而"越用越懂你"的全部说服力
    // 就在这一句上 —— 一个不说的机制，对使用者来说等于不存在。
    if (kb.store_ready) {
        interaction::ReuseBrief brief;
        // 词条来自 kb.terms（能进识别/翻译的），含义从库里按展示形取。
        // 这里只做**组装**，措辞在 Interaction 层（它不认识 KnowledgeItem）。
        for (const auto& t : kb.terms) {
            interaction::KnownItem it;
            it.name = t;
            KnowledgeItem row;
            // 用展示形当键去查（normalize 后匹配）：kb.terms 里存的正是展示形
            if (SessionStore::instance().long_term_memory_ready() &&
                KnowledgeStore::instance().get("term", knowledge::normalize_key(t), &row)) {
                it.meaning = row.definition;
            } else if (KnowledgeStore::instance().get("person", knowledge::normalize_key(t), &row)) {
                it.meaning = row.definition;
            }
            brief.items.push_back(std::move(it));
        }
        brief.background_only = kb.confirmed_total > kb.terms.size()
                                    ? kb.confirmed_total - kb.terms.size() : 0;
        std::cout << "\n[复用] " << interaction::reuse_intro(brief) << std::endl;
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

// ---------------------------------------------------------------------------
// 入口：Windows 走 wmain，其它平台走 main
// ---------------------------------------------------------------------------
//
// 【为什么必须用 wmain —— 中文参数会把程序直接搞崩】
// Windows 上 `main(int, char** argv)` 拿到的 argv 是按**当前控制台代码页**
// 编码的。中文 Windows 默认 936（GBK），于是：
//
//     Translator.exe --report "整理过去一周的工作"
//       → argv[2] 是 GBK 字节（D5 FB C0 ED …），不是 UTF-8
//       → 程序当 UTF-8 用 → 出现非法字节
//       → nlohmann::json::dump() 抛 type_error.316 (invalid UTF-8 byte)
//       → 未捕获 → std::terminate → **进程直接崩，exit code 0xC0000409**
//
// 实测：`--search EnglishPod` 一切正常，`--search 凤凰项目` 崩掉，
// 而且**什么错误都不打** —— 看起来像"命令没生效"，最难查的那种。
//
// `wmain` 拿到的宽字符参数是 Windows 原生的 UTF-16，**与控制台代码页无关**，
// 转成 UTF-8 之后中文参数在任何代码页下都对。
//
// ⚠️ `chcp 65001` 解决不了这个问题：程序内部那行 `system("chcp 65001")`
// 是在**参数已经被编码之后**才执行的；实测先在 PowerShell 里 chcp 再跑也无效。
#if defined(_WIN32)
int wmain(int argc, wchar_t** wargv) {
    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) args.push_back(utf8::from_wide(wargv[i]));
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& s : args) argv.push_back(s.data());
    return run_app(argc, argv.data());
}
#else
int main(int argc, char** argv) { return run_app(argc, argv); }
#endif