#include "ModelLog.h"

#include <cstdio>

// ggml_log_level 与 ggml_log_callback 定义在这里
#include "ggml.h"
#include "llama.h"
#include "whisper.h"

namespace {

bool g_verbose = true;

void filtered_log(enum ggml_log_level level, const char* text, void* /*user_data*/) {
    if (g_verbose) {
        if (text) std::fputs(text, stderr);
        return;
    }
    // 只放行"可能意味着出事了"的级别。
    //
    // 【为什么保留 WARN 而不是只留 ERROR】实测踩过：显存不足时
    // llama.cpp 先打一串 WARN 再失败；只留 ERROR 的话，用户看到的是
    // "加载失败"而看不到**为什么**。
    //
    // 【为什么 CONT 也放行】GGML_LOG_LEVEL_CONT 表示"接着上一行的输出"
    // （不换行地续打）。把它挡掉会让前面那条 WARN 断在半句话上 ——
    // 那比不打更糟（读起来像文件损坏）。
    switch (level) {
    case GGML_LOG_LEVEL_WARN:
    case GGML_LOG_LEVEL_ERROR:
    case GGML_LOG_LEVEL_CONT:
        if (text) std::fputs(text, stderr);
        break;
    default:
        break;   // DEBUG / INFO / NONE 丢掉
    }
}

}  // namespace

namespace modellog {

void install_silencer(bool keep_verbose) {
    g_verbose = keep_verbose;
    if (keep_verbose) return;   // 不装回调 = 保持各库默认行为

    // 三个都要装：llama / whisper 各自有一份，而真正落地的打印走 ggml 的全局回调。
    // 只装其中一个的话，另一个的输出照旧会漏出来（实测 whisper 的
    // "whisper_init_state: kv pad size = ..." 就是这么漏的）。
    ggml_log_set(filtered_log, nullptr);
    llama_log_set(filtered_log, nullptr);
    whisper_log_set(filtered_log, nullptr);
}

}  // namespace modellog
