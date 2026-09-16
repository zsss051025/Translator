# STATE · 进度与工作方式

> **这份是滚动状态，每次收工必须更新。**
>
> - `PROJECT.md` = 稳定基准（架构 / 设计决定 / 规划 / 开发流程）——不常变
> - **`STATE.md`（本文件）= 现在到哪了、该怎么干活** ——每次干完就改
>
> **给"上下文被清空后的自己"看。** 读的顺序：先读这份，再按需回 `PROJECT.md` 查细节。
>
> 最后更新：2026-09-16

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
| | **2.2 `KnowledgeStore`** | ← **下一个动作** |
| | 2.3 缺口检测四条规则 | 未开始 |
| | 2.4 结束时确认交互 | 未开始 |
| | 2.5 三腿复用接知识库 | 未开始 |
| | 2.6 `--ask` 检索 | 未开始 |
| | 2.7 Action 跨会话追踪 | 未开始 |
| **第 3 阶段** 遗留质量项 | 3.1 分句碎片化 / 3.2 场景判定 / 3.3 Evidence / 3.4 静音收尾裁尾 | 未开始 |
| **第 4 阶段** 桌面产品化 | 4.1 默认路径 / 4.2 key 存储 / 4.3 GUI 壳 | 未开始 |

### 下一个动作（具体到文件）

**2.2 `KnowledgeStore`**：

- 新建 `inc/KnowledgeStore.h` + `src/KnowledgeStore.cpp`
- `normalize_key()`：把专名归一成查询键（大小写、空格、标点、全半角）
- upsert：同 `(kind, key)` 只保留一个当前值，旧值写进 `knowledge_history`
- Confirmed ↔ Candidate 提升（红线：**Candidate 绝不进识别提示和翻译约束**，见 `PROJECT.md` §6.5）
- **纯函数优先** —— 归一化和提升规则都要能进 L1 自检（§8.2：否则验证要从秒级变分钟级，人就会跳过验证）

### 其它状态

| | |
|---|---|
| 分支 | `main`（本地主线）；远程备份在 `assistant-baseline` |
| 远程 `main` | `da97d96` —— **有意不动**，详见 `PROJECT.md` §3.6 末「历史决策：不合并 da97d96」 |
| tag | `translator-final` → `7717ace`（翻译版本封存） |
| 自检 | **15 组 54 例**，秒级，不需要模型 |
| 闭环八步完成度 | 约 **35%**（听见 90 / 理解 90 / 提取 78 / 记忆 10 / 后四步 0） |

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
  `WavReader::parse`、`ITranslator::glossary_constraint`。
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
| 09-16 | 2.1 FTS5 + 三张表 | `CREATE TABLE IF NOT EXISTS` 顺带就是迁移，老库打开自动补表。FTS5 宏没定义时**建表会直接失败**，所以"能建表"本身就是宏生效的实证 |
| 09-16 | 截止日期依据改用整场转录 | **"无法判断" ≠ "无依据"**：云端路径 `ActionItem.source` 是空的（LCS 跨语言匹配不上），只看 source 会把所有日期清空。上一次的"修复"就是栽在这 |
| 09-16 | 术语约束译文（2 腿 → 3 腿） | A/B 复现并修好会话 #11 的 `Erica / 埃里卡`。**但第三条腿依赖第二条腿**：拼写不一致时约束不命中 |
| 09-16 | `--wav` 分块改 10ms → 100ms | 实时路径每轮睡 100ms，一次 `get_buffer_and_clear()` 实际拿到约 100ms 音频。按 10ms 喂会让"多少块连续静音"的语义差 10 倍，**A/B 前提不成立** |
| 09-16 | `--wav` 跳过主循环尾部那句 sleep | 11 秒素材跑了 112 秒（0.1 倍速）。**改完 20 秒** |
| 09-16 | `--wav` 文件放完要喂静音收尾 | `stop()` 是"先判 `is_running_` 再取队列"，直接 break 会丢掉队列里没推理的段 |
| 09-14 | 假行动项校验器 | 走**纯函数校验器**而不是只改 prompt。放在三条摘要路径的汇合点，`write()` 内再兜一层 |
| 09-14 | 仓库基线提交 | 之前 3.5 个月没提交，没有任何回退点。现已分 10 笔补上 + tag `translator-final` |
| 09-14 | 远程与本地分叉 | 远程 `main` 有 `da97d96`（Linux/WAV），其中把 `ma_device_type_loopback` **无条件**改成 `capture` —— 合并会毁掉 Windows 的系统音频采集。**决定不合并** |
| 09-14 | 平台边界定论 | 只做 Windows 桌面版。采集和字幕窗是平台专有，Wayland 下置顶字幕可能做不到。要 Linux 就做**无界面批处理版**（`--wav` 是它的地基） |

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

---

## 六、收工前自检

```powershell
# 0. 先杀进程，否则链接失败 LNK1168
Get-Process Translator -ErrorAction SilentlyContinue | Stop-Process -Force

cd C:\dev\projects\AudioTranslator

# 1. 构建
cmd /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"" >nul 2>&1 && set VCPKG_ROOT=C:\dev\vcpkg && cmake --build build --config RelWithDebInfo --target Translator"

# 2. L1 自检：必须 15 组全过
.\build\RelWithDebInfo\Translator.exe --selftest --db t.db

# 2b. 知识库数据层（第 2 阶段每步都要跑）——独立实现验证，且不改动原库
python tools\verify_memory.py t.db

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
