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
	src/ITranslator.cpp
	src/DeepSeekTranslator.cpp
	src/HunyuanTranslator.cpp
	src/SessionStore.cpp
	src/KnowledgeStore.cpp
	src/KnowledgeGap.cpp
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