#pragma once
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

// 半透明置顶字幕窗。
//
// 为什么需要它：控制台窗口会挡住正在看的视频，而用户要"边看画面边看字幕"。
// 这个窗口是分层（layered）的：半透明、可拖动、可拉伸、不抢焦点、不进任务栏。
//
// 它还负责注册**系统级全局热键**。原因：用户看全屏视频时控制台没有焦点，
// 用 _kbhit() 根本收不到按键，"按键结束"就无从谈起。
//
// 两种显示模式：
//   实时模式（默认）：三行
//     ① 上一句译文（小字、暗）—— 提供连续性，让字幕"滚动"而不是"跳变"
//     ② 当前句原文（中字、灰）
//     ③ 当前句译文（大字、亮）—— 主体；还没翻好时留空
//   历史回看模式（鼠标滚轮上滚进入）：
//     整个窗口变成可滚动的译文列表，一句一块（译文在上、原文在下），
//     滚回到底部自动回到实时模式。
class SubtitleWindow {
public:
    SubtitleWindow() = default;
    ~SubtitleWindow();

    SubtitleWindow(const SubtitleWindow&) = delete;
    SubtitleWindow& operator=(const SubtitleWindow&) = delete;

    // 创建窗口并启动消息线程
    bool start();

    // 关闭窗口并结束消息线程
    void stop();

    // 以下方法线程安全，可从任意线程调用。
    void set_original(const std::string& text);            // ② 当前句原文
    void set_translation(const std::string& text);         // ③ 当前句译文（空 = 还没翻好）
    void set_status(const std::string& text);

    // 记录一对已经完成的"原文 → 译文"，供滚轮回看。
    // 由调用方在翻译完成时投递（调用方才知道这一句的原文是什么）。
    void push_history(const std::string& source, const std::string& translation);

    // 用户是否按下了结束热键
    bool quit_requested() const { return quit_requested_.load(); }

    // 内部接口：低级鼠标钩子回调与 WM_MOUSEWHEEL 都走这里。
    // screen_x / screen_y 是屏幕物理坐标，delta 是滚轮增量（一格 = 120）。
    // 返回 true 表示这次滚轮被字幕窗消化掉了，调用方应当拦截、不再传给其它程序。
    bool handle_wheel_at(int screen_x, int screen_y, int delta);

    // 热键提示文字，供命令行输出
    static const char* hotkey_hint();

private:
    // 一条已完成的历史记录
    struct Entry {
        std::string source;
        std::string translation;
    };

    void thread_main();
    void on_paint();
    void post_repaint();
    void update_title();

    // 处理一次滚轮：notches > 0 表示上滚（往更早的历史翻）。
    bool on_wheel(int notches);

    // 最近的历史条数上限。用户可能连看两小时，不设上限内存会一直涨。
    static constexpr size_t kMaxHistory = 500;

    void* hwnd_raw() const { return hwnd_.load(); }

    // 窗口句柄：窗口线程写，其它线程读，所以必须是原子的
    std::atomic<void*> hwnd_{nullptr};
    std::thread       thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> quit_requested_{false};
    std::atomic<bool> started_{false};

    // 窗口 DPI，创建时确定。WM_NCHITTEST 里算边框宽度要用，
    // 而鼠标每动一下都会进来一次，不能每次都去查 DC。
    std::atomic<int>  dpi_{96};

    // 滚轮防重复：低级鼠标钩子和 WM_MOUSEWHEEL 两条路都可能送来同一个滚轮，
    // 记一下钩子上次处理的时间戳，短时间内的 WM_MOUSEWHEEL 直接忽略。
    std::atomic<unsigned long long> hook_wheel_tick_{0};

    // 光标落在窗口上的滚轮事件计数。只用于诊断（写进窗口标题），
    // 用来区分"钩子根本没收到事件"和"收到了但状态没变"这两种故障。
    std::atomic<unsigned long long> hook_seen_{0};

    // 上一次回看画面实际画下了几条记录（窗口拉大就能多画几条）。
    // 也只用于诊断。
    std::atomic<int> last_drawn_{0};

    // 上一次实时画面里"顶部行"和"正文"各画了多长（UTF-8 字节数）。
    // 用于自动断言顶部行确实是**上一句**、而不是当前句的重复。
    std::atomic<int> last_prev_len_{0};
    std::atomic<int> last_live_len_{0};

    // 高精度滚轮/触控板一次只发几个单位，攒够一格（WHEEL_DELTA）才算一次翻页。
    // 只在窗口线程上访问（钩子回调也跑在窗口线程），不需要加锁。
    int wheel_accum_ = 0;

    // post_repaint() 每次都要写窗口标题做诊断，标题没变就不写
    std::wstring title_cache_;
    mutable std::mutex mutex_;
    std::string        original_;           // ② 当前句原文
    std::string        translation_;        // ③ 当前句译文（空 = 还没翻好）
    std::string        status_;

    // ---- 历史回看 ----
    // 用"序号"而不是"下标"来记忆滚动位置：新句子是往尾部追加的，
    // 用户正在往回看的时候新句子还在进来，用下标记位置会导致整个视图
    // 往上漂移（越看越跳）。序号是稳定的，前面被裁掉时再收敛一下即可。
    std::deque<Entry> history_;
    long long         front_seq_{1};   // history_.front() 的序号
    long long         anchor_seq_{0};  // 顶部显示的那条的序号；0 = 实时模式
};
