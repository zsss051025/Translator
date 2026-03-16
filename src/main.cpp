#include <iostream>
#include <vector>
#include <string>
#include <conio.h>    // 用于 _kbhit() 和 _getch()
#include <chrono>
#include <thread>
#include "audio_capture.h"
#include "SpeechEngine.h"

int main() {
    // 1. 设置控制台为 UTF-8 编码，防止中文乱码
    system("chcp 65001");

    // 2. 初始化引擎（建议使用 small 模型，精度与速度的平衡点）
    SpeechEngine engine;
    const std::string model_path = "C:/dev/projects/whisper.cpp/models/ggml-large-v3.bin";

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
    std::cout << ">>> 提示：对着麦克风说话，按 'Q' 键退出系统 <<<" << std::endl;
    std::cout << "------------------------------------------------" << std::endl;

    // --- 核心逻辑变量 ---
    std::vector<float> audio_accumulator;      // 音频累加缓冲区
    const int sample_rate = 16000;             // Whisper 标准采样率
    const float trigger_seconds = 3.0f;        // 攒够 2 秒音频再进行一次推理
    const size_t trigger_size = static_cast<size_t>(sample_rate * trigger_seconds);

    while (true) {
        // A. 退出检测
        if (_kbhit()) {
            char ch = static_cast<char>(_getch());
            if (ch == 'q' || ch == 'Q') break;
        }

        // B. 采集音频片段
        std::vector<float> pcm_chunk = capture.get_buffer_and_clear();


        if (!pcm_chunk.empty()) {
            // 将当前采集到的一丁点声音放入“大池子”
            audio_accumulator.insert(audio_accumulator.end(), pcm_chunk.begin(), pcm_chunk.end());
        }

        // C. 检查是否达到推理长度阈值
        if (audio_accumulator.size() >= trigger_size) {
            // 将攒够的 2 秒音频通过生产者接口塞入 SpeechEngine
            // 这里使用的是 std::move 来减少一次内存拷贝，符合高性能 C++ 习惯
            engine.push_audio(audio_accumulator);

            // 发送后清空累加器，准备下一次积累
            audio_accumulator.clear();
        }

        // D. 获取并显示最新文字结果
        std::string result = engine.get_last_text();

        int count = engine.get_inference_count();
        if (!result.empty()) {
            // \r 会让光标回到行首，实现原地刷新的效果
            // 后面加一些空格是为了覆盖掉之前可能更长的文字
            std::cout << "\r[第 " << count << " 次推理] 文字: " << result << "          " << std::flush;
        }

        // E. 适当休眠，避免主线程空转占满 CPU
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 5. 资源清理
    std::cout << "\n\n[System] 正在关闭系统..." << std::endl;
    capture.stop();
    engine.stop();

    return 0;
}