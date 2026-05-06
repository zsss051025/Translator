# AudioTranslator

实时语音翻译系统 —— 采集系统音频 → Whisper 语音识别 → DeepSeek AI 翻译。

## 架构

```
┌─────────────────┐     ┌─────────────────┐     ┌─────────────────┐
│  AudioCapture   │ ──▶ │   SpeechEngine   │ ──▶ │   Translator    │
│  (miniaudio)    │     │  (whisper.cpp)   │     │  (DeepSeek API) │
│                 │     │                  │     │                 │
│  Loopback 采集   │     │  GPU/CUDA 推理   │     │  HTTPS 长连接    │
│  16kHz / mono   │     │  生产者-消费者队列  │     │  指数退避重试    │
└─────────────────┘     └─────────────────┘     └─────────────────┘
        ↑                       ↑                       ↑
        │                       │                       │
    回调写入缓冲          VAD 分句触发推理          HTTP POST 翻译
```

## 依赖

| 依赖 | 用途 |
|------|------|
| CMake >= 3.20 | 构建系统 |
| MSVC 2022 或 GCC 13+ | 编译器（需支持 C++20） |
| vcpkg | 包管理（安装 OpenSSL） |
| [whisper.cpp](https://github.com/ggerganov/whisper.cpp) | 语音识别（需放在本项目同级目录） |
| CUDA Toolkit（可选） | GPU 加速推理 |
| miniaudio | 音频采集（已内嵌在 `external/`） |
| cpp-httplib | HTTP 客户端（已内嵌在 `external/`） |
| nlohmann/json | JSON 解析（已内嵌在 `external/`） |

## 编译

```bash
# 1. 设置环境变量
set VCPKG_ROOT=C:/your-path/vcpkg
set DEEPSEEK_API_KEY=sk-xxxxxxxx

# 2. 确保 whisper.cpp 在同级目录
# 你的目录结构应为：
#   projects/
#     AudioTranslator/   ← 本项目
#     whisper.cpp/       ← 依赖

# 3. 配置 CMake（启用 GPU）
cmake -B build -DGGML_CUDA=ON

# 4. 编译
cmake --build build --config Release
```

## 运行

```bash
# 设置模型路径
set WHISPER_MODEL_PATH=C:/path/to/ggml-large-v3.bin

# 运行
./build/Release/Translator.exe

# 按 Q 键退出
```

## 工作流程

1. **音频采集**：通过 miniaudio Loopback 采集系统声卡输出，16kHz / 32-bit float / 单声道
2. **VAD 分句**：基于 RMS 能量检测语音活动，句子结束后触发推理；滑动窗口（3 秒）兜底防止长句延迟过高
3. **语音识别**：whisper.cpp large-v3 模型 GPU 推理，多 segment 拼接为完整文本
4. **幻觉过滤**：长度阈值 + 黑名单双重过滤，减少静音段误输出
5. **翻译**：通过 DeepSeek Chat API 翻译为中文，失败自动重试

## 技术点

- **生产者-消费者模式**：AudioCapture（生产）→ SpeechEngine（消费+生产）→ Translator（消费），通过 `condition_variable` 实现线程间唤醒
- **RAII 资源管理**：各模块析构时自动停止线程、释放模型资源
- **队列背压**：推理队列和翻译队列均有上限，满时丢弃最旧数据并告警
- **长连接复用**：cpp-httplib HTTP/1.1 keep-alive，TLS 会话复用
- **GPU 加速**：whisper 推理默认启用 CUDA，检测不到 GPU 自动退回 CPU

## 后续计划

- [ ] WebRTC VAD 替代 RMS 能量检测
- [ ] 混元翻译模型本地部署（llama.cpp）
- [ ] Web 前端配置界面
- [ ] 配置文件支持（JSON）
- [ ] 翻译结果日志持久化

## License

MIT
