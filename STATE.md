# STATE · 进度与工作方式

> **这份是滚动状态，每次收工必须更新。**
>
> - `PROJECT.md` = 稳定基准（架构 / 设计决定 / 规划 / 开发流程）——不常变
> - **`STATE.md`（本文件）= 现在到哪了、该怎么干活** ——每次干完就改
>
> **给"上下文被清空后的自己"看。** 读的顺序：先读这份，再按需回 `PROJECT.md` 查细节。
>
> 最后更新：2026-09-17（**修完 2.6 抽取精度回归 + 清理被污染的库 + 新增 `--terms`**；下一步 = 5.2 规划循环 → 5.3 `--ask`）

---

## 零、3 分钟恢复上下文

```
1. 读本文件 §一（进度）+ §二（工作方式）——这两节决定"接着干什么、怎么干"
2. 跑一次验证，确认环境是好的（§六）
3. 看 §一 的「下一个动作」，直接开工
```

**不要**从 `PROJECT.md` 第一章从头读 —— 它是查询用的，不是入口。

**三份文档的分工**：`STATE.md`（这份，滚动状态）· `PROJECT.md`（稳定基准 / 规格）·
**`PRODUCT_LOOP.md`（闭环审查 + 八个结构性漏洞 + 完成/未完成清单）** ——
要评估"这东西成不成"、要写方案或答辩时读第三份。

**当前阶段**：第 2 阶段（知识库骨架）**收口完成**（2.1→2.6c），**正在做第 5 阶段 Agent 化**
（5.1 Tool Registry ✅ → 5.2 规划循环是下一步）。
2.7 / 2.8 不再单独做，**并入 5.3 / 5.5**（否则同一个能力会有两份实现）。

> ⚠️ **第 5 阶段插在第 4 阶段（GUI）之前做，这是有意的。**
> 赛题赛道维度 20 分里写明「工具调用…是区别于'问答机器人'的关键」——
> GUI 影响的是完成度观感，**Agent 影响的是定性**。文字类产出（方案大纲、答辩材料、
> 提交文档）都要靠它，而 GUI 只影响截图。
`PROJECT.md` §7 第 4 阶段有：三个决策、可行性证据、11 步顺序、6 条架构约束、
以及**产品形态举例（例 0~6）**—— 那份是需求，动 GUI 之前先读它。

---

## 一、当前进度

### 阶段总览（对应 `PROJECT.md` §7 执行顺序）

| 阶段 | 步骤 | 状态 |
|---|---|---|
| **第 0 阶段** 修会话 #11 的质量问题 | 0.1 假行动项校验器 | ✅ 完成 |
| | 0.2 术语约束翻译 | ✅ 完成 |
| **第 1 阶段** 让全链路可自动化 | 1.1 `--wav` 文件输入 | ✅ 完成 |
| | 1.2 去掉启动选择题 | ✅ 完成 |
| **第 2 阶段** 知识库骨架 | 2.1 FTS5 + 三张表 + 迁移 | ✅ 完成 |
| | **2.2 `KnowledgeStore`**（归一化 / upsert / 状态提升 / 历史） | ✅ 完成 |
| | **2.3 缺口检测四条规则** | ✅ 完成 |
| | **2.4 结束时确认交互** | ✅ 完成 |
| | **2.5 三腿复用接知识库** | ✅ 完成 |
| | **2.6 自动抽取 + `newly_seen` 规则** | ✅ 完成 |
| | **2.5b 交付物 UTF-8 净化 + 「的」被当成全角冒号（真病根）** | ✅ 完成 |
| | **2.6b 中文专名抽取（R6/R7）** | ✅ 完成（⚠️ 只在手写语料上验过） |
| | **2.6c 抽取精度回归修复 + 库清理 + `--terms` 诊断** | ✅ 完成（用户真跑 #47 暴露的） |
| | **2.6d 提问质量：写法冲突合并 / R3 全大写收紧 / 措辞改人话** | ✅ 完成（用户真跑 #13 暴露的） |
| | 2.7 `--ask` 检索 | 未开始（**归入 5.3**，做成 agent 的退化情形） |
| | 2.8 Action 跨会话追踪 | 未开始（**归入 5.5**） |
| **第 3 阶段** 遗留质量项 | 3.1 分句碎片化 / 3.2 场景判定 / 3.3 Evidence / 3.4 静音收尾裁尾 | 未开始 |
| **第 4 阶段** 桌面产品化 | 4.1 输出路径+按助手分目录 / 4.2 配置+助手注册表 / 4.3 key 存储 / 4.4 WebView2 壳 / 4.5–4.10 各页面 / 4.11 任务状态流转 | **方案已定，未开工** |
| **第 5 阶段** Agent 化（**插在第 4 阶段之前**） | 5.1 Tool Registry ✅ / 5.2 规划循环 / 5.3 `--ask` / 5.4 写工具+闸 / 5.5 Action 落库 / 5.6 `--report` / 5.7 Evidence / 5.8 Demo | **5.1 ✅，其余未开始** |

