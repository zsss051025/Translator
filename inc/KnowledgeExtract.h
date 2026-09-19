#pragma once
#include <string>
#include <vector>

#include "KnowledgeStore.h"
#include "SessionStore.h"   // Segment

// 自动抽取 —— 见 PROJECT.md §6.4①
//
// **闭环的入口。** 在 2.6 之前，2.1~2.5 建的表、落库、找缺口、问用户、三条腿复用
// 全都建立在"库里已经有知识"这个假设上，而**没有任何代码把会话内容写进去** ——
// 实测跑完一场真实会话（16 段、DeepSeek 摘要成功），knowledge 表仍然是 0 行。
// 后果：一声都不问、三条腿一条都拿不到东西。这个文件补的就是这一环。
//
// ---------------------------------------------------------------------------
// 设计取向（§6.4① + §8.3）
// ---------------------------------------------------------------------------
//
// **抽取宁宽勿漏，判定宁严勿滥。** 所以这里只做"认出可能是专名"，
// 一律落成 **Candidate**，绝不直接进 Confirmed —— 猜错了也只是多个待确认项，
// 而 Candidate 进不了任何约束（§6.5 红线）。
//
// **不碰模型。** 让大模型抽实体成本高且不稳定，而"首字母大写 + 位置"这套规则
// 在英文转录上已经够用；而且规则版能进 L1（秒级可验证，不需要加载 3GB 模型）。
// 中文侧只处理"我叫X"这类明确的人称模式 —— 中文没有大小写，
// 想靠规则认专名不现实，那部分留给后面的模型路径（如果真需要）。

namespace knowledge {

// 一条抽出来的候选。
//
// 【为什么不用 KnowledgeItem 直接当输出】抽取器不该知道"怎么落库"——
// status/source_session 这些是落库语义。它只负责"从这段文本里认出了什么"。
struct ExtractedCandidate {
    std::string kind;                 // term | person
    std::string key;                  // 归一化键
    std::string value;                // 库里要存的写法（取最常见的那种拼写）
    int         hits       = 0;       // 本场出现几次
    double      confidence = 0.0;     // 取**最低**的那次识别置信度（保守）
    long long   source_session = -1;
    int         source_seq     = -1;
    std::string source_text;          // 第一次出现的原话（证据，不可省）

    // 给用户看的"为什么认为它是专名"，也是自检断言用得到的信息
    std::string why;
};

// **核心纯函数**：从会话段落里抽候选。不碰数据库、不碰模型、不依赖时间。
//
// 规则（英文转录）：
//   R1 首字母大写的词 —— **但句首的大写不算证据**，这是最容易出错的地方：
//      "Ask not what your country..." 里的 Ask/What 不是专名，
//      而 "My name is Marco." 里的 Marco 是。
//      判定：该词只要在**非句首**位置出现过一次，就算数。
//   R2 词中间还有大写（CamelCase）：EnglishPod / EchoMind —— 这是强信号，
//      即使只在句首出现也算。
//   R3 连续 ≥2 个大写字母的缩写：API / CRM / ASR。
//   R4 引号里的内容：「EchoMind」/《项目名》/"Phoenix"
//   R5 人称模式：my name is X / I'm X / I am X / this is X / 我叫X / 我是X → kind=person
//
// 明确**不做**的事（写下来免得以后当 bug 修）：
//   · 不抽多词专名（"New York"、"Li Wei"）—— 先只做单词，多词要处理跨段和连接词，容易误抽
//   · 不抽中文专名（中文没有大小写，规则认不准）
//   · 不判断"是不是真的专名"—— 那是用户在第 2.4 步确认时做的
std::vector<ExtractedCandidate> extract_candidates(const std::vector<Segment>& segs,
                                                   long long session_id,
                                                   size_t max_candidates = 40);

// 落库：**全部以 candidate 写入**，返回写入条数。
//
// 已存在的条目会走 upsert 的累加/降级语义（值不同 → 降级 + 写历史），
// 所以同一场重复跑不会产生垃圾行。
//
// ⚠️ 上面那句"不会产生垃圾行"在 2.12 之前**只对 kind 不变的条目成立**：
//    身份是 (kind, key)，而抽取器给的 kind 是形状猜测，会被分诊层改掉
//    （term → product），下一次抽取同一个词就又建了一行 —— 同一实体两行、
//    用户被问两遍、kMaxAsks 各算各的。现在这里会先 `find_by_key()` 认一次旧行
//    并沿用它的 kind，那句话才算真的成立。详见 KnowledgeStore::find_by_key 的说明。
int save_candidates(const std::vector<ExtractedCandidate>& cands, std::string* err = nullptr);

// 这一小段中文**看起来像人名吗**？
//   ① 以常见姓氏字开头，且不是"姓氏开头的常用词"（需要/于是/成为…），且名字部分没有虚词
//   ② 或者以称谓结尾（张总 / 李经理 / 王工）
//
// 【为什么把它公开出来，而不是让调用方自己判断】
// 交付物里的"负责人"抽取（`DeliverableWriter::find_owner`）要做的判断，
// 和抽取器的 R6 规则是**同一件事**：这一小段是不是人名。
// 那边原来自己写了个"从动词往前数 4 个汉字"，于是抽出这些垃圾：
//     「负责人: 了，张伟」   ← 往前数 4 个字，把标点也数进去了
//     「负责人: 这块张伟」   ← 多带了"这块"
//     「负责人: 我们需要」   ← "需要"以姓氏字"需"?不是 —— 是它压根没判断是不是人名
// 而且它不知道这里有姓氏表和"姓氏开头的常用词"表。
// 本项目"两份实现迟早走散"已经栽过四次，所以公开**谓词**，不复制判断。
//
// 判据刻意保守（宁可返回 false）：返回 false 的后果只是"负责人留空"，
// 而返回 true 判错的后果是**纪要里写着一个不存在的人**。
bool looks_like_person_name(const std::string& s);

}  // namespace knowledge
