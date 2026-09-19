#include "AppConfig.h"
#include "Assistant.h"   // 助手：库路径/输出目录由它推出来（§7 第 4 阶段 4.1）

#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

namespace {

// 取可执行文件所在目录。argv[0] 可能是相对路径，一律先转绝对路径。
fs::path executable_dir(const char* argv0) {
    std::error_code ec;
    fs::path p = fs::absolute(argv0 ? argv0 : ".", ec);
    if (ec || p.empty()) {
        fs::path cwd = fs::current_path(ec);
        return ec ? fs::path(".") : cwd;
    }
    return p.parent_path();
}

// 目录里是否真的存在模型文件（.bin / .gguf）
bool has_model_files(const fs::path& dir) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) return false;
        if (!entry.is_regular_file(ec)) continue;
        const std::string ext = entry.path().extension().string();
        if (ext == ".bin" || ext == ".gguf") return true;
    }
    return false;
}

// 从起始目录逐级向上找 models/ 目录，最多向上 6 层。
// 覆盖 build/Release、out/build/x64-Debug 这类多级输出目录。
//
// 注意：不能只判断"目录是否存在"。本项目根目录下有一个空的 models/ 占位目录，
// 而真实模型放在上一级的 projects/models/。若只判断存在性，会先命中空目录，
// 随后误报"模型不存在"。因此优先返回确实含有模型文件的目录。
fs::path locate_models_dir(const fs::path& start) {
    std::error_code ec;
    fs::path fallback;                       // 第一个存在的 models/，仅作兜底
    fs::path cur = start;
    for (int depth = 0; depth < 6; ++depth) {
        fs::path candidate = cur / "models";
        if (fs::is_directory(candidate, ec)) {
            if (has_model_files(candidate)) return candidate;
            if (fallback.empty()) fallback = candidate;
        }
        if (!cur.has_parent_path()) break;
        fs::path parent = cur.parent_path();
        if (parent == cur) break;
        cur = parent;
    }
    return fallback;
}

// 读取环境变量，未设置时返回 fallback
std::string env_or(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}

}  // namespace