### 刚做完（2.6c，2026-09-17）

用户的真实会话 #47 暴露了 2.6 的抽取精度回归，连带挖出三个别的问题，都已修完：

| 修了什么 | 在哪 | 验证 |
|---|---|---|
| 抽取太松 → 8 个假阳性 | `src/KnowledgeExtract.cpp`（三条判据 ①②③） | 自检 ⑨b 用真实转录；`--extract 47` 只剩 4 个真名 |
| 库被污染（9 条脏数据进了 `initial_prompt`） | 用 `tools/knowledge_audit.py` **降级**为 archived | `--terms` 从 12 条脏词 → `Erica, EnglishPod, TV, Marco` |
| 看不见"库里的东西有没有生效" | 新增 `--terms` 诊断 + 收敛重复的 `build_glossary()` | 秒级，不加载模型 |
| 同 key 两种 kind 在摘要背景里出现两行 | `src/KnowledgeStore.cpp` `background_lines()` 去重 | 新增自检组「背景去重」 |

**这一步最该记住的一条**：污染库里的 8 条**全是用户逐条确认过的**。
"已经被用户确认"不等于"是对的" —— 上游该拦的不能推给用户。

### 下一个动作（具体到文件）

**5.2 最小规划循环**（`inc/AgentLoop.h` + `src/AgentLoop.cpp`）

- 复用已注册的 5 个只读工具（`agent::ToolRegistry`）
- **把"调 LLM"抽成可注入回调** —— 和 2.4 的 `LineReader` 同一招，
  于是规划循环能进 L1（喂脚本化的 tool call 序列，不联网）
- Agent State：消息历史 + 已调用过的工具 + 步数/预算计数
- 预算：最大步数（默认 6~8）/ 最大 token / 最大墙钟时间（默认 60s），**超了要有定义好的行为**
  （倾向：返回已完成的部分 + 明确说明为什么停，而不是报错）
- 超时 / 失败 / 取消 / 重试
- **`--audit` 逐步打印**：`→ 调用 list_sessions(days=7)` / `← 3 场会话`
  —— 这是评审判断"它真的在做事"的唯一依据，也是调试的唯一手段
- ⚠️ 失败模式必须是"**说清楚我做不到**"，绝不能编一份报告 ——
  这直接决定赛题"结果交付与可验收性"是加分还是负分

紧接着 **5.3 `--ask`**：它不是独立实现，而是 agent 的**退化情形**（单次工具调用、不规划）
—— 否则将来有两份代码。


### 其它状态

| | |
|---|---|
| 分支 | `main`（本地主线）；远程备份在 `assistant-baseline` |
| 远程 `main` | `da97d96` —— **有意不动**，详见 `PROJECT.md` §3.6 末「历史决策：不合并 da97d96」 |
| tag | `translator-final` → `7717ace`（翻译版本封存） |
| 自检 | **33 组**，秒级，不需要模型 |
| 闭环八步完成度 | 八步**等权平均 88%**（听见 90 / 理解 88 / 提取 80 / **记忆 90** / **发现变化 85** / **询问 90** / **更新 90** / **再次利用 90**）<br>2.6 补上了"自动抽取"这个**原本不存在的环节**；2.5b 修掉了摘要要点标题被切坏的问题（理解 82→88、提取 78→80） |

**工作树状态**：干净（2.5b 已完成，`inc/Utf8.h` / `src/Utf8.cpp` 已入库）。

**"越用越懂你"的可验证里程碑**（第二句承诺）：

| 里程碑 | 状态 |
|---|---|
| 知识能落库、能区分 confirmed/candidate | ✅ 2.2 |
| 能发现"该问什么" | ✅ 2.3 |
| 用户答了能改进知识库 | ✅ 2.4 |
| 答过的东西真的改变识别/翻译/摘要行为 | ✅ 2.5（翻译腿已用真实失败句 A/B 证明） |
| 会答的问题不重复问 | ✅ 2.4（`asked_count` + `kMaxAsks`） |
| **能从会话里自己长出候选知识** | ✅ **2.6** ← 缺了这一步，上面五条全部拿不到输入 |
| **整条闭环在真实音频上跑通**（不靠手工插数据） | ✅ **2.6**：真实会话 #43 → 自动抽出 4 个 → 用户确认 → 译文被纠正 |

### 2.6 的端到端证据（真数据，不是编的）

```
[Extract] 会话 #43 共 16 段，开始抽取
[Extract] 抽出 4 个候选： Marco(person,2) Erica(term,2) EnglishPod(term,1) TV(term,1)
[Extract] 已写入 4 条候选（全部 status=candidate，未被用作任何约束）
[确认] 这场会话里有 4 条知识想跟你核对：
  1. 第一次听到「Marco」。这个词的写法对吗？      已确认：Marco
  2. 第一次听到「Erica」。这个词的写法对吗？      已确认：Erica
  ...
[确认] 记下 4 条，跳过 0 条

然后同一句真实失败句：
  会话 #43 当时的译文:  埃里卡，你怎么样？马可，我过得很好。
  现在:                Erica，你怎么样？Marco，我过得很好。
```

