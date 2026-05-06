#pragma once
#include <string>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

class Translator {
public:
	Translator(const std::string& api_key);
	~Translator();

	void push_text(const std::string& text); // SpeechEngine 调用此方法传入待翻译文本

	void stop();
	long long get_last_api_ms();


private:
	void network_worker();                   // 网络请求真正执行的地方

	std::string api_key_;
	static const size_t MAX_QUEUE_SIZE = 100; // 队列最大长度，防止内存占用过大
	std::queue<std::string> text_queue_;

	// 锁和标志位必须在线程对象之前声明，保证生命周期足够长
	std::mutex queue_mutex_;
	std::condition_variable cv_;
	std::atomic<bool> running_{ true };
	// .h 文件里线程对象永远放最后，保证类成员初始化完毕后线程才能访问
	std::thread worker_thread_;

	std::atomic<long long> last_api_ms_{0};
};
