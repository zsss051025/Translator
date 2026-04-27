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
	
	//互斥锁和标志位必须在线程对象之前声明，保证它们的生命周期足够长，避免线程访问已经销毁的成员变量导致未定义行为
	std::mutex queue_mutex_;
	std::condition_variable cv_;
	std::atomic<bool> running_{ true };
	//.h文件中永远要将线程对象放在最后面，保证其他成员变量的生命周期足够长，避免线程访问已经销毁的成员变量导致未定义行为
	std::thread worker_thread_;
};