⚠️ **三条腿各自的验证级别仍然不一样**（别混为一谈）：

| 腿 | 验到了什么 | 没验到什么 |
|---|---|---|
| ① 识别提示 | prompt 字符串确实被构造并送进 `SpeechEngine`（日志 `[识别提示]`），candidate 不在里面 | **whisper 的识别结果是否真的因此变准** —— 需要"含会被识别错的专名的音频" |
| ② 翻译约束 | **行为级 A/B 通过**（`tools/ab_translation_constraint.py`） | 云端翻译路径（本机没有 API key） |
| ③ 摘要背景 | 背景确实拼进送给模型的 user 内容（自检 + 日志 `[摘要] 已注入`） | **模型是否真的不与之矛盾** —— 本机没有 instruct 模型 |

---

## 二、工作方式（用户的明确要求，**别丢**）

> 这一节是用户反复纠正后才定下来的。丢掉它 = 用户会说"你和记忆丢失了一样"。

1. **每轮必须有代码动作。** 改 `.cpp/.h`，或者跑构建/测试。**纯写 md 的轮次不算一轮。**
   （历史教训：有一段时间连续十几轮只写文档，用户直接指出风格变了。）
2. **报告用固定结构**：
   ```
   # 步骤 N 完成 ✅ <产出名>
   ## 做了什么          ← 文件级表格
   ## 为什么是这一步    ← 排序理由
   ## 验证证据          ← 实测数字，不是"应该没问题"
   ```
3. **交付时说清三段**：**怎么测 / 理想结果 / 出问题怎么判断**。
4. **每个功能走完整循环**：**实现 → 验证 → git 提交 → code review → 告诉用户怎么检验**。
   code review 是**我自己审自己**，要写出发现的问题和未修的遗留，不是走过场。
5. **不要问"要不要"**。产品判断才问（清单见 `PROJECT.md` §8.9）；实现细节自己定并说明理由。
6. **证据优先，不猜。** 用实测数字说话；被证伪就当场承认并修。
   （已经发生过两次：把模型加载猜成 100 秒，实际 8.4 秒；声称修好了截止日期误杀，实际没修好。）
7. **收工前更新这份 `STATE.md`。**

---

## 三、代码风格（这个项目的写法）

### 注释

- **写"为什么"，不写"是什么"。**
- 修过的坑必须留三件套：
  ```cpp
  // 【现象】窗口永远停在"翻译中…"
  // 【原因】译文轮询被放在带 continue 的分支之后，只跑了一次
  // 【判断】若复现，先查这段轮询有没有被挪到 continue 后面
  ```
  **本项目没有测试框架，注释就是回归记忆。**

### 结构

- **纯逻辑做成 `static` 函数 / 纯函数**，这样能进 L1 自检（秒级、不需要模型）。
  已经这么做的：`strip_overlap`、`collapse_repeats`、`TermFixer`、
  `SpeechEngine::looks_like_prompt_echo` / `looks_repetitive`、
  `sanitize_actions`、`canonicalize_date`、`choose_translator_backend`、
  `WavReader::parse`、`ITranslator::glossary_constraint`、
  `knowledge::normalize_key` / `usable_as_constraint` / `fts_query_from_user_text`、
  `knowledge::detect_gaps`、`knowledge::interpret_answer`。
  > 判据：**能不能不碰数据库、不碰模型就验完？** 能，就提成纯函数。
- **交互逻辑要把"输入源"抽成回调**，别写死读 `std::cin`。
  `run_confirmation()` 收一个 `std::function<bool(std::string&)>`：
  main 注入读控制台，自检注入读 stringstream —— 于是问答循环能进 L1，
  **而且落库走的是真实路径**（不是自检构造的理想数据）。
  GUI 壳将来也从同一个接缝接进来。
- **断言要断言到机制，不能只断言"东西在"**。
  「四张表在不在」这种断言让"FTS 索引其实一条没建起来"藏了两轮。
  改成"写进去→必须查得回来→删掉→必须查不到"之后，**当场抓出两个真 bug**。
- **触发表与判定表必须分开**：抽取宁宽勿漏，判定宁严勿滥。
  （`action_markers_*` vs `strict_verbs_*` —— 混用过一次，结果一句寒暄被当成作业。）
- **接口上的共享逻辑放接口里**，别让多个后端各写一份。
  （`glossary_constraint` 就是三处 prompt 各写一份之后提取出来的。）

### 错误处理

- **不支持就报错，绝不静默给错数据。**
  （`WavReader` 遇到不支持的位深直接报错，而不是输出整段静音。）
