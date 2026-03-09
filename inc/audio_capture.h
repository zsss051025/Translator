#pragma once
#include "miniaudio.h"
#include<vector>
#include<mutex>

class AudioCapture {
public:
	AudioCapture();
	~AudioCapture();

	bool init();		//初始化配置，编码器，设备
	void start();		//启动采集
	void stop();		//停止并失能

	// 获取当前缓冲区数据（供 whisper 使用）
	std::vector<float> get_buffer_and_clear();

private:
	ma_device device;
	//ma_encoder encoder;
	
	std::vector<float> audio_buffer;
	std::mutex buffer_mutex;

	static void data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount);

};