#pragma once
#include "whisper.h"
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>

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
	long long get_last_inference_ms();//测试用，获取上次推理耗时

private:
	void run_inference_loop();

	whisper_context* whisper_ctx = nullptr;         // whisper 模型句柄

	std::string last_text_;                         // 最新识别文本缓存，UI线程和推理线程共享，需加锁
	std::atomic<int> inference_count_{ 0 };
	std::mutex text_mutex_;

	std::thread worker_thread_;
	std::atomic<bool> is_running_{ false };         // 原子变量，保证状态变更对所有线程可见

	std::queue<std::vector<float>> audio_queue_;    // 音频数据队列
	std::mutex  queue_mutex_;                       // 队列锁
	std::condition_variable cv_;                    // 条件变量：有数据时通知消费者线程

	const size_t MAX_QUEUE_SIZE = 10;

	std::atomic<long long> last_inference_ms_{0};//测试用

};
