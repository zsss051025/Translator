#集成vcpkg
set(CMAKE_TOOLCHAIN_FILE "C:\dev\vcpkg\scripts\buildsystems\vcpkg.cmake" CACHE STRING "")

cmake_minimum_required(VERSION 3.20)
project(AudioTranslator LANGUAGES CXX)

#c++20标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 关闭 shared，生成静态库
set(BUILD_SHARED_LIBS OFF)


# 添加 whisper 子工程
add_subdirectory(../whisper.cpp whisper_build EXCLUDE_FROM_ALL)

#文件名 src/main.cpp  src/xxx.cpp 
add_executable(Translator 
        src/main.cpp 
        src/miniaudio_impl.cpp
        src/audio_capture.cpp
        src/whisper_processor.cpp
)

target_include_directories(Translator
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/external/miniaudio
        ${CMAKE_CURRENT_SOURCE_DIR}/inc
)


# 链接 whisper
target_link_libraries(Translator PRIVATE whisper)

#链接windows库
if(WIN32)
    target_link_libraries(Translator PRIVATE ole32 winmm dsound ksuser)
endif()


