#include <iostream>
#include <vector>
#include <string>
#include <conio.h>    // 用于 _kbhit() 和 _getch()
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
        const int n_drop = DeliverableWriter::sanitize_actions(summary.actions, &notes);
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
    // 实测踩过：加了术语约束之后 --dump-prompt 仍然不显示它，因为这条路径
    // 自己创建翻译器、从没调 set_glossary() —— 于是诊断工具开始撒谎，
    // 排查时看到的 prompt 和真实运行的不是同一个。
    // 【判断】以后凡是往翻译 prompt 里加东西，都要回来确认这里也加上了。
    const std::vector<std::string> glossary = load_glossary_terms(cfg.glossary_path);
    hy.set_glossary(glossary);
    std::cout << "[Dump] 已载入术语 " << glossary.size() << " 条作为翻译约束" << std::endl;

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
        // 三个用例全部来自真实运行，不是编的。
        {
            struct DueCase { const char* task; const char* due; const char* source; bool keep_due; };
            const DueCase due_cases[] = {
                // ① 实测**误杀**：云端大模型把 "next Friday" 翻成「下周五」，
                //    只比字面必然对不上。归一化之后必须保留。
                {u8"准备更新后的路线图", u8"下周五",
                 "Alice will prepare the updated roadmap by next Friday.", true},
                // ② 具体数字日期不判定：跨语言（10 月 vs October、31 日 vs 31st）没法比
                {u8"我们需要把交付推迟到 10 月 31 日。", u8"10 月 31 日",
                 "We need to push the delivery to October 31st.", true},
                // ③ 真的没依据：原文只说 next Friday，没提下周三
                {u8"请在下周五前提交报价。", u8"下周三",
                 "Please submit the quotation by next Friday.", false},
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
                DeliverableWriter::sanitize_actions(v);

                const bool kept_due = (v.size() == 1) && !v[0].due.empty();
                if (kept_due == c.keep_due) { ++due_pass; continue; }
                due_ok = false;
                std::cerr << "[SelfTest] 截止日期依据判定失败\n"
                          << "    任务: " << c.task << "\n"
                          << "    截止: " << c.due << "\n"
                          << "    来源: " << c.source << "\n"
                          << "    期望: " << (c.keep_due ? "保留截止" : "清空截止") << "\n"
                          << "    实际: " << (kept_due ? "保留截止" : "清空截止") << std::endl;
            }
            std::cout << "[SelfTest] 截止日期依据（含跨语言）: " << (due_ok ? "✅ " : "❌ ")
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

    // 术语：一份列表，两个用途——
    //   ① 喂给 Whisper 作为 initial_prompt（识别时偏向专名）
    //   ② 交给 TermFixer 做识别后纠错（实测提示的约束力不够，名字仍会乱跳）
    const std::vector<std::string> glossary = load_glossary_terms(cfg.glossary_path);
    engine.set_initial_prompt(terms_to_prompt(glossary));

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