#pragma once
#include <string>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

#include "ITranslator.h"


class DeepSeekTranslator : public ITranslator {//云端翻译的类继承自ITranslator虚类
public:
	DeepSeekTranslator(const std::string& api_key);
	~DeepSeekTranslator();																///写到这里了////

	void start()	override;                            // 启动网络工作线程
	void push_text(const std::string& text, double confidence)	override; // SpeechEngine 调用此方法传入待翻译文本

	void stop()	override;
	long long get_last_api_ms()	override;
	const char* name() const override { return "DeepSeek"; }
	void set_target_language(const std::string& lang) override;
	std::string get_last_translation() const override;
	int get_translation_count() const override;
	std::string get_last_source() const override;


private:
	void network_worker();                   // 网络请求真正执行的地方
	std::string target_name() const;         // 目标语言的人类可读名，用于 system prompt

	std::string api_key_;
	std::string target_lang_ = "zh";         // 译文语言
	static const size_t MAX_QUEUE_SIZE = 100; // 队列最大长度，防止内存占用过大
	std::queue<TranslationRequest> text_queue_;

	// 锁和标志位必须在线程对象之前声明，保证生命周期足够长
	std::mutex queue_mutex_;
	std::condition_variable cv_;
	std::atomic<bool> running_{ false };      // 由 start() 置位，构造阶段不启动线程
	// .h 文件里线程对象永远放最后，保证类成员初始化完毕后线程才能访问
	std::thread worker_thread_;

	std::atomic<long long> last_api_ms_{0};

	// 最新译文及它对应的原文，供悬浮窗做同步显示
	mutable std::mutex  result_mutex_;
	std::string         last_translation_;
	std::string         last_source_;
	std::atomic<int>    translation_count_{0};
};
