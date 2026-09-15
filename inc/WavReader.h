#pragma once
#include <string>
#include <vector>

// WAV 文件读取 —— 服务于测试入口 `--wav`（见 PROJECT.md §7 第 1 阶段）。
//
// 【为什么不直接用 da97d96 里那份 inc/wav_reader.h】
// 它作为起点可以，但有四个真问题，直接用会得到"静默错误"的结果：
//   ① 收了一个 expected_rate 参数却**从未使用** —— 完全没有重采样。
//      44.1kHz 的文件会被当成 16kHz 喂进 Whisper，听上去是被拉长的声音，
//      识别结果全是垃圾，而且不会报任何错。
//   ② 16bit 以外的位深（8/24/32 整数）一个分支都不命中，sample 保持 0，
//      输出一整段静音 —— 同样不报错。
//   ③ 用 reinterpret_cast<int32_t*> 在任意字节偏移上取值：未对齐访问 + 违反严格别名。
//   ④ chunk 的 size 直接来自文件且不做边界检查，pos += 8 + size 可以越界。
//
// 【为什么转录链路对格式这么敏感】
// Whisper 只认 16kHz 单声道 float。磁盘上的 WAV 什么规格都可能是
// （44.1kHz / 48kHz、立体声、16 或 24bit），所以这一层必须
// "读进来 → 混单声道 → 重采样到 16k" 做干净，否则测出来的东西没有意义。
class WavReader {
public:
    struct Result {
        bool               ok = false;
        std::string        error;
        std::vector<float> samples;      // 单声道，已重采样到 target_rate，取值 [-1, 1]
        int                src_rate     = 0;
        int                src_channels = 0;
        int                src_bits     = 0;
        int                src_format   = 0;   // 1 = PCM 整数, 3 = IEEE float
        double             src_seconds  = 0.0;
    };

    // 读文件 → 单声道 → 线性插值重采样到 target_rate
    static Result read_file(const std::string& path, int target_rate);

    // 从内存解析（自检用：不依赖磁盘文件，用例可以现场造 WAV 字节）
    static Result parse(const std::vector<unsigned char>& bytes, int target_rate);

    // 供自检造素材：生成最小合法 WAV（PCM 16bit）
    static std::vector<unsigned char> make_test_wav(const std::vector<float>& mono,
                                                    int rate, int channels);
};
