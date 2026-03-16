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

	//禁止拷贝构造与拷贝赋值，因为要用到的模型句柄whisper_context* whisper_ctx是唯一的,所以要禁止这两个操作
	SpeechEngine(const SpeechEngine&) = delete;
	SpeechEngine& operator = (const SpeechEngine&) = delete;

	//要暴露给外部的接口
	bool init(const std::string& model_path);				//传入模型的路径初始化模型
	void start();																//启动推理线程
	void stop();																//停止所有工作
	void push_audio(const std::vector<float>& data);//塞入音频数据的生产者
	std::string get_last_text();										//获取最新文字的消费者
	int get_inference_count();

private:
	void run_inference_loop();

	//whisper模型句柄
	whisper_context* whisper_ctx = nullptr;
	
	std::string last_text_;//将推理出的文本暂存，由于ui线程和推理线程都会访问它所以给他加个锁
	std::atomic<int> inference_count_{ 0 };
	std::mutex text_mutex_;

	std::thread worker_thread_;
	std::atomic<bool> is_running_{ false };//设为原子变量。这样可以保证状态改变后一定会被线程看到，从而安全退出

	std::queue<std::vector<float>> audio_queue_;//音频队列
	std::mutex  queue_mutex_;//保护队列的锁
	std::condition_variable cv_;//信号量： 有新音频时唤醒线程,,当队列为空时就让消费者线程阻塞等待，新数据被放入后再唤醒消费者

	const size_t MAX_QUEUE_SIZE = 10;

};