#include "audio_capture.h"
#include "whisper.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <conio.h>
#include <windows.h> // 必须包含，用于处理控制台编码

whisper_context* whisper_ctx = nullptr;//模型本体非常大所以只能用指针去接受模型的句柄

int main() {
    // 强制控制台使用 UTF-8 编码，解决中文乱码
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);

    std::cout << "==========================================" << std::endl;
    std::cout << "   AudioTranslator - 实时语音识别 (Release)" << std::endl;
    std::cout << "==========================================" << std::endl;

    //模型路径与加载模型
    const char* model_path = "C:/dev/projects/whisper.cpp/models/ggml-base.bin";
    whisper_ctx = whisper_init_from_file(model_path);
    if (!whisper_ctx) {
        std::cerr << "加载模型失败！" << std::endl;
        return -1;
    }

    AudioCapture capture;
    if (!capture.init()) return -1;
    capture.start();

    std::cout << "\n>>> 正在监听... 按 'Q' 键安全退出 <<<\n" << std::endl;

    auto last_process = std::chrono::steady_clock::now();
    std::vector<float> accumulated_audio;//整体的音频数据存储的地方

    while (true) {
        // 非阻塞退出检测
        if (_kbhit()) {//读取一个键盘的输入并判断是否为"q/Q"
            char c = _getch();
            if (c == 'q' || c == 'Q') break;
        }

        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - last_process).count();//计算时间差

        // 每 3 秒检查一次
        if (elapsed >= 3.0) {
            std::vector<float> new_samples = capture.get_buffer_and_clear();
            accumulated_audio.insert(accumulated_audio.end(), new_samples.begin(), new_samples.end());

            // 积攒够 5 秒音频才处理
            if (accumulated_audio.size() >= 16000 * 5) {

                whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
                params.language = "zh";       // 固定中文，减少乱猜
                params.n_threads = 8;        // 核心数
                params.print_timestamps = false;
                params.no_context = true;    // 实时识别设为 true，减少幻听重复文字
                params.single_segment = true; // 强制输出单段，适合短句实时显示

                auto start_t = std::chrono::steady_clock::now();

                if ( whisper_full(whisper_ctx, params, accumulated_audio.data(), (int)accumulated_audio.size() ) == 0) {
                    int n = whisper_full_n_segments(whisper_ctx);
                    for (int i = 0; i < n; ++i) {
                        const char* text = whisper_full_get_segment_text(whisper_ctx, i);
                        // 过滤掉只有符号或过短的干扰项
                        if (strlen(text) > 3) {
                            std::cout << "[识别] " << text << std::endl;
                        }
                    }
                }

                auto end_t = std::chrono::steady_clock::now();
                double dur = std::chrono::duration<double>(end_t - start_t).count();
                // std::cout << "  (耗时: " << dur << "s)" << std::endl; // 调试用

                // --- 【核心改进：滑动窗口】 ---
                // 识别完后不要 clear 全库，保留最后 1.5 秒的音频。
                // 这样如果一句话没说完，它的结尾会作为下一段的开头，识别更准。
                size_t keep_samples = (size_t)(16000 * 3);
                if (accumulated_audio.size() > keep_samples) {//擦除3s之前的数据，带着这些数据进行下一次循环
                    accumulated_audio.erase(accumulated_audio.begin(), accumulated_audio.end() - keep_samples);
                }
            }
            last_process = std::chrono::steady_clock::now();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "\n正在停止采集并释放资源..." << std::endl;
    capture.stop();
    if (whisper_ctx) whisper_free(whisper_ctx);

    std::cout << "程序已安全退出。" << std::endl;
    return 0;
}