- 错误走 `std::cerr`，正常输出走 `std::cout`，统一 `[模块名]` 前缀。
- 失败路径要给**下一步提示**，不只说"失败了"。

### 测试

- 用例的输入**必须来自真实路径**，不能自己手搓一个"更干净的"版本。
  **这条栽过两次**：`--test-window` 自己编时序；截止日期用例配了真实管线不产生的配对。
- 加自检时，**先确认这条用例在修复前会失败** —— 否则它是安慰用例。
- **别用"表存在/文件存在"当验收**。第 2 阶段已栽过一次：FTS 索引空着也能过。
  每加一层基础设施，就加一条**走真实写入路径的往返断言**。
- **断言必须与真实数据隔离**。别断言"检索某词恰好 1 条" ——
  库里真有一条同名的知识时，自检就永远红。用**本用例独有的 token**
  （`ZqFtsRoundTrip9` 那种），断言才只反映本用例干了什么。

### 提交

- **一次改动一次提交**；提交信息写**为什么**，修 bug 要带现象/原因/判断。
- 不要巨型提交（一个 36 文件的提交出问题只能整体回退，无法 `git bisect`）。
- **不要 `git add -A`** —— 先 `git status` 确认。`t.db`、`out/`、`build/` 都不该进仓库。
- 推送前先 `git fetch` 确认没分叉，**永远不要 `--force`**。

### 编码与命名

- 源码一律 **UTF-8 无 BOM**。项目用 `/utf-8` 编译，GBK 文件会刷 C4828 警告
  （`miniaudio_impl.cpp` 原来就是，已修）。**别用会写回 GBK 的编辑器。**
- 注释用中文，标识符和字符串常量用英文，**面向用户的输出用中文**。
- 常量上提为具名 `constexpr` / `static const`，不写魔法数。

---

## 四、最近的关键决定与踩坑（滚动，只留最近 10 条）

