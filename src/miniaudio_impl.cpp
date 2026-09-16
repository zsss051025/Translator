#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

// 把单文件库 miniaudio 的“实现”部分展开出来。
//
// 【为什么这个文件必须存在】miniaudio.h 是 header-only 的单文件库：
// 声明部分随便 include，但实现部分必须由**恰好一个**翻译单元定义
// MINIAUDIO_IMPLEMENTATION 之后再 include。缺了它就是大片 undefined symbol，
// 而两个翻译单元都这么做就是重复定义。
//
// 【编码注意】本文件原来是 GBK 编码，而项目用 /utf-8 编译（见 CMakeLists.txt），
// 于是每次构建刷一串 C4828 警告——GBK 字节不是合法 UTF-8，注释里是中文时尤其明显。
// 已统一为 UTF-8。以后改这个文件不要用会写回 GBK 的编辑器。