#pragma once
#include <string>

// 一条待翻译的请求。
// confidence 是上游语音识别的置信度，会一路带到数据库，
// 供后续"难度自适应路由"和交付物质量评估使用；
// <0 表示上游没有提供这个值。
struct TranslationRequest {
    std::string text;
    double      confidence = -1.0;
};

// 翻译后端统一接口。
// 生命周期的约定：构造 -> init 阶段由派生类自己的工厂/构造完成 ->
// start() 启动工作线程 -> push_text() 投递 -> stop() 收尾。
// start() 必须是幂等的：重复调用不应启动第二个线程。
class ITranslator{
public:
    virtual ~ITranslator() = default;

    virtual void start() = 0;                              // 启动后台翻译线程
    virtual void push_text(const std::string& text, double confidence) = 0;   // 投递待翻译文本
    virtual void stop() = 0;                               // 停止并回收线程
    virtual long long get_last_api_ms() = 0;               // 上次翻译耗时(ms)
    virtual const char* name() const = 0;                  // 后端名，用于日志与界面展示

    // 目标语言（译文语言），如 "zh" / "en" / "ja"。
    // 放在接口上是因为本地与云端两条后端都要用它来构造 prompt。
    virtual void set_target_language(const std::string& lang) = 0;

    // 最新一条完成的译文，供悬浮字幕窗显示。
    // 用计数器判断"是否有新译文"，避免比较文本（两段译文相同时会漏判）。
    virtual std::string get_last_translation() const = 0;
    virtual int get_translation_count() const = 0;

    // 上一条译文**对应的原文**。
    //
    // 为什么必须要有它：识别和翻译是两条独立异步流，翻译总滞后一句。
    // 如果窗口直接把「最新原文」和「最新译文」拼在一起，就会出现
    // 「英文第 N 句 + 中文第 N-1 句」的错配。
    // 调用方拿到译文后要先比对来源，一致才更新，否则显示"翻译中…"。
    virtual std::string get_last_source() const = 0;
};
