#include "SubtitleWindow.h"

#include <iostream>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
// GET_X_LPARAM / GET_Y_LPARAM
#include <windowsx.h>

namespace {

constexpr wchar_t kClassName[] = L"AudioTranslatorSubtitleWnd";
constexpr int     kHotkeyId   = 1;

// 自定义消息：其它线程请求重绘
constexpr UINT WM_AT_REPAINT = WM_APP + 1;

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// 主显示器工作区（像素）
RECT primary_monitor_rect() {
    RECT r{0, 0, 1920, 1080};
    POINT pt{0, 0};
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) r = mi.rcMonitor;
    return r;
}

// ---- 排版参数 ----
//
// 为什么要有这个结构：行高**不能**手写像素值再整体按 DPI 缩放。
// 实测 Microsoft YaHei UI 在 144 DPI 下的字高：
//   字号像素 24→31px、30→39px、32→41px、48→62px
// 即"行高 ≈ 字号像素 × 1.30"。手写行高会踩坑：22px 的行放 15pt 字，
// 在 150% 缩放下就是 39px 的字高挤进 33px 的行，上下各裁 3px，笔画被削平。
//
// 所以行高一律由字号推算，窗口高度由这些行高求和得到——
// on_paint() 和 thread_main() 用的是同一份参数，不会再出现"字号变了窗口没变"。
int line_height(int pt, int dpi) {
    // 实测系数 1.30，取 1.35 留一点余量，
    // 避免 GDI 自己取整后行高比实际字高少 1px 而裁到笔画。
    return MulDiv(MulDiv(pt, dpi, 72), 135, 100);
}

struct Layout {
    int pad_x    = 18;
    int pad_y    = 10;
    int gap      = 4;    // ② 与 ③ 之间的分隔
    int prev_row = 0;    // ① 上一句译文
    int orig_row = 0;    // ② 当前句原文
    int tran_row = 0;    // ③ 译文主体单行高
    int tran_area = 0;   // ③ 译文主体区域（默认两行）
    int status_h = 0;    // 底部状态行
    int sep_row  = 0;    // 历史回看时，两条记录之间的分隔

    // 默认窗口高度。可以拉大——拉大后译文区和历史列表都跟着变高。
    int height() const {
        return pad_y + prev_row + orig_row + gap + tran_area + status_h + pad_y;
    }
};

Layout make_layout(int dpi) {
    Layout L{};
    L.pad_x     = MulDiv(18, dpi, 96);
    L.pad_y     = MulDiv(10, dpi, 96);
    L.gap       = MulDiv(4,  dpi, 96);
    L.prev_row  = line_height(14, dpi);   // 小字，单行
    L.orig_row  = line_height(16, dpi);   // 中字，单行
    L.tran_row  = line_height(22, dpi);   // 大字
    L.tran_area = L.tran_row * 2;         // 默认露出两行译文
    L.status_h  = line_height(12, dpi);
    L.sep_row   = MulDiv(8, dpi, 96);
    return L;
}

// 历史回看用的字号
constexpr int kHistTranPt = 16;
constexpr int kHistSrcPt  = 11;

// 低级鼠标钩子的目标窗口。进程内只有一个字幕窗。
SubtitleWindow* g_hook_target = nullptr;

