#include "SpeechEngine.h"
#include  <iostream>

SpeechEngine::SpeechEngine() {

}

SpeechEngine::~SpeechEngine() {
	stop();//若是不停止线程，则可能对象被销毁了但是线程还在运行
	if (whisper_ctx) whisper_free(whisper_ctx);//RAII,在析构函数中释放句柄这个资源
}


bool SpeechEngine::init(const std::string& model_path) {
	whisper_context_params params = whisper_context_default_params();//默认配置
	params.use_gpu = true;//先默认开启gpu的加速，若是没检测到gpu等错误就会自动退回使用cpu

	whisper_ctx = whisper_init_from_file_with_params(model_path.c_str(), params);//模型路径在传入的时候得将std::string转换称const char*
	return whisper_ctx != nullptr;

};				//传入模型的路径初始化模型
void SpeechEngine::start() {
	if (is_running_) return;
	is_running_ = true;
	worker_thread_ = std::thread(&SpeechEngine::run_inference_loop, this);
};																//启动推理线程
void SpeechEngine::stop() {
	is_running_ = false;

	cv_.notify_all();//唤醒worker_thread
	if (worker_thread_.joinable()) {//让主线程等待到woker运行完，这样若是要销毁对象的话就不会出现worker还在使用而资源已被销毁的情况
		worker_thread_.join();
	}

};	
void SpeechEngine::push_audio(const std::vector<float>& data) {
	std::unique_lock<std::mutex> lock(queue_mutex_);

	//背压控制
	if (audio_queue_.size() >= MAX_QUEUE_SIZE) {
		audio_queue_.pop();
	}

	audio_queue_.push(data);
	cv_.notify_one();//告诉后台线程有活干了

};
std::string SpeechEngine::get_last_text() {
		std::lock_guard<std::mutex> lock(text_mutex_);
		return last_text_;
};


void SpeechEngine::run_inference_loop() {
	while (is_running_) {
		std::vector<float> audio_to_process;
		{
			//拿数据时加锁，拿完就解锁
			std::unique_lock<std::mutex> lock(queue_mutex_);
			cv_.wait(lock, [this] { return !audio_queue_.empty() || !is_running_; });//lamda表达式

			if (!is_running_) break;

			audio_to_process = std::move(audio_queue_.front());
			audio_queue_.pop();
		}
		//执行模型推理
		whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
		wparams.n_threads = 8;
		wparams.language = "zh";
		wparams.print_timestamps = false;
		wparams.no_context = true; // 实时模式建议开启
		wparams.offset_ms = 0;
		wparams.single_segment = false;

		if (whisper_full(whisper_ctx, wparams, audio_to_process.data(), (int)audio_to_process.size()) == 0) {
			const int n_segments = whisper_full_n_segments(whisper_ctx);
			if (n_segments > 0) {
				const char* text = whisper_full_get_segment_text(whisper_ctx, 0);

				std::lock_guard<std::mutex> lock(text_mutex_);
				last_text_ = text; // 更新结果
				inference_count_++;
			}
		}

		
	}
}

int SpeechEngine::get_inference_count() {
	return inference_count_.load();//.load()原子读取，表示我在读这个值的时候谁也不许碰它
}



