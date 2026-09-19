#pragma once
#include <string>
#include <vector>

struct Segment;   // 前向声明 —— 只有 resolve() 需要它，头文件不拖 SessionStore.h

// ===========================================================================
// 出处（§7 步骤 5.7）
// ===========================================================================
//
// 【为什么"随口说出处"不算做了这件事】
// 一份纪要里写「张伟负责重写接口文档」，读者要么信、要么回去翻 117 段转录。
// 没有可点的出处，这句话的价值就只等于"有人这么说过" —— 而"可验收"
// 正是评审看重的那个维度。所以这一节要做的不是"加一句出处"，而是：
//
//   ① **引用有唯一格式**（下面 format）。报告、CSV、控制台、agent 的回答
//      都打同一个形状的字符串。
//   ② **引用能被机器核对**（下面 verify）：回去查那一段到底存不存在。
//      ⚠️ 这才是关键 —— **模型编出来的出处长得和真的一模一样**。
//      不核对的话，"带出处的报告"反而比"不带出处"更危险：
//      它看着更可信，而其中一部分是凭空生成的。
//
// 所以本模块的一半是"打印出处"，另一半是"**证伪**出处"。
//
// ---------------------------------------------------------------------------
// 【格式为什么长这样】
//      #<会话>·<段>        例如  #9002·3
//   · `#` 开头：在 markdown / 网页里天然能当锚点用（`<a href="#seg-3">`）
//   · `·`（U+00B7）分隔：**中文正文里绝不会自然出现**，所以从一段散文里
//     捞引用不会误抓（用 `:` 或 `-` 都会，正文里的时间、书名号里到处都是）
//   · 会话号在前：跨会话的报告里，同一段号在不同场里是不同的东西
//
// 【唯一格式这件事为什么必须写死在代码里】只要有第二处各写各的，
// 就会立刻出现最糟的组合：**人看到的引用能点开，机器校验的那个点不开** ——
// 于是校验器把正确引用判成无效，或者更坏，把编造的判成有效。
// 本项目"两份实现迟早走散"已经栽过四次。

namespace evidence {

struct Locator {
    long long session_id = -1;
    int       seq = 0;

    bool valid() const { return session_id >= 0 && seq > 0; }
    bool operator==(const Locator& o) const {
        return session_id == o.session_id && seq == o.seq;
    }
};

// 唯一格式：`#9002·3`
std::string format(const Locator& loc);

// 从一段文本里认出所有引用，按出现顺序、去重后的结果。
// 认不出任何引用时返回空 —— **空不是错误**（很多结论本来就没有出处，
// 比如"这次会议讨论了三个主题"）。
std::vector<Locator> extract(const std::string& text);

// 核对结果
struct VerifyResult {
    int                  total = 0;   // 认出几处
    int                  ok = 0;      // 其中几处指向真实存在的段落
    std::vector<Locator> bad;         // 对不上的 —— **这就是"编造出处"的证据**
    std::vector<Locator> dup;         // 重复出现的（不算错，但报告里会提一句）
};

// 核对用的存储访问。**单独一个类**，理由和 ActionStore 一样：
// 它要碰 SessionStore 的私有连接，而"哪些函数需要真库"应该从 API 上看出来。
// `format` / `extract` 是纯函数，自检不建库就能验（引用格式错了最难查，
// 因为它只在"有人真去看报告"的时候才暴露）。
class Evidence {
public:
    // 位置是否存在。存在时带回那一段（time/文本都在里面）。
    static bool resolve(const Locator& loc, Segment* out);

    // 那一场的段落数（用于判断"段号越界"和给出有效范围）
    static int segment_count(long long session_id);

    // 核对一段文本里的全部引用。
    static VerifyResult verify(const std::string& text);

    // 给人看的出处一行：`#9002·3（00:02:00）`；位置不存在时明确写出来
    // **不假装它是对的** —— 编造的出处必须在报告里就露出来。
    static std::string describe(const Locator& loc);
};

}  // namespace evidence
