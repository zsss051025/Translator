#include "HunyuanTranslator.h"
#include <iostream>
#include <chrono>

///////////////////////////////////////////////////////////

HunyuanTranslator::HunyuanTranslator(const std::string& model_path) 
    :model_path_(model_path),running_(false)
{   
    
}

HunyuanTranslator::~HunyuanTranslator() {
    stop();
    if(ctx) {
        llama_free(ctx);
        ctx = nullptr;
    }
    if(model) {
        llama_free_model(model);
        model = nullptr;
    }
}


////////////////////////////////////////////////////////////


bool HunyuanTranslator::init() {
    //加载模型
    llama_model_params model_params = llama_model_default_params();// 获取默认参数
    model_params.n_gpu_layers = 99; // 将部分层加载到GPU上（一步步调试）
    model = llama_model_load_from_file(model_path_.c_str(), model_params);
    if(!model) {
        std::cerr << "[Error]加载混元模型失败" << model_path_ << std::endl;
        running_ = false; // 加载失败，直接标记为不运行
        return false;
    }

    //创建上下文
    llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = 1024;
    ctx = llama_new_context_with_model(model,ctx_params);
    if(!ctx) {
        std::cerr << "[Error]创建混元模型上下文失败" << model_path_ << std::endl;
        llama_free_model(model); // 释放模型资源
        model = nullptr;
        running_ = false; // 创建上下文失败，直接标记为不运行
        return false;
    }

    std::cout << "[混元] 模型加载完成" << std::endl;
    return true;
}

void HunyuanTranslator::start() {
    if(running_) return; //如果已经在运行，直接返回，避免重复启动线程

    running_ = true; // 标记为正在运行
    //启动推理线程
    worker_thread_ = std::thread(&HunyuanTranslator::translation_worker, this);//在构造函数的最后创建线程，保证线程不会访问到未进行初始化的成员变量
    std::cout << "混元翻译器初始化成功,本地翻译已就绪"<< std::endl;
}

void HunyuanTranslator::stop() {
	running_ = false;                         // 标记为需要退出的状态
	cv_.notify_all();
	if (worker_thread_.joinable()) {
		worker_thread_.join();                // 等待子线程执行完再析构
	}
}


/////////////////////////////////////////////////////////



void HunyuanTranslator::push_text(const std::string& text) {
	if (text.empty()) {                       // 空文本直接跳过
		return;
	}
	{
		std::lock_guard<std::mutex> lock(queue_mutex_);
		if(text_queue_.size() >= MAX_QUEUE_SIZE) {
			std::cerr << "[警告] 翻译队列已满(" << MAX_QUEUE_SIZE<< ")，丢弃最旧文本: " << text_queue_.front() << std::endl;
			text_queue_.pop();
		}
		text_queue_.push(text);
	}
	cv_.notify_one();
}

long long HunyuanTranslator::get_last_api_ms() {
    return last_api_ms_.load();
}


/////////////////////////////////////////////////////////////


