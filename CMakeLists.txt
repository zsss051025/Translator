# 集成 vcpkg
set(CMAKE_TOOLCHAIN_FILE "C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")

cmake_minimum_required(VERSION 3.20)
# sqlite3.c 是 C 源文件，本项目自身也需要 C 语言支持
project(AudioTranslator LANGUAGES C CXX)

# C++17（原注释写 C++20，与实际设置及 README 不一致，已统一）
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 关闭 shared 库，只生成静态库
set(BUILD_SHARED_LIBS OFF)
set(GGML_CUDA ON CACHE BOOL "Enable CUDA acceleration" FORCE)


# 寻找系统依赖 (OpenSSL 和多线程)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)


# 引入 llama 子工程
add_subdirectory(../llama.cpp llama_build EXCLUDE_FROM_ALL)
# 引入 whisper 子工程
add_subdirectory(../whisper.cpp whisper_build EXCLUDE_FROM_ALL)



# 可执行文件
add_executable(Translator
	src/main.cpp
	src/AppConfig.cpp
	src/miniaudio_impl.cpp
	src/audio_capture.cpp
	src/SpeechEngine.cpp
	src/SpeechFilter.cpp
	src/LangPolicy.cpp
	src/UtteranceMerge.cpp
	src/Assistant.cpp
	src/WebViewWindow.cpp
	src/ITranslator.cpp
	src/DeepSeekTranslator.cpp
	src/HunyuanTranslator.cpp
	src/SessionStore.cpp
	src/KnowledgeStore.cpp
	src/KnowledgeGap.cpp
	src/ConfirmGaps.cpp
	src/Interaction.cpp
	src/CommonWords.cpp
	src/CandidateTriage.cpp
	src/TriageCache.cpp
	src/ActionStore.cpp
	src/Evidence.cpp
	src/ModelLog.cpp
	src/KnowledgeExtract.cpp
	src/Utf8.cpp
	src/AgentTool.cpp
	src/AgentLoop.cpp
	src/DeliverableWriter.cpp
	src/LlmSummarizer.cpp
	src/CaBundle.cpp
	src/SubtitleWindow.cpp
	src/TermFixer.cpp
	src/WavReader.cpp

	inc/AppConfig.h
	inc/DeepSeekTranslator.h
	inc/ITranslator.h
	inc/HunyuanTranslator.h
	inc/SessionStore.h
	inc/KnowledgeStore.h
	inc/KnowledgeGap.h
	inc/ConfirmGaps.h
	inc/KnowledgeExtract.h
	inc/Utf8.h
	inc/AgentTool.h
	inc/DeliverableWriter.h
	inc/LlmSummarizer.h
	inc/CaBundle.h
	inc/SubtitleWindow.h
	inc/TermFixer.h
	inc/WavReader.h

	external/sqlite3/sqlite3.c)

target_include_directories(Translator
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/external/miniaudio
		${CMAKE_CURRENT_SOURCE_DIR}/external/sqlite3
		${CMAKE_CURRENT_SOURCE_DIR}/inc
		${CMAKE_CURRENT_SOURCE_DIR}/external
)

# 链接 whisper
target_link_libraries(Translator PRIVATE whisper)

# 链接 llama
target_link_libraries(Translator PRIVATE llama)

# 链接 OpenSSL 和 C++ 线程库
target_link_libraries(Translator
	PRIVATE
		OpenSSL::SSL
		OpenSSL::Crypto
		Threads::Threads
)