AppConfig AppConfig::from(int argc, char** argv) {
    AppConfig cfg;

    // ---- 1. 自动探测 models 目录 ----
    fs::path models = locate_models_dir(executable_dir(argc > 0 ? argv[0] : nullptr));
    std::string whisper_default = "ggml-large-v3.bin";
    std::string hunyuan_default = "HY-MT1.5-1.8B-Q4_K_M.gguf";
    if (!models.empty()) {
        whisper_default = (models / "ggml-large-v3.bin").string();
        hunyuan_default = (models / "HY-MT1.5-1.8B-Q4_K_M.gguf").string();
    }

    // ---- 2. 环境变量 ----
    cfg.whisper_model  = env_or("AT_WHISPER_MODEL", whisper_default);
    cfg.hunyuan_model  = env_or("AT_HUNYUAN_MODEL", hunyuan_default);
    cfg.db_path        = env_or("AT_DB_PATH", "translations.db");
    cfg.deepseek_api_key = env_or("DEEPSEEK_API_KEY", "");

    // ---- 3. 命令行参数（最高优先级）----
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&](std::string& out) {
            if (i + 1 < argc) out = argv[++i];
        };
        if (arg == "--whisper")            next(cfg.whisper_model);
        else if (arg == "--hunyuan")       next(cfg.hunyuan_model);
        else if (arg == "--db")          { next(cfg.db_path); cfg.db_path_explicit = true; }
        else if (arg == "--out")         { next(cfg.deliverable_dir); cfg.out_explicit = true; }
        else if (arg == "--api-key")       next(cfg.deepseek_api_key);
        else if (arg == "--help" || arg == "-h" || arg == "--list") cfg.list_only = true;
        else if (arg == "--selftest")      cfg.selftest = true;
        else if (arg == "--demo-session")  cfg.demo_session = true;
        else if (arg == "--test-window")   cfg.test_window = true;
        else if (arg == "--verbose")       cfg.verbose = true;
        else if (arg == "--report")        next(cfg.report_goal);
        // ⚠️ 刻意不叫 --ask —— 那个已被确认交互开关占用（见 AppConfig.h 的说明）
        else if (arg == "--search")        next(cfg.search_query);
        else if (arg == "--wav")           next(cfg.wav_path);
        else if (arg == "--gaps")          cfg.show_gaps = true;
        else if (arg == "--extract") {
            // 数值参数：不合法就保持 -1（不静默当成 0 —— 0 号会话不存在，
            // 会变成"跑完了什么都没抽到"，看起来像抽取器坏了）
            std::string num_buf;
            next(num_buf);
            try { cfg.extract_session = std::stoll(num_buf); }
            catch (...) { cfg.extract_session = -1; }
        }
        else if (arg == "--apply")         cfg.extract_apply = true;
        else if (arg == "--tools") {
            // 后面跟不跟名字都合法：单独出现 = 只打印
            cfg.tools_list_only = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                cfg.tools_name      = argv[++i];
                cfg.tools_list_only = false;
            }
        }
        else if (arg == "--context-lines") next(cfg.context_lines_text);
        else if (arg == "--context") {
            std::string v; next(v);
            try { if (!v.empty()) cfg.context_lines = std::stoi(v); }
            catch (const std::exception&) {
                std::cerr << "[Config] --context 需要数字（段数），收到: " << v << std::endl;
            }
        }
        else if (arg == "--endpoint-ms") {
            std::string v; next(v);
            try { if (!v.empty()) cfg.endpoint_ms = std::stoi(v); }
            catch (const std::exception&) {
                std::cerr << "[Config] --endpoint-ms 需要数字（毫秒），收到: " << v << std::endl;
            }
        }
        else if (arg == "--max-utter-sec") {
            std::string v; next(v);
            try { if (!v.empty()) cfg.max_utter_sec = std::stod(v); }
            catch (const std::exception&) {
                std::cerr << "[Config] --max-utter-sec 需要数字（秒），收到: " << v << std::endl;
            }
        }
        else if (arg == "--actions")       cfg.show_actions = true;
        else if (arg == "--fix") {
            // 后面跟不跟词都合法：单独出现 = 列出全部
            cfg.show_fix = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') cfg.fix_word = argv[++i];
        }
        else if (arg == "--assistants")        cfg.list_assistants = true;
        else if (arg == "--new-assistant")     next(cfg.new_assistant);
        else if (arg == "--assistant")         next(cfg.use_assistant);
        else if (arg == "--remove-assistant")  next(cfg.remove_assistant);
        else if (arg == "--set-key")           next(cfg.set_key);
        else if (arg == "--ui-data")           next(cfg.ui_data);
        else if (arg == "--session") {
            std::string v; next(v);
            try { if (!v.empty()) cfg.ui_session = std::stoll(v); }
            catch (const std::exception&) {
                std::cerr << "[Config] --session 需要数字 id，收到: " << v << std::endl;
            }
        }
        else if (arg == "--value")        next(cfg.fix_value);
        else if (arg == "--as-kind")      next(cfg.fix_kind);
        else if (arg == "--define")       next(cfg.fix_define);
        else if (arg == "--archive")      cfg.fix_archive = true;
        else if (arg == "--verify-report") next(cfg.verify_report);
        else if (arg == "--status")        next(cfg.actions_status);
        // --done/--doing/--todo 三个都是"把某条 action 改成这个状态"。
        // 合成一个分支是因为它们除了目标值之外完全一样 —— 写三遍就是三处要走散。
        else if (arg == "--done" || arg == "--doing" || arg == "--todo") {
            cfg.action_set_to = arg.substr(2);   // 去掉开头的 "--"
            try {
                if (i + 1 < argc) cfg.action_set_id = std::stoll(argv[++i]);
            } catch (const std::exception&) {
                std::cerr << "[Config] " << arg << " 需要一个数字 id" << std::endl;
                cfg.action_set_id = -1;
            }
        }
        else if (arg == "--args")          next(cfg.tools_args);
        else if (arg == "--ask")           cfg.ask_mode = AppConfig::AskMode::Always;
        else if (arg == "--no-ask")        cfg.ask_mode = AppConfig::AskMode::Never;
        else if (arg == "--translator")    next(cfg.translator);
        else if (arg == "--mic")           cfg.enable_mic = true;
        else if (arg == "--lang")          next(cfg.source_lang);
        else if (arg == "--target")        next(cfg.target_lang);
        else if (arg == "--dump-prompt")   next(cfg.dump_prompt);
        else if (arg == "--terms")         cfg.dump_terms = true;
        else if (arg == "--triage-cache")  cfg.triage_cache = true;
        else if (arg == "--forget")        next(cfg.triage_forget);
        else if (arg == "--clear")         cfg.triage_clear = true;
        else if (arg == "--glossary")      next(cfg.glossary_path);
        else if (arg == "--asr-prompt-kb") {
            std::string v; next(v);
            try { if (!v.empty()) cfg.asr_prompt_kb = std::stoi(v); }
            catch (const std::exception&) {
                std::cerr << "[Config] --asr-prompt-kb 需要数字，收到: " << v << std::endl;
            }
        }
        else if (arg == "--summarizer")    next(cfg.summarizer);
        else if (arg == "--llm-model")     next(cfg.llm_model);
        else if (arg == "--lang-switch-confirm") {
            std::string v; next(v);
            try { if (!v.empty()) cfg.lang_switch_confirm = std::stoi(v); }
            catch (const std::exception&) {
                std::cerr << "[Config] --lang-switch-confirm 需要数字（段数），收到: " << v << std::endl;
            }
        }
        else if (arg == "--lang-recheck") {            std::string v; next(v);
            try { if (!v.empty()) cfg.lang_recheck_sec = std::stoi(v); }
            catch (const std::exception&) {
                std::cerr << "[Config] --lang-recheck 需要数字参数，收到: " << v << std::endl;
            }
        }
        else if (arg == "--export") {
            std::string v;
            next(v);
            try {
                if (!v.empty()) cfg.export_session = std::stoll(v);
            } catch (const std::exception&) {
                std::cerr << "[Config] --export 需要数字参数，收到: " << v << std::endl;
            }
        }
        else std::cerr << "[Config] 忽略未知参数: " << arg << std::endl;
    }

    // ---- 助手：库路径与输出目录（§7 第 4 阶段 4.1）----
    //
    // ⚠️ **显式给的 --db / --out 永远优先。**
    //    整个验证阶梯（`--selftest` / `--wav` / `--gaps` / 那些 python harness）
    //    全靠显式路径指向**临时库**。助手如果把它们覆盖掉，那套保障会
    //    **静默失效** —— 命令照样跑、结果看着正常，但动的是用户的真实数据。
    //    这是本项目踩过最疼的一类坑（"少一个参数就往用户库里写假会话"）。
    //
    // 顺序也重要：助手解析在**参数解析之后**，所以它能看见 explicit 标志。
    if (!cfg.use_assistant.empty() || !cfg.list_assistants) {
        // 用哪个助手：显式指定的 > 上次用过的。都没有就不动路径（保持旧行为）。
        std::string want = cfg.use_assistant;
        assistant::Settings s;
        if (want.empty()) assistant::load_settings(&s);
        if (want.empty()) want = s.last_assistant;

        if (!want.empty()) {
            assistant::Info info;
            if (assistant::find(want, &info)) {
                if (!cfg.db_path_explicit) cfg.db_path = info.db_path;
                if (!cfg.out_explicit)     cfg.deliverable_dir = info.out_dir;
                cfg.assistant_slug = info.slug;
                // 记住"上次用的"（只显式指定时才写盘，避免每次 --selftest 都改配置）
                if (!cfg.use_assistant.empty() && s.last_assistant != info.slug) {
                    s.last_assistant = info.slug;
                    std::string e;
                    if (!assistant::save_settings(s, &e)) {
                        std::cerr << "[Config] 记不住'上次用的助手'：" << e << std::endl;
                    }
                }
            } else if (!cfg.use_assistant.empty()) {
                std::cerr << "[Config] 没有叫「" << cfg.use_assistant
                          << "」的助手（用 --assistants 看有哪些，--new-assistant 新建）"
                          << std::endl;
            }
        }
    }

    return cfg;
}

