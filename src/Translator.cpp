#include "Translator.h"
#define CPPHTTPLIB_OPENSSL_SUPPORT             // 启用 HTTPS 支持
#include "httplib.h"
#include "json.hpp"
#include <iostream>
#include <chrono>

using json = nlohmann::json;

Translator::Translator(const std::string& api_key) : api_key_(api_key), running_(true)
{
	worker_thread_ = std::thread(&Translator::network_worker, this);

}

Translator::~Translator() {
	stop();
}

void Translator::push_text(const std::string& text) {
	if (text.empty()) {                       // 空文本直接跳过
		return;
	}
	{
		std::lock_guard<std::mutex> lock(queue_mutex_);
		if(text_queue_.size() >= MAX_QUEUE_SIZE) {
			std::cerr << "[警告] 翻译队列已满(" << MAX_QUEUE_SIZE<< ")，丢弃最旧文本: " << text_queue_.front() << std::endl;
			text_queue_.pop();
		}
		text_queue_.push(text);
	}
	cv_.notify_one();
}

void Translator::stop() {
	running_ = false;                         // 标记为需要退出的状态
	cv_.notify_all();
	if (worker_thread_.joinable()) {
		worker_thread_.join();                // 等待子线程执行完再析构
	}
}

void Translator::network_worker() {
	httplib::Client cli("https://api.deepseek.com");
	cli.set_connection_timeout(5, 0);
	cli.set_read_timeout(10, 0);

	while (running_) {
		std::string text_to_translate;

		// 等待数据
		{
			std::unique_lock<std::mutex> lock(queue_mutex_);
			cv_.wait(lock, [this] {
				return !text_queue_.empty() || !running_;
			});

			if (!running_ && text_queue_.empty()) break;

			text_to_translate = text_queue_.front();
			text_queue_.pop();
		}

		// 构建请求体
		json payload = {
			{"model", "deepseek-chat"},
			{"messages", json::array({
				{{"role", "system"}, {"content", "你是一个翻译官，直接把文本翻译成中文，不要废话"}},
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
			// if (current_try > 0) {
			// 	std::this_thread::sleep_for(std::chrono::seconds(1));
			// 	std::cout << "重试第 " << current_try + 1 << " 次..." << std::endl;
			// }

			auto t1 = std::chrono::steady_clock::now();
			if (auto res = cli.Post("/chat/completions", headers, payload.dump(-1, ' ', false, json::error_handler_t::replace), "application/json")) {
				auto t2 = std::chrono::steady_clock::now();
				last_api_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
				if (res->status == 200) {
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
				} else if(res->status == 429 || res->status >= 500) {
					//限流或服务器错误，可以重试
					std::cerr << "请求失败，状态码: " << res->status << std::endl;
					current_try++;
					std::this_thread::sleep_for(std::chrono::seconds(1 << current_try));//指数退避等待
				} else{
					//其他客户端错误，不重试
					std::cerr << "请求失败，状态码: " << res->status << "，不重试" << std::endl;
					break;
				}
			}
			else {
				auto err = res.error();
				std::cerr << "网络请求失败，错误码: " << static_cast<int>(err) << std::endl;
				current_try++;
			}
		}
		if (!success) {
			std::cerr << "翻译失败，已达到最大重试次数" << std::endl;
		}
	}



}

long long Translator::get_last_api_ms() {
    return last_api_ms_.load();
}
