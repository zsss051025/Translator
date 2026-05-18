# AudioTranslator

实时语音翻译系统 —— 采集系统音频 → Whisper 语音识别 → 双后端翻译（云端 DeepSeek / 本地混元 1.8B），支持 GPU 加速与运行时切换。

## 架构

```
┌─────────────────┐     ┌─────────────────┐     ┌──────────────────────┐
│  AudioCapture   │ ──▶ │   SpeechEngine   │ ──▶ │     ITranslator       │
│  (miniaudio)    │     │  (whisper.cpp)   │     │  (抽象接口/运行时切换)   │
│                 │     │                  │     ├──────────────────────┤
│  Loopback 采集   │     │  GPU/CUDA 推理   │     │ DeepSeekTranslator   │ ← 云端 API
│  16kHz / mono   │     │  生产者-消费者队列  │     │ HunyuanTranslator    │ ← 本地 llama.cpp
└─────────────────┘     └─────────────────┘     └──────────────────────┘
        ↑                       ↑                        ↑
        │                       │                        │
    回调写入缓冲          VAD 分句触发推理           虚函数多态分发
```

## 性能实测

| 后端 | 单次耗时 | 特点 |
|------|---------|------|
| 混元本地 (HY-MT1.5-1.8B Q4_K_M) | 180~380ms | 离线、零费用、隐私 |
| DeepSeek 云端 (deepseek-chat) | 600~1600ms | 质量好、需网络 |
| Whisper large-v3 (GPU) | 400~1300ms | CUDA 加速 |

## 依赖

| 依赖 | 用途 |
|------|------|
| CMake >= 3.20 | 构建系统 |
| MSVC 2022 或 GCC 13+ | 编译器（需支持 C++20） |
| vcpkg | 包管理（安装 OpenSSL） |
| CUDA Toolkit 12.x | GPU 加速（可选） |
| [whisper.cpp](https://github.com/ggerganov/whisper.cpp) | 语音识别 |
| [llama.cpp](https://github.com/ggerganov/llama.cpp) | 混元本地翻译推理 |
| miniaudio | 音频采集（内嵌 `external/`） |
| cpp-httplib | HTTP 客户端（内嵌 `external/`） |
| nlohmann/json | JSON 解析（内嵌 `external/`） |

## 目录结构

```
AudioTranslator/
├── inc/
│   ├── ITranslator.h            # 翻译器抽象接口
│   ├── DeepSeekTranslator.h     # 云端 DeepSeek 实现
│   ├── HunyuanTranslator.h      # 本地混元实现
│   ├── SpeechEngine.h           # Whisper 语音识别引擎
│   └── audio_capture.h          # miniaudio 音频采集
├── src/
│   ├── main.cpp                 # 主入口 + VAD + 后端选择
│   ├── SpeechEngine.cpp         # Whisper GPU 推理线程
│   ├── DeepSeekTranslator.cpp   # HTTPS 翻译 + 重试策略
│   ├── HunyuanTranslator.cpp    # llama.cpp 推理 + ChatML prompt
│   ├── audio_capture.cpp        # 声卡 Loopback 采集
│   └── miniaudio_impl.cpp       # miniaudio 实现
├── external/                    # 第三方头文件
├── CMakeLists.txt
└── README.md
```

## 编译

```bash
# 1. 确保 whisper.cpp 与 llama.cpp 在同级目录
# 你的目录结构应为：
#   projects/
#     AudioTranslator/   ← 本项目
#     whisper.cpp/       ← 依赖
#     llama.cpp/         ← 依赖
#     models/            ← 模型文件

# 2. 设置环境变量
set VCPKG_ROOT=C:/your-path/vcpkg
set DEEPSEEK_API_KEY=sk-xxxxxxxx   # 使用云端后端时需要

# 3. 配置 CMake
cmake -B build -DGGML_CUDA=ON

# 4. 编译
cmake --build build --config Release
```

## 运行

```bash
cd build/Release   # 或 build/Debug
./Translator.exe

# 按提示选择后端：
#   1 = DeepSeek 云端
#   2 = 混元本地
#   0 = 退出
# 进入翻译会话后按 Q 退出，可重新选择后端
```

## 模型下载

- Whisper: `whisper.cpp/models/download-ggml-model.cmd large-v3` 或 medium
- 混元: 从 HuggingFace 下载 `AngelSlim/HY-MT1.5-1.8B-GGUF` 的 `Q4_K_M` 量化版（约 1.1GB）

## 工作流程

1. **音频采集**：miniaudio Loopback 回调写入缓冲区，`get_buffer_and_clear()` 返回音频段
2. **VAD 分句**：RMS 能量检测 + `has_speech` 状态机防底噪 + 滑动窗口兜底
3. **语音识别**：whisper.cpp large-v3，CUDA GPU，束搜索（beam_size=2），多 segment 拼接
4. **幻觉过滤**：长度阈值 + 黑名单双重过滤
5. **翻译分发**：通过 `ITranslator` 虚函数分派到当前后端
   - **DeepSeek**：HTTPS POST，429/5xx 指数退避重试，4xx 放弃
   - **混元**：ChatML prompt 格式，`llama_decode` + 贪心采样，KV cache 逐轮清理

## 技术点

- **生产者-消费者**：两条独立队列链路（音频→推理→翻译），`condition_variable` 线程间唤醒
- **双后端多态**：`ITranslator` 抽象接口 → `unique_ptr` 多态持有 → 运行时切换
- **GPU 显存管理**：Whisper ~3.1GB + 混元 ~0.7GB，分开初始化为首次 CUDA JIT 预热
- **RAII 生命周期**：析构链 `stop() → join() → free()`

## License

MIT