| 时间 | 事 | 关键点 |
|---|---|---|
| 09-17 | 2.6c **抽取太松：8 个假阳性真的进了 `initial_prompt`** | 用户真跑 #47（53 段播客）后确认了 8 个假专名（`down/Keep/Midnight movies/Speaking/Learners/Exactly/preview`）。**不是"库里多了几条脏数据"，是 Whisper 的识别提示被污染了** —— 下次识别会被主动往这些词上带。→ 加三条判据：强形状 / 佐证 / `cap_noninit>=2 且从未全小写出现`。真实数据上 8 个假阳性全消、4 个真名全留 |
| 09-17 | 2.6c **"用户确认过的"不等于"对的"** | 这 8 条都是用户**逐条按了确认**的。人面对"是否记住了 down？"会顺手按 y —— **把确认当成质量保证是错的**，上游该拦住的不该推给用户。这也是 §6.7 提问稀缺的另一个理由 |
| 09-17 | 2.6c **"某个键是专名"和"它出现了几次"是两件事** | 我给计数环节加了 `classify_token(t) == Ev::None` 过滤，自检立刻报「Marco 出现次数应为 2」：`Marco` 在句首那次被判成句子开头而跳过。**判定的输入是证据，计数的输入是全部出现** —— 混在一起就会给用户看错数字 |
| 09-17 | 2.6c **诊断命令行必须能看到"库里的东西有没有生效"** | §6.5 红线的可观测面就是那串 `initial_prompt`，但以前只有真跑一场会（录音 + 等 Whisper 加载上百秒）才能看到。**验一次的成本高到没人会验**，所以库被污染了没人发现，直到用户自己翻日志问"这些词哪来的"。→ 新增 `--terms`（不加载模型，几秒给同一答案） |
| 09-17 | 2.6c **同一段逻辑抄了第三遍** | 「知识库 confirmed + `--glossary` 去重合并」在主路径和 `run_dump_prompt` 里各写了一份（注释还写着"两边不可能再走散"）。本项目"诊断与真实路径走散"已踩三次，每次都是因为存在第二份拷贝 → 收敛成唯一的 `build_glossary()` |
| 09-17 | 2.6c **同 key 两种 kind 在背景里出现两行** | 条目身份是 `(kind,key)`：`Marco` 一场抽成 `person`、另一场确认成 `term`，于是摘要背景里同时有「Marco（人名）」和「Marco（术语）」。存储层没错，**下游没收敛**，对模型是自相矛盾的噪声 → `background_lines()` 按 key 去重（`constraint_terms()` 早就有，我漏了这一处） |
| 09-17 | 2.6c **测具用了真实数据的形状，就会被真实数据绊倒** | `verify_memory.py` 的 FTS 探针写的是 `("term","englishpod")` —— 库里真有了 englishpod 之后，INSERT 撞 `UNIQUE(kind,key)`，脚本自己崩，而真正要验的"触发器维护索引"根本没跑到。→ 探针必须用**不可能碰撞的合成键**（同 `__selftest_` 的思路） |
| 09-17 | 2.6c **FTS 探针不能用带下划线的词** | 换合成键后第二次假失败：`__verify_fts_a__` 被 unicode61 分词器按 `_` 切开，索引里只有 `verify`/`fts`/`a`，`MATCH 'verifyftsa'` 当然找不到。**用 `fts5vocab` 看过真实 token 表才确认**，没靠猜 |
| 09-17 | 2.6c **清理只能降级，不能删** | 清理污染库时选择 `status='archived'` 而不是 `DELETE`：archived 已经不进提示/约束（§6.5 红线），效果等价，但**可逆、可复查**。`tools/knowledge_audit.py` 强制这条（脚本里没有 delete 分支） |
| 09-17 | **`--selftest` 不指定 `--db` 会往默认库写假会话** | 默认库是**相对当前目录**的 `translations.db`，而自检要验落库往返、所以真的写会话。两个事实叠起来就是陷阱：从 exe 所在目录（`build\RelWithDebInfo\`）跑一次不带 `--db` 的 `--selftest`，**历史库里就多出几场 `engine=SelfTest` 的假会话**，它们会出现在"最近会话"里，将来还会被 agent 的 `list_sessions` / 交付物当成真会话读进去。我在项目根目录跑了几次，就留下一个只装着 11 场自检会话的 `translations.db`。**已修**：没显式给 `--db` 时自检自己开临时库（`%TEMP%`），跑完删掉；显式给了就照旧（那是"让新版程序打开某库一次以补触发器/迁移"的入口，必须保留） |
| 09-17 | **我给用户的命令少了一个参数，代价是他的库被写脏** | 我在交付说明里写 `--selftest`，文档里写的是 `--selftest --db t.db`。**从文档里抄命令，不要凭记忆重写** —— 少一个参数的后果不是"命令失败"，是**静默往用户的真实数据里写东西**。这次真正被写脏的只是一个新建的根目录 `translations.db`（历史库没被碰），但性质上不该靠运气 |
| 09-17 | **`--db` 是不是用户给的，本身就是个信号** | 修上面那个坑时发现：光有默认值不够，必须知道"用户到底指没指库"。于是 `AppConfig.db_path_explicit`。**别用一个值同时表达"默认"和"用户明确要求"** —— 需要区分时再加一个标志，而不是去猜路径长得像不像默认值 |
| 09-17 | **修复前的候选会在下次真跑时变成蠢问题** | 用户真实库里 11 条 candidate 全是修复前抽取器留的，虽然红线守住了（0 条 confirmed，从没进过提示词），但下次会话会问「已经听到 5 次「Froze」，它是对的说法吗？」。**"不影响行为"不等于"不用清理"** —— 提问是稀缺资源，一个蠢问题就在消耗用户对记忆的信任 |
| 09-17 | 2.6d **问用户一个他无法回答的问题** | 用户原话：「感觉根本不是人类的会问出的东西」。旧措辞「这个词的写法对吗？」把"拼写对不对"推给一个**刚听到、还没看到字**的人 —— 他在听音频，他不知道那个词该拼成什么样，只能瞎猜或跳过。**真正属于他的决定是"要不要让我把这个词记下来"**。更糟的是猜错会被记成约束（他按了 y，`Erika` 进了 `initial_prompt`）。**判据：问之前先问自己"用户凭什么知道这个答案"** |
| 09-17 | 2.6d **同一个实体被问成两个独立问题** | 库里 `Erica`（#43）和 `Erika`（#13）只差一个字母，旧代码当两条不相干的知识各问一遍。用户的两个回答**互相矛盾**，而他无从知道这两个写法指同一个人 —— 因为两个问题都没提到对方。**正确答案一直在我们自己手里**（两条只差一字母的记录就是"疑似同一实体"），把判断推给用户之前必须先把冲突摆出来 |
| 09-17 | 2.6d **`from == to` 被当成失败报给用户** | 写法冲突里用户选中**本来就是 confirmed** 的那个（最常见答案："现在用的对，另一个是听错的"）时，`set_status(id,"confirmed")` 失败 —— `can_promote` 里 `from == to` 返回 false（"状态没变就不是一次状态变更"）。于是用户答对了却看到 `[失败] 状态不允许从 confirmed 变为 confirmed`。**"无需变更"不是错误** |
| 09-17 | 2.6d **手写的用例计数会撒谎** | 缺口检测那组写着 `"✅ 11 例通过"` 是**硬编码**的，我加了 ⑫⑬⑭ 三条之后它照样打"11 例通过"。这跟"诊断工具撒谎"是同一类问题，只是更隐蔽（数字看起来像真的）。**计数必须跟着用例走，否则宁可不报数字** —— 失败时 `why` 会指名道姓，那才是有用信息 |
| 09-17 | 2.6d **全大写 ≠ 缩写** | 播客小标题 `PUTTING IT TOGETHER` 整行大写，8 字母的 `TOGETHER` 满足"连续 ≥2 大写"被当缩写抽出来。**真缩写都在 5 字母内**（CEO/KPI/OKR/SLA/SDK），而"被大写强调的普通词"任意长度 —— 长度就是分界。另外**整行大写是排版不是形状**，这种段落在"大写"上零信息量，应该整段作废而不是逐词猜 |
| 09-17 | 2.6d **合并错了比不合并更糟** | 写法冲突的合并条件卡得很紧（同首字母 + 长度 ≥4 + 编辑距离 ≤1）：判错的代价是让用户在两个**无关**词之间做选择。`PC`/`PB` 这类两字母词一律不合并，`Phoenix`/`Marco` 这种不相干的也必须各出一问（都有专门的自检用例） |
| 09-17 | 2.6b **中文会议原本"学不到任何知识"** | 抽取器的英文规则靠**首字母大写**，中文没有大小写 → 中文会议里「张伟负责下周的报价」**一个候选都抽不出来** → 知识库不长 → 没问题可问 → 三条腿没输入 → **"越用越懂你"在中文场景下完全不成立**。而中文会议正是这产品的主场。已补 R6（姓氏+佐证）/R7（后缀） |
| 09-17 | 2.6b **`strcmp` 用在前缀判断上** | 判断"人名后面跟的是不是动词"时，传进去的是**后面剩下的整串**（"负责下周的报价"），而我用了 `strcmp` **全等** → 只有动词正好落在串尾才匹配。后果：「张伟负责下周的报价」里的**张伟完全抽不到**，而「那边的**反馈**」里的假人名「边的」**反而被抽到**（反馈正好在串尾）。**批量用例才抓得出来，单个用例很可能刚好避开** |
| 09-17 | 2.6b **"人名里不能含虚词"要从第 2 个字查起** | 「那边**的**反馈」→ 边是姓氏、反馈是佐证动词 → 抽出假人名「边的」。但第 1 个字不能查：「于」本身是常见姓氏，一查就会把「于伟」这类真名字挡掉 |
| 09-17 | 2.6b **弱后缀要有出现次数门槛** | 「项目/系统/平台」太宽泛（"交付项目"），刻意**不收** 计划/方案/产品/版本/部门/团队/中心 —— 实测「交付计划」会被当成专名。报错的代价不是"多一条数据"，而是**用户被问一个蠢问题** |
| 09-17 | 2.6b **中文抽取没有真实数据可验** ⚠️ | 手写语料只能验"规则的形状"，验不了真实 ASR 输出上的准确率（同音错字、有时不加标点）。**需要用户录一场中文会议，再 `--extract <id>` 回来看**。这根 §8.8⑨ 是同一个约束，只是这次没有替代方案 |
| 09-17 | **环境坑：PowerShell 5.1 的 `>` 默认写 UTF-16LE** | 我按 UTF-8 读日志，看到"中文乱码"，**连续两次得出错误结论**（先说"命令行参数编码坏了"，更早还说"中译中模型拒答"）。日志必须按真实编码读 —— 用 Python 解 `utf-16` 或看开头有没有 `\xff\xfe` |
| 09-17 | **`--dump-prompt` 第三次和真实路径不一致** | 它无条件调 `translate_once`，**没有**主循环里那条"源==目标就跳过翻译"的保护。拿它测"中文会议"会看到误导结果（源==目标时模型做同语言改写：`let us start`→`let us begin`）。**真实链路**是：检测到 zh == 目标 zh → `passthrough` 落库（src=tgt），摘要那边 `tgt != src` 才重复发 → **中文会议本来就能出纪要** |
| 09-17 | **术语约束会凭空编造专名（回归，已修）** | 用户真跑会话 #44 发现：`and wife get ready to go` → 「埃丽卡和马可准备出发了」。**对照实验证实**是约束造成。修法：`ITranslator::glossary_for_text()` 只约束本段真出现过的术语 |
| 09-17 | **我造成的数据丢失事故（已修已恢复）** | `tools/ab_translation_constraint.py` 第一版直接 `DELETE FROM knowledge`，把用户 4 条 confirmed 全删了。已改成一次性 scratch 库 + 拒绝在有数据的库上运行 |
| 09-17 | 2.5b **诊断错了，是靠"数字对不上账"纠正的** | 净化器报 6 处坏字节、上游一行没报 → 模型回复是干净的 → 真病根是 `find_first_of(u8"：:")` 按单字节比较，把「的」（`E7 9A 84`）的 `0x9A` 当成了全角冒号 |
| 09-16 | 2.6 **闭环缺了一整个环节，是用户真跑才暴露的** | **设计文档里写了、执行步骤表里没有 = 不存在** |
| 09-16 | 2.6 **只修数据流一环 ≠ 修好** | 改了 `upsert` 采信 `hits`，重跑还是 1 —— 真相是 `save_candidates` 压根没传 `c.hits` |
| 09-16 | 2.4 **"提问稀缺"要有个地方记"问过了"** | `asked_count` + `kMaxAsks = 2`，**跳过也计数** |
| 09-16 | 2.3 **外部内容 FTS5 表必须用触发器维护** | 手动 `INSERT INTO knowledge_fts` 等于白写；`DELETE FROM knowledge_fts` 直接报 `database disk image is malformed` |
| 09-16 | 2.3 弱断言 = 假绿灯 | 旧自检只验"四张表在不在"，索引空的 bug 藏了两轮。改成"写进去→必须查回来→删掉→必须查不到"后**立刻抓出两个真 bug** |
| 09-16 | 2.2 **值变了必须把 confirmed 降回 candidate** | 原来那条确认是针对旧值的，值一变它就不成立（§6.5 要防的正是这个） |
| 09-16 | 2.1 迁移的边界 | `CREATE TABLE IF NOT EXISTS` 只补表、**永远不补列** —— 加列必须另走 `ALTER TABLE` |

---

## 五、这台机器的环境坑

完整清单见 `PROJECT.md` §4.5，这里是**最常撞到的四条**：

| 坑 | 解法 |
|---|---|
| `git push` / `ls-remote` 报 `schannel: SEC_E_NO_CREDENTIALS` | 加 `-c http.sslBackend=openssl`。**`ls-remote` 匿名可读，所以容易误判成"远程没问题"** |
| `git push` 报 `sh.exe: couldn't create signal pipe` + 读不到用户名 | 沙箱禁命名管道 → 凭据助手起不来。**需要在更宽权限下跑，或由用户在普通终端推** |
| 构建报 `LNK1168`（exe 被占用） | 先 `Get-Process Translator \| Stop-Process -Force` |
| PowerShell 读中文文件显示乱码（`绾跨▼瀹夊叏`） | **误报**：文件是合法 UTF-8，是 `Get-Content` 用 GBK 解码显示。**别据此改编码**，用字节级校验 |
| `Select-String` 默认**不区分大小写** | 扫平台 API 时 `WPARAM` 会撞上 `wparams`，产生假命中（被骗过一次） |
| 改了 `CMakeLists.txt` 的编译定义后只 build | **必须重新 configure**，否则不生效（加 `SQLITE_ENABLE_FTS5` 时踩过） |
| **`--db t.db` 是相对当前目录的**，而 `build\RelWithDebInfo\` 和仓库根**各有一个 `t.db`** | 两个不同的文件。验证/排查前先 `pwd`。`verify_memory.py` 现在会打**绝对路径**，一眼看出验的是哪个（这个坑真实发生过：验证报"缺少记忆表"，其实只是验错了文件） |

此外几条常撞到的：**仓库里有 8 个 `.db` 文件**（`build\RelWithDebInfo\` 下有 t.db / translations.db / sum.db /
t6.db / test_s4.db / test_step1.db，根目录还有 t.db），而 `--db` 是相对路径 ——
**真实会话历史在 `build\RelWithDebInfo\translations.db`，不在 `t.db`**（2.5 找"会话 #11 案例"时在这上面绕了一圈）。
引用"真实案例"时务必写清是哪个库。**`_` 在 SQL `LIKE` 里是单字符通配符**（`purge_key_prefix` 用的就是 `LIKE`，
所以 `__selftest_` 实际匹配 "任意两字符 + selftest + 任意一字符"，隔离性比看上去弱 ——
目前够用，但别把前缀隔离当成安全边界）；**FTS5 外部内容表不能用普通 `DELETE`/`INSERT`**
（报 `database disk image is malformed` 或静默不生效，只能用触发器或 `'rebuild'`）；
**MATCH 左侧不认表别名**；**PowerShell 5.1 的 `>` 默认写 UTF-16LE**（按 UTF-8 读日志会看到“中文乱码”，我因此连错两次结论）；**PowerShell 5.1 把无 BOM 的 `.ps1` 当 GBK 读**（脚本里的中文串会被破坏，
所以驱动脚本用 Python 写，不用 .ps1）。

---

## 六、收工前自检

```powershell
# 0. 先杀进程，否则链接失败 LNK1168
Get-Process Translator -ErrorAction SilentlyContinue | Stop-Process -Force