// 为什么用低级鼠标钩子（WH_MOUSE_LL）而不是等 WM_MOUSEWHEEL：
//
// 这个窗口是 WS_EX_NOACTIVATE 的，永远拿不到焦点，而滚轮消息默认只发给
// **焦点窗口**。Windows 10 起有"悬停滚动"（把滚轮发给光标下的窗口），
// 但那是系统设置项（SPI_GETMOUSEWHEELROUTING），用户或某些播放器/游戏会关掉它。
// 依赖它就等于"能不能滚历史要看系统设置"，不可接受。
//
// 低级钩子只看滚轮、只看光标是否落在本窗口内，其余事件一律放行，
// 而且只在需要时（见 on_wheel 的返回值）拦截，不影响其它程序。
LRESULT CALLBACK mouse_hook_proc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && wp == WM_MOUSEWHEEL && g_hook_target != nullptr) {
        auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
        const int delta = GET_WHEEL_DELTA_WPARAM(ms->mouseData);
        if (g_hook_target->handle_wheel_at(ms->pt.x, ms->pt.y, delta)) {
            return 1;   // 拦截：这个滚轮是给字幕窗翻历史用的
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

}  // namespace

const char* SubtitleWindow::hotkey_hint() {
    return "Ctrl+Alt+Q";
}

SubtitleWindow::~SubtitleWindow() {
    stop();
}

bool SubtitleWindow::start() {
    if (started_.exchange(true)) return true;   // 幂等
    running_ = true;
    thread_  = std::thread(&SubtitleWindow::thread_main, this);

    // 等窗口创建完成（最多 3 秒）
    for (int i = 0; i < 300 && hwnd_.load() == nullptr && running_; ++i) {
        Sleep(10);
    }
    return hwnd_.load() != nullptr;
}

void SubtitleWindow::stop() {
    if (!started_.exchange(false)) return;
    running_ = false;

    void* h = hwnd_.load();
    if (h != nullptr) {
        PostMessageW(static_cast<HWND>(h), WM_CLOSE, 0, 0);
    }
    if (thread_.joinable()) thread_.join();
    hwnd_ = nullptr;
}

void SubtitleWindow::set_original(const std::string& text) {
    { std::lock_guard<std::mutex> lock(mutex_); original_ = text; }
    post_repaint();
}

void SubtitleWindow::set_translation(const std::string& text) {
    { std::lock_guard<std::mutex> lock(mutex_); translation_ = text; }
    post_repaint();
}

void SubtitleWindow::set_status(const std::string& text) {
    { std::lock_guard<std::mutex> lock(mutex_); status_ = text; }
    post_repaint();
}

void SubtitleWindow::push_history(const std::string& source, const std::string& translation) {
    if (source.empty() && translation.empty()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        history_.push_back(Entry{source, translation});
        // 不设上限的话，连看两小时内存会一直涨
        while (history_.size() > kMaxHistory) {
            history_.pop_front();
            ++front_seq_;
        }
        // 被裁掉的正好是用户正在看的那条时，把锚点收敛到还能看到的最早一条
        if (anchor_seq_ != 0 && anchor_seq_ < front_seq_) anchor_seq_ = front_seq_;
    }
    post_repaint();
}

bool SubtitleWindow::on_wheel(int notches) {
    if (notches == 0) return false;

    bool consumed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (history_.empty()) return false;

        const long long newest = front_seq_ + static_cast<long long>(history_.size()) - 1;
        const bool was_live = (anchor_seq_ == 0);

        if (was_live && notches < 0) {
            // 实时模式继续往下滚：没有"更新"的东西可看了。
            // 这里**不拦截**，让滚轮照常传给下面的播放器——
            // 否则鼠标恰好停在字幕上时就没法调音量/翻进度，很烦人。
        } else if (was_live) {
            // 上滚进入回看。第一格停在最新的一条上，之后每格往前一条。
            consumed = true;
            long long target = newest - (static_cast<long long>(notches) - 1);
            if (target < front_seq_) target = front_seq_;
            anchor_seq_ = target;
        } else {
            consumed = true;
            const long long target = anchor_seq_ - static_cast<long long>(notches);
            // 下滚越过最新一条就回到实时
            anchor_seq_ = (target >= newest) ? 0 : (target < front_seq_ ? front_seq_ : target);
        }
    }

    if (consumed) post_repaint();
    return consumed;
}

