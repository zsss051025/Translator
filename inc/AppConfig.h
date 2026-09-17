#pragma once
#include <string>

// 全局运行配置。
// 解析优先级：命令行参数 > 环境变量 > 自动探测 > 内置默认值。
// 这样现场演示时不必依赖某个固定的工作目录（原实现硬编码 "../../../models/"，
// 换个 cwd 就找不到模型）。
struct AppConfig {
    std::string whisper_model;      // Whisper GGML 模型路径
    std::string hunyuan_model;      // 混元 GGUF 模型路径
    std::string db_path;            // 翻译记录 SQLite 路径
    bool        db_path_explicit = false;  // 用户是否显式给了 --db（见下面 run_selftest 的用法）
    std::string deepseek_api_key;   // 可为空，选择云端后端时才要求非空
    bool        list_only = false;  // --help / --list：只打印配置后退出
    bool        selftest  = false;  // --selftest：不加载模型，只验证数据层通路
    long long   export_session = -1; // --export <id>：把指定会话导出为交付物后退出
    std::string deliverable_dir = "deliverables";  // --out <目录>：交付物输出根目录
    bool        demo_session = false; // --demo-session：写入一场演示会话后退出（用于验证导出）
    bool        test_window  = false; // --test-window：只弹悬浮字幕窗看外观，不加载模型/不采集音频

    // --wav <文件>：不采集真实音频，改为回放一个 WAV 走完整管线。
    //
    // 它存在的理由不是"多一个输入源"，而是**让端到端可复现**：
    // 验证"术语约束有没有让译名一致"这类改动，必须**同一段音频开/关各跑一遍**做 A/B，
    // 而手动说话说不出两遍一样的音频。没有它，L3 只能靠人耳听，改动无从对比。
    //
    // 走的是与实时采集**完全相同**的主循环，只替换"音频从哪来"那几行（见 main.cpp）。
    std::string wav_path;

    // --gaps：只打印"知识缺口检测"的结果（该问用户哪些问题），不加载模型、不采集音频。
    //
    // 存在的理由：缺口检测是纯规则、有单测，但**单测用的是我构造的数据**。
    // 这个项目已经两次栽在"测试数据与真实数据形状不一致"上（§8.8⑨），
    // 所以必须留一条能对着**真实库**跑的路。
    bool        show_gaps = false;

    // --extract <会话id>：对一场**已存下来的**会话跑自动抽取并打印结果，**不写库**。
    //
    // 存在的理由：抽取器的自检用不了真实转录，而这个项目栽过两次
    // "测试数据与真实数据形状不一致"（§8.8⑨）。它让人能拿真会话验证抽取器认出了什么，
    // 而不用重新录一遍音频。刻意只读 —— 写库会让 hits 累加、把缺口排序搅乱。
    long long   extract_session = -1;

    // --apply：配合 --extract 用，真的写库并接着跑确认交互。
    // 不加就是**只读**的排查模式（反复跑不改变库状态）。
    bool        extract_apply = false;

    // ---- Agent 工具层（§7 步骤 5.1）----
    //
    // --tools                 只打印已注册的工具、description 和 JSON Schema
    // --tools <名字>           真跑一次那个工具
    // --tools <名字> --args '<json>'   带参数跑
    //
    // 【为什么必须有这个命令】Agent 出问题时，"模型不会用工具"和"工具本身坏了"
    // 在日志里长得一模一样（都表现为"最后没给出有用答案"）。这个命令把两者分开：
    // 打印 schema 能看出描述写歪了没有；跑一次能看出工具在真实数据上返回什么。
    // 而且**命令行和 Agent 走的是同一个 ToolRegistry::call** ——
    // 不给自己留第二条路径（这个项目因为"诊断工具和真实路径不一致"栽过三次）。
    bool        tools_list_only = false;
    std::string tools_name;
    std::string tools_args;

