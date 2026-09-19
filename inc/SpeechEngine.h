#pragma once
#include "whisper.h"
#include <string>
#include <vector>
#include <atomic>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <queue>

// 一次推理的质量指标。用于幻觉过滤和后续的"难度自适应路由"。
struct SpeechQuality {
    double confidence   = -1.0;  // 平均 token 概率，越高越可信；<0 表示本次未计算
    double no_speech    =  0.0;  // 平均"非语音"概率，Whisper 判断这段是不是压根没人说话
    bool   rejected     = false; // 是否被判定为幻觉而丢弃
    std::string reject_reason;
};

class SpeechEngine {
public:
	SpeechEngine();
	~SpeechEngine();

	// 禁止拷贝构造和拷贝赋值，whisper_ctx 是唯一句柄，不可复制
	SpeechEngine(const SpeechEngine&) = delete;
	SpeechEngine& operator = (const SpeechEngine&) = delete;

	// 对外接口
	bool init(const std::string& model_path);       // 根据模型路径初始化模型
	void start();                                   // 启动推理线程
	void stop();                                    // 停止推理工作
	void push_audio(const std::vector<float>& data);// 将音频数据推入队列
	std::string get_last_text();                    // 获取最新的识别结果
	int get_inference_count();
	long long get_last_inference_ms();              // 获取上次推理耗时

	// 当前待推理的音频段数（背压观察用）。
	//
	// 为什么需要它：`--wav` 回放测试音频时会尽可能快地喂数据，
	// 而队列上限是 MAX_QUEUE_SIZE，满了会**丢弃最旧的音频段**——
	// 也就是测试会静默丢掉内容。喂数据的一方必须能看到队列深度并等待。
	size_t get_queue_depth() const;

	// ---- 语言策略 ----
	// source_lang = "auto" 时：首次推理自动检测并锁定，之后每 recheck_sec 秒重新检测一次
	// （应对中途换了内容语言）。指定具体语言（"en"/"zh"/"ja"...）则完全跳过检测。
	// 每段都做自动检测是识别错误的常见来源——3 秒碎片上模型经常只有 20% 把握。
	void set_language_policy(const std::string& source_lang, int recheck_sec);

	// 语言（锁定或切换）需要**连续多少段判断一致**才认（默认 3）。
	//
	// 【为什么需要它】原来**一次判断就切换**，于是音乐/转场那一段被判成别的语言时
	// 锁定语言当场就被改掉 —— 真实会话里直接导致英文课上出现中文原文
	// （机制见 inc/LangPolicy.h）。1 = 退回旧行为（一次就切）。
	void set_language_switch_confirm(int n);

	// 当前锁定的源语言（如 "en"）；尚未检测出来时为空
	std::string get_language() const;

	// ---- 术语提示 ----
	// 把已知专有名词（人名/产品名/缩写）作为 initial_prompt 喂给 Whisper，
	// 让识别结果偏向这些词。实测中同一个人名会在 Marko/Marco 之间乱跳，
	// 这类问题用术语提示能显著改善。
	// 必须在 start() 之前调用。
	void set_initial_prompt(const std::string& prompt);

	// ---- 质量指标 ----
	SpeechQuality get_last_quality() const;

	// ---- 供自检使用的纯函数（不依赖模型）----
	// 识别结果是否只是把 initial_prompt 续写了出来
	static bool looks_like_prompt_echo(const std::string& text, const std::string& prompt);
	// 在给定窗口里，这句话是否已经重复太多次
	static bool looks_repetitive(const std::vector<std::string>& window, const std::string& text);

private:
	void run_inference_loop();

	// 本次推理是否需要（重新）检测语言
	bool should_detect_language() const;
	// 推理结束后读取 whisper 检测到的语言并更新锁定状态
	void update_detected_language();

	// 识别结果是否只是把 initial_prompt 续写了出来。
	// 实测：术语表塞进 initial_prompt 后，模型在音频不清晰时会把词表原样吐出来
	// （"EnglishPod, Phoenix, SKU, Q4, roadmap"），置信度还高达 0.99。
	bool is_prompt_echo(const std::string& text) const;

	// 跨段重复抑制。
	// 实测：音乐/噪声段 Whisper 会反复吐同一句（"and more." 出现 6 次，
	// 置信度 0.99），靠置信度和 no_speech_prob 都抓不到。
	// 注意：这个函数会更新内部窗口，所以是非 const 的。
	bool is_repetitive(const std::string& text);

	whisper_context* whisper_ctx = nullptr;         // whisper 模型句柄

	std::string last_text_;                         // 最新识别文本缓存，UI线程和推理线程共享，需加锁
	std::atomic<int> inference_count_{ 0 };
	std::mutex text_mutex_;

	std::thread worker_thread_;
	std::atomic<bool> is_running_{ false };         // 原子变量，保证状态变更对所有线程可见

	std::queue<std::vector<float>> audio_queue_;    // 音频数据队列
	mutable std::mutex queue_mutex_;                // 队列锁（mutable：get_queue_depth() 是 const）
	std::condition_variable cv_;                    // 条件变量：有数据时通知消费者线程

	const size_t MAX_QUEUE_SIZE = 10;

	std::atomic<long long> last_inference_ms_{0};   // 上次推理耗时

	// ---- 语言状态 ----
	std::string requested_lang_ = "auto";           // 用户请求的语言（"auto" 或具体代码）
	std::string current_lang_;                      // 已锁定的源语言
	int         lang_recheck_sec_ = 120;            // auto 模式下的重检间隔

	// 语言状态的"候选"部分：判出别的语言时先记在这里，连续够多段才真的切换。
	// 状态机本身在 langpolicy::decide()（纯函数、有单测）。
	std::string pending_lang_;
	int         pending_count_ = 0;
	int         lang_switch_confirm_ = 3;
	mutable std::mutex lang_mutex_;
	std::chrono::steady_clock::time_point last_detect_{};

	// ---- 质量状态 ----
	mutable std::mutex quality_mutex_;
	SpeechQuality last_quality_;

	// ---- 术语提示 ----
	std::string initial_prompt_;                    // 送给 Whisper 的术语提示

	// ---- 跨段重复抑制 ----
	std::vector<std::string> recent_outputs_;       // 最近若干段的规范化文本
	mutable std::mutex       recent_mutex_;
	// 窗口要够大。实测 "and more." 这类幻觉在音乐段每隔 9~14 段就冒一次，
	// 窗口 10 会让它每次都只剩下 1 次计数、永远凑不够阈值。
	// 20 段（约 1 分钟）能覆盖这种间隔，又不至于把正常讲话算进来。
	static const size_t      RECENT_WINDOW = 20;
};
