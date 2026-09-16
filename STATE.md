# STATE · 进度与工作方式

> **这份是滚动状态，每次收工必须更新。**
>
> - `PROJECT.md` = 稳定基准（架构 / 设计决定 / 规划 / 开发流程）——不常变
> - **`STATE.md`（本文件）= 现在到哪了、该怎么干活** ——每次干完就改
>
> **给"上下文被清空后的自己"看。** 读的顺序：先读这份，再按需回 `PROJECT.md` 查细节。
>
> 最后更新：2026-09-16（2.5 三腿复用 ✅，下一个动作 = 2.5b 交付物 UTF-8 净化）

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
| | **2.5 三腿复用接知识库** | ✅ 完成 |
| | **2.5b 交付物 UTF-8 净化** | ← **下一个动作**（2.5 验证时发现的真缺陷，见 §四） |
| | 2.6 `--ask` 检索 | 未开始 |
| | 2.7 Action 跨会话追踪 | 未开始 |
| **第 3 阶段** 遗留质量项 | 3.1 分句碎片化 / 3.2 场景判定 / 3.3 Evidence / 3.4 静音收尾裁尾 | 未开始 |
| **第 4 阶段** 桌面产品化 | 4.1 默认路径 / 4.2 key 存储 / 4.3 GUI 壳 | 未开始 |

### 下一个动作（具体到文件）

**2.5b 交付物 UTF-8 净化**（2.5 验证时发现的，**与 2.5 无关，单独一笔**）

- 【现象】用 `--summarizer local`（混元）生成的 `meeting-42.md` **整体不是合法 UTF-8**。
  字节级证据：`### 询问你\xe7` —— `\xe7` 是一个没写完的三字节首字节；
  要点下一行还有孤立的续字节 `\x84`。任何编辑器/浏览器打开都是乱码。
- 【原因】混元是翻译模型，被逼着做结构化输出时会吐出**半个字符的 token**；
  而 `DeliverableWriter` **不做任何校验就写盘了**。
- 【修法】加一个纯函数 `utf8_sanitize()`：
  - 非法字节序列替换成 U+FFFD（或用 `?`），保证输出文件**永远是合法 UTF-8**
  - 位置放在 `DeliverableWriter::write()` 的**写出边界**上（一次覆盖 md/html/csv/srt 四条）
  - 另外在 `LlmSummarizer::parse_reply` 里也过一遍，免得垃圾进到后续逻辑
  - 同时按"绝不静默给错数据"的规矩：净化发生时 `std::cerr` 报一行，别悄悄改
- 【为什么单独一笔】它不由 2.5 引入，也不属于 2.5 的验收范围；
  混提交违反"一次改动一次提交"

之后才是 **2.6 `--ask` 检索**（`KnowledgeStore::search()` 已经就绪，只差暴露成命令行）。

### 其它状态

| | |
|---|---|
| 分支 | `main`（本地主线）；远程备份在 `assistant-baseline` |
| 远程 `main` | `da97d96` —— **有意不动**，详见 `PROJECT.md` §3.6 末「历史决策：不合并 da97d96」 |
| tag | `translator-final` → `7717ace`（翻译版本封存） |
| 自检 | **22 组**，秒级，不需要模型 |
| 闭环八步完成度 | 八步**等权平均 78%**（听见 90 / 理解 90 / 提取 78 / 记忆 55 / 发现变化 55 / **询问 85** / **更新 85** / **再次利用 75**）<br>2.5 把"再次利用"从 30 拉到 75：知识现在真的会进识别提示、翻译约束、摘要背景，并已用真实失败句 A/B 证明译文被纠正 |

**"越用越懂你"的可验证里程碑**（第二句承诺）：

| 里程碑 | 状态 |
|---|---|
| 知识能落库、能区分 confirmed/candidate | ✅ 2.2 |
| 能发现"该问什么" | ✅ 2.3 |
| 用户答了能改进知识库 | ✅ 2.4 |
| **答过的东西真的改变识别/翻译/摘要行为** | ✅ **2.5**（翻译腿已用真实失败句 A/B 证明；识别腿只验到"prompt 真的送进引擎"，摘要腿只验到"背景真的进 user 内容"——见下方 ⚠️） |
| 会答的问题不重复问 | ✅ 2.4（`asked_count` + `kMaxAsks`） |

⚠️ **2.5 三条腿各自的验证级别不一样，别混为一谈**：