    // --ask / --no-ask：会话结束时的确认交互（§6.7 / §7 步骤 2.4）。
    //
    // 三态，而不是一个 bool：
    //   · 未指定 → **自动**：stdin 是终端、且不是 --wav 批处理 → 问；否则不问
    //   · --ask   → 强制问（--wav 下想手动试一遍交互时用）
    //   · --no-ask→ 强制不问
    //
    // 【为什么默认要"看情况"】产品承诺是"打开就听，结束就给总结"，
    // 所以真实使用时该问；但 --wav 是用来跑自动化验证的，在那里停下来等输入
    // 会让脚本永远挂住 —— 这类"验证时被交互卡死"的坑比问错问题更浪费时间。
    // 用三态而不是两态，是为了让"为什么这次没问"永远能从配置里读出来。
    enum class AskMode { Auto, Always, Never };
    AskMode ask_mode = AskMode::Auto;

    // --translator local|cloud：翻译后端，默认 local。
    //
    // 【为什么不看 --api-key 自动决定】有 key 不代表要用云端翻译——
    // 用户完全可能只让"摘要"上云，而翻译必须留在本机（"数据不出本机"是卖点）。
    // 所以云端翻译必须是**显式**选择，不能靠 key 的存在来推断。
    //
    // 这个字段取代了原来启动时的那道选择题：产品承诺是"打开就听"，
    // 不该让用户在开始记录之前先答一道题。
    std::string translator = "local";

    // --mic：同时采集麦克风。
    // 系统音频环回能听见"对方/视频在说什么"，但听不见"你和房间里在说什么"。
    // 面对面交谈、以及线上通话里你自己说的话，都要靠麦克风。
    bool        enable_mic = false;

    // ---- 语言策略 ----
    // source_lang: 源语言。"auto" = 首次自动检测后锁定（默认）；
    //              也可直接指定 "en"/"zh"/"ja"/"ko" 等，跳过检测。
    // target_lang: 目标语言（译文语言），默认中文。
    // lang_recheck_sec: auto 模式下多久重新检测一次源语言（应对中途换内容）。
    std::string source_lang = "auto";
    std::string target_lang = "zh";
    int         lang_recheck_sec = 120;

    // --dump-prompt <文本>：加载混元模型，打印实际 prompt 与 tokenize 结果后退出。
    // 用于验证特殊 token 是否被正确识别（不需要声卡）。
    std::string dump_prompt;

    // --terms：不加载任何模型，只打印"这次启动会把什么喂给识别和翻译"。
    //
    // 存在的理由：知识库里的东西到底有没有进提示词，是 §6.5 红线的**可观测面**。
    // 以前只能靠真跑一场会、在启动日志里找 `[识别提示]` 那一行来确认 ——
    // 要录音、要等 Whisper 加载 100 秒，验一次成本太高，于是没人验。
    // 这条命令几秒钟给同一个答案。
    bool dump_terms = false;

    // --glossary <文件>：术语表（每行一个词/短语，# 为注释）。
    // 内容会作为 initial_prompt 喂给 Whisper，让专有名词识别更稳定。
    std::string glossary_path;

    // --summarizer auto|rules|local|cloud：导出交付物时用哪种摘要方式。
    //   auto  —— 有 --llm-model 就用它本地生成；否则有 API Key 就用云端；都没有则用规则
    //   rules —— 纯规则抽取，完全离线（兜底路径）
    //   local —— 用 --llm-model 指定的本地模型生成（内容不出本机）
    //   cloud —— DeepSeek 生成（质量好，但转录会上云）
    // 任何一种失败都会自动回退到 rules，保证导出永不失败。
    std::string summarizer = "auto";

    // --llm-model <gguf>：做"理解"任务（摘要、行动项抽取）的本地模型。
    //
    // 重要：这里不能用 HY-MT 那个翻译模型。HY-MT1.5-1.8B 是翻译专用模型，
    // 实测它面对摘要指令会把转录原样回显，完全不执行任务。
    // 建议指向一个小型指令模型，例如 Qwen2.5-1.5B-Instruct 的 Q4_K_M GGUF（约 1GB）。
    std::string llm_model;

    // 从命令行与环境变量组装配置
    static AppConfig from(int argc, char** argv);

    // 打印最终生效的配置，便于现场排查路径问题
    void dump() const;

    // 配置是否足以启动（whisper 模型存在、云端后端所需的 key 已就绪）
    bool whisper_ready() const;
};
