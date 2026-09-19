#include "SpeechEngine.h"
#include "SpeechFilter.h"   // 归一化 + 幻觉黑名单 + 非内容判据（可单测）
#include "LangPolicy.h"     // 语言锁定/切换状态机（纯函数、可单测）
#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <chrono>

namespace {

// 幻觉判定阈值
// 说明：Whisper 对纯音乐/静音段会输出"谢谢观看""字幕由XX提供"这类固定幻觉。
// no_speech 是模型自己给出的"这段压根没人说话"的概率，比黑名单通用得多。
constexpr double kNoSpeechReject = 0.60;       // 超过则判为幻觉
constexpr double kMinConfidenceReject = 0.25;  // 平均 token 概率低于此值判为垃圾输出

// 跨段重复抑制参数。
// 窗口内同一句已出现 kRepeatReject 次以上就丢弃；取 2（第 3 次起丢弃）是保守值——
// 真实讲话里同一整句连说三遍很少见，而 Whisper 的幻觉循环动辄 5~6 次。
constexpr int    kRepeatReject = 2;
constexpr size_t kRepeatMinLen = 6;    // 规范化后至少这么长才做重复判定
constexpr size_t kRepeatMaxLen = 60;   // 太长的句子即使重复也更可能是真实内容

// 归一化 + 垃圾判据都搬去了 SpeechFilter（那边能单测）。
// 这里只留一个 using，让下面的调用点读起来不变。
using speechfilter::normalize_for_compare;

// 按空白切词
std::vector<std::string> split_words_ws(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

}  // namespace

SpeechEngine::SpeechEngine() {

}

SpeechEngine::~SpeechEngine() {
	stop();                                         // 即使不停止线程，也要防止析构时线程还在跑
	if (whisper_ctx) whisper_free(whisper_ctx);     // RAII,在对象销毁时释放推理资源
}


bool SpeechEngine::init(const std::string& model_path) {
	whisper_context_params params = whisper_context_default_params(); // 默认参数
	params.use_gpu = true;                    // 启用GPU加速，检测不到GPU自动退回CPU

	whisper_ctx = whisper_init_from_file_with_params(model_path.c_str(), params);
	return whisper_ctx != nullptr;

}

void SpeechEngine::set_language_policy(const std::string& source_lang, int recheck_sec) {
	std::lock_guard<std::mutex> lock(lang_mutex_);
	requested_lang_   = source_lang.empty() ? "auto" : source_lang;
	lang_recheck_sec_ = recheck_sec > 0 ? recheck_sec : 120;

	// 【用户指定了语言，那它就是当前源语言 —— 不能只是清空等检测】
	//
	// 【现象】`--lang en --target en`（源==目标，本该跳过翻译）实际走了翻译，
	// 而且**把内容改了**：实测 jfk.wav 的 `Ask not!` 被"译"成 `Do not ask!`，
	// `what your country...` 被改成 `What your country...`；落库 engine 是 Hunyuan
	// 而不是 passthrough。
	//
	// 【原因】三处串起来才看得出来：
	//   ① requested_lang_ != "auto" → should_detect_language() 永远返回 false
	//   ② update_detected_language() 只在 lang_arg == "auto" 时才被调用
	//   ⇒ current_lang_ **永远是空的** ⇒ get_language() 返回 ""
	//   ⇒ 主循环里那条"源语言 == 目标语言就跳过翻译"的守卫永远不触发。
	//
	// 【判断】指定了就是指定了 —— 直接把它当当前源语言。
	// auto 模式下仍然保持为空，等 whisper 检测（那条路本来就能正常写上）。
	current_lang_ = (requested_lang_ == "auto") ? std::string() : requested_lang_;
	last_detect_ = std::chrono::steady_clock::time_point{};
	pending_lang_.clear();
	pending_count_ = 0;
}

void SpeechEngine::set_language_switch_confirm(int n) {
	std::lock_guard<std::mutex> lock(lang_mutex_);
	lang_switch_confirm_ = n > 0 ? n : 1;
	pending_lang_.clear();
	pending_count_ = 0;
}

std::string SpeechEngine::get_language() const {
	std::lock_guard<std::mutex> lock(lang_mutex_);
	return current_lang_;
}

SpeechQuality SpeechEngine::get_last_quality() const {
	std::lock_guard<std::mutex> lock(quality_mutex_);
	return last_quality_;
}

void SpeechEngine::set_initial_prompt(const std::string& prompt) {
	initial_prompt_ = prompt;
}

// 识别结果是否只是把 initial_prompt 续写了出来。
//
// 实测：把术语表作为 initial_prompt 喂进去后，Whisper 在音频不清晰时
// 会把整张词表原样吐出来（"EnglishPod, Phoenix, SKU, Q4, roadmap"），
// 而且置信度高达 0.99 —— 靠置信度过滤完全抓不到，只能靠内容比对。
bool SpeechEngine::is_prompt_echo(const std::string& text) const {
	return looks_like_prompt_echo(text, initial_prompt_);
}

bool SpeechEngine::looks_like_prompt_echo(const std::string& text, const std::string& prompt) {
	if (prompt.empty() || text.empty()) return false;

	const std::string tn = normalize_for_compare(text);
	const std::string pn = normalize_for_compare(prompt);
	if (tn.size() < 4 || pn.empty()) return false;

	// ① 整串是提示词的子串 —— 最常见的形态（词表被原样吐出）
	if (pn.find(tn) != std::string::npos) return true;

	// ② 逐词命中率：正常讲话只会偶尔撞上一两个术语，
	//    而"续写提示词"会几乎全中。
	const auto tw = split_words_ws(text);
	if (tw.size() < 3) return false;

	size_t hit = 0, total = 0;
	for (const auto& w : tw) {
		const std::string wn = normalize_for_compare(w);
		if (wn.size() < 2) continue;          // 跳过 "a" "of" 这类
		++total;
		if (pn.find(wn) != std::string::npos) ++hit;
	}
	return total >= 3 && hit * 5 >= total * 4;   // 命中率 >= 80%
}

bool SpeechEngine::is_repetitive(const std::string& text) {
	std::lock_guard<std::mutex> lock(recent_mutex_);
	if (looks_repetitive(recent_outputs_, text)) return true;

	// 命中时**不**记入窗口，避免幻觉把窗口占满、把真实内容挤出去
	recent_outputs_.push_back(normalize_for_compare(text));
	while (recent_outputs_.size() > RECENT_WINDOW) {
		recent_outputs_.erase(recent_outputs_.begin());
	}
	return false;
}

bool SpeechEngine::looks_repetitive(const std::vector<std::string>& window,
                                    const std::string& text) {
	const std::string key = normalize_for_compare(text);
	if (key.size() < kRepeatMinLen || key.size() > kRepeatMaxLen) return false;

	const int seen = static_cast<int>(std::count(window.begin(), window.end(), key));
	return seen >= kRepeatReject;
}

bool SpeechEngine::should_detect_language() const {
	std::lock_guard<std::mutex> lock(lang_mutex_);
	if (requested_lang_ != "auto") return false;    // 用户指定了固定语言，永不检测
	if (current_lang_.empty())     return true;     // 还没锁定
	// 候选期间**每段都检测**，不用等下一个重检周期 ——
	// 否则"连续 N 段一致"会变成"连续 N 个 120 秒"（6 分钟才切换，太迟钝）。
	if (langpolicy::should_probe(pending_lang_)) return true;
	const auto now = std::chrono::steady_clock::now();
	return (now - last_detect_) > std::chrono::seconds(lang_recheck_sec_);
}

void SpeechEngine::update_detected_language() {
	const int id = whisper_full_lang_id(whisper_ctx);
	if (id < 0) return;
	const char* s = whisper_lang_str(id);
	if (s == nullptr) return;

	std::lock_guard<std::mutex> lock(lang_mutex_);
	const std::string detected = s;
	last_detect_ = std::chrono::steady_clock::now();

	// 状态机在 LangPolicy 里（纯函数、有单测）。这里只负责打日志。
	const auto act = langpolicy::decide(detected, current_lang_,
	                                    &pending_lang_, &pending_count_,
	                                    lang_switch_confirm_);

	if (act == langpolicy::Action::Lock) {
		// 【为什么首次锁定也要"连续 N 段一致"】开场往往是音乐/片头，
		// 那一段的语言判断同样不可靠 —— 这正是这套素材最容易踩的地方。
		std::cout << "\n[Lang] 源语言锁定: " << detected
		          << "（连续 " << lang_switch_confirm_ << " 段一致才锁）" << std::endl;
		current_lang_ = detected;
	} else if (act == langpolicy::Action::Switch) {
		// ⚠️ 这行值得看清楚：它意味着**源语言真的换了**。
		// 换错一次不只是脏一段 —— 若新语言恰好等于目标语言，
		// 主循环会把它当成"无需翻译"，**后面的翻译会静默停掉**。
		std::cout << "\n[Lang] 源语言切换: " << current_lang_ << " -> " << detected
		          << "（连续 " << lang_switch_confirm_ << " 段一致）" << std::endl;
		current_lang_ = detected;
	} else if (!pending_lang_.empty()) {
		// 有候选但还不够 —— 打出来，让"被音乐带偏但没切换"变成可观测的
		std::cout << "[Lang] 疑似语言变化 " << current_lang_ << " -> " << pending_lang_
		          << "（" << pending_count_ << "/" << lang_switch_confirm_
		          << " 段，暂不切换）" << std::endl;
	}
}

void SpeechEngine::start() {
	if (is_running_) return;//如果已经在运行，直接返回，避免重复启动线程
	is_running_ = true;
	worker_thread_ = std::thread(&SpeechEngine::run_inference_loop, this);
}

void SpeechEngine::stop() {
	is_running_ = false;

	cv_.notify_all();                               // 通知 worker_thread
	if (worker_thread_.joinable()) {                // 等待 worker 执行完再析构，避免提早释放资源
		worker_thread_.join();
	}

}

void SpeechEngine::push_audio(const std::vector<float>& data) {
	std::unique_lock<std::mutex> lock(queue_mutex_);

	// 防堆积
	if (audio_queue_.size() >= MAX_QUEUE_SIZE) {//背压机制：如果队列满了，丢弃最旧的音频段，保持系统响应性
		std::cerr << "[警告] 推理队列已满(" << MAX_QUEUE_SIZE << ")，丢弃最旧音频段" << std::endl;
		audio_queue_.pop();
	}

	audio_queue_.push(data);
	cv_.notify_one();                               // 唤醒后台线程去消费

}

size_t SpeechEngine::get_queue_depth() const {
	std::lock_guard<std::mutex> lock(queue_mutex_);
	return audio_queue_.size();
}

std::string SpeechEngine::get_last_text() {
		std::lock_guard<std::mutex> lock(text_mutex_);
		return last_text_;
}


void SpeechEngine::run_inference_loop() {
	while (is_running_) {
		std::vector<float> audio_to_process;
		{
			// 临界区：等待和取数据
			std::unique_lock<std::mutex> lock(queue_mutex_);
			cv_.wait(lock, [this] { return !audio_queue_.empty() || !is_running_; });

			if (!is_running_) break;

			audio_to_process = std::move(audio_queue_.front());
			audio_queue_.pop();
		}

		// ---- 语言策略 ----
		// 每段都让 whisper 自己检测语言是识别错误的常见来源：
		// 3 秒碎片上模型经常只有 20% 把握，判错一次整段就废了。
		// 所以默认"检测一次后锁定"，只在超时后才重新检测。
		const bool do_detect = should_detect_language();
		std::string lang_arg;
		{
			std::lock_guard<std::mutex> lock(lang_mutex_);
			if (requested_lang_ == "auto") {
				lang_arg = do_detect ? "auto" : current_lang_;
			} else {
				lang_arg = requested_lang_;
			}
		}
		if (lang_arg.empty()) lang_arg = "auto";

		// 执行模型推理
		whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
		wparams.n_threads = std::min(4, (int)std::thread::hardware_concurrency() / 2);
		wparams.language = lang_arg.c_str();
		wparams.detect_language = (lang_arg == "auto");  // 锁定后不再每段检测
		wparams.print_timestamps = false;
		wparams.strategy = WHISPER_SAMPLING_BEAM_SEARCH;
		wparams.beam_search.beam_size = 5;   // 2 偏小；GPU 有余量时 5 明显更准
		wparams.no_context = true;           // 实时模式参数
		wparams.offset_ms = 0;
		wparams.single_segment = false;
		wparams.suppress_blank = true;       // 抑制空白输出
		wparams.suppress_nst   = true;       // 抑制非语音 token
		wparams.no_speech_thold = 0.6f;      // Whisper 自带的幻觉抑制阈值
		// 术语提示：让识别偏向已知专有名词
		wparams.initial_prompt = initial_prompt_.empty() ? nullptr : initial_prompt_.c_str();


		auto t1 = std::chrono::steady_clock::now();
		int whisper_ret = whisper_full(whisper_ctx, wparams, audio_to_process.data(), (int)audio_to_process.size());
		auto t2 = std::chrono::steady_clock::now();

		if (whisper_ret == 0) {
			if (lang_arg == "auto") update_detected_language();

			const int n_segments = whisper_full_n_segments(whisper_ctx);
			//将获取到的多个 segment 拼接成一个字符串，作为最终结果返回
			std::string combined_text;
			for (int i = 0; i < n_segments; i++) {
				const char* segment_text = whisper_full_get_segment_text(whisper_ctx, i);
				if (segment_text && segment_text[0] != '\0') {
					if (!combined_text.empty()) {
						combined_text += " ";  // segment 之间加空格
					}
					combined_text += segment_text;
				}
			}

			// ---- 质量指标 ----
			// 置信度 = 所有输出 token 的平均概率。
			// 不额外消耗算力（whisper 本来就算了概率），
			// 后续直接作为"难度自适应路由"的输入。
			double sum_p = 0.0;  long long n_tok = 0;
			double sum_ns = 0.0; long long n_seg = 0;
			for (int i = 0; i < n_segments; ++i) {
				const int nt = whisper_full_n_tokens(whisper_ctx, i);
				for (int t = 0; t < nt; ++t) {
					sum_p += whisper_full_get_token_p(whisper_ctx, i, t);
					++n_tok;
				}
				sum_ns += whisper_full_get_segment_no_speech_prob(whisper_ctx, i);
				++n_seg;
			}

			SpeechQuality q;
			q.confidence = (n_tok > 0) ? (sum_p / static_cast<double>(n_tok)) : -1.0;
			q.no_speech  = (n_seg > 0) ? (sum_ns / static_cast<double>(n_seg)) : 0.0;

			// ---- 幻觉 / 非内容过滤 ----
			//
			// ⚠️ **顺序即判据强度**：把"不需要用到模型自报指标"的两条放在最前面。
			// 真实会话（2026-09-19）暴露了下面两条概率阈值会**系统性漏掉**
			// "自信的幻觉"（`¶¶` conf 0.70~0.79、`Sous-titrage…` conf 0.75~0.81），
			// 所以现在先看"这句话本身是不是内容"，再看模型自报的概率。
			if (!combined_text.empty()) {
				if (speechfilter::has_no_content(combined_text)) {
					// ` ¶¶` / ` ...` —— 一个字母/汉字都没有，不可能是内容。
					// 这类段以前**每一段都被送去翻译了一次**，再原样进交付物。
					q.rejected = true;
					q.reject_reason = "整段没有任何字母/汉字（符号或空白，疑似音乐/静音）";
				} else if (speechfilter::is_boilerplate_hallucination(combined_text)) {
					// Whisper 的套话幻觉。**置信度对这类完全无效**（见函数注释）。
					q.rejected = true;
					q.reject_reason = "命中 Whisper 套话幻觉黑名单（字幕组署名/平台推广语）";
				} else if (is_prompt_echo(combined_text)) {
					// 模型在"续写术语提示词"，不是识别音频
					q.rejected = true;
					q.reject_reason = "疑似续写 initial_prompt（术语提示被回显）";
				} else if (q.no_speech > kNoSpeechReject) {
					q.rejected = true;
					q.reject_reason = "no_speech_prob 过高（疑似音乐/静音段）";
				} else if (q.confidence >= 0.0 && q.confidence < kMinConfidenceReject) {
					q.rejected = true;
					q.reject_reason = "平均 token 概率过低（疑似幻觉）";
				} else if (is_repetitive(combined_text)) {
					// 跨段重复：Whisper 在音乐/噪声里锁死在同一短语上，
					// 这种幻觉置信度往往很高，只能靠"反复出现"来识别
					q.rejected = true;
					q.reject_reason = "同一句在近期重复出现（疑似幻觉循环）";
				}
			}

			{
				std::lock_guard<std::mutex> lock(quality_mutex_);
				last_quality_ = q;
			}

			if (q.rejected) {
				std::cerr << "[过滤] 丢弃疑似幻觉片段 (" << q.reject_reason
				          << ", conf=" << q.confidence
				          << ", no_speech=" << q.no_speech << ")" << std::endl;
			} else if (!combined_text.empty()) {
				last_inference_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
				std::lock_guard<std::mutex> lock(text_mutex_);
				last_text_ = std::move(combined_text);
				inference_count_++;
			}
		} else {
			std::cerr << "[警告] 推理异常，音频长度: " << audio_to_process.size() << " 采样点" << std::endl;
		}


	}
}

long long SpeechEngine::get_last_inference_ms() {
	return last_inference_ms_.load();
}

int SpeechEngine::get_inference_count() {
	return inference_count_.load();                 // .load() 原子读取
}
