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

    // ---- 还没锁定：攒够 N 段一致才锁 ----
    //
    // ⚠️ 首次锁定也要确认，不能"第一次判什么就锁什么"：
    //    开场往往是音乐/片头，那一段的语言判断同样不可靠。
    //    （这正是会话 #2 那种素材最容易踩的地方。）
    if (current.empty()) {
        if (*pending == detected) {
            ++(*count);
        } else {
            *pending = detected;
            *count   = 1;
        }
        if (*count >= threshold) {
            *pending = std::string();
            *count   = 0;
            return Action::Lock;
        }
        return Action::None;
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
