# STATE · 进度与工作方式

> **这份是滚动状态，每次收工必须更新。**
>
> - `PROJECT.md` = 稳定基准（架构 / 设计决定 / 规划 / 开发流程）——不常变
> - **`STATE.md`（本文件）= 现在到哪了、该怎么干活** ——每次干完就改
>
> **给"上下文被清空后的自己"看。** 读的顺序：先读这份，再按需回 `PROJECT.md` 查细节。
>
> 最后更新：2026-09-16（2.4 确认交互 ✅，下一个动作 = 2.5 三腿复用接知识库）

---

## 零、3 分钟恢复上下文

```
1. 读本文件 §一（进度）+ §二（工作方式）——这两节决定"接着干什么、怎么干"
2. 跑一次验证，确认环境是好的（§六）
3. 看 §一 的「下一个动作」，直接开工
```

**不要**从 `PROJECT.md` 第一章从头读 —— 它是查询用的，不是入口。

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
| | **2.5 三腿复用接知识库** | ← **下一个动作** |
| | 2.6 `--ask` 检索 | 未开始 |
| | 2.7 Action 跨会话追踪 | 未开始 |
| **第 3 阶段** 遗留质量项 | 3.1 分句碎片化 / 3.2 场景判定 / 3.3 Evidence / 3.4 静音收尾裁尾 | 未开始 |
| **第 4 阶段** 桌面产品化 | 4.1 默认路径 / 4.2 key 存储 / 4.3 GUI 壳 | 未开始 |

### 下一个动作（具体到文件）

**2.5 三腿复用接知识库**（`PROJECT.md` §6.4②）：

三条腿现在只接了术语表，要改成**从 `knowledge` 取 confirmed 条目**：

| 腿 | 位置 | 现状 | 要做的 |
|---|---|---|---|
| 识别提示 | `SpeechEngine` 的 `initial_prompt` | 读 `terms.sample.txt` | 改读 `knowledge` 里 kind=term 的 confirmed 条目 |
| 翻译约束 | `ITranslator::glossary_constraint` | 调用方喂 `--glossary` 文件 | 调用方改成喂 `KnowledgeStore` 里的 confirmed |
| 摘要背景 | `LlmSummarizer` 的 prompt | 无 | 把 confirmed 条目当背景知识塞进 prompt |

**关键前提**：`usable_as_constraint()` 是唯一守门人（§6.5 红线），三腿都必须走它 ——
**2.4 之前库里全是 candidate，所以这一步即使接上了也拿不到东西**。
2.6/2.7 排在后面，因为它们不挡"越用越懂你"这条主线。

### 其它状态

| | |
|---|---|
| 分支 | `main`（本地主线）；远程备份在 `assistant-baseline` |
| 远程 `main` | `da97d96` —— **有意不动**，详见 `PROJECT.md` §3.6 末「历史决策：不合并 da97d96」 |
| tag | `translator-final` → `7717ace`（翻译版本封存） |
| 自检 | **21 组**，秒级，不需要模型 |
| 闭环八步完成度 | 八步**等权平均 66%**（听见 90 / 理解 90 / 提取 78 / 记忆 55 / 发现变化 55 / 询问 75 / 更新 55 / 再次利用 30）<br>⚠️ 之前写的"≈45%"和它自己那组数字（平均 57%）对不上，是我的笔误；现在改成就地可复算的等权平均 |

**"越用越懂你"的可验证里程碑**（第二句承诺）：

