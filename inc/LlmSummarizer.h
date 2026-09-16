#pragma once
#include <functional>
#include <string>
#include <vector>

#include "SessionStore.h"
#include "DeliverableWriter.h"   // 复用 MeetingSummary / TopicSummary / ActionItem

// 用大模型生成会议摘要与行动项，替代规则抽取。
//
// 两条后端：
//   Local —— 复用本地混元模型，内容不出本机（与产品的隐私定位一致）
//   Cloud —— 调用 DeepSeek，摘要质量更好，但本场转录会上云
//
// 失败（模型不按 JSON 输出、网络错误等）时返回 false，
// 由调用方回退到 DeliverableWriter::extract_by_rules()，保证导出永不失败。
class LlmSummarizer {
public:
    enum class Backend { Local, Cloud };

    // 本地后端的生成函数由调用方注入（通常是 HunyuanTranslator::generate_once），
    // 这样本类不需要直接依赖 llama.cpp。
    using GenerateFn = std::function<bool(const std::string& system,
                                          const std::string& user,
                                          std::string& out)>;

    explicit LlmSummarizer(Backend backend) : backend_(backend) {}

    void set_generate_fn(GenerateFn fn) { gen_ = std::move(fn); }
    void set_api_key(const std::string& key) { api_key_ = key; }
    void set_target_language(const std::string& lang) { target_lang_ = lang; }

    // 已确认的背景知识（来自知识库，§7 步骤 2.5 第三条腿）。
    //
    // 每条形如 `- Erika（人名）`。它们**拼在 user 内容里，不进 system 提示**：
    // 那是"数据"不是"指令"，混进 system 会削弱格式约束，而本地 1.8B 模型
    // 本来就容易把 system 里的示例当答案抄回来（见 build_system_prompt 的说明）。
    //
    // 调用方必须已经用 KnowledgeStore::constraint_items() 过滤过 ——
    // candidate 绝不允许出现在这里（§6.5 红线）。
    void set_background(std::vector<std::string> lines) { background_ = std::move(lines); }

    // 成功时填充 out 并返回 true
    bool summarize(const std::vector<Segment>& segs, MeetingSummary& out, std::string& err);

    const char* name() const;

    // 组装给模型的 system / user 内容（公开出来便于单测与调参）
    std::string build_system_prompt() const;
    static std::string build_transcript(const std::vector<Segment>& segs);

    // user 内容 = 背景知识（可选）+ 转录。公开出来是为了让自检能直接断言
    // "背景知识有没有真的进到 prompt 里" —— 而不是只看配置项设了没有。
    std::string build_user_content(const std::vector<Segment>& segs) const;

    // 从模型回复里解析出 MeetingSummary。
    // 本地后端用结构化纯文本，云端后端用 JSON —— 见 .cpp 里的说明。
    bool parse_reply(const std::string& raw, MeetingSummary& out, std::string& err) const;

private:
    bool call_model(const std::string& system, const std::string& user,
                    std::string& raw, std::string& err);

    Backend     backend_;
    std::string api_key_;
    std::string target_lang_ = "zh";
    GenerateFn  gen_;
    std::vector<std::string> background_;
};
