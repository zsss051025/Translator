#include "HunyuanTranslator.h"
#include <iostream>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <vector>
#include "SessionStore.h"
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

void HunyuanTranslator::set_target_language(const std::string& lang) {
    if (!lang.empty()) target_lang_ = lang;
}

std::string HunyuanTranslator::get_last_translation() const {
    std::lock_guard<std::mutex> lock(result_mutex_);
    return last_translation_;
}

int HunyuanTranslator::get_translation_count() const {
    return translation_count_.load();
}

std::string HunyuanTranslator::get_last_source() const {
    std::lock_guard<std::mutex> lock(result_mutex_);
    return last_source_;
}

std::string HunyuanTranslator::translate_system_prompt(const std::string& text) const {
    // 系统提示负责固定"只输出译文"。
    // 不要写成"将以下文本翻译为中文："这种祈使句——
    // 模型会把待译文本当成同一段话的续写，于是指令被原样吐出来。
    //
    // 【术语约束必须按段过滤】只带上**这段原文里真的出现**的术语。
    // 不过滤的话，模型会在没提到该词的片段里凭空把它补出来 ——
    // 实测 `and wife get ready to go` 被译成「埃丽卡和马可准备出发了」，
    // 原文里一个名字都没有。详见 ITranslator::glossary_for_text 的说明。
    return "You are a professional translator. Translate the user's message into " + target_name() +
           ". Output only the translation itself, with no explanation, no prefix, "
           "no quotation marks, and no repetition of these instructions." +
           ITranslator::glossary_constraint(ITranslator::glossary_for_text(glossary_, text));
}

void HunyuanTranslator::debug_dump_prompt(const std::string& text) const {
    // 用翻译任务的 system prompt 来展示，与实际运行路径一致
    const std::string sys = translate_system_prompt(text);
    const std::string prompt = build_prompt(sys, text);

    // 把全角竖线 + SentencePiece 空格符换成肉眼可读的形式
    std::string vis = prompt;
    auto replace_all = [](std::string& s, const std::string& from, const std::string& to) {
        size_t p = 0;
        while ((p = s.find(from, p)) != std::string::npos) {
            s.replace(p, from.size(), to);
            p += to.size();
        }
    };
    replace_all(vis, u8"<｜", u8"《");
    replace_all(vis, u8"｜>", u8"》");
    replace_all(vis, u8"▁",   u8"·");

    std::cout << "===== 实际送给模型的 Prompt =====\n" << vis << "\n" << std::endl;

    const llama_vocab* vocab = llama_model_get_vocab(model);
    std::vector<llama_token> toks(1024);
    int n = llama_tokenize(vocab, prompt.c_str(), (int32_t)prompt.size(),
                           toks.data(), (int32_t)toks.size(),
                           /*add_special=*/false, /*parse_special=*/true);
    if (n < 0) {
        toks.resize(static_cast<size_t>(-n));
        n = llama_tokenize(vocab, prompt.c_str(), (int32_t)prompt.size(),
                           toks.data(), (int32_t)toks.size(),
                           /*add_special=*/false, /*parse_special=*/true);
    }

    std::cout << "===== Tokenize 结果 =====\n";
    std::cout << "token 总数: " << n << "\n" << std::endl;

    const int show = n < 30 ? n : 30;
    for (int i = 0; i < show; ++i) {
        char buf[512];
        const int len = llama_token_to_piece(vocab, toks[i], buf, sizeof(buf), 0, true);
        std::string piece = (len > 0) ? std::string(buf, static_cast<size_t>(len)) : std::string();
        for (size_t k = 0; k < piece.size(); ++k) {
            if (piece.compare(k, 3, u8"▁") == 0) { piece.replace(k, 3, u8"·"); }
        }
        std::cout << "  [" << std::setw(3) << i << "] id=" << std::setw(6) << toks[i]
                  << "  '" << piece << "'" << std::endl;
    }
    if (n > show) std::cout << "  ... 中间 " << (n - show) << " 个 token 略" << std::endl;

    // 尾部 token 同样重要：<｜hy_Assistant｜> 必须是单个 token，
    // 否则模型看不到"轮到自己说话"的边界，就可能把 prompt 原样续写出来。
    if (n > show) {
        std::cout << "\n  ---- 最后 8 个 token ----" << std::endl;
        const int from = (n > 8) ? (n - 8) : 0;
        for (int i = from; i < n; ++i) {
            char buf[512];
            const int len = llama_token_to_piece(vocab, toks[i], buf, sizeof(buf), 0, true);
            std::string piece = (len > 0) ? std::string(buf, static_cast<size_t>(len)) : std::string();
            size_t k = 0;
            while ((k = piece.find(u8"▁", k)) != std::string::npos) {
                piece.replace(k, 3, u8"·");
                k += 2;
            }
            std::cout << "  [" << std::setw(3) << i << "] id=" << std::setw(6) << toks[i]
                      << "  '" << piece << "'" << std::endl;
        }
    }
}

