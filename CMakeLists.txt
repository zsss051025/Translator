#集成vcpkg
set(CMAKE_TOOLCHAIN_FILE "C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")

cmake_minimum_required(VERSION 3.20)
project(AudioTranslator LANGUAGES CXX)

#c++20标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 关闭 shared，生成静态库
set(BUILD_SHARED_LIBS OFF)
set(GGML_CUDA ON CACHE BOOL "Enable CUDA acceleration" FORCE)


# 寻找系统依赖包 (OpenSSL 和 多线程)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)


# 添加 whisper 子工程
add_subdirectory(../whisper.cpp whisper_build EXCLUDE_FROM_ALL)

#文件名 src/main.cpp  src/xxx.cpp 
add_executable(Translator 
        src/main.cpp 
        src/miniaudio_impl.cpp
        src/audio_capture.cpp
        src/SpeechEngine.cpp
 "inc/Translator.h" "src/Translator.cpp")

target_include_directories(Translator
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/external/miniaudio
        ${CMAKE_CURRENT_SOURCE_DIR}/inc
        ${CMAKE_CURRENT_SOURCE_DIR}/external
)

# 链接 whisper
target_link_libraries(Translator PRIVATE whisper)

#链接 OpenSSL 和 C++ 线程库
target_link_libraries(Translator 
    PRIVATE 
        OpenSSL::SSL 
        OpenSSL::Crypto 
        Threads::Threads
)


#链接windows库
if(WIN32)
    target_link_libraries(Translator
               PRIVATE 
               ole32 
               winmm 
               dsound 
               ksuser
               ws2_32   # Windows Socket 2 (网络底层必须)
               crypt32  # 证书与加密 (HTTPS 握手必须)
               bcrypt   # 基础加密支持
    )
endif()





