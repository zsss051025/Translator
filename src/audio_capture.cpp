#include "audio_capture.h"
#include <iostream>

AudioCapture::AudioCapture() {

}

AudioCapture::~AudioCapture() {
	stop();
}

bool AudioCapture::init() {
	// 初始化配置
	ma_device_config config = ma_device_config_init(ma_device_type_loopback);
	config.sampleRate = 16000;
	config.capture.format = ma_format_f32;
	config.capture.channels = 1;
	config.pUserData = this;//在这里传入this指针，以便在回调函数中访问类成员
	config.dataCallback = data_callback;


	// 初始化设备
	ma_result result = ma_device_init(nullptr, &config, &device);
	if (result != MA_SUCCESS) {
		std::cout << "设备初始化失败，错误码: " << result << std::endl;
		return false;
	}

	std::cout << "音频采集初始化成功" << std::endl;
	return true;
}

// 开始采集
void AudioCapture::start() {
	ma_device_start(&device);;
	std::cout << "采集已启动" << std::endl;
}

// 停止采集
void AudioCapture::stop() {
	ma_device_stop(&device);
	ma_device_uninit(&device);
	std::cout << "采集已停止，资源已释放" << std::endl;
}

// 数据传输给 whisper
std::vector<float> AudioCapture::get_buffer_and_clear() {
	std::lock_guard<std::mutex> lock(buffer_mutex);
	std::vector<float> temp = std::move(audio_buffer);   // 临时创建 vector 接管缓冲区数据
	audio_buffer.clear();
	return temp;
}

void AudioCapture::data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
	AudioCapture* self = static_cast<AudioCapture*> (pDevice->pUserData);
	if (self == nullptr) return;//类指针为空，无法访问类成员，直接返回

	if (pInput == nullptr) return;//要采集的内容为空

	//ma_encoder_write_pcm_frames(&self->encoder, pInput, frameCount, nullptr);

	std::lock_guard<std::mutex> lock(self->buffer_mutex);
	const float* src = static_cast<const float*>(pInput);//显示转换为 float* 类型，以便插入到缓冲区
	self->audio_buffer.insert(self->audio_buffer.end(), src, src + frameCount);

	static int count = 0;
	count++;
	if (count % 20 == 0) {
	   // std::cout << "已经采集 " << self->audio_buffer.size() / 16000.0 << "秒音频" << std::endl;
	}

}
