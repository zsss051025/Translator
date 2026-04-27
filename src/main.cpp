#include <iostream>
#include <vector>
#include <string>
#include <conio.h>    // 用于 _kbhit() 和 _getch()
#include <chrono>
#include <thread>
#include <cmath>

#include "Translator.h"
#include "audio_capture.h"
#include "SpeechEngine.h"



int main() {
    //  设置控制台为 UTF-8 编码，防止中文乱码
    system("chcp 65001");


    // 1. 从环境变量读取 API Key
    const char* env_api_key = std::getenv("DEEPSEEK_API_KEY");
    if (!env_api_key) {
        std::cerr << "[Error] 未检测到环境变量 DEEPSEEK_API_KEY，请先设置 API Key！" << std::endl;
        return -1;
    }
    std::string api_key = env_api_key;
    Translator translator(api_key);


    // 2. 初始化引擎
    SpeechEngine engine;
    const std::string model_path = "C:/dev/projects/whisper.cpp/models/ggml-large-v3.bin";//模型路径

    std::cout << "[System] 正在加载模型: " << model_path << " ..." << std::endl;
    if (!engine.init(model_path)) {
        std::cerr << "[Error] 模型加载失败，请检查路径！" << std::endl;
        return -1;
    }

    // 3. 启动后台推理线程
    engine.start();

    // 4. 初始化音频采集
    AudioCapture capture;
    if (!capture.init()) {
        std::cerr << "[Error] 音频设备初始化失败！" << std::endl;
        return -1;
    }

    capture.start();

    std::cout << "\n>>> 实时翻译系统已就绪 <<<" << std::endl;
    std::cout << ">>>    按 'Q' 键退出系统 <<<" << std::endl;
    std::cout << "------------------------------------------------" << std::endl;

    // --- 核心逻辑变量 ---
    std::vector<float> audio_accumulator;      // 音频累加缓冲区(后续换成环形缓冲区)
    const int sample_rate = 16000;             // Whisper 标准采样率
    const float trigger_seconds = 3.0f;        // 攒够3秒音频再进行一次推理
    const size_t trigger_size = static_cast<size_t>(sample_rate * trigger_seconds);


    float silence_threshold = 0.001f;
    int silence_count = 0;

	std::string last_displayed_text; // 记录上次显示的文本，避免重复输出
    while (true) {
        // A. 退出检测
        if (_kbhit()) {
            char ch = static_cast<char>(_getch());
            if (ch == 'q' || ch == 'Q') break;
        }

        // B. 采集音频片段
        std::vector<float> pcm_chunk = capture.get_buffer_and_clear();


        if (pcm_chunk.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        // 将当前采集到的一丁点声音放入“大池子”
        audio_accumulator.insert(audio_accumulator.end(), pcm_chunk.begin(), pcm_chunk.end());

        float sum_squares = 0.0f;
        for (float sample : pcm_chunk) {
              sum_squares += sample * sample;
         }
         float rms = std::sqrt(sum_squares / pcm_chunk.size());

         // 
         //拦截部分
            if (rms <= silence_threshold) {//小于设计的阈值说明没有说话
                silence_count++;
                // 池子里有超过 1 秒的声音，就说明一句话说完了
                if (silence_count > 20 && audio_accumulator.size() > sample_rate * 1) {
                    engine.push_audio(audio_accumulator);
                    audio_accumulator.clear();
                    silence_count = 0;
                    continue;
                }
            }
            else {
                silence_count = 0;
            }

        // C. 检查是否达到推理长度阈值
        if (audio_accumulator.size() >= trigger_size) {
            // 将攒够的 2 秒音频通过生产者接口塞入 SpeechEngine
            // 这里使用的是 std::move 来减少一次内存拷贝，符合高性能 C++ 习惯
            engine.push_audio(audio_accumulator);

            // 发送后，不要全清空！采用“滑动窗口”保留最后 1.75 秒的音频作为上下文
            const float keep_seconds = 1.5f;
            const size_t keep_size = static_cast<size_t>(sample_rate * keep_seconds);

            if (audio_accumulator.size() > keep_size) {
                // 用 vector 的迭代器，把最后 keep_size 长度的数据切下来
                std::vector<float> tail(audio_accumulator.end() - keep_size, audio_accumulator.end());
                // 把大池子替换成这个尾巴，剩下的丢弃
                audio_accumulator = std::move(tail);
            }
            else {
                audio_accumulator.clear(); // 兜底：如果不足 1.5 秒（极少发生），才全清
            }
        }

        // D. 获取并显示最新文字结果
        std::string result = engine.get_last_text();
        bool is_hallucination = false;
        if (result.length() < 4) {
            is_hallucination = true;
        }
        std::vector<std::string> blacklist = {
            "谢谢观看", "请不吝点赞", "订阅", "打赏", "明镜与点点",
            "字幕", "Amara", "[音乐]", "(音乐)", "翻译","yoyo"
            "♪"
        };

        for (const auto& bad_word : blacklist) {
            // std::string::npos 意思是“没找到”
            if (result.find(bad_word) != std::string::npos) {
                is_hallucination = true;
                break;
            }
        }

        if (!result.empty() && !is_hallucination) {
            if(result != last_displayed_text) {
                int count = engine.get_inference_count();
                std::cout << "\r[第 " << count << " 次推理] 文字: " << result << "     " << std::flush;
				last_displayed_text = result;

				translator.push_text(result);
			}
            // \r 会让光标回到行首，实现原地刷新的效果
            // 后面加一些空格是为了覆盖掉之前可能更长的文字
          
        }

        // E. 适当休眠，避免主线程空转占满 CPU
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 5. 资源清理
    std::cout << "\n\n[System] 正在关闭系统..." << std::endl;
    capture.stop();
    engine.stop();
	translator.stop();

    return 0;
}