bool AppConfig::whisper_ready() const {
    std::error_code ec;
    return fs::exists(whisper_model, ec) && !ec;
}

void AppConfig::dump() const {
    // 数据库和交付物目录都打**绝对路径**，因为它们是相对当前目录解析的。
    //
    // 【为什么这一行值得改】`--db` 是相对 cwd 的，而同一个 `translations.db`
    // 这个名字在仓库根和 `build\RelWithDebInfo\` 下都存在（是**两个不同的文件**）。
    // 只打相对名的话，"我现在到底写进哪个库"要靠用户自己 `pwd` 才能判断 ——
    // 而这个项目已经在这上面栽过（`verify_memory.py` 报"缺少记忆表"，
    // 其实只是验错了文件；那次之后它改成打绝对路径）。
    //
    // 还要标出"这个库是不是新建的"：从仓库根不带 --db 跑会**新建一个空库**，
    // 而用户看到空的会话列表时，第一反应是"我的记录丢了"——
    // 其实只是写进了另一个文件。
    auto abs_of = [](const std::string& p) {
        std::error_code ec;
        const auto a = fs::absolute(p, ec);
        return ec ? p : a.lexically_normal().string();
    };
    const std::string db_abs = abs_of(db_path);
    std::error_code ec_db;
    const bool db_exists = fs::exists(db_abs, ec_db);

    std::cout << "[Config] Whisper 模型 : " << whisper_model
              << (whisper_ready() ? "  (已找到)" : "  (!! 不存在)") << "\n"
              << "[Config] 混元模型     : " << hunyuan_model << "\n"
              << "[Config] 数据库       : " << db_abs
              << (db_exists ? "" : "  (不存在，本次会新建一个空库)")
              << "\n"
              << "[Config] 交付物目录   : " << abs_of(deliverable_dir) << "\n"
              << "[Config] 语言策略     : 源=" << source_lang
              << "  目标=" << target_lang
              << (source_lang == "auto"
                    ? ("  (自动检测后锁定，每 " + std::to_string(lang_recheck_sec) + "s 重检)")
                    : "  (固定，不检测)")
              << "\n";
    if (!glossary_path.empty()) {
        std::error_code ec;
        std::cout << "[Config] 术语表       : " << glossary_path
                  << (fs::exists(glossary_path, ec) ? "  (已找到)" : "  (!! 不存在)")
                  << "\n";
    }
    std::cout << "[Config] DeepSeek Key : "
              << (deepseek_api_key.empty() ? "(未设置，云端后端不可用)" : "(已设置)")
              << std::endl;
}