std::string HunyuanTranslator::target_name() const {
    if (target_lang_ == "zh")  return "Simplified Chinese";
    if (target_lang_ == "en")  return "English";
    if (target_lang_ == "ja")  return "Japanese";
    if (target_lang_ == "ko")  return "Korean";
    if (target_lang_ == "fr")  return "French";
    if (target_lang_ == "de")  return "German";
    if (target_lang_ == "es")  return "Spanish";
    if (target_lang_ == "ru")  return "Russian";
    return target_lang_;
}

std::string HunyuanTranslator::build_prompt(const std::string& system,
                                            const std::string& user) const {
    const char* tmpl_raw = llama_model_chat_template(model, nullptr);
    const std::string tmpl = tmpl_raw ? tmpl_raw : "";

    // ---------------- 混元自家模板 ----------------
    // 本模型（HY-MT1.5-1.8B）GGUF 内嵌的模板是：
    //   <｜hy_begin▁of▁sentence｜>{system}<｜hy_place▁holder▁no▁3｜>
    //   <｜hy_User｜>{user}<｜hy_Assistant｜>
    // 注意：'｜' 是全角竖线 U+FF5C，'▁' 是 SentencePiece 空格符 U+2581。
    //
    // 这里不用 llama_chat_apply_template()，因为 llama.cpp 的模板嗅探
    // 见到 "hy_Assistant" + "hy_begin_of_sentence" 会先判成 HUNYUAN_OCR，
    // 而本模型实际是 DENSE 形态。按内嵌模板自己渲染最可靠。
    if (tmpl.find(u8"<｜hy_User｜>") != std::string::npos) {
        return u8"<｜hy_begin▁of▁sentence｜>" + system +
               u8"<｜hy_place▁holder▁no▁3｜>" +
               u8"<｜hy_User｜>" + user +
               u8"<｜hy_Assistant｜>";
    }

    // ---------------- 其他模型：交给 llama.cpp ----------------
    if (tmpl_raw != nullptr) {
        llama_chat_message msgs[2] = {
            { "system", system.c_str() },
            { "user",   user.c_str()   },
        };
        std::vector<char> buf(2 * (system.size() + user.size()) + 1024, 0);
        int32_t n = llama_chat_apply_template(tmpl_raw, msgs, 2, true,
                                              buf.data(), (int32_t)buf.size());
        if (n > (int32_t)buf.size()) {
            buf.assign(static_cast<size_t>(n) + 1, 0);
            n = llama_chat_apply_template(tmpl_raw, msgs, 2, true,
                                          buf.data(), (int32_t)buf.size());
        }
        if (n > 0) return std::string(buf.data(), static_cast<size_t>(n));
    }

    // ---------------- 兜底：ChatML ----------------
    return "<|im_start|>system\n" + system + "<|im_end|>\n"
           "<|im_start|>user\n" + user + "<|im_end|>\n"
           "<|im_start|>assistant\n";
}

