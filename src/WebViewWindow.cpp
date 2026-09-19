#include "WebViewWindow.h"

#include <atomic>
#include <cctype>    // std::isalnum（to_file_url）
#include <cstdio>    // std::snprintf（to_file_url 的 %XX 编码、hr_text）
#include <filesystem>
#include <thread>

#include "Assistant.h"   // base_dir()：WebView2 用户数据目录要落在这里
#include "Utf8.h"        // 中文路径必须先转宽（见 Utf8.h 的 to_wide）

#include "WebView2.h"
#include <wrl.h>         // Microsoft::WRL::Callback —— 让 COM 回调写起来只有几行
#include <windows.h>

using namespace Microsoft::WRL;

namespace {

// file:// URL。反斜杠要换成斜杠，另外**非 ASCII 字符要百分号编码** ——
// 中文路径直接拼进 URL，WebView2 会解不出来（窗口一片空白，不报错）。
std::string to_file_url(const std::string& utf8_path) {
    std::error_code ec;
    const auto abs = std::filesystem::absolute(
        std::filesystem::path(utf8::to_wide(utf8_path)), ec);
    std::string p = ec ? utf8_path : utf8::from_wide(abs.wstring().c_str());

    std::string out = "file:///";
    for (unsigned char c : p) {
        if (c == '\\') {
            out += '/';
        } else if (c < 0x80 && (std::isalnum(c) || c == '/' || c == ':' ||
                                c == '.' || c == '-' || c == '_' || c == '~')) {
            out += static_cast<char>(c);
        } else {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// COM 套间守卫：**所有**返回路径都要 CoUninitialize，所以用 RAII 而不是手写
// —— 这个函数里有 4 个出口，手写必然漏一个。
struct ComApartment {
    HRESULT hr = E_FAIL;
    explicit ComApartment(DWORD model) { hr = CoInitializeEx(nullptr, model); }
    ~ComApartment() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
    bool ok() const { return SUCCEEDED(hr); }
};

// HRESULT 转成 `0x8000FFFF (E_UNEXPECTED)` 这种能查的形状。
// 十进制 `-2147418113` 在网上搜不到任何东西，十六进制能。
std::string hr_text(HRESULT hr) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "0x%08lX",
                  static_cast<unsigned long>(hr));
    std::string s = buf;
    switch (static_cast<unsigned long>(hr)) {
    case 0x8000FFFF: s += " (E_UNEXPECTED)"; break;
    case 0x80070005: s += " (E_ACCESSDENIED)"; break;
    case 0x80010106: s += " (RPC_E_CHANGED_MODE —— 线程套间不对)"; break;
    case 0x80070002: s += " (找不到 WebView2 运行时)"; break;
    default: break;
    }
    return s;
}

struct Impl {
    HWND                                hwnd = nullptr;
    std::atomic<bool>                   ready{false};
    std::atomic<bool>                   failed{false};
    std::thread                         thread;
    ComPtr<ICoreWebView2Controller>     controller;
    ComPtr<ICoreWebView2>               webview;
    std::string                         error;
    std::wstring                        class_name;
    std::wstring                        pending_url;   // 在控制器回调里导航用

    // ---- 自动检验模式 ----
    std::wstring                        probe_js;      // 空 = 普通模式（等人关窗）
    std::atomic<bool>                   probe_done{false};
    std::atomic<bool>                   nav_ok{false};
    std::string                         probe_result;
};

// probe 模式下最多等这么久。**必须有** —— 否则导航一失败就永远等不到
// NavigationCompleted，窗口不关、进程挂着，"检验"变成了手动 Ctrl+C。
constexpr UINT_PTR kProbeTimeoutId = 0xA7A1;
constexpr UINT     kProbeTimeoutMs = 20000;

Impl* g_impl = nullptr;   // 单窗口，够用（管理窗口只会有一个）

// 追加一条错误（多条时用 ` | ` 连起来）。
// **不覆盖** —— 一次失败可能有好几个原因，全留着才好排查。
void append_error(Impl* im, const std::string& msg) {
    if (im->error.empty()) im->error = msg;
    else                  im->error += " | " + msg;
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        if (g_impl && g_impl->controller) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            g_impl->controller->put_Bounds(rc);
        }
        return 0;
    case WM_TIMER:
        // 超时兜底：probe 没跑完就把窗口关掉，让 wait() 能返回
        // （结果里 probe_done=false，调用方据此判失败）。
        if (wp == kProbeTimeoutId) {
            KillTimer(hwnd, kProbeTimeoutId);
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

}  // namespace

WebViewWindow::~WebViewWindow() {
    stop();
    if (impl_) {
        delete static_cast<Impl*>(impl_);
        impl_ = nullptr;
        g_impl = nullptr;
    }
}

bool WebViewWindow::start(const std::string& html_path, const std::string& title) {
    if (impl_) return true;

    // 文件不存在就别开窗口了 —— 开了是一片空白，用户看不出是"文件没有"
    // 还是"WebView2 坏了"。**先失败，并说清原因**（本项目的规矩）。
    if (!std::filesystem::exists(
            std::filesystem::path(utf8::to_wide(html_path)))) {
        error_ = u8"文件不存在：" + html_path;
        return false;
    }

    auto* im = new Impl();
    im->class_name = L"AudioTranslatorWebView";
    g_impl = im;
    impl_ = im;

    const std::wstring wtitle = utf8::to_wide(title);
    const std::wstring url    = utf8::to_wide(to_file_url(html_path));
    // ⚠️ 用户数据目录**必须重定向**（约束 #4）：不传的话 WebView2 会在
    //    exe 旁边建 `<exe>.WebView2\`，把 build 目录搞脏。
    const std::wstring user_data = utf8::to_wide(assistant::base_dir() + "\\webview2");
    im->pending_url = url;
    im->probe_js    = utf8::to_wide(probe_js_);

    im->thread = std::thread([im, wtitle, url, user_data]() {
        // ⚠️ **这个线程必须是 STA（单线程套间）**，否则 WebView2 起不来。
        //
        // 实测（`--ui` 单独跑、三种组合）：
        //   · 请求 MTA     → **环境**创建就失败 `0x80010106 RPC_E_CHANGED_MODE`
        //   · 请求 STA     → 正常起窗口
        //   两种模式都不请求 → 同样失败。
        //   所以 WebView2 的宿主线程**必须是 STA**，这不是可选项。
        //
        // ⚠️ 另有一条独立的坑（别和上面混为一谈）：**在 DSH 的文件沙箱里跑，
        //    即使 STA 正确，控制器创建仍会失败 `0x8000FFFF E_UNEXPECTED`**
        //    （窗口建得出来、环境也建得出来、`msedgewebview2` 子进程照常起、
        //    连用户数据目录都写满了，只有最后一步失败）。**沙箱外同样的
        //    二进制正常。** —— 这条是测试环境的产物，不是产品缺陷；
        //    将来若又见到 `E_UNEXPECTED`，先确认是不是在沙箱里跑的，
        //    别去改 STA 那段代码（我第一次就是猜错了方向）。
        ComApartment com(COINIT_APARTMENTTHREADED);
        if (!com.ok()) {
            // 不直接退出：先让 WebView2 自己试，它也许会给一个更具体的错误码。
            // ⚠️ 用 append 而不是赋值 —— 后面那个环境回调会**覆盖** im->error，
            //    我第一次排查时线索就是这么丢掉的（结果只看到环境无关的错误码）。
            append_error(im, u8"线程套间初始化失败 " + hr_text(com.hr));
        }

        // 窗口类注册在**这个线程**上（RegisterClass 是进程级的，但窗口过程
        // 绑定线程消息队列 —— 和 SubtitleWindow 同一套做法）。
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = wnd_proc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.lpszClassName = im->class_name.c_str();
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassExW(&wc);

        im->hwnd = CreateWindowExW(
            0, im->class_name.c_str(), wtitle.c_str(),
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1100, 800,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (im->hwnd == nullptr) {
            append_error(im, u8"CreateWindowEx 失败");
            im->failed = true;
            return;
        }
        ShowWindow(im->hwnd, SW_SHOW);

        // ---- WebView2 环境 ----
        // 两个回调（环境、控制器）都用 WRL 的 Callback<>，比手写 COM 实现短得多。
        auto env_done = Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [im](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(hr) || env == nullptr) {
                    append_error(im, u8"WebView2 环境创建失败（运行时没装？）hr=" +
                                      hr_text(hr));
                    im->failed = true;
                    return S_OK;
                }
                auto ctl_done =
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [im](HRESULT hr2, ICoreWebView2Controller* ctl) -> HRESULT {
                            if (FAILED(hr2) || ctl == nullptr) {
                                append_error(im, u8"WebView2 控制器创建失败 hr=" +
                                                  hr_text(hr2));
                                im->failed = true;
                                return S_OK;
                            }
                            im->controller = ctl;
                            ctl->get_CoreWebView2(&im->webview);
                            RECT rc;
                            GetClientRect(im->hwnd, &rc);
                            ctl->put_Bounds(rc);

                            // ---- 自动检验模式：加载完 → 跑 JS → 关窗 ----
                            if (!im->probe_js.empty() && im->webview) {
                                im->webview->add_NavigationCompleted(
                                    Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                        [im](ICoreWebView2*,
                                             ICoreWebView2NavigationCompletedEventArgs* a)
                                            -> HRESULT {
                                            BOOL ok = FALSE;
                                            if (a) a->get_IsSuccess(&ok);
                                            im->nav_ok = (ok != FALSE);
                                            im->webview->ExecuteScript(
                                                im->probe_js.c_str(),
                                                Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
                                                    [im](HRESULT hr,
                                                         LPCWSTR json) -> HRESULT {
                                                        // ⚠️ 失败时也要置 probe_done：
                                                        //    "跑失败"和"没跑"必须能区分，
                                                        //    否则空结果会被当成通过。
                                                        if (SUCCEEDED(hr) && json)
                                                            im->probe_result =
                                                                utf8::from_wide(json);
                                                        im->probe_done = true;
                                                        PostMessageW(im->hwnd, WM_CLOSE, 0, 0);
                                                        return S_OK;
                                                    })
                                                    .Get());
                                            return S_OK;
                                        })
                                        .Get(),
                                    nullptr);
                                SetTimer(im->hwnd, kProbeTimeoutId, kProbeTimeoutMs, nullptr);
                            }

                            if (im->webview) im->webview->Navigate(im->pending_url.c_str());
                            im->ready = true;
                            return S_OK;
                        });
                env->CreateCoreWebView2Controller(im->hwnd, ctl_done.Get());
                return S_OK;
            });
        // ⚠️ 传 nullptr 版本 = 用本机装的 WebView2 运行时（不固定版本）。
        CreateCoreWebView2EnvironmentWithOptions(
            nullptr, user_data.c_str(), nullptr, env_done.Get());