void HunyuanTranslator::translation_worker() {
    const llama_vocab* vocab = llama_model_get_vocab(model);

    while(running_) {
        //等待数据
        llama_sampler* smpl = llama_sampler_init_greedy();
        std::string text;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            cv_.wait(lock,[this] {return !text_queue_.empty() || !running_;});
            if(!running_ && text_queue_.empty()) break;
            text = text_queue_.front();
            text_queue_.pop();
        }

        auto t1  = std::chrono::steady_clock::now();//打表
        
        // 记录当前这轮翻译的位置进度
        int n_past = 0;

        //构造promt
        std::string prompt = "<|im_start|>user\n将以下文本翻译为中文：" + text + "<|im_end|>\n<|im_start|>assistant\n";

        //tokenize token化
        std::vector<llama_token> tokens(1024); // 预分配足够的空间，实际使用时会根据需要调整大小
        int n_in = llama_tokenize(vocab,prompt.c_str(),prompt.size(),tokens.data(),tokens.size(),true,false);
        tokens.resize(n_in); // 调整大小以适应实际的token数量

        //修改前
        // //推理
        // llama_batch batch = llama_batch_get_one(tokens.data(),n_in);
        // if (llama_decode(ctx,batch) != 0) {
        //     std::cerr << "Failed to decode prompt" << std::endl;
        //     continue;
        // }



        // [新代码] 动态申请内存并手动填充 Batch
        llama_batch batch = llama_batch_init(n_in, 0, 1);
        for (int i = 0; i < n_in; i++) {
            batch.token[i] = tokens[i];
            batch.pos[i] = i;                  // 逐个赋予正确的绝对位置
            batch.n_seq_id[i] = 1;
            batch.seq_id[i][0] = 0;
            batch.logits[i] = false;           // Prompt 中间的字不需要计算输出概率，大幅节省 GPU 算力
        }
        batch.logits[n_in - 1] = true;         // 只有 Prompt 的最后一个字需要激活概率预测

        // 👇 加这一行：显式告诉 Batch 里面有 n_in 个 token 需要处理
        batch.n_tokens = n_in;


        if (llama_decode(ctx, batch) != 0) {
            std::cerr << "Failed to decode prompt" << std::endl;
            llama_batch_free(batch);           // 失败时防内存泄漏
            llama_sampler_free(smpl);
            continue;
        }
        llama_batch_free(batch);               // Prompt 喂完后，立刻释放这段申请的内存
        // =========================================================================



        // Prompt 消化完毕，位置指针向后移动 n_in，告诉模型接下来生成的新 token 就是从这个位置开始的
        n_past += n_in;       

        llama_token eos = llama_token_eos(vocab);//获取EOS token id
        //逐步获取输出token并解码成文本
        std::string output;
        int max_new = 128;

        // =========================================================================
        // 【修改点 2：单字生成部分】避免污染底层全局变量，依然采用动态申请
        // =========================================================================
        // [新代码] 在 for 循环外动态申请一个容量为 1 的 batch 用于逐字生成
        llama_batch one = llama_batch_init(1, 0, 1);



        for(int i = 0; i < max_new;i++) {
            llama_token  next =llama_sampler_sample(smpl,ctx,-1);

            if (next == eos) break;

            char buf[256];
            int len = llama_token_to_piece(vocab,next,buf,sizeof(buf),0,false);
            output.append(buf,len);

            //修改前
            // llama_batch one = llama_batch_get_one(&next,1);

            // // 给新生成的 token 指定正确的绝对位置
            // one.pos[0] = n_past;



            // [新代码] 手动安全地填充这个单字 batch
            one.token[0] = next;
            one.pos[0] = n_past;               // 指定正确的绝对位置
            one.n_seq_id[0] = 1;
            one.seq_id[0][0] = 0;
            one.logits[0] = true;              // 逐字生成时必须计算概率

            // 👇 加这一行：显式告诉 Batch 里面只有 1 个 token
            one.n_tokens = 1;

            int ret = llama_decode(ctx, one);
            if (ret != 0) {
                std::cerr << "[错误] llama_decode 失败，返回码 " << ret << std::endl;
                break;   // 立即退出生成循环
            }
            n_past++;// 生成成功，位置继续推进
        }

        // [新代码] 生成循环结束后，记得释放这个容量为 1 的 batch
        llama_batch_free(one);
        // =========================================================================

        auto t2 = std::chrono::steady_clock::now();
        last_api_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();

        size_t pos = output.find("<|im_end|>");
        if (pos != std::string::npos) {
            output.erase(pos);
        }
        std::cout << "\n[混元翻译] " << output << " | 耗时:" << last_api_ms_.load() << "ms\n" << std::endl;



        // ==========================================
        // 【关键修复】当前这轮翻译彻底结束，清空记忆！
        // ==========================================
        llama_memory_t mem = llama_get_memory(ctx);
        if (mem != nullptr) {
            llama_memory_clear(mem, true);
        }
        llama_sampler_free(smpl);
    }
    

}

