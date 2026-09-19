#pragma once
#include <string>

// ===========================================================================
// WebView2 宿主窗口（§7 第 4 阶段 4.4）
// ===========================================================================
//
// 【它为什么这么短】交付物本来就是**自包含单文件 HTML**，WebView2 直接渲染 ——
// "纪要查看器"是白送的，不需要自己写渲染层。
//
// 【架构约束（每条都有代价，见 PROJECT.md §7 第 4 阶段）】
//   ① **保持控制台子系统**：验证阶梯（--selftest/--wav/--gaps/python harness）
//      全走控制台。所以这里**不碰 /SUBSYSTEM**，只是"再开一个窗口"。
//   ② 窗口跑在**自己的线程**上，和 `SubtitleWindow` 同一套做法
//      （自己开线程 + GetMessageW/DispatchMessageW）。录制主循环留在 main 线程。
//   ③ **不注册全局热键**：`RegisterHotKey` 是线程绑定的，字幕窗已经在自己
//      线程上注册了 `Ctrl+Alt+Q`。这里再注册同一个会让两边都不确定。
//   ④ 用户数据目录必须重定向到 `%LOCALAPPDATA%`，否则 WebView2 会在 exe 旁边
//      建 `<exe>.WebView2\`，污染 `build\RelWithDebInfo\`。
//   ⑤ 需要 `WebView2Loader.dll` 在 exe 旁边（CMake 已显式部署）。
//   ⑥ 结束时的提问要由 GUI 弹窗实现 —— **那个还没做**，现在走
//      `std::getline`，GUI 里会直接跳过提问（见 PROJECT.md 约束 #6）。
//
// ⚠️ **本窗口是"最小可信版本"**：只做"开一个窗口 + 渲染一份 HTML"。
//    它存在的意义是**一次验证四件事**：窗口能开、WebView2 运行时能连上、
//    HTML 能渲染、控制台子系统没被破坏。页面（4.5–4.10）在后面接。
class WebViewWindow {
public:
    // 打开一个窗口并渲染文件。
    // html_path：本地文件路径（UTF-8）。title：窗口标题。
    // 返回 false = 启不来（**不抛异常** —— 调用方要能优雅退回命令行行为）。
    bool start(const std::string& html_path, const std::string& title);

    // ---- 自动检验模式（给"怎么检验"用的，见 STATE.md §六）----
    //
    // 【为什么需要它】"窗口起来了"是个**看不见的**结论：没有它，每次改 UI
    // 都只能靠人眼盯一眼，而且"窗口是白屏"和"HTML 没问题"分不出来。
    // 这一段让窗口**自己把渲染结果说出来**，于是 UI 改动也能进自动检验。
    //
    // 给了 js 之后：页面加载完成 → 执行 js → 退出（**自己关窗**，不等人）。
    // js 的返回值必须是 **JSON**（通常写 `JSON.stringify({...})`）。
    // ⚠️ 必须在 start() 之前调用。
    void set_probe(const std::string& js);

    // 页面是否加载成功（probe 模式下有效）
    bool nav_ok() const;

    // probe 的原始返回值（JSON 文本；没跑成 = 空串）
    std::string probe_result() const;

    // probe 是否跑完了（跑完 = 结果可信；没跑完 = 超时，别把空结果当通过）
    bool probe_done() const;

    // 阻塞直到用户关掉窗口（消息循环在窗口线程上跑）。
    void wait();

    // 请求关闭（幂等）
    void stop();

    // 最近一次失败的原因（start 返回 false 时看它）
    const std::string& error() const { return error_; }

    ~WebViewWindow();

private:
    std::string error_;
    std::string probe_js_;
    void*       impl_ = nullptr;   // 隐藏实现（win32 句柄等），头文件不引 windows.h
};
