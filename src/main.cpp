#include<iostream>
#include "miniaudio.h"
#include<thread>
#include "whisper.h"

ma_encoder my_encoder;


//回调函数 传输的参数是按照miniaudio给的回调函数的格式来写的,此处在回调函数中往文件里写入数据
//注意！！回调函数会被频繁的调用所以最好别进行cout等耗时长的操作，以防扰乱采样与数据的写入
void My_Callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount)
{
	//判空
	if (pInput == nullptr) return;
	//写入
	ma_encoder_write_pcm_frames(&my_encoder, pInput, frameCount, nullptr);

	
	static int count = 0;
	count++;
	if (count % 20 == 0) {  // 每20次打印一次，避免刷屏
		std::cout << "回调被调用 " << count << " 次，收到 " << frameCount << " 帧\n";
	}
}
//全局的wav编码器


int main()
{
	//初始化配置
	ma_device_config config = ma_device_config_init(ma_device_type_loopback);
	config.sampleRate = 16000;
	config.capture.format = ma_format_f32;
	config.capture.channels = 1;
	config.dataCallback = My_Callback;

	//初始化WAV编码器
	ma_encoder_config encoderConfig = ma_encoder_config_init(
			ma_encoding_format_wav,
		ma_format_f32,
		1,
		16000
	);

	ma_result encResult = ma_encoder_init_file("test.wav", &encoderConfig, &my_encoder);
	if ((encResult != MA_SUCCESS)) {
		std::cout << "WAV文件创建失败 ,错误码为" << encResult << std::endl;
		return -1;
	}

	std::cout << "已准备写入文件" << std::endl;




	//初始化设备
	ma_device device;
	ma_result result = ma_device_init(nullptr, &config, &device);
	//检查结果
	if (result != MA_SUCCESS) {
		std::cout << "设备打开失败，错误码:" << result << std::endl;
		return -1;
	}

	std::cout << "音频设备已初始化" << std::endl;

	//启动采集
	ma_device_start(&device);
	if (result != MA_SUCCESS) {
		std::cout << "启动捕获失败，错误码: " << result << std::endl;
		ma_device_uninit(&device);
		ma_encoder_uninit(&my_encoder);
		return -1;
	}
	std::cout << "采集已启动，正在捕获系统音频" << std::endl;

	//按键退出机制,阻塞main线程，但是miniaudio的函数中创建的线程自己会运行
	std::cout << "播放声音后进行测试，按q + Enter退出" << std::endl;
	char ch;
	do {
		std::cin >> ch;
	} while (ch != 'q' && ch != 'Q');

	std::cout << "正在停止设备并保存文件...\n";
	ma_device_stop(&device);       // 先停止采集
	ma_device_uninit(&device);     // 释放设备资源
	ma_encoder_uninit(&my_encoder); // 关闭文件，写入头信息
	std::cout << "文件 test.wav 已保存完成\n";


	return 0;
}