# ---- WebView2（§7 第 4 阶段 4.4）----
#
# 【为什么用它】交付物本来就是**自包含单文件 HTML**，WebView2 直接渲染就等于
# "纪要查看器"白送 —— 省掉自己写一个渲染层。
#
# 【装法（已做）】`vcpkg install webview2:x64-windows` →
#   webview2 1.0.3719.77 + wil。vcpkg 是**经典模式**（没有 vcpkg.json），
#   和已有的 portaudio/openssl 一样。
#
# ⚠️ **它是 DLL 版 Loader**：链接的是 `WebView2Loader.dll.lib`，
#    运行时必须有 `WebView2Loader.dll` 在 exe 旁边（架构约束 #5）。
#    vcpkg 的 CMake 集成会在构建时把它拷到输出目录 —— 但那**只在
#    `find_package` + link 这对同时成立时才发生**，所以打包/部署时
#    要确认 `build\RelWithDebInfo\WebView2Loader.dll` 真的在。
#
# ⚠️ **不要改 /SUBSYSTEM**（架构约束 #1）：整个验证阶梯
#    （--selftest / --wav / --gaps / 那些 python harness）全走控制台，
#    改成纯 GUI 子系统会把它们全废掉。
find_package(unofficial-webview2 CONFIG REQUIRED)
target_link_libraries(Translator PRIVATE unofficial::webview2::webview2)

# ⚠️ **显式把 WebView2Loader.dll 拷到 exe 旁边**（架构约束 #5）。
#
# 【为什么不能靠 vcpkg 的 applocal 自动拷】它是"可执行文件**真的**依赖了这个 dll"
# 时才拷 —— 而 WebView2Loader 是运行时按需加载的：只要还没有代码调用 WebView2，
# 链接器就把依赖丢掉了，post-build 扫描也就看不到它。
# 实测就是这样：接上 find_package + link 之后重新构建，
# `build\RelWithDebInfo\WebView2Loader.dll` **仍然不存在**。
# 等到真有代码用它时才自动出现 —— 而那种"依赖某个条件才生效"的部署步骤，
# 一旦条件在某台机器上不成立就是**运行时崩溃/静默打不开窗口**。
# 所以这里显式拷，让它在**首次构建时**就必须出现（这是可验证的）。
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
	set(_wv2_dll "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/WebView2Loader.dll")
	if(EXISTS "${_wv2_dll}")
		add_custom_command(TARGET Translator POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
			        "${_wv2_dll}" "$<TARGET_FILE_DIR:Translator>"
			COMMENT "部署 WebView2Loader.dll（架构约束 #5）")
	else()
		message(WARNING "找不到 ${_wv2_dll} —— WebView2 窗口会在运行时打不开")
	endif()
endif()

# cpp-httplib 的 HTTPS 支持。
#
# 【为什么放在这里，而不是每个 .cpp 里写一遍 #define】
# 原来 `LlmSummarizer.cpp` 和 `DeepSeekTranslator.cpp` 各自在 include 之前
# 手写 `#define CPPHTTPLIB_OPENSSL_SUPPORT`。这个写法的问题不是啰嗦，是
# **漏一个就报错、而且报得莫名其妙**：我新加的 CandidateTriage.cpp 就漏了，
# 报的是「"set_ca_cert_path": 不是 "httplib::Client" 的成员」——
# 看错误信息完全想不到"是因为少了个宏"。
#
# 放到 target_compile_definitions 之后，**每个编译单元都一样**，
# 不用再靠"记得写"。同时也保证不会出现"A 文件有 HTTPS、B 文件没有"这种
# 只在运行时才暴露的分裂。
target_compile_definitions(Translator PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)


# 链接 Windows 平台库
if(WIN32)
	target_link_libraries(Translator
		PRIVATE
			ole32
			winmm
			dsound
			ksuser
			ws2_32   # Windows Socket 2 (网络底层)
			crypt32  # 证书验证 (HTTPS 连接必需)
			bcrypt   # 现代加密支持
	)
endif()

if(MSVC)
    target_compile_options(Translator PRIVATE /utf-8)
	add_compile_options(/Zc:char8_t-)
endif()

# 全文检索（--ask 要用）。
#
# sqlite3.c 里本来就带 FTS5 源码（44 处符号，被 #ifdef SQLITE_ENABLE_FTS5 保护着），
# 只是宏没定义，所以一直是关的 —— 直接用 FTS5 语法会报 "no such module: fts5"。
target_compile_definitions(Translator PRIVATE
	SQLITE_ENABLE_FTS5
	SQLITE_ENABLE_COLUMN_METADATA)