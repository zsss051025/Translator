#include "SpeechEngine.h"
#include <iostream>
#include <chrono>

SpeechEngine::SpeechEngine() {

}

SpeechEngine::~SpeechEngine() {
	stop();                                         // 即使不停止线程，也要防止析构时线程还在跑
	if (whisper_ctx) whisper_free(whisper_ctx);     // RAII,在对象销毁时释放推理资源
}


bool SpeechEngine::init(const std::string& model_path) {
	whisper_context_params params = whisper_context_default_params(); // 默认参数
	params.use_gpu = true;                    // 启用GPU加速，检测不到GPU自动退回CPU

	whisper_ctx = whisper_init_from_file_with_params(model_path.c_str(), params);
	return whisper_ctx != nullptr;

}
void SpeechEngine::start() {
	if (is_running_) return;//如果已经在运行，直接返回，避免重复启动线程
	is_running_ = true;
	worker_thread_ = std::thread(&SpeechEngine::run_inference_loop, this);
}
void SpeechEngine::stop() {
	is_running_ = false;

	cv_.notify_all();                               // 通知 worker_thread
	if (worker_thread_.joinable()) {                // 等待 worker 执行完再析构，避免提早释放资源
		worker_thread_.join();
	}

}
void SpeechEngine::push_audio(const std::vector<float>& data) {
	std::unique_lock<std::mutex> lock(queue_mutex_);

	// 防堆积
	if (audio_queue_.size() >= MAX_QUEUE_SIZE) {//背压机制：如果队列满了，丢弃最旧的音频段，保持系统响应性
		std::cerr << "[警告] 推理队列已满(" << MAX_QUEUE_SIZE << ")，丢弃最旧音频段" << std::endl;
		audio_queue_.pop();
	}

	audio_queue_.push(data);
	cv_.notify_one();                               // 唤醒后台线程去消费

}
std::string SpeechEngine::get_last_text() {
		std::lock_guard<std::mutex> lock(text_mutex_);
		return last_text_;
}


void SpeechEngine::run_inference_loop() {
	while (is_running_) {
		std::vector<float> audio_to_process;
		{
			// 临界区：等待和取数据
			std::unique_lock<std::mutex> lock(queue_mutex_);
			cv_.wait(lock, [this] { return !audio_queue_.empty() || !is_running_; });

			if (!is_running_) break;

			audio_to_process = std::move(audio_queue_.front());
			audio_queue_.pop();
		}
		// 执行模型推理
		whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
		wparams.n_threads = std::min(4, (int)std::thread::hardware_concurrency() / 2);
		wparams.language = "auto";                     //自动语言识别，提升多语种环境下的准确率
		wparams.print_timestamps = false;
		wparams.strategy = WHISPER_SAMPLING_BEAM_SEARCH;
		wparams.beam_search.beam_size = 2; // 兼顾极速与高精度
		wparams.no_context = true;              // 实时模式参数
		wparams.offset_ms = 0;
		wparams.single_segment = false;


		auto t1 = std::chrono::steady_clock::now();
		int whisper_ret = whisper_full(whisper_ctx, wparams, audio_to_process.data(), (int)audio_to_process.size());
		auto t2 = std::chrono::steady_clock::now();

		if (whisper_ret == 0) {
			const int n_segments = whisper_full_n_segments(whisper_ctx);
			//将获取到的多个 segment 拼接成一个字符串，作为最终结果返回
			std::string combined_text;
			for (int i = 0; i < n_segments; i++) {
				const char* segment_text = whisper_full_get_segment_text(whisper_ctx, i);
				if (segment_text && segment_text[0] != '\0') {
					if (!combined_text.empty()) {
						combined_text += " ";  // segment 之间加空格
					}
					combined_text += segment_text;
				}
			}

			if (!combined_text.empty()) {
				last_inference_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
				std::lock_guard<std::mutex> lock(text_mutex_);
				last_text_ = std::move(combined_text);
				inference_count_++;
			}
		} else {
			std::cerr << "[警告] 推理异常，音频长度: " << audio_to_process.size() << " 采样点" << std::endl;
		}


	}
}

long long SpeechEngine::get_last_inference_ms() {
	return last_inference_ms_.load();
}

int SpeechEngine::get_inference_count() {
	return inference_count_.load();                 // .load() 原子读取
}
