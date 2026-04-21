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

	void push_text(const std::string& text);//speechengine调用将文字塞入这个容器中

	void stop();


private:
	void network_worker();//将对应文字处理传递

	std::string api_key_;
	std::queue<std::string> text_queue_;
	
	std::thread worker_thread_;
	std::mutex queue_mutex_;
	std::condition_variable cv_;
	std::atomic<bool> running_{ true };
};