bool SubtitleWindow::handle_wheel_at(int screen_x, int screen_y, int delta) {
    void* h = hwnd_.load();
    if (h == nullptr) return false;
    HWND hwnd = static_cast<HWND>(h);

    RECT r{};
    if (!GetWindowRect(hwnd, &r)) return false;

    // 光标不在字幕窗上：与我无关，放行
    if (screen_x < r.left || screen_x >= r.right ||
        screen_y < r.top  || screen_y >= r.bottom) {
        wheel_accum_ = 0;
        return false;
    }

    hook_wheel_tick_ = GetTickCount64();
    ++hook_seen_;

    // 高精度滚轮/触控板一次只发几个单位，攒够一格才算一次翻页
    wheel_accum_ += delta;
    const int notches = wheel_accum_ / WHEEL_DELTA;
    if (notches == 0) return false;
    wheel_accum_ -= notches * WHEEL_DELTA;

    return on_wheel(notches);
}

void SubtitleWindow::update_title() {
    void* h = hwnd_.load();
    if (h == nullptr) return;
    HWND hwnd = static_cast<HWND>(h);

    // 诊断用：把"实时 / 回看到第几句 / 这一屏几条 / 收到几个滚轮"写进窗口标题。
    // 窗口没有标题栏、又是 WS_EX_TOOLWINDOW（不进 Alt+Tab、不进任务栏），
    // 所以用户看不到；但外部工具可以用 GetWindowText 读到当前状态，
    // 于是"滚轮翻历史""拉大后多显示几条"这类纯交互行为也能被自动化验证，
    // 而不是只能靠肉眼。出问题时也能一眼看出滚轮到底有没有被窗口收到。
    std::wstring title;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const long long total = static_cast<long long>(history_.size());
        const std::wstring tail = L" wheel=" + std::to_wstring(hook_seen_.load());
        if (anchor_seq_ != 0 && total > 0) {
            long long idx = anchor_seq_ - front_seq_;
            if (idx < 0) idx = 0;
            if (idx > total - 1) idx = total - 1;
            // 与屏幕上的状态行同口径：数字是"顶部那条距最新多少句"
            const long long behind = total - 1 - idx;
            title = L"AT hist " + std::to_wstring(behind) + L"/" + std::to_wstring(total)
                  + L" shown=" + std::to_wstring(last_drawn_.load()) + tail;
        } else {
            title = L"AT live " + std::to_wstring(total) + tail
                  + L" p=" + std::to_wstring(last_prev_len_.load())
                  + L" c=" + std::to_wstring(last_live_len_.load());
        }
        // title_cache_ 也在这个锁里：update_title() 既会被识别线程经
        // post_repaint() 调到，也会被窗口线程在画完之后调到。
        if (title == title_cache_) return;
        title_cache_ = title;
    }
    SetWindowTextW(hwnd, title.c_str());
}

void SubtitleWindow::post_repaint() {
    void* h = hwnd_.load();
    if (h == nullptr) return;

    update_title();
    PostMessageW(static_cast<HWND>(h), WM_AT_REPAINT, 0, 0);
}