| 腿 | 验到了什么 | 没验到什么 |
|---|---|---|
| ① 识别提示 | prompt 字符串确实被构造并送进 `SpeechEngine`（日志 `[识别提示]`），且 candidate 不在里面 | **whisper 的识别结果是否真的因此变准** —— 需要"含会被识别错的专名的音频"，手上没有 |
| ② 翻译约束 | **行为级 A/B 通过**（`tools/ab_translation_constraint.py`）：真实失败句被纠正 | 云端翻译路径（我的环境没有 API key） |
| ③ 摘要背景 | 背景确实拼进送给模型的 user 内容（自检 + 日志 `[摘要] 已注入`） | **模型是否真的因此不与之矛盾** —— 本机没有 instruct 模型，混元做不了摘要 |

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
| 09-16 | 2.5 **差点用一个不现实的探针得出错误结论** | 我先把术语值设成模型绝不可能产出的 `马可波罗`，看到模型没照做，就下了"约束进了 prompt 但模型不服从"的结论。**换成真实失败句立刻翻过来了**：基线逐字复现历史失败，confirmed 组把 `埃里卡/马可` 纠正成 `Erica/Marco`。**探针的输入不现实，结论就不成立** |
| 09-16 | 2.5 **真实失败案例在另一个库里** | 我一直引用"会话 #11 的 Erica/埃里卡"，但在 `t.db` 里全库搜不到 —— 它其实在 `build\RelWithDebInfo\translations.db`（旧库，11 场 206 段）。仓库里有 **8 个 .db 文件**，`--db` 又是相对路径。**引用"真实案例"必须写清是哪个库**，否则下一个人（和下一个我）会以为记录是编的 |
| 09-16 | 2.5 三条腿的来源统一了 | 原来①识别提示②翻译约束吃 `--glossary` 文件、③摘要背景什么都没有 → 用户确认的知识一个字都影响不到输出。现在三处都经 `KnowledgeStore::constraint_items()` 取，**红线只有一处**。`--glossary` 保留为"手工预喂"的补充入口 |
| 09-16 | 2.5 name-like kind 必须和 fact/decision 分开 | `term`/`person`/`project` 的值是短名字，能进 initial_prompt；`fact`/`decision` 的值是**句子**，塞进去会诱发 whisper 提示回显（`looks_like_prompt_echo` 就是为这个写的），只适合当摘要背景 |
| 09-16 | 2.5 `--dump-prompt` 第二次差点撒谎 | 它原来只读 `--glossary` 文件。2.5 把来源改成知识库之后，这条路会显示"干干净净"的 prompt —— 而真实运行里全有。**已改成复用同一个取数函数**，两边不可能再走散 |
| 09-16 | 2.5 **交付物会被写成非法 UTF-8** | 混元做摘要时吐半个字符的 token（`\xe7` 未写完），`DeliverableWriter` 不校验就写盘 → 整个 .md 不是合法 UTF-8。**下一笔（2.5b）修** |
| 09-16 | 2.4 **是/否词表不能省** | 把 `n` 当成"用户输入的新值"会把字面量 `no` 写进库当专名，而且是 confirmed（会进翻译约束） |
| 09-16 | 2.4 **首尾不可见字符要按集合去** | 实跑抓到：PowerShell 管道第一行带 UTF-8 BOM，`"\uFEFFy"` 掉进"其余当新值"→ **把 `y` 存成了专名**。自检 13 个用例全绿，真跑才露出来 |
| 09-16 | 2.4 **"提问稀缺"要有个地方记"问过了"** | 加 `asked_count` + `kMaxAsks = 2`，**跳过也计数**。实测生命周期：第 1 场问 3 个全答完 → 第 2 场 1 个（最后一次机会）→ 第 3 场 0 个 |
| 09-16 | 2.4 **confirmed 的不能因历史而复活** | 规则①只看 `value_changed_demoted` 不看状态 → 用户刚答完的那条下一场又是第 1 问。加 `status != confirmed` |
| 09-16 | 2.3 **外部内容 FTS5 表必须用触发器维护** | 手动 `INSERT INTO knowledge_fts` 等于白写（外部内容表的 `count(*)` 读内容表，看着有行、MATCH 查不到）；`DELETE FROM knowledge_fts` 直接报 `database disk image is malformed` |
| 09-16 | 2.3 弱断言 = 假绿灯 | 旧自检只验"四张表在不在"，于是索引空的 bug 藏了两轮。改成"写进去→必须查回来→删掉→必须查不到"后**立刻抓出两个真 bug** |
| 09-16 | 2.2 **值变了必须把 confirmed 降回 candidate** | 原来那条确认是针对旧值的，值一变它就不成立（§6.5 要防的正是这个） |
| 09-16 | 2.1 迁移的边界 | `CREATE TABLE IF NOT EXISTS` 只补表、**永远不补列** —— 加列必须另走 `ALTER TABLE`（2.4 踩到） |

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
**MATCH 左侧不认表别名**；**PowerShell 5.1 把无 BOM 的 `.ps1` 当 GBK 读**（脚本里的中文串会被破坏，
所以驱动脚本用 Python 写，不用 .ps1）。

---

## 六、收工前自检

```powershell
# 0. 先杀进程，否则链接失败 LNK1168
Get-Process Translator -ErrorAction SilentlyContinue | Stop-Process -Force

cd C:\dev\projects\AudioTranslator

# 1. 构建
cmd /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"" >nul 2>&1 && set VCPKG_ROOT=C:\dev\vcpkg && cmake --build build --config RelWithDebInfo --target Translator"

# 2. L1 自检：必须 22 组全过
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