cd C:\dev\projects\AudioTranslator

# 1. 构建
cmd /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"" >nul 2>&1 && set VCPKG_ROOT=C:\dev\vcpkg && cmake --build build --config RelWithDebInfo --target Translator"

# 2. L1 自检：必须 33 组全过
#    ⚠️ 不给 --db 时自检自己开 %TEMP% 下的一次性库并跑完删掉（不会碰你的 translations.db）。
#       要验**指定的库**（或让它补触发器/迁移）就显式加 --db。
.\build\RelWithDebInfo\Translator.exe --selftest
.\build\RelWithDebInfo\Translator.exe --selftest --db build\RelWithDebInfo\t.db

# 2a. **长期记忆有没有真的生效**（2.6c 起）—— 不加载模型，秒级
.\build\RelWithDebInfo\Translator.exe --terms --db build\RelWithDebInfo\t.db
#     期望：① 那串 initial_prompt **只含真专名**，一个普通词都不许有
#           ② 条数 = confirmed 里 name-like 的去重数（同 key 不同 kind 只算一次）
#     ⚠️ 这是唯一能低成本看见 §6.5 红线可观测面的入口。库一脏这里立刻现形：
#        曾经打出 `Erica, down, Keep, EnglishPod, movies, TV, Marco, ..., 1`

