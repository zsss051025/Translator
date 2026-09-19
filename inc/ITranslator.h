#pragma once
#include <mutex>
#include <string>
#include <vector>

// 一条待翻译的请求。
// confidence 是上游语音识别的置信度，会一路带到数据库，
// 供后续"难度自适应路由"和交付物质量评估使用；
// <0 表示上游没有提供这个值。
struct TranslationRequest {
    std::string text;
    double      confidence = -1.0;
};

// 翻译后端统一接口。
// 生命周期的约定：构造 -> init 阶段由派生类自己的工厂/构造完成 ->
// start() 启动工作线程 -> push_text() 投递 -> stop() 收尾。
// start() 必须是幂等的：重复调用不应启动第二个线程。
class ITranslator{
public:
    virtual ~ITranslator() = default;

    virtual void start() = 0;                              // 启动后台翻译线程
    virtual void push_text(const std::string& text, double confidence) = 0;   // 投递待翻译文本
    virtual void stop() = 0;                               // 停止并回收线程
    virtual long long get_last_api_ms() = 0;               // 上次翻译耗时(ms)
    virtual const char* name() const = 0;                  // 后端名，用于日志与界面展示

    // 目标语言（译文语言），如 "zh" / "en" / "ja"。
    // 放在接口上是因为本地与云端两条后端都要用它来构造 prompt。
    virtual void set_target_language(const std::string& lang) = 0;

    // 最新一条完成的译文，供悬浮字幕窗显示。
    // 用计数器判断"是否有新译文"，避免比较文本（两段译文相同时会漏判）。
    virtual std::string get_last_translation() const = 0;
    virtual int get_translation_count() const = 0;

    // 上一条译文**对应的原文**。
    //
    // 为什么必须要有它：识别和翻译是两条独立异步流，翻译总滞后一句。
    // 如果窗口直接把「最新原文」和「最新译文」拼在一起，就会出现
    // 「英文第 N 句 + 中文第 N-1 句」的错配。
    // 调用方拿到译文后要先比对来源，一致才更新，否则显示"翻译中…"。
    virtual std::string get_last_source() const = 0;

    // ---- 术语约束（让译文里的专名保持同一写法）----
    //
    // 【为什么需要】实测会话 #11：同一个 `Erica`，第 1 句保留成 "Erica"、
    // 第 2 句被音译成「埃里卡」——**同一场会话内自相矛盾**。
    // 原因是术语表原来只有两条腿在工作：喂 Whisper 的 initial_prompt、
    // 以及 TermFixer 的识别后纠错；**对译文输出零约束**。
    //
    // 【为什么放在接口上】三个地方各自构造翻译用的 system prompt
    // （混元 translate_once、混元 debug_dump_prompt、云端 worker），
    // 约束文案如果各写一份，漏改一处就会出现"本地一致、云端不一致"。
    void set_glossary(const std::vector<std::string>& terms);

    // 术语约束文本（纯函数，便于自检）。
    // 没有术语时返回空串，调用方直接拼在后面即可。
    static std::string glossary_constraint(const std::vector<std::string>& terms);

    // **只保留这段原文里真的出现过的术语**（纯函数，便于自检）。
    //
    // 【为什么必须有这一步 —— 实测事故】
    // 约束原来对**每一段**都生效，包括那些根本没提到该术语的片段。
    // 对照实验（同一段原文，只改知识库）：
    //
    //   原文 `and wife get ready to go`（真值里没有任何专名）
    //     无约束 → 「妻子也准备出发了」                    ✅ 忠实
    //     有约束 → 「埃丽卡和马可准备出发了」              ❌ 凭空编了两个名字
    //   原文 `Marco, I'm doing really well. How about you?`
    //     无约束 → 「Marco，我过得很好。你呢？」            ✅
    //     有约束 → 「Erica, I'm doing really well. …」      ❌ 整句没翻译，名字还被换掉
    //
    // 【原因】约束的话是"这几个词必须原样出现"。弱翻译模型（HY-MT 1.8B）
    // 看到这句话、又发现本段里没有那些词，就会**给它补上**；
    // 或者干脆退化成"照抄原文 + 换个名字"。
    //
    // 【判断】只在该术语真的出现在本段时才约束它。这既保住了约束的用途
    // （那段里确实有 Erica，就该让它别音译），又不会让它去无中生有。
    //
    // 匹配是**大小写不敏感的子串匹配**，故意放宽：
    // 把本段里其实有的术语漏掉，会丢掉整条修复；而多带上一个无关术语，
    // 只要它在段里出现过就不算"无中生有"。
    static std::vector<std::string> glossary_for_text(const std::vector<std::string>& terms,
                                                      const std::string& text);

protected:
    // 由 set_glossary() 写入，在 start() 之前设置完成，因此不需要加锁
    std::vector<std::string> glossary_;

