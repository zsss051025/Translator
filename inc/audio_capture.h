#pragma once
#include "miniaudio.h"
#include <vector>
#include <mutex>

class AudioCapture {
public:
	AudioCapture();
	~AudioCapture();

	bool init();                                 // 初始化配置，设置音频设备
	void start();                                // 开始采集
	void stop();                                 // 停止与析构

	// 获取当前缓冲区的音频数据，供 whisper 使用
	std::vector<float> get_buffer_and_clear();

private:
	ma_device device;

	std::vector<float> audio_buffer;
	std::mutex buffer_mutex;

	static void data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount);

};