# 2a2. 库的卫生审计（2.6c 起）—— 独立 sqlite3 oracle，列出可疑条目
python tools\knowledge_audit.py --db build\RelWithDebInfo\t.db
#     清理只能用**降级**（脚本里没有 delete 分支）：
python tools\knowledge_audit.py --db build\RelWithDebInfo\t.db --archive <key>... # 先 dry-run
python tools\knowledge_audit.py --db build\RelWithDebInfo\t.db --archive <key>... --apply

# 2b. 知识库数据层（第 2 阶段每步都要跑）——独立实现验证，且不改动原库
#     ⚠️ 一定要用**程序真正在用的那个库**：--db 是相对当前目录的
python tools\verify_memory.py build\RelWithDebInfo\t.db

# 2c. 缺口检测端到端（2.3 起）：seed → 应问 3 个 → clear → 应问 0 个
python tools\verify_memory.py build\RelWithDebInfo\t.db --seed-demo
.\build\RelWithDebInfo\Translator.exe --gaps --db build\RelWithDebInfo\t.db
python tools\verify_memory.py build\RelWithDebInfo\t.db --clear
.\build\RelWithDebInfo\Translator.exe --gaps --db build\RelWithDebInfo\t.db

# 2d. 确认交互端到端（2.4 起）：管道喂回答，走**真实循环 + 真实落库**
python tools\verify_memory.py build\RelWithDebInfo\t.db --seed-demo
"y`nn`nMarco" | .\build\RelWithDebInfo\Translator.exe --wav C:\dev\projects\whisper.cpp\samples\jfk.wav --summarizer rules --ask --db build\RelWithDebInfo\t.db --out v_out
#    期望：已确认：Qwen ASR / 已记下这个写法不对 / 记住了：Marco / 记下 3 条，跳过 0 条
.\build\RelWithDebInfo\Translator.exe --gaps --db build\RelWithDebInfo\t.db
#    期望：只剩 1 个（Phoenix，最后一次机会）—— confirmed 的不再问
python tools\verify_memory.py build\RelWithDebInfo\t.db --clear