std::string HunyuanTranslator::sanitize_output(std::string s) {
    // 模型偶尔会把 prompt 的开头原样回显出来，这里做兜底剥离。
    // 即使 chat template 修好了，保留这一层也更稳。
    static const std::vector<std::string> kEcho = {
        u8"将以下文本翻译为中文：", u8"将以下文本翻译为：", u8"将以下文本翻译",
        u8"翻译：", u8"译文：",
        "Translate the following text into Chinese:",
        "Translate the following text into:",
        "Translate the following:", "Translation:",
    };

    for (int pass = 0; pass < 4; ++pass) {
        const size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return {};
        s.erase(0, b);
        const size_t e = s.find_last_not_of(" \t\r\n");
        s.erase(e + 1);

        bool stripped = false;
        for (const auto& p : kEcho) {
            if (s.compare(0, p.size(), p) == 0) {
                s.erase(0, p.size());
                stripped = true;
            }
        }
        if (!stripped) break;
    }

    // 去掉首尾成对的引号
    if (s.size() >= 2 &&
        ((s.front() == '"'  && s.back() == '"') ||
         (s.front() == '\'' && s.back() == '\''))) {
        s = s.substr(1, s.size() - 2);
    }
    return s;
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
    // 1024 太小：摘要任务要同时装下系统提示 + 整场转录 + 几百 token 的输出。
    // 模型本身支持 262144，4096 对 1.8B 来说 KV cache 开销很小。
    ctx_params.n_ctx = 4096;
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



void HunyuanTranslator::push_text(const std::string& text, double confidence) {
	if (text.empty()) {                       // 空文本直接跳过
		return;
	}
	{
		std::lock_guard<std::mutex> lock(queue_mutex_);
		if(text_queue_.size() >= MAX_QUEUE_SIZE) {
			std::cerr << "[警告] 翻译队列已满(" << MAX_QUEUE_SIZE<< ")，丢弃最旧文本: " << text_queue_.front().text << std::endl;
			text_queue_.pop();
		}
		text_queue_.push(TranslationRequest{text, confidence});
	}
	cv_.notify_one();
}

long long HunyuanTranslator::get_last_api_ms() {
    return last_api_ms_.load();
}


/////////////////////////////////////////////////////////////


bool HunyuanTranslator::translate_once(const std::string& text, std::string& out) {
    const std::string sys = translate_system_prompt(text);
    std::string raw;
    if (!generate_once(sys, text, raw, /*max_new=*/128)) return false;
    out = sanitize_output(std::move(raw));   // 剥掉可能被回显的 prompt 片段
    return true;
}


bool HunyuanTranslator::generate_once(const std::string& system, const std::string& user,
                                      std::string& out, int max_new) {
    const llama_vocab* vocab = llama_model_get_vocab(model);

    llama_sampler* smpl = llama_sampler_init_greedy();
    if (smpl == nullptr) {
        std::cerr << "[Error] 采样器创建失败" << std::endl;
        return false;
    }

    // 无论从哪条路径返回，都要清 KV cache 并释放采样器
    auto cleanup = [&]() {
        llama_memory_t mem = llama_get_memory(ctx);
        if (mem != nullptr) llama_memory_clear(mem, true);
        llama_sampler_free(smpl);
    };

    auto t1 = std::chrono::steady_clock::now();
    int n_past = 0;

    // 构造 prompt：交给模型自带的 chat template
    const std::string prompt = build_prompt(system, user);

    // tokenize
    // 关键：parse_special 必须为 true，否则 <｜hy_User｜> 这类特殊 token
    // 不会被识别，而是被当成普通文本切成碎片，模型就看不到角色边界了。
    // add_special 用 false：prompt 已经显式带了 <｜hy_begin▁of▁sentence｜>（BOS），
    // 再自动加一次会出现重复 BOS。
    std::vector<llama_token> tokens(1024);
    int n_in = llama_tokenize(vocab, prompt.c_str(), (int32_t)prompt.size(),
                              tokens.data(), (int32_t)tokens.size(),
                              /*add_special=*/false, /*parse_special=*/true);
    if (n_in < 0) {                       // 负值 = 需要的容量
        tokens.resize(static_cast<size_t>(-n_in));
        n_in = llama_tokenize(vocab, prompt.c_str(), (int32_t)prompt.size(),
                              tokens.data(), (int32_t)tokens.size(),
                              /*add_special=*/false, /*parse_special=*/true);
    }
    if (n_in <= 0) {
        std::cerr << "[Error] tokenize 失败 (n_in=" << n_in << ")" << std::endl;
        cleanup();
        return false;
    }
    tokens.resize(static_cast<size_t>(n_in));

    // 填充 prompt batch
    llama_batch batch = llama_batch_init(n_in, 0, 1);
    for (int i = 0; i < n_in; i++) {
        batch.token[i]     = tokens[i];
        batch.pos[i]       = i;
        batch.n_seq_id[i]  = 1;
        batch.seq_id[i][0] = 0;
        batch.logits[i]    = false;        // prompt 中间不做预测，省算力
    }
    batch.logits[n_in - 1] = true;         // 只有最后一个 token 需要输出概率
    batch.n_tokens = n_in;

    if (llama_decode(ctx, batch) != 0) {
        std::cerr << "[Error] prompt decode 失败" << std::endl;
        llama_batch_free(batch);
        cleanup();
        return false;
    }
    llama_batch_free(batch);
    n_past += n_in;

    // 逐 token 生成
    const llama_token eos = llama_token_eos(vocab);
    std::string output;

    llama_batch one = llama_batch_init(1, 0, 1);
    for (int i = 0; i < max_new; ++i) {
        const llama_token next = llama_sampler_sample(smpl, ctx, -1);
        if (next == eos) break;

        char buf[256];
        const int len = llama_token_to_piece(vocab, next, buf, sizeof(buf), 0, false);
        if (len > 0) output.append(buf, static_cast<size_t>(len));

        one.token[0]     = next;
        one.pos[0]       = n_past;
        one.n_seq_id[0]  = 1;
        one.seq_id[0][0] = 0;
        one.logits[0]    = true;
        one.n_tokens     = 1;

        if (llama_decode(ctx, one) != 0) {
            std::cerr << "[错误] llama_decode 失败" << std::endl;
            break;
        }
        ++n_past;
    }
    llama_batch_free(one);

    auto t2 = std::chrono::steady_clock::now();
    last_api_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();

    const size_t pos = output.find("<|im_end");
    if (pos != std::string::npos) output.erase(pos);

    // 这里只做最基础的截断，不做 prompt 回显清洗——
    // 清洗放在 translate_once 里做，这样摘要等其它调用方能看到模型原始输出，
    // 便于排查"模型到底吐了什么"。
    out = std::move(output);
    cleanup();
    return true;
}


void HunyuanTranslator::translation_worker() {
    while(running_) {
        TranslationRequest req;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            cv_.wait(lock,[this] {return !text_queue_.empty() || !running_;});
            if(!running_ && text_queue_.empty()) break;
            req = text_queue_.front();
            text_queue_.pop();
        }

        std::string output;
        if (!translate_once(req.text, output)) continue;

        std::cout << "\n[混元翻译] " << output
                  << " | 耗时:" << last_api_ms_.load() << "ms\n" << std::endl;
        // 把识别置信度一并落库，供后续难度路由与交付物质量评估使用
        SessionStore::instance().log_segment(req.text, output, name(),
                                             last_api_ms_.load(), req.confidence);

        // 发布最新译文与它对应的原文，供悬浮窗做同步显示
        {
            std::lock_guard<std::mutex> lock(result_mutex_);
            last_translation_ = output;
            last_source_      = req.text;
        }
        translation_count_.fetch_add(1);
    }
}