void SubtitleWindow::on_paint() {
    HWND hwnd = static_cast<HWND>(hwnd_.load());
    if (hwnd == nullptr) return;

    // ---- 先把这一帧要画的东西取出来，取完就放锁 ----
    // 不整份拷贝 history_（最多 500 条），只拷贝可见的那一段。
    struct Block { std::string source, translation; };

    std::vector<Block> blocks;      // 历史回看模式：从锚点开始的记录
    std::string live_src, live_tran, prev_tran, stat;
    bool scrolled = false;
    long long behind = 0;
    long long total  = 0;
    int drawn = 0;      // 回看模式下这一屏实际画下了几条

    {
        std::lock_guard<std::mutex> lock(mutex_);
        total = static_cast<long long>(history_.size());
        stat  = status_;

        if (anchor_seq_ != 0 && total > 0) {
            long long idx = anchor_seq_ - front_seq_;
            if (idx < 0) idx = 0;
            if (idx > total - 1) idx = total - 1;
            scrolled = true;
            behind = total - 1 - idx;
            for (long long i = idx; i < total; ++i) {
                const Entry& e = history_[static_cast<size_t>(i)];
                blocks.push_back(Block{e.source, e.translation});
            }
        } else {
            live_src  = original_;
            live_tran = translation_;
            if (!history_.empty()) prev_tran = history_.back().translation;
        }
    }

    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);
    if (hdc == nullptr) return;

    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int W = rc.right;
    const int H = rc.bottom;

    // ---- 全部按实际 DPI 缩放 ----
    const int dpi_y = GetDeviceCaps(hdc, LOGPIXELSY);
    const Layout L = make_layout(dpi_y);

    // ---- 淡蓝色底 ----
    HBRUSH bg = CreateSolidBrush(RGB(214, 232, 248));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);
    // 顶部一条稍深的蓝线，让边界更清楚
    RECT accent{0, 0, W, MulDiv(3, dpi_y, 96)};
    HBRUSH ab = CreateSolidBrush(RGB(126, 174, 224));
    FillRect(hdc, &accent, ab);
    DeleteObject(ab);

    SetBkMode(hdc, TRANSPARENT);

    auto mkfont = [&](int pt, int weight) {
        return CreateFontW(-MulDiv(pt, dpi_y, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                           L"Microsoft YaHei UI");
    };
    HFONT f_prev = mkfont(14, FW_NORMAL);
    HFONT f_orig = mkfont(16, FW_NORMAL);
    HFONT f_tran = mkfont(22, FW_SEMIBOLD);
    HFONT f_stat = mkfont(12, FW_NORMAL);
    HFONT f_hist_tran = mkfont(kHistTranPt, FW_SEMIBOLD);
    HFONT f_hist_src  = mkfont(kHistSrcPt,  FW_NORMAL);

    // 用 DT_CALCRECT 量文字实际有多高，而不是猜行数。
    // 量完再加行距把 y 往下推，这样每条记录占多少高度是"文字说了算"，
    // 中英混排、长短句都不会互相压住。
    auto measure = [&](HFONT f, const std::wstring& w, int width, bool wrap) {
        RECT c{0, 0, width, 0};
        HFONT old = static_cast<HFONT>(SelectObject(hdc, f));
        DrawTextW(hdc, w.c_str(), -1, &c,
                  DT_CALCRECT | DT_NOPREFIX | (wrap ? DT_WORDBREAK : DT_SINGLELINE));
        SelectObject(hdc, old);
        return c.bottom - c.top;
    };

    const int text_bottom = H - L.pad_y - L.status_h;
    const int text_w = W - 2 * L.pad_x;
    if (text_w <= 0 || text_bottom <= L.pad_y) {   // 拉得太小，先不画
        DeleteObject(f_prev); DeleteObject(f_orig); DeleteObject(f_tran);
        DeleteObject(f_stat); DeleteObject(f_hist_tran); DeleteObject(f_hist_src);
        EndPaint(hwnd, &ps);
        return;
    }

    if (scrolled) {
        // ---- 历史回看模式：一句一块，译文在上、原文在下，从锚点往下顺时间排 ----
        const int max_tran_h = line_height(kHistTranPt, dpi_y) * 3;   // 单条最多三行
        int y = L.pad_y;
        bool drew_any = false;

        for (size_t i = 0; i < blocks.size(); ++i) {
            const std::wstring wt = utf8_to_wide(blocks[i].translation);
            const std::wstring ws = utf8_to_wide(blocks[i].source);

            int th = wt.empty() ? 0 : measure(f_hist_tran, wt, text_w, true);
            if (th > max_tran_h) th = max_tran_h;
            const int sh = ws.empty() ? 0 : measure(f_hist_src, ws, text_w, false);
            const int sep = drew_any ? L.sep_row : 0;

            // 放不下就停在这里，不画半截 —— 半条记录比少一条更难读
            if (y + sep + th + sh > text_bottom && drew_any) break;

            if (sep > 0) {
                const int ly = y + sep / 2;
                RECT sr{L.pad_x, ly, W - L.pad_x, ly + 1};
                HBRUSH sb = CreateSolidBrush(RGB(176, 203, 231));
                FillRect(hdc, &sr, sb);
                DeleteObject(sb);
                y += sep;
            }
            if (th > 0) {
                RECT r{L.pad_x, y, W - L.pad_x, y + th};
                SelectObject(hdc, f_hist_tran);
                SetTextColor(hdc, RGB(16, 38, 72));
                DrawTextW(hdc, wt.c_str(), -1, &r,
                          DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
                y += th;
            }
            if (sh > 0) {
                RECT r{L.pad_x, y, W - L.pad_x, y + sh};
                SelectObject(hdc, f_hist_src);
                SetTextColor(hdc, RGB(96, 118, 148));
                DrawTextW(hdc, ws.c_str(), -1, &r,
                          DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                y += sh;
            }
            drew_any = true;
            ++drawn;
        }
        last_drawn_.store(drawn);

        // 状态行换成回看提示
        const std::wstring hint =
            utf8_to_wide(std::string(u8"● 历史回看   顶部距最新 ") + std::to_string(behind) +
                         u8" 句 / 共 " + std::to_string(total) + u8" 句   下滚到底回到实时");
        RECT r{L.pad_x, H - L.pad_y - L.status_h, W - L.pad_x, H - L.pad_y};
        SelectObject(hdc, f_stat);
        SetTextColor(hdc, RGB(45, 95, 165));
        DrawTextW(hdc, hint.c_str(), -1, &r,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    } else {
        // ---- 实时模式：三行 ----
        int y = L.pad_y;

        // 诊断用：把"顶部行画了多长、正文画了多长"记下来写进窗口标题。
        // 这样"顶部行到底是上一句还是当前这句"可以被自动断言，
        // 而不是只能靠肉眼看——上一版就是在这里把顶部行画成了当前句的重复。
        last_prev_len_.store((prev_tran.empty() || prev_tran == live_tran)
                                 ? 0 : static_cast<int>(prev_tran.size()));
        last_live_len_.store(static_cast<int>(live_tran.size()));

        // ① 上一句译文：小字、暗色。作用是在新句子出现时**留在屏幕上**，
        //    让字幕看起来是"滚动"而不是整块跳变。
        //
        //    兜底：顶部行是"上一句"，万一它和正文成了同一句——同一句被重复识别、
        //    或者哪次改代码把入历史的时机弄错了——画出来就是同一句话上下各一遍。
        //    那既白占一行，又像是程序坏了。这种情况干脆不画。
        if (!prev_tran.empty() && prev_tran != live_tran) {
            const std::wstring w = utf8_to_wide(prev_tran);
            RECT r{L.pad_x, y, W - L.pad_x, y + L.prev_row};
            SelectObject(hdc, f_prev);
            // 对比度：这个颜色对窗口底色约 3.5:1（原文行 4.05:1、译文主体 12:1）。
            // 之前用的 RGB(120,142,170) 只有 2.68:1，比正文淡太多，
            // 压在明亮的视频画面上基本看不清——而这一行的意义正是"刚说过的话还能回看一眼"。
            // 层次感改由字号（14 < 16 < 22）和字重来体现，不靠把字调到看不见。
            SetTextColor(hdc, RGB(100, 122, 152));
            DrawTextW(hdc, w.c_str(), -1, &r,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            y += L.prev_row;
        }

        // ② 当前句原文
        if (!live_src.empty()) {
            const std::wstring w = utf8_to_wide(live_src);
            RECT r{L.pad_x, y, W - L.pad_x, y + L.orig_row};
            SelectObject(hdc, f_orig);
            SetTextColor(hdc, RGB(92, 112, 138));      // 蓝灰
            DrawTextW(hdc, w.c_str(), -1, &r,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            y += L.orig_row;
        }
        y += L.gap;

        // ③ 当前句译文（主体）。
        //    还没翻好时**留空**而不是显示占位文字——占位文字随后被译文替换，
        //    那本身就是一次多余的跳变；"翻译中"改由底部状态行承担。
        if (!live_tran.empty()) {
            const std::wstring w = utf8_to_wide(live_tran);
            RECT r{L.pad_x, y, W - L.pad_x, text_bottom};
            SelectObject(hdc, f_tran);
            SetTextColor(hdc, RGB(16, 38, 72));        // 深海军蓝
            DrawTextW(hdc, w.c_str(), -1, &r,
                      DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
        } else if (live_src.empty()) {
            SelectObject(hdc, f_tran);
            SetTextColor(hdc, RGB(132, 158, 186));
            RECT r{L.pad_x, y, W - L.pad_x, text_bottom};
            DrawTextW(hdc, L"正在聆听…", -1, &r, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
        }

        // ---- 状态行（贴底，与译文区之间留出 pad_y 的间隔）----
        if (!stat.empty()) {
            const std::wstring w = utf8_to_wide(stat);
            RECT r{L.pad_x, H - L.pad_y - L.status_h, W - L.pad_x, H - L.pad_y};
            SelectObject(hdc, f_stat);
            SetTextColor(hdc, RGB(45, 95, 165));
            DrawTextW(hdc, w.c_str(), -1, &r,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
    }

    DeleteObject(f_prev);
    DeleteObject(f_orig);
    DeleteObject(f_tran);
    DeleteObject(f_stat);
    DeleteObject(f_hist_tran);
    DeleteObject(f_hist_src);
    EndPaint(hwnd, &ps);

    // 标题里的诊断值（一屏几条、顶部行多长）要画完才知道，
    // 而标题是在 post_repaint() 里生成的，所以这里补一次。
    // update_title() 内部会比较，标题没变就不会真的写，也不会来回重画。
    update_title();
}

void SubtitleWindow::thread_main() {
    // 让进程感知 DPI。不做的话，在缩放 125%/150% 的笔记本上，
    // Windows 会把整个窗口位图放大，字幕文字会明显发虚。
    // on_paint() 用 GetDeviceCaps(LOGPIXELSY) 取实际 DPI 来缩放字号。
    SetProcessDPIAware();

    HINSTANCE hinst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
        SubtitleWindow* self = reinterpret_cast<SubtitleWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return TRUE;
        }
        case WM_AT_REPAINT:
            // 注意：这里不能直接调 BeginPaint —— 它只能在 WM_PAINT 处理中调用。
            // 正确做法是把区域标记为脏，让系统派发 WM_PAINT。
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_PAINT:
            if (self) self->on_paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;   // 自己填背景，避免闪烁
        case WM_SIZE:
            // 用户拖大了/拖小了：重画（窗口高度不再写死，排版按客户区现算）
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_MOUSEWHEEL:
            // 悬停滚动（如果系统开着）也会把滚轮送到这里。
            // 低级钩子已经处理过的，150ms 内不重复计算。
            if (self && GetTickCount64() - self->hook_wheel_tick_.load() > 150) {
                self->handle_wheel_at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp),
                                      GET_WHEEL_DELTA_WPARAM(wp));
            }
            return 0;
        case WM_GETMINMAXINFO: {
            // 最小尺寸：小于这个尺寸就只剩裁字了。
            // 高度直接用默认排版高度（三行 + 状态行）。
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            const int d = self ? self->dpi_.load() : 96;
            mmi->ptMinTrackSize.x = MulDiv(420, d, 96);
            mmi->ptMinTrackSize.y = make_layout(d).height();
            return 0;
        }
        case WM_HOTKEY:
            if (self && wp == kHotkeyId) {
                self->quit_requested_ = true;
            }
            return 0;
        case WM_NCHITTEST: {
            // 外圈 8px 用于拉伸窗口，其余整块用于拖动。
            // 没有这个的话，WS_THICKFRAME 的边框区域会被我们返回的 HTCAPTION 吃掉，
            // 用户就只能拖不能拉。
            if (self == nullptr) break;
            RECT r{};
            GetWindowRect(hwnd, &r);
            const int bw = MulDiv(8, self->dpi_.load(), 96);
            const int cw = r.right - r.left;
            const int ch = r.bottom - r.top;
            const int cx = GET_X_LPARAM(lp) - r.left;
            const int cy = GET_Y_LPARAM(lp) - r.top;
            const bool left = cx < bw, right = cx >= cw - bw;
            const bool top  = cy < bw, bottom = cy >= ch - bw;
            if (top && left)     return HTTOPLEFT;
            if (top && right)    return HTTOPRIGHT;
            if (bottom && left)  return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left)   return HTLEFT;
            if (right)  return HTRIGHT;
            if (top)    return HTTOP;
            if (bottom) return HTBOTTOM;
            return HTCAPTION;   // 全窗口可拖动
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (self) self->hwnd_ = nullptr;
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    };
    wc.hInstance     = hinst;
    wc.hCursor       = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);   // 已注册会失败，忽略

    const RECT mon = primary_monitor_rect();
    const int mon_w = mon.right - mon.left;
    const int mon_h = mon.bottom - mon.top;

    // 窗口高度必须跟着 DPI 缩放。写死数值的话，在 150% 缩放的屏幕上
    // 字号变成 1.5 倍而窗口不变高，内容就会互相压住。
    // 高度与 on_paint() 共用同一份排版参数，改字号不用两处改。
    HDC screen = GetDC(nullptr);
    const int dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);
    dpi_ = dpi;   // WM_NCHITTEST / WM_GETMINMAXINFO 要用，必须在建窗之前设好

    const int win_w = static_cast<int>(mon_w * 0.56);
    const int win_h = make_layout(dpi).height();
    const int win_x = mon.left + (mon_w - win_w) / 2;
    // 放在偏下方，但不压住视频播放控制条
    int win_y = mon.top + static_cast<int>(mon_h * 0.78);

    // 兜底：保证窗口**完整可见**。按屏幕高度百分比定位在"小屏 + 高 DPI"上会溢出：
    // 例如 768 高的屏在 125% 缩放下窗口高 197，0.78*768 + 197 = 796 > 768，
    // 字幕底部（状态行）就被推到屏幕外面去了。
    const int bottom_margin = static_cast<int>(mon_h * 0.04);
    if (win_y + win_h > mon.bottom - bottom_margin) {
        win_y = mon.bottom - bottom_margin - win_h;
    }
    if (win_y < mon.top) win_y = mon.top;

    g_hook_target = this;

    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kClassName, L"AudioTranslator",
        // WS_THICKFRAME：让用户能拖边框把窗口拉大（配合 WM_NCHITTEST）
        WS_POPUP | WS_THICKFRAME,
        win_x, win_y, win_w, win_h,
        nullptr, nullptr, hinst, this);

    if (hwnd == nullptr) {
        std::cerr << "[字幕窗] 创建失败，错误码 " << GetLastError() << std::endl;
        g_hook_target = nullptr;
        running_ = false;
        return;
    }

    // 先记下句柄，再显示窗口 —— 否则首次 WM_PAINT 时 on_paint() 拿不到句柄会直接返回，
    // 表现为"窗口是空的"。
    hwnd_ = hwnd;

    // 半透明：淡蓝底 + 92% 不透明，保证文字清晰又看得见底下的画面
    SetLayeredWindowAttributes(hwnd, 0, 235, LWA_ALPHA);

    // 全局热键：无论焦点在哪个程序都能收到
    if (!RegisterHotKey(hwnd, kHotkeyId, MOD_CONTROL | MOD_ALT, 'Q')) {
        std::cerr << "[字幕窗] 全局热键注册失败（可能被其它程序占用），"
                     "仍可在控制台按 Q 结束" << std::endl;
    }

    // 鼠标滚轮 → 翻翻译历史。
    // 用低级钩子是因为本窗口永远没有焦点（WS_EX_NOACTIVATE），
    // 而滚轮默认只发给焦点窗口。详见 mouse_hook_proc 的注释。
    HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, mouse_hook_proc, hinst, 0);
    if (hook == nullptr) {
        std::cerr << "[字幕窗] 鼠标滚轮钩子安装失败，翻历史改为依赖系统\"悬停滚动\"设置"
                  << std::endl;
    }

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);

    MSG msg{};
    while (running_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hook != nullptr) UnhookWindowsHookEx(hook);
    g_hook_target = nullptr;
    UnregisterHotKey(hwnd, kHotkeyId);
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    hwnd_ = nullptr;
}
