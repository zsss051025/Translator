#include "CandidateTriage.h"

#include <iostream>

#include "CaBundle.h"   // find_ca_bundle()：Windows 上没有默认 CA 路径
#include "CommonWords.h"
#include "httplib.h"
#include "json.hpp"

// ⚠️ 注意这个文件**没有** include KnowledgeStore.h / SessionStore.h ——
// 头文件里说明了原因：分诊层没有任何写知识库的能力，这是红线的结构性保证。

namespace triage {

namespace {

using nlohmann::json;

// 云端判断器的 system prompt。
//
// 【三件事必须说清楚，缺一个模型就会答歪】
//   ① 它**不是**在回答问题，是在做"该不该占用用户注意力"的判决
//   ② 「通用」的标准是"任何行业里的成年人都知道"，不是"我见过这个词"
//      （不说清楚的话，模型倾向于把所有词都判成"通用"，于是什么都不问 → 闭环死掉）
//   ③ 猜含义只是**给人一个确认的引子**，猜不出就留空 ——
//      不许编。因为用户会看到这个猜测，编出来的会误导他。
const char* kSystemPrompt =
    u8"你在帮一个会议助手判断：某个从会议转录里抽出来的词，"
    u8"值不值得在会议结束时占用用户的注意力去问一句。\n"
    u8"\n"
    u8"判断标准（按顺序）：\n"
    u8"1. 如果这个词是【任何行业里的成年人都知道】的通用词、常识缩写、"
    u8"普通英语单词 → 不值得问（general = true）。\n"
    u8"   例：TV / PC / API / people / exactly / together 都是通用词。\n"
    u8"2. 如果它看起来像【某个组织、项目、产品或人】特有的名字 → 值得问。\n"
    u8"   例：EnglishPod / WGBH / CO-RE / 凤凰项目 / Marco。\n"
    u8"3. 如果它**看起来像识别错误**（比如两个英文单词被粘成一个，"
    u8"CarsPacked 这种）→ 仍然算值得问（general = false），"
    u8"因为用户需要确认这个词到底存不存在。\n"
    u8"\n"
    u8"⚠️ 不确定时一律判 general = false（值得问）。"
    u8"少问一次的损失比多问一次大得多。\n"
    u8"\n"
    u8"只输出 JSON，不要任何其它文字：\n"
    u8"{\"general\": true/false, "
    u8"\"guess\": \"如果这是个你认识的专有概念，写出它的含义（一句话、中文）。"
    u8"**不要以这个词本身开头**，直接写它的意思（比如写'播客，一种数字音频节目'，"
    u8"而不是'PodCast 是播客'）；"
    u8"不认识或不确定就留空字符串\", "
    u8"\"why\": \"一句话说明理由（中文）\"}";

}  // namespace

Decision decide(const std::string& value,
                const std::string& kind,
                const std::string& evidence,
                const Judge& judge) {
    Decision d;

    // ---- 第 ① 层：本地通用词表（确定性、离线、零延迟）----
    //
    // 放在最前面不是"优化"，是**顺序即语义**：这一层的结论不依赖任何外部状态，
    // 所以它永远一致。让不确定的判断先去问模型，会让同一句话在不同时间
    // 得到不同结论 —— 那种不可复现比"多问一次"糟得多。
    if (commonwords::is_general(value)) {
        d.verdict = Verdict::Skip;
        d.source  = "common_words";
        d.reason  = u8"本地通用词表命中（人人皆知，问了没价值）";
        return d;
    }

    // ---- 第 ③ 层：模型判断（judge 为空 = 这一层不存在）----
    if (!judge) {
        d.verdict = Verdict::Ask;
        d.source  = "fallback";
        d.reason  = u8"没有可用的判断器（本地表也没命中）→ 默认问";
        return d;
    }

    Decision from_model;
    const bool ok = judge(value, kind, evidence, &from_model);
    if (!ok) {
        // **判断失败一律退化成"问"。**
        //
        // 【为什么必须是这个方向】反过来（失败就不问）会造成最坏的一种失败：
        // 网络断了 → 系统**悄悄停止学习** → 用户完全看不出来，
        // 只会觉得"这东西好像没在记东西"。而"多问一次"的代价只是他按个回车。
        // 一个坏掉的判断器不该有能力关掉整个闭环。
        d.verdict = Verdict::Ask;
        d.source  = "fallback";
        d.reason  = u8"模型判断失败（网络/超时/解析）→ 默认问";
        return d;
    }

    // 模型给的结论也要过一遍本地表：如果它说"通用"而本地表没收录，
    // 以模型为准（本地表本来就是不够全的补丁）；
    // 但如果它说"不通用"而**本地表明确收录了**，那走到这里不可能 ——
    // 上面已经返回了。所以这里只做搬运。
    from_model.source = "model";
    return from_model;
}

Judge make_cloud_judge(const std::string& api_key, int timeout_sec) {
    if (api_key.empty()) {
        // 没有 Key → 返回一个**空** judge。调用方（decide）会走 fallback 分支
        // 并默认"问"。这比返回一个"永远说不值得问"的 judge 安全得多 ——
        // 后者会让整个闭环在你没配 Key 的时候静默停摆。
        return nullptr;
    }

    return [api_key, timeout_sec](const std::string& value,
                                  const std::string& kind,
                                  const std::string& evidence,
                                  Decision* out) -> bool {
        if (!out) return false;

        // 【只发候选词 + 一句证据，**不发整场转录**】
        // 这是这一层相对"摘要上云"最大的区别：隐私面小一两个数量级，
        // 成本也可以忽略（一个词的 prompt）。演示时这一点值得明说。
        json user_msg = {
            {"word",     value},
            {"kind",     kind},          // term / person / project
            {"evidence", evidence},      // 它在转录里出现的那一句话
        };

        httplib::Client cli("https://api.deepseek.com");
        cli.set_connection_timeout(5, 0);
        cli.set_read_timeout(timeout_sec, 0);

        // 同 LlmSummarizer / DeepSeekTranslator：Windows 上 OpenSSL 没有默认 CA 路径，
        // 不显式指定的话 HTTPS 校验必定失败。
        const std::string ca = find_ca_bundle();
        if (!ca.empty()) cli.set_ca_cert_path(ca);

        json payload = {
            {"model", "deepseek-chat"},
            {"messages", json::array({
                {{"role", "system"}, {"content", kSystemPrompt}},
                {{"role", "user"},
                 {"content", user_msg.dump(-1, ' ', false, json::error_handler_t::replace)}}
            })},
            {"response_format", {{"type", "json_object"}}},
            {"temperature", 0.2}     // 判决要稳，不要"创意"
        };
        httplib::Headers headers = {
            {"Authorization", "Bearer " + api_key},
            {"Content-Type", "application/json"}
        };

        auto res = cli.Post("/chat/completions", headers,
                            payload.dump(-1, ' ', false, json::error_handler_t::replace),
                            "application/json");
        if (!res || res->status != 200) return false;   // → 调用方退化成"问"

        try {
            const auto j = json::parse(res->body);
            const auto content = j["choices"][0]["message"]["content"].get<std::string>();
            const auto verdict = json::parse(content);

            // ⚠️ 判据只有"通用/不通用"这一个。**模型的 guess 不写库**，
            // 它只作为问题的引子（见头文件的红线说明）。
            const bool general = verdict.value("general", false);
            out->verdict = general ? Verdict::Skip : Verdict::Ask;
            out->reason  = verdict.value("why", std::string());
            // 引子照搬，但**只是引子** —— 见 Decision::primer 的说明。
            // 空字符串 = 模型不认识这个词，那就照旧问开放式的。
            out->primer  = verdict.value("guess", std::string());
            return true;
        } catch (const std::exception&) {
            // 解析失败 = 判断失败 → 调用方默认"问"。
            // **不要**在这里 log 一句就算了然后返回 true。
            return false;
        }
    };
}

}  // namespace triage