    // =========================================================================
    // 上下文（2026-09-19 新增）
    // =========================================================================
    //
    // 【用户的原话】「感觉翻译没有带着上下文一起理解，只是一句一句的翻译，
    //   很多时候被切碎的语句翻译出来就四不像了」
    //
    // 他说得对，而且接口上就看得出来：`push_text(text, confidence)` ——
    // **上下文根本没有传进来的地方**。每段都是一次孤立翻译，于是：
    //   · 代词（他 / 它 / 这个）找不到指代对象
    //   · 同一件事第一句叫「接口文档」、第三句变成「那个文件」
    //   · 专名写法在段与段之间漂移
    //   · 被切断的残句更无从判断该补什么
    //
    // 这里加一层**只读前文**：把最近几段的**原文**带给模型，
    // 明确告诉它"这些是上文，供你理解，**不要翻译它们**"。
    //
    // 三个设计都是为了"别帮倒忙"：
    //   ① **前文只进 system，不进 user**。user 里只有待译文本 ——
    //      否则模型分不清哪句要译、哪句是背景，容易把前文也译一遍。
    //   ② **前文用原文，不用译文**。用译文会累积误差：
    //      上一段译错了，这一段的判断会被它带偏。
    //   ③ **段数可配、可关**（默认 2，`--context 0` 关闭）。小模型对长 prompt
    //      很敏感，本项目的实测里 prompt 一长它就开始续写/回显，
    //      所以这个量必须能调、也必须能关。
    //
    // ⚠️ 前文按**翻译顺序**滚动：worker 每译完一段就把那段原文推进去。
    int                      context_lines_ = 2;
    mutable std::mutex       context_mutex_;
    std::vector<std::string> context_;

public:
    // 设置上下文段数（0 = 关闭）。在 start() 之前调用。
    void set_context_lines(int n);

    // 把一段**已翻译完**的原文推进滚动上下文（由各后端的 worker 调用）。
    //
    // ⚠️ 必须在**译完**之后推：前文要的是"这一段之前说过什么"，
    // 把当前这段也推进去会让模型看到两遍。
    void note_source(const std::string& src);

    // 给模型看的前文块。**没有前文时返回空串**（调用方直接拼即可）。
    //
    // 【为什么做成共享函数】本地和云端两个后端都要拼这一块，
    // 各写一份必然走散 —— 本项目"两份实现迟早走散"已经栽过五次。
    std::string context_block() const;

    // ---- 输入清洗与回显兜底（2026-09-19 新增）----
    //
    // =====================================================================
    // 【真实事故】用户真跑一部电影，其中一段的译文是「我是一个专业的翻译人员。」
    // =====================================================================
    // 原文是 `and I'm`。一开始我判断成"小模型替残句补全了" —— **那个判断是错的**。
    // 真正的原因是：那句话是 **system prompt 的第一句**
    // （`You are a professional translator. Translate the user's message into …`）
    // 被模型**当成待译内容翻译了一遍**。也就是**提示回显**，
    // 这个项目在 ASR 侧已经遇到过（术语提示被续写），现在在翻译侧也出现了。
    //
    // 触发器定位到了，而且是可复现的：
    //     --dump-prompt "and I'm"    → 而我也是          ✅
    //     --dump-prompt " and I'm"   → 我是一个专业的翻译人员。  ❌（前导空格！）
    // 而 **Whisper 的输出几乎总带前导空格**（实测那一场 126 段里 35 段有，28%）。
    // 所以清洗输入不是洁癖，它直接消掉了一个能产出伪造内容的触发器。
    //
    // 两道防线，缺一不可：
    //   ① `clean_source()` —— 去掉首尾空白（**消除触发器**）
    //   ② `looks_like_prompt_echo()` —— 万一还是回显了，**认出它**。
    //      认出之后的处理是**回退成原文**，而不是删掉这一段：
    //      显示一句看不懂的英文，比显示一句看着很通顺的伪造中文**安全得多**
    //      —— 前者用户知道"这句没译出来"，后者他会当真。

    // 去掉首尾空白（含全角空格）。两个后端都必须过这一步。
    static std::string clean_source(const std::string& text);

    // 这段译文是不是在"翻译/复述我们的指令"？（纯函数，便于自检）
    //
    // 【为什么不能用"完全相等"来判】模型不会一字不差地复述，
    // 它会**翻译**指令（`You are a professional translator` →
    // 「我是一个专业的翻译人员」），也可能只译半句。所以只能按特征词判。
    static bool looks_like_prompt_echo(const std::string& output);
};
