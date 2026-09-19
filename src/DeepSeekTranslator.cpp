#include "DeepSeekTranslator.h"
// HTTPS 支持由 CMake 的 target_compile_definitions 统一提供
// （CPPHTTPLIB_OPENSSL_SUPPORT）。**不要在这里写 #define** ——
// 漏一个文件就会报「set_ca_cert_path 不是成员」这种看不懂的错。
#include "httplib.h"
#include "json.hpp"
#include <iostream>
#include <chrono>
#include "SessionStore.h"
#include "CaBundle.h"

using json = nlohmann::json;

DeepSeekTranslator::DeepSeekTranslator(const std::string& api_key) : api_key_(api_key)
{
	// 构造阶段只保存配置，线程统一由 start() 启动，
	// 与 HunyuanTranslator 的生命周期保持一致。
}

DeepSeekTranslator::~DeepSeekTranslator() {
	stop();
}

void DeepSeekTranslator::start() {
	if (running_) return;                     // 幂等：避免重复启动线程
	running_ = true;
	worker_thread_ = std::thread(&DeepSeekTranslator::network_worker, this);
}

void DeepSeekTranslator::push_text(const std::string& text, double confidence) {
	if (text.empty()) {                       // 空文本直接跳过
		return;
	}
	{
		std::lock_guard<std::mutex> lock(queue_mutex_);
		if(text_queue_.size() >= MAX_QUEUE_SIZE) {
			std::cerr << "[警告] 翻译队列已满(" << MAX_QUEUE_SIZE<< ")，丢弃最旧文本: " << text_queue_.front().text << std::endl;
			text_queue_.pop();
		}
		text_queue_.push(TranslationRequest{text, confidence});
	}
	cv_.notify_one();
}

void DeepSeekTranslator::stop() {
	running_ = false;                         // 标记为需要退出的状态
	cv_.notify_all();
	if (worker_thread_.joinable()) {
		worker_thread_.join();                // 等待子线程执行完再析构
	}
}

void DeepSeekTranslator::set_target_language(const std::string& lang) {
	if (!lang.empty()) target_lang_ = lang;
}

std::string DeepSeekTranslator::get_last_translation() const {
	std::lock_guard<std::mutex> lock(result_mutex_);
	return last_translation_;
}

int DeepSeekTranslator::get_translation_count() const {
	return translation_count_.load();
}

std::string DeepSeekTranslator::get_last_source() const {
	std::lock_guard<std::mutex> lock(result_mutex_);
	return last_source_;
}

std::string DeepSeekTranslator::target_name() const {
	if (target_lang_ == "zh")  return "Simplified Chinese";
	if (target_lang_ == "en")  return "English";
	if (target_lang_ == "ja")  return "Japanese";
	if (target_lang_ == "ko")  return "Korean";
	if (target_lang_ == "fr")  return "French";
	if (target_lang_ == "de")  return "German";
	if (target_lang_ == "es")  return "Spanish";
	if (target_lang_ == "ru")  return "Russian";
	return target_lang_;
}

void DeepSeekTranslator::network_worker() {
	httplib::Client cli("https://api.deepseek.com");
	cli.set_connection_timeout(5, 0);
	cli.set_read_timeout(10, 0);

	// OpenSSL 在 Windows 上没有默认 CA 路径，不显式指定的话
	// HTTPS 校验必定失败（"SSL server verification failed"），云端后端等于不可用。
	const std::string ca = find_ca_bundle();
	if (!ca.empty()) {
		cli.set_ca_cert_path(ca);
	} else {
		std::cerr << "[警告] 未找到 CA 证书包，HTTPS 请求可能失败。"
		             "可设置环境变量 SSL_CERT_FILE 指向 ca-bundle.crt" << std::endl;
	}

	while (running_) {
		TranslationRequest req;

		// 等待数据
		{
			std::unique_lock<std::mutex> lock(queue_mutex_);
			cv_.wait(lock, [this] {
				return !text_queue_.empty() || !running_;
			});

			if (!running_ && text_queue_.empty()) break;

			req = text_queue_.front();
			text_queue_.pop();
		}
		const std::string& text_to_translate = req.text;

		// ① 清洗输入（去首尾空白）—— 和本地混元走同一个函数。
		//    理由见 ITranslator::clean_source：前导空格会让弱模型把指令翻出来。
		const std::string src = ITranslator::clean_source(text_to_translate);
		if (src.empty()) { continue; }

		// 构建请求体
		// system prompt 由目标语言决定，不再硬编码"翻译成中文"
		//
		// 术语约束按段过滤：只带这段里真出现过的术语，否则云端模型也会
		// 在没提到该词的片段里把它补出来（本地混元实测已复现，见 ITranslator::glossary_for_text）
		const std::string sys_prompt =
			"You are a professional translator. Translate the user's message into " +
			target_name() +
			". Output only the translation itself, with no explanation and no quotation marks." +
			ITranslator::glossary_constraint(
				ITranslator::glossary_for_text(glossary_, src)) +
			// 上下文：**和本地混元用同一个 context_block()**。
			// 前文只进 system（不进 user），否则模型分不清哪句要译、哪句是背景。
			ITranslator::context_block();

		json payload = {
			{"model", "deepseek-chat"},
			{"messages", json::array({
				{{"role", "system"}, {"content", sys_prompt}},
				{{"role", "user"}, {"content", src}}
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
						// ② 回显兜底：万一还是把指令翻了出来，回退成原文
						//（显示看不懂的英文，好过显示看着通顺的伪造中文）
						if (ITranslator::looks_like_prompt_echo(translated_text)) {
							std::cerr << "[翻译] ⚠️ 输出疑似「把指令翻译了一遍」，已回退为原文："
							          << src << std::endl;
							translated_text = src;
						}
						std::cout << "翻译结果: " << translated_text << std::endl;
						SessionStore::instance().log_segment(text_to_translate, translated_text, name(), last_api_ms_.load(), req.confidence);
						{
							std::lock_guard<std::mutex> lock(result_mutex_);
							last_translation_ = translated_text;
							last_source_      = text_to_translate;
						}
						translation_count_.fetch_add(1);
						// 译完才推进滚动上下文（理由同本地混元：前文是"之前说过什么"，
						// 不能把当前段也塞进去）
						note_source(text_to_translate);
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

long long DeepSeekTranslator::get_last_api_ms() {
    return last_api_ms_.load();
}
