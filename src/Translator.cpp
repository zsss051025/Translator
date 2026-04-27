#include"Translator.h"
#define CPPHTTPLIB_OPENSSL_SUPPORT//启用https支持
#include"httplib.h"
#include"json.hpp"
#include<iostream>

using json = nlohmann::json;

Translator::Translator(const std::string& api_key) : api_key_(api_key),running_(true)//这里提前为类内的成员进行赋值,显示初始化
{
	worker_thread_ = std::thread(&Translator::network_worker, this);//需要传入this指针来告诉函数我传入的函数属于哪个对象

}

Translator::~Translator() {
	stop();
}

void Translator::push_text(const std::string& text) {
	if (text.empty()) {//文字是空的话就直接返回
		return;
	}
	else {//
		std::lock_guard<std::mutex> lock(queue_mutex_);//要操作这个容器了，加个锁
		text_queue_.push(text);
	}
	cv_.notify_one();
}

void Translator::stop() {
	running_ = false; //标记设为将要退出的状态
	cv_.notify_all();
	if (worker_thread_.joinable()) {//先判断是否可以join
		worker_thread_.join();//主线程等待此线程运行结束再结束
	}
}

void Translator::network_worker() {
	httplib::Client cli("https://api.deepseek.com");
	cli.set_connection_timeout(5, 0);
	cli.set_read_timeout(10, 0);

	while (running_) {
		std::string text_to_translate;//要用于传出去的容器

		//等待任务
		{
			std::unique_lock<std::mutex> lock(queue_mutex_);
			cv_.wait(lock, [this] {//cv_.wait(lock, 条件);,条件为false的话就cv_.wait(lock)
				return !text_queue_.empty() || !running_;
				});

			if (!running_ && text_queue_.empty()) break;//当已经通知线程要结束并且队列为空的时候退出循环

			text_to_translate = text_queue_.front();
			text_queue_.pop();
		}

		//发送请求
		json payload = {
					{"model", "deepseek-chat"},
					{"messages", json::array({
						{{"role", "system"}, {"content", (const char*)u8"你是一个翻译官，直接把文本翻译成中文，不要废话。"}},//强转成utf-8编码的字符串，防止中文乱码
						{{"role", "user"}, {"content", text_to_translate}}
					})}
		};

		httplib::Headers headers = {
			{"Authorization", "Bearer " + api_key_},
			{"Content-Type", "application/json"}
		};

		int max_retries = 3;
		int current_try = 0;
		bool success = false;

		while (current_try < max_retries && !success) {
			if (current_try > 0) {
				std::this_thread::sleep_for(std::chrono::seconds(1)); // 简单的重试间隔
				std::cout << "重试第 " << current_try + 1 << " 次..." << std::endl;
			}

			if (auto res = cli.Post("/chat/completions", headers, payload.dump(-1, ' ', false, json::error_handler_t::replace), "application/json")) {// 传参解释：-1(不缩进格式化), ' '(空格符), false(不强制ASCII),
				if (res->status == 200) {																																										 //json::error_handler_t::replace(遇到乱码自动替换而不是崩溃)
					try {
						auto response_json = json::parse(res->body);
						std::string translated_text = response_json["choices"][0]["message"]["content"];
						std::cout << "翻译结果: " << translated_text << std::endl;
						success = true;
					}
					catch (const std::exception& e) {
						std::cerr << "解析响应失败: " << e.what() << std::endl;
						break;
					}
				}
				else {
					std::cerr << "请求失败，状态码: " << res->status << std::endl;
					current_try++;
				}
			}
			else {
				auto err = res.error();
				std::cerr << "网络请求失败，错误码: " << static_cast<int>(err) << std::endl;
				current_try++;
			}
		}
		if (!success) {
			std::cerr << "请求失败，已达到最大重试次数。" << std::endl;
		}
	} // <-- 这里是 while (running_) 的结尾
}     // <-- 这里是 network_worker() 的结尾



