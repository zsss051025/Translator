#include "AppConfig.h"

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
        else if (arg == "--db")            next(cfg.db_path);
        else if (arg == "--out")           next(cfg.deliverable_dir);
        else if (arg == "--api-key")       next(cfg.deepseek_api_key);
        else if (arg == "--help" || arg == "-h" || arg == "--list") cfg.list_only = true;
        else if (arg == "--selftest")      cfg.selftest = true;
        else if (arg == "--demo-session")  cfg.demo_session = true;
        else if (arg == "--test-window")   cfg.test_window = true;
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
        else if (arg == "--args")          next(cfg.tools_args);
        else if (arg == "--ask")           cfg.ask_mode = AppConfig::AskMode::Always;
        else if (arg == "--no-ask")        cfg.ask_mode = AppConfig::AskMode::Never;
        else if (arg == "--translator")    next(cfg.translator);
        else if (arg == "--mic")           cfg.enable_mic = true;
        else if (arg == "--lang")          next(cfg.source_lang);
        else if (arg == "--target")        next(cfg.target_lang);
        else if (arg == "--dump-prompt")   next(cfg.dump_prompt);
        else if (arg == "--glossary")      next(cfg.glossary_path);
        else if (arg == "--summarizer")    next(cfg.summarizer);
        else if (arg == "--llm-model")     next(cfg.llm_model);
        else if (arg == "--lang-recheck") {
            std::string v; next(v);
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

    return cfg;
}

bool AppConfig::whisper_ready() const {
    std::error_code ec;
    return fs::exists(whisper_model, ec) && !ec;
}

void AppConfig::dump() const {
    std::cout << "[Config] Whisper 模型 : " << whisper_model
              << (whisper_ready() ? "  (已找到)" : "  (!! 不存在)") << "\n"
              << "[Config] 混元模型     : " << hunyuan_model << "\n"
              << "[Config] 数据库       : " << db_path << "\n"
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
