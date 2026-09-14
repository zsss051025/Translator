#pragma once
#include "miniaudio.h"
#include <vector>
#include <mutex>

// 音频来源。
// 环回解决"对方/视频在说什么"，麦克风解决"你和房间里在说什么"。
// 面对面交谈时麦克风能收进双方；线上通话（戴耳机）时环回收对方、麦克风收你。
enum class CaptureSource {
	SystemLoopback,   // 系统音频环回（WASAPI loopback，采集扬声器输出）
	Microphone,       // 麦克风（系统默认录音设备）
};

class AudioCapture {
public:
	explicit AudioCapture(CaptureSource source = CaptureSource::SystemLoopback);
	~AudioCapture();

	// 禁止拷贝：内部持有设备句柄与回调用的 self 指针
	AudioCapture(const AudioCapture&) = delete;
	AudioCapture& operator=(const AudioCapture&) = delete;

	bool init();                                 // 初始化配置，设置音频设备
	void start();                                // 开始采集
	void stop();                                 // 停止并释放（幂等，可重复调用）

	// 获取当前缓冲区的音频数据，供 whisper 使用
	std::vector<float> get_buffer_and_clear();

	const char* source_name() const {
		return source_ == CaptureSource::Microphone ? "麦克风" : "系统音频";
	}

private:
	ma_device device;
	CaptureSource source_;
	// 标记设备是否已初始化。stop() 与析构都会释放设备，
	// 靠这个标志保证 ma_device_uninit 只被调用一次。
	bool initialized_ = false;

	std::vector<float> audio_buffer;
	std::mutex buffer_mutex;

	static void data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount);

};