# 2e. 自动抽取 + 整条闭环（2.6 起）—— **拿真实会话补学，不用重录音频**
#     先用只读模式看抽取器认出了什么（不会改库）
.\build\RelWithDebInfo\Translator.exe --extract 43 --db build\RelWithDebInfo\t.db
#     期望：EnglishPod 播客那种素材应抽出 Marco / Erica / EnglishPod；
#           jfk.wav 那种"每句首词大写但没专名"的应抽出 **0 个**
#     再真的走一遍闭环（抽取 → 落候选 → 问 → 确认）
"y`ny`ny`ny" | .\build\RelWithDebInfo\Translator.exe --extract 43 --apply --ask --db build\RelWithDebInfo\t.db
#     期望：[Extract] 已写入 4 条候选（全部 status=candidate）
#           [确认] 第一次听到「Marco」… → 已确认
#     然后拿原来那句失败译文验约束生效：
.\build\RelWithDebInfo\Translator.exe --dump-prompt "How are you, Erica? Marco, I'm doing really well." --db build\RelWithDebInfo\t.db
#     期望：输出 Erica，你怎么样？Marco，我过得很好。（原来会被音译成 埃里卡/马可）

# 3. 端到端（可选，约 20 秒，零交互）
.\build\RelWithDebInfo\Translator.exe --wav C:\dev\projects\whisper.cpp\samples\jfk.wav --summarizer rules --db v.db --out v_out

# 3b. §6.5 红线的**行为级**回归（2.5 起）：真句子 + 真模型，约 40 秒
#     断言：① 基线逐字复现历史失败 ② candidate 与基线逐字相同 ③ confirmed 纠正译文
python tools\ab_translation_constraint.py
#     跑完会自己清空 knowledge 表

# 4. 提交
git add -- <明确列出的文件>
git status --short          # 确认没有多余文件
git commit -F <写好的提交信息文件>

# 5. 推送（不触碰 main）
git branch -f assistant-baseline HEAD
git push origin assistant-baseline
```

**然后回答三个问题，答不上就是没做完**：

1. 这一步跑到哪一级验证了？没跑到的级别，如实标注 ⚠️
2. **本文件（`STATE.md`）更新了吗？**
3. `PROJECT.md` 该同步的同步了吗？（规则见它 §8.5）
