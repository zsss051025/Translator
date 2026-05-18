#pragma once
#include <string>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

#include "llama.h"
#include "ITranslator.h"

class HunyuanTranslator : public ITranslator {
public:
    HunyuanTranslator(const std::string& model_path);
    ~HunyuanTranslator();

    void push_text(const std::string& text) override;
    void stop() override;
    long long get_last_api_ms() override;
    bool init();
    void start();                                   // 启动推理线程

private:
    void translation_worker();// 推理线程函数


    std::string model_path_;
    static const size_t MAX_QUEUE_SIZE = 100;
    std::queue<std::string> text_queue_;

    std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{ false };
    std::thread worker_thread_;

    std::atomic<long long> last_api_ms_{0};

    llama_model* model  = nullptr; // LLaMA 模型对象指针
    llama_context* ctx = nullptr; // LLaMA 上下文对象指针
};



