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
// 【四件事必须说清楚，缺一个模型就会答歪】
//   ① 它**不是**在回答问题，是在做"该不该占用用户注意力"的判决
//   ② 「通用」的标准是"任何行业里的成年人都知道"，不是"我见过这个词"
//      （不说清楚的话，模型倾向于把所有词都判成"通用"，于是什么都不问 → 闭环死掉）
//   ③ **必须结合 evidence 里那句话判断** —— 这条是 2026-09-19 补的，
//      用户的例子正好证明了它是必需的：
//          单独的 `penny` 是通用词（便士）→ 会被判"不问"
//          但「我们的 PM Penny 说…」里的 `Penny` 是花名 → **必须问**
//      我原来只把"词"传给模型，还在注释里写"人名判断不依赖长上下文"——
//      那句话是错的，而且后果是**静默挡掉用户最想被问的那类词**。
//   ④ 猜含义只是**给人一个确认的引子**，猜不出就留空 ——
//      不许编。用户会看到这个猜测，编出来的会误导他。
//      而且是**组织内部私有名字**时特别容易编（"Penny 是财务负责人"），
//      所以要单独强调一句。
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
    u8"⚠️ **必须结合 evidence 里那句话判断，不要只看这个词本身。**\n"
    u8"   同一个写法在不同句子里可能是完全不同的东西：\n"
    u8"     · 孤立的 `penny` 是通用词（便士）→ 不值得问\n"
    u8"     · 但「我们的 PM Penny 说…」里的 `Penny` 是个人名 → **值得问**\n"
    u8"   evidence 就是它出现的那句原话。这一条比什么都重要。\n"
    u8"\n"
    u8"⚠️ 不确定时一律判 general = false（值得问）。"
    u8"少问一次的损失比多问一次大得多。\n"
    u8"\n"
    u8"另外顺便判断它的**类型**，这决定我们用什么问法：\n"
    u8"  person  —— 人的名字（花名、昵称、英文名都算）\n"
    u8"  project —— 项目的名字\n"
    u8"  product —— 产品 / 工具 / 内部系统 / 方案的名字\n"
    u8"  term    —— 技术术语、概念、缩写\n"
    u8"  判断不了就留空字符串。\n"
    u8"\n"
    u8"只输出 JSON，不要任何其它文字：\n"
    u8"{\"general\": true/false, "
    u8"\"kind\": \"person\"|\"project\"|\"product\"|\"term\"|\"\", "
    u8"\"guess\": \"如果这是个你认识的专有概念，写出它的含义 / 用途 / 身份（一句话、中文）。"
    u8"**不要以这个词本身开头**，直接写它的意思（比如写'播客，一种数字音频节目'，"
    u8"而不是'PodCast 是播客'）。"
    u8"**如果是他们组织内部私有的名字（你不认识），必须留空字符串，绝对不要编**"
    u8"\", "
    u8"\"why\": \"一句话说明理由（中文）\"}";

}  // namespace

Decision decide(const std::string& value,
                const std::string& kind,
                const std::string& evidence,
                const Judge& judge,
                const Cache& cache) {
    Decision d;

    // ---- 第 ① 层：本地通用词表（确定性、离线、零延迟）----
    //
    // 放在最前面不是"优化"，是**顺序即语义**：这一层的结论不依赖任何外部状态，
    // 所以它永远一致。让不确定的判断先去问模型，会让同一句话在不同时间
    // 得到不同结论 —— 那种不可复现比"多问一次"糟得多。
    //
    // 同理，它也排在缓存**前面**：本地表的答案是确定的，而缓存是"以前的模型判决"。
    // 让一个可能过时的缓存去覆盖本地表的确定结论，等于把确定性换成了历史偶然。
    // （而且本地表命中时根本不需要读缓存，省一次查询。）
    if (commonwords::is_general(value)) {
        d.verdict = Verdict::Skip;
        d.source  = "common_words";
        d.reason  = u8"本地通用词表命中（人人皆知，问了没价值）";
        return d;
    }

    // ---- 第 ② 层：判断缓存（离线、零延迟）----
    //
    // 命中就**不再联网**。这一层是"越用越省、断网也能用"的落点：
    // 第一次遇到某个普通词问一次模型，之后永远本地解决。
    if (cache) {
        CachedVerdict cv;
        if (cache.lookup(value, &cv)) {
            d.verdict = cv.verdict;
            d.source  = "cache";
            // 【理由里必须能看出来这是缓存的旧结论】否则看日志的人会以为
            // 模型刚刚又判了一次 —— 那会让人误以为网络是通的。
            d.reason  = cv.why.empty()
                            ? u8"判断缓存命中（以前判过，未再联网）"
                            : (u8"判断缓存命中（" + cv.why + u8"）");
            d.kind    = cv.kind;
            d.primer  = cv.primer;
            return d;
        }
    }

    // ---- 第 ③ 层：模型判断（judge 为空 = 这一层不存在）----
    if (!judge) {
        d.verdict = Verdict::Ask;
        d.source  = "fallback";
        d.reason  = u8"没有可用的判断器（本地表与缓存都没命中）→ 默认问";
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
        // ⚠️ **这里刻意不写缓存，而且这个 return 就在写缓存那段之前 —— 顺序本身就是闸。**
        //
        // 反例的具体后果：某次网络抖动 → 模型调用失败 → 若把这次失败当判决存下来，
        // 那个词**以后永远不再被问**，而用户看不到任何报错。
        // 所以「判断失败」和「模型说不问」必须在这里就分开，不能靠调用方记得判断。
        // 自检 ⑩(c) 专门守这一条（故意破坏它，自检必须报错）。
        return d;
    }

    // 模型给的结论也要过一遍本地表：如果它说"通用"而本地表没收录，
    // 以模型为准（本地表本来就是不够全的补丁）；
    // 但如果它说"不通用"而**本地表明确收录了**，那走到这里不可能 ——
    // 上面已经返回了。所以这里只做搬运。
    from_model.source = "model";

    // ---- 落缓存：**唯一的写入口，条件写死在这里** ----
    //
    // 两个条件缺一不可，且都在这一处判定：
    //   · `Skip` —— 只有"不问"值得缓存（Ask 由 KnowledgeGap 的"问过不再问"负责）
    //   · 能走到这一行，就说明 source 必然是 "model"（上面所有非 model 的分支都 return 了）
    //
    // 【为什么把闸放在这里而不是缓存实现里】放在调用方（main）就等于每个调用点
    // 都要记得判一次；放在缓存实现里，实现就无从知道这个判决是从哪来的。
    // 放在 decide() 里，它是**编排的一部分**，和"先本地表再缓存再模型"同一个地方，
    // 也就只有这一处需要理解。
    if (from_model.verdict == Verdict::Skip && cache && cache.store) {
        cache.store(value, kind, from_model);
    }
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
            // 类型 —— 只影响标签和问法，不碰 status/hits（见 Decision::kind 的说明）。
            // 校验一遍再收：模型可能吐出 "Person" 或 "人物" 这种非白名单值，
            // 而 kind 要进数据库、要参与条目身份，不能放进非法值。
            {
                const std::string k = verdict.value("kind", std::string());
                if (k == "person" || k == "project" ||
                    k == "product" || k == "term") {
                    out->kind = k;
                }
            }
            return true;
        } catch (const std::exception&) {
            // 解析失败 = 判断失败 → 调用方默认"问"。
            // **不要**在这里 log 一句就算了然后返回 true。
            return false;
        }
    };
}

}  // namespace triage
