#pragma once
#include <string>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

#include "llama.h"
#include "ITranslator.h"

class HunyuanTranslator : public ITranslator {
public:
    HunyuanTranslator(const std::string& model_path);
    ~HunyuanTranslator();

    void start() override;                          // 启动推理线程（幂等）
    void push_text(const std::string& text, double confidence) override;
    void stop() override;
    long long get_last_api_ms() override;
    const char* name() const override { return "Hunyuan"; }
    bool init();

    // 目标语言（译文语言），"zh" / "en" / "ja" ... 默认中文
    void set_target_language(const std::string& lang) override;
    std::string get_last_translation() const override;
    int get_translation_count() const override;
    std::string get_last_source() const override;

    // 诊断用：打印实际送给模型的 prompt，以及它的 tokenize 结果。
    // 用来验证特殊 token（<｜hy_User｜> 等）是否被当成单个 token 识别——
    // 如果被切成碎片，说明 parse_special 没生效，模型看不到角色边界。
    void debug_dump_prompt(const std::string& text) const;

    // 同步翻译一条文本（阻塞直到出结果）。
    // 抽出来有两个用处：① 不接音频也能端到端验证翻译链路；
    // ② 后续的大模型摘要器可以复用同一条推理路径。
    bool translate_once(const std::string& text, std::string& out);

    // 通用单轮生成：给定 system 与 user 内容，返回模型输出。
    // 摘要、行动项抽取这类"让模型做理解"的任务都走这里。
    // max_new 是生成长度上限——摘要需要比翻译长得多。
    bool generate_once(const std::string& system, const std::string& user,
                       std::string& out, int max_new = 128);

private:
    void translation_worker();// 推理线程函数

    // 用模型自带的 chat template 组装 prompt。
    // 之前手拼 ChatML 会导致模型偶尔不切到 assistant 角色，
    // 而是把 user 那句话继续补完，于是 prompt 文本被当成译文输出。
    std::string build_prompt(const std::string& system, const std::string& user) const;
    // 目标语言的人类可读名（用于写进 prompt）
    std::string target_name() const;
    // 翻译任务的 system prompt。
    // **必须只有一处**：它原来在 translate_once 和 debug_dump_prompt 里各写了一份，
    // 于是加"术语约束"时两处就得同步——而 debug_dump_prompt 是给人看 prompt 用的，
    // 不同步就会看到与实际运行不一致的 prompt，把排查带偏。
    std::string translate_system_prompt() const;
    // 兜底清洗：剥掉可能被回显的 prompt 片段与首尾引号
    static std::string sanitize_output(std::string s);


    std::string model_path_;
    std::string target_lang_ = "zh";                // 译文语言
    static const size_t MAX_QUEUE_SIZE = 100;
    std::queue<TranslationRequest> text_queue_;

    std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{ false };
    std::thread worker_thread_;

    std::atomic<long long> last_api_ms_{0};

    // 最新译文及它对应的原文，供悬浮窗做同步显示
    mutable std::mutex  result_mutex_;
    std::string         last_translation_;
    std::string         last_source_;
    std::atomic<int>    translation_count_{0};

    llama_model* model  = nullptr; // LLaMA 模型对象指针
    llama_context* ctx = nullptr; // LLaMA 上下文对象指针
};



