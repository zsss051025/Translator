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
    // 用户是否显式给了 --out。和 db_path_explicit 同一个用途，见下面助手的说明。
    bool        out_explicit = false;
    bool        demo_session = false; // --demo-session：写入一场演示会话后退出（用于验证导出）
    bool        test_window  = false; // --test-window：只弹悬浮字幕窗看外观，不加载模型/不采集音频

    // --verbose：保留 llama.cpp / whisper.cpp 的**全部**加载日志。
    //
    // 默认它们是**关掉**的（只留警告和错误）。实测一次真实启动会打几百行
    // control token / tensor / KV cache，把
    //     >>> 已开始记录 <<<
    // 那一行埋在中间 —— 用户不知道自己该什么时候开始放视频，
    // 猜错几秒就意味着那几秒的音频没被录到。
    // 排查"模型加载失败 / 显存不够"时才需要打开它。
    bool        verbose      = false;

    // --report "<任务>"：把一件事派给 agent（§7 步骤 5.6）。
    // 它自己决定调哪些工具、调几次，给出**带出处**的结论；
    // 资料不足时必须说"查不到"，绝不编报告。
    // 需要 DeepSeek API Key —— 本地翻译模型不会 function calling。
    std::string report_goal;

    // --search "<关键词>"：单次检索长期记忆（§7 步骤 5.3）。
    //
    // ⚠️ **刻意不叫 `--ask`**：那个名字已经被"会话结束确认交互的开关"占用
    // （`--ask` / `--no-ask`，见下面的 AskMode）。计划文档里写的是"5.3 `--ask` 检索"，
    // 落地时必须改名 —— 否则两个语义撞在同一个参数上，谁先生效取决于解析顺序。
    //
    // 它是 agent 的**退化情形**：一次工具调用、零规划、不联网、不需要 Key。
    std::string search_query;

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

    // --lang-switch-confirm N：语言锁定/切换要**连续 N 段判断一致**才认（默认 3）。
    //
    // 【为什么需要】原来一次判断就切换。真实会话（live.db #2，英文课）里，
    // 120 秒重检那一拍正好落在音乐/转场段上，Whisper 对那 3~4 秒判成了中文
    // → 吐出「春日的留书,你在路上做一回。」→ **锁定语言被切换成 zh**。
    // 机制与连带风险见 inc/LangPolicy.h。
    //
    // 1 = 退回旧行为（一次就切），排查时可以用。
    int         lang_switch_confirm = 3;

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

    // --triage-cache                  看判断缓存里存了什么
    // --triage-cache --forget <词>     忘掉一个词（判决错了的手动出口）
    // --triage-cache --clear          清空
    //
    // 【为什么必须有个"看"的入口】判断缓存的全部价值是"下次不再联网问同一个词"，
    // 而它一旦判断错了，症状是**这个词从此再也不被问** —— 用户看不到任何报错，
    // 只会觉得"系统怎么不学这个"。一个会静默改变行为的缓存，必须能被看见和撤销，
    // 否则它就是个隐患而不是优化。
    bool        triage_cache = false;
    std::string triage_forget;   // 非空 = 忘掉这一个词
    bool        triage_clear = false;

    // --actions                    列出行动项（跨会话聚合后的总表）
    // --actions --status todo      只看某一档
    // --actions --done <id>        标记完成（--doing / --todo 同理）
    //
    // 【为什么状态流转是命令行而不是 agent 工具】见 ActionStore.h 的说明：
    // "做完了吗"是**只有用户知道的事实**。模型从一句话推断出 done，
    // 代价是那条任务**静默从待办列表里消失** —— 用户看不见它消失。
    bool        show_actions = false;
    std::string actions_status;       // 过滤（todo/doing/done，空 = 全部）
    long long   action_set_id = -1;   // 要改状态的 id
    std::string action_set_to;        // 改成什么

    // --verify-report <文件>：核对一份报告里每处出处的真伪（5.7）。
    // 非空 = 执行这条命令并退出。
    std::string verify_report;

    // ---- --fix：改正一条知识（用户手改的唯一入口）----
    //
    //   --fix                    列出全部知识（找到要改的那条）
    //   --fix <词>               查看这一条（含变化历史）
    //   --fix <词> --value X     改正写法
    //   --fix <词> --as-kind K   改正类型（term/person/project/product/fact/decision）
    //   --fix <词> --define D    改正含义
    //   --fix <词> --archive     归档（**不删** —— §6.5 只降级，删了就不可复查）
    //
    // 【为什么必须有它】在这之前，用户发现某条知识是错的**没有任何直接办法**：
    // 只能靠重跑抽取（碰运气）或清判断缓存（那是另一个东西）。
    // **一个改不了的记忆是产品风险** —— 它会一直出现在识别提示/翻译约束/
    // 摘要背景里，而用户只能看着。
    bool        show_fix = false;
    std::string fix_word;        // 空 = 列出全部
    std::string fix_value;    std::string fix_kind;
    std::string fix_define;
    bool        fix_archive = false;

    // ---- 助手（§7 第 4 阶段 4.1/4.2）----
    //
    //   --assistants            列出所有助手
    //   --new-assistant <名>    新建一个助手（建目录 + 元信息）
    //   --assistant <名>        用这个助手（库路径/输出目录由它推出来）
    //   --remove-assistant <名> 移除一个助手（**软删除**：目录改名，不真删）
    //   --set-key <key>         把 API key 用 DPAPI 加密后存进 config.json
    //
    // ⚠️ **显式给的 --db / --out 永远优先于助手**。
    //    整个验证阶梯（--selftest / --wav / --gaps / 那些 python harness）
    //    全靠显式路径指向临时库；助手若把它们覆盖掉，那套保障会**静默失效**。
    bool        list_assistants = false;
    std::string new_assistant;
    std::string use_assistant;
    std::string remove_assistant;
    std::string set_key;
    // 解析出来的助手 slug（空 = 没用助手）。给日志用，让"这次动的是哪个助手"
    // 在启动横幅里能看见。
    std::string assistant_slug;

    // --glossary <文件>：术语表（每行一个词/短语，# 为注释）。
    // 内容会作为 initial_prompt 喂给 Whisper，让专有名词识别更稳定。
    std::string glossary_path;

    // --asr-prompt-kb N：让**知识库**的词条也进识别提示，最多 N 个（默认 0 = 不进）。    //
    // 【为什么默认是 0 —— 这是有实测证据的，不是保守起见】
    // A/B 实测（`tools/asr_prompt_ab.py`，每档重复 3 次且结果完全一致）：
    // 给 Whisper 喂**和音频内容无关**的词，它会**漏字**、会**幻觉**，而且越多越糟：
    //     N=0  无提示            → …can do for you. **ask what you** can do for yourself…
    //     N=5  真实库的 4 个词    → …can do for you. can do for yourself…   ← 吞掉 "ask what you"
    //     N=4  4 个编造词        → …Thank you **for watching!**             ← 幻觉
    //     N=40 40 个词（产品上限）→ 整句 "Thank you" 不见了
    // 而知识库是**跨会议全局累积**的，"和本次内容无关"恰恰是常态
    // （上周学的播客人名，这周开项目会照样会被喂进去）。
    // 另一头，"有用"那一半**至今没有证据**（需要一段含易错专名的音频才能验）。
    // 所以默认值取"不拿没被证明的收益去换已被证明的代价"。
    //
    // 收益那一半有**确定性**的落点，且不受这个开关影响：
    // `TermFixer`（识别后按词表纠错，不会幻觉）、② 翻译约束、③ 摘要背景
    // —— 它们照旧吃**全量**知识库。
    int asr_prompt_kb = 0;

    // ---- 分句（句尾判定）参数 ----
    //
    // --endpoint-ms N     停多久算"一句话说完了"（默认 500ms）
    // --max-utter-sec X   连续说话的安全阀，超过就切（默认 4.0 秒）
    //
    // 【为什么做成开关】它们**该在真实会议录音上校**，而不是由我在这里拍板：
    //   · 定得太低 → 说话人句中的自然换气被当成句尾，又回到碎片化
    //   · 定得太高 → 等于回到旧的"按固定时长切"
    //
    // 【默认值的来由 —— jfk.wav 上的参数扫描，四个组合逐个跑】
    //     endpoint 300 / max 6  → 1 段，丢了开头 "Ask not"
    //     endpoint 700 / max 6  → 2 段，丢开头，尾巴变成 "Ah!"
    //     endpoint 500 / max 4  → 2 段，**两半都完整**，正好切在自然停顿处  ← 取这个
    //     endpoint 900 / max 10 → 1 段，**只剩 "can do for your country."**，内容几乎丢光
    //
    // ⚠️ 最后一行的教训值得记：**窗口不是越长越好。**
    //    10 秒窗口那一档的输出大概被幻觉/低置信过滤器**整段丢掉**了 ——
    //    也就是说"把窗口开大以看到完整句子"这个直觉在 Whisper 上会反噬。
    //    所以安全阀只从原来的 3.0 提到 4.0，**刻意保守**。
    int    endpoint_ms    = 500;
    double max_utter_sec  = 4.0;

    // --context N：给翻译带上最近 N 段的**原文**作为前文（**默认 0 = 关闭**）。
    //
    // 【为什么需要它】用户的原话：
    //   「感觉翻译没有带着上下文一起理解，只是一句一句的翻译，
    //     很多时候被切碎的语句翻译出来就四不像了」
    // 接口上原来根本没有上下文的位置（`push_text(text, confidence)`），
    // 每段都是孤立翻译。
    //
    // =====================================================================
    // ⚠️ 实测：**本地小模型上它是有害的，所以默认关**
    // =====================================================================
    // 给 HY-MT1.5-1.8B 加上前文之后，它会把**前文**翻译出来，而不是待译的那句：
    //
    //   --dump-prompt "can do for yourself."
    //     不带前文 → 自己可以做到。
    //     带前文   → 不要问你的国家能为你做些什么，      ← 把前文译了一遍
    //
    // 我在这块 prompt 里**明确写了** "do NOT translate them, do NOT repeat them"
    // —— 1.8B 模型不遵守。这和之前 initial_prompt 的教训是同一个形状：
    // **弱模型对"多出来的上下文"的处理方式是把它们也当成内容。**
    //
    // 所以默认值和那边的决定一致：**收益未证实 + 代价已证实 → 默认关**。
    // 想用就显式开；云端模型（DeepSeek）大概率能正确处理，但**我还没验**，
    // 所以不替它做默认决定。
    //
    // ⚠️ 注意：`--dump-prompt` **只能验本地混元** —— 那个函数里直接构造了
    //    `HunyuanTranslator`，不看 `--translator`。我第一次"验云端"就被这个骗了：
    //    加 `--translator cloud` 跑出来耗时 177ms（本地量级），而输出和本地一模一样。
    //    要验云端得单独写 harness（走 `--wav` 或真跑一场，带 key）。
    int context_lines = 0;

    // --context-lines "A|B"：给 `--dump-prompt` 直接指定前文内容（`|` 分隔）。
    // 只用于**对照实验**：同一句残句，带前文 / 不带前文，看译文差多少。
    std::string context_lines_text;


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
