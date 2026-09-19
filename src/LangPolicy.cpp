#include "LangPolicy.h"

namespace langpolicy {

Action decide(const std::string& detected,
              const std::string& current,
              std::string* pending,
              int* count,
              int need) {
    if (pending == nullptr || count == nullptr) return Action::None;
    if (detected.empty()) return Action::None;      // 检测失败，不动状态

    const int threshold = need > 0 ? need : 1;

    // ---- 还没锁定：**一段就锁** ----
    //
    // ⚠️ 这里我原本也要求"连续 N 段一致"，理由是"开场往往是音乐，判断不可靠"。
    //    实测把这个理由否掉了 —— **代价远大于收益**：
    //
    //      --wav jfk.wav（11 秒）三种配置的转录：
    //        --lang en 固定            → 3 段 / 106 字符（最完整，连开头都在）
    //        首段检测后锁定（旧行为）   → 2 段 /  79 字符
    //        前 3 段都检测（先前的改法）→ 1 段 /   8 字符  ← 内容几乎丢光
    //
    //    原因：**`auto` 逐段检测本身就在损害识别** —— Whisper 要在那 3~4 秒里
    //    同时判语言和转写，判错的概率不低（代码别处早写着"3 秒碎片上模型
    //    经常只有 20% 把握"）。所以"多检测几段"不是稳妥，是**多毁几段**。
    //
    // 结论：**锁定要快，切换要慢**。
    //   · 首次锁定：一段就锁（少走 auto）
    //   · 之后切换：必须连续 N 段一致（防音乐段把它带偏 —— 那才是真正的事故）
    // 判错语言的代价由"切换机制"兜住；而 delayed lock 的代价没有兜底。
    if (current.empty()) {
        *pending = std::string();
        *count   = 0;
        return Action::Lock;
    }

    // ---- 已锁定：判回锁定语言 → 候选清零 ----
    if (detected == current) {
        *pending = std::string();
        *count   = 0;
        return Action::None;
    }

    // ---- 已锁定：判出别的语言 → 记候选，够 N 段才切 ----
    if (*pending == detected) {
        ++(*count);
    } else {
        *pending = detected;
        *count   = 1;
    }
    if (*count >= threshold) {
        *pending = std::string();
        *count   = 0;
        return Action::Switch;
    }
    return Action::None;
}

bool should_probe(const std::string& pending) {
    // 有候选就每段都检测 —— 否则"连续 N 段"会变成"连续 N 个重检周期"：
    // 重检周期默认 120 秒，N=3 就成了 6 分钟才切换，那太迟钝了。
    return !pending.empty();
}

}  // namespace langpolicy