| 里程碑 | 状态 |
|---|---|
| 知识能落库、能区分 confirmed/candidate | ✅ 2.2 |
| 能发现"该问什么" | ✅ 2.3 |
| 用户答了能改进知识库 | ✅ 2.4 |
| **答过的东西真的改变识别/翻译/摘要行为** | ❌ **2.5** ← 差这一步，第二句承诺才第一次成立 |
| 会答的问题不重复问 | ✅ 2.4（`asked_count` + `kMaxAsks`） |

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
| 09-16 | 2.4 **是/否词表不能省** | 把 `n` 当成"用户输入的新值"会把字面量 `no` 写进库当专名，而且是 confirmed（会进翻译约束）。判定不能靠"看起来像不像值" |
| 09-16 | 2.4 **首尾不可见字符要按集合去** | 实跑抓到：PowerShell 管道第一行带 UTF-8 BOM，`"\uFEFFy"` 既不是 `y` 也不是空 → 掉进"其余当新值"→ **把 `y` 存成了专名**。**自检 13 个用例全绿，真跑才露出来**。零宽空格、全角空格 U+3000 同理 |
| 09-16 | 2.4 **"提问稀缺"要有个地方记"问过了"** | 加 `knowledge.asked_count` + `kMaxAsks = 2`。**跳过也计数** —— 连按 5 次回车就是"别再问了"。只在答了才计数的话，同一问题永远排在候选里。实测生命周期：第 1 场问 3 个全答完 → 第 2 场只剩 1 个（最后一次机会）跳过 → 第 3 场 0 个 |
| 09-16 | 2.4 **confirmed 的条目不能因历史而复活** | 规则①只看 `value_changed_demoted` 不看状态，于是用户刚答完"以后都用 Qwen ASR"的那条，下一场**又是第 1 问**，直接违反"confirmed 且无变化一个字都不问"。加 `status != confirmed` |
| 09-16 | 2.4 **自检断言撞真实数据** | FTS 往返用例断言 `search("phoenix")` 恰好 1 条 → 库里真有 `Phoenix` 时自检永远红。**用户知识库里只要有这么一条真知识，自检就失败**。改用本用例独有的 token |
| 09-16 | 2.4 交互的接缝设计 | 把"读一行"抽成 `std::function<bool(std::string&)>`：main 注入读控制台，自检注入读 stringstream。于是问答循环能进 L1，且**落库走真实路径**（不是构造理想数据）。GUI 壳（4.3）将来也从这个接缝接进来 |
| 09-16 | 2.4 **必须在 `write()` 之后问** | 交付物先落盘：用户中途 Ctrl+C 或直接走开，纪要一个字不少。反过来就是"问了半天没生成纪要"，最不可原谅的失败方式 |
| 09-16 | 2.3 **外部内容 FTS5 表必须用触发器维护** | 我第一版在 C++ 里手写 `INSERT INTO knowledge_fts` —— **索引根本没建起来**：外部内容表的 `count(*)` 读的是内容表，所以"看着有 4 行、MATCH 一条也查不到"。而 `DELETE FROM knowledge_fts` 会让 SQLite 报 `database disk image is malformed`。正解：挂 `ai/ad/au` 三个触发器 + 打开库时 `'rebuild'` 一次自愈历史库 |
| 09-16 | 2.3 弱断言 = 假绿灯 | 旧自检只验"四张表在不在"，所以上面那个 bug 藏了两轮。**修法不是修表，是把断言变强**：写进去 → 必须查回来 → 删掉 → 必须查不到。加断言后立刻抓出两个真 bug |
| 09-16 | 2.2 KnowledgeStore | **值变了必须把 confirmed 降回 candidate** —— 原来那条确认是针对旧值的，值一变它就不成立了（§6.5 要防的正是这个） |
| 09-16 | 2.2 自检抓到两个真 bug | ① `normalize_key` 只处理 ASCII 标点，`埃里卡。` 和 `埃里卡` 会变成两个键；② `_` 被当标点，`asr_engine` 变 `asr engine`、`__selftest_` 前缀被吃掉导致清理失效 |
| 09-16 | 2.1 FTS5 + 三张表 | `CREATE TABLE IF NOT EXISTS` 顺带就是迁移，老库打开自动补表。但**它只补表、永远不补列** —— 加列必须另走 `ALTER TABLE`（2.4 加 `asked_count` 时踩到，用 `PRAGMA table_info` 做幂等判断） |
| 09-16 | 截止日期依据改用整场转录 | **"无法判断" ≠ "无依据"**：云端路径 `ActionItem.source` 是空的（LCS 跨语言匹配不上），只看 source 会把所有日期清空 |
| 09-14 | 平台边界定论 | 只做 Windows 桌面版。采集和字幕窗是平台专有。要 Linux 就做**无界面批处理版**（`--wav` 是它的地基） |

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

此外四条最常撞到的：**`_` 在 SQL `LIKE` 里是单字符通配符**（`purge_key_prefix` 用的就是 `LIKE`，
所以 `__selftest_` 实际匹配 "任意两字符 + selftest + 任意一字符"，隔离性比看上去弱 ——
目前够用，但别把前缀隔离当成安全边界）；**FTS5 外部内容表不能用普通 `DELETE`/`INSERT`**
（报 `database disk image is malformed` 或静默不生效，只能用触发器或 `'rebuild'`）；
**MATCH 左侧不认表别名**；**自检自带的 `t.db` 与用户库是同一个文件**，
所以自检用例必须自己清理干净（`__selftest_` 前缀）。

---

## 六、收工前自检

```powershell
# 0. 先杀进程，否则链接失败 LNK1168
Get-Process Translator -ErrorAction SilentlyContinue | Stop-Process -Force

cd C:\dev\projects\AudioTranslator

# 1. 构建
cmd /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"" >nul 2>&1 && set VCPKG_ROOT=C:\dev\vcpkg && cmake --build build --config RelWithDebInfo --target Translator"

# 2. L1 自检：必须 21 组全过
.\build\RelWithDebInfo\Translator.exe --selftest --db t.db

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

# 3. 端到端（可选，约 20 秒，零交互）
.\build\RelWithDebInfo\Translator.exe --wav C:\dev\projects\whisper.cpp\samples\jfk.wav --summarizer rules --db v.db --out v_out

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