        // 消息泵（和 SubtitleWindow 一样）
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        im->controller.Reset();
        im->webview.Reset();
    });

    // 等窗口起来（最多 20 秒）。**不无限等** —— 起不来就要能返回失败，
    // 而不是让用户对着黑屏。
    //
    // ⚠️ 这里**必须**在超时后也返回 false。原来的写法只判 `failed`，
    //    于是"8 秒过去、既不成功也没报错"会被当成**成功**返回 ——
    //    而 `--ui-verify` 接着会调 wait()，可看门狗定时器是在控制器回调里
    //    才装的（回调没跑过），结果就是**永久挂住**。
    //    首启动 + 冷 profile 本来就慢，所以我同时把上限放宽到 20 秒。
    for (int i = 0; i < 200 && !im->ready && !im->failed; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (im->failed) {
        error_ = im->error;
        return false;
    }
    if (!im->ready) {
        append_error(im, u8"20 秒内 WebView2 控制器没建起来"
                          u8"（运行时没装、被杀软拦住，或处在受限沙箱里）");
        error_ = im->error;
        return false;
    }
    return true;
}

void WebViewWindow::set_probe(const std::string& js) {
    probe_js_ = js;
}

bool WebViewWindow::nav_ok() const {
    auto* im = static_cast<Impl*>(impl_);
    return im && im->nav_ok.load();
}

std::string WebViewWindow::probe_result() const {
    auto* im = static_cast<Impl*>(impl_);
    return im ? im->probe_result : std::string();
}

bool WebViewWindow::probe_done() const {
    auto* im = static_cast<Impl*>(impl_);
    return im && im->probe_done.load();
}

void WebViewWindow::wait() {
    auto* im = static_cast<Impl*>(impl_);
    if (im && im->thread.joinable()) im->thread.join();
}

void WebViewWindow::stop() {
    auto* im = static_cast<Impl*>(impl_);
    if (im == nullptr) return;
    if (im->hwnd) PostMessageW(im->hwnd, WM_CLOSE, 0, 0);
    if (im->thread.joinable()) im->thread.join();
}
