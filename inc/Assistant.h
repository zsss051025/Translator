#pragma once
#include <string>
#include <vector>

// ===========================================================================
// 助手（assistant）与用户级配置 —— §7 第 4 阶段 4.1 / 4.2 / 4.3
// ===========================================================================
//
// 【"助手"是这一阶段的核心概念】用户原话：
//   「通过一个 ui 界面选择是否要新创建一个助手还是使用之前已经保存的助手，
//     已经保存的助手可以给他命名，然后后续使用的时候可以根据不同的场景
//     选择不同的助手，然后也可以通过 ui 界面看纪要或者任务清单」
//
// **一个助手的名字，就是一套独立的知识与历史。**
// "工作助手"和"英语课助手"必须互不污染 —— 工作会上学到的项目名，
// 不该被喂进英语课的识别提示。
//
// 【为什么用"一个助手一个目录"来实现】那是"数据边界"最不容易出错的形式：
//   · 隔离是**文件系统级的**，不靠每条 SQL 都记得带上 assistant_id
//     （那种做法只要有一处忘了过滤，两个助手就串了，而且很难发现）
//   · 备份/导出/删除一个助手 = 复制/打包/删一个目录
//   · 库路径、交付物目录都是**从助手目录推出来的**，没有第二处定义
//
// 【全局只有两样】模型路径、API key —— 模型 3GB 不该复制多份，key 是账号级的。
// 它们放在 config.json（§7 第 4 阶段决策①）。
//
// 目录布局（都在 %LOCALAPPDATA%\AudioTranslator 下，不污染安装目录）：
//     config.json                   全局设置
//     assistants\<助手>\assistant.json  这个名字的元信息
//     assistants\<助手>\data.db         知识 + 会话 + 行动项
//     assistants\<助手>\deliverables\   纪要/行动项/字幕
//
// ⚠️ **本文件同时管"助手"和"全局设置"**，因为它们回答的是同一个问题的两半：
//    "这台机器上，用户现在的产品配置是什么"。拆成两个文件只会让
//    "config.json 里存了 last_assistant、而助手在另一个模块里"变成一件要跳文件才能读懂的事。

namespace assistant {

// ---- 纯函数：助手名 → 目录名 ----
//
// 【为什么要它】助手名是**人取的**，可能带空格、中文、emoji、甚至 `..` 或 `/`。
// 直接拿来当目录名有两个后果：一是路径穿越（`--new-assistant ../../x`），
// 二是 Windows 上非法字符直接建目录失败。
//
// 规则：保留中文/字母/数字；空格转下划线；其余替换成下划线；
//       连续下划线压成一个；去首尾下划线；空结果回退成 "assistant"；
//       长度截断到 48 字节（按字符边界退，保证合法 UTF-8）。
//
// ⚠️ **不做小写化**：Windows 文件系统不区分大小写，但用户看到目录名时
//    希望还是他取的那个样子（`工作助手` 就该是 `工作助手`）。
//    重名由**文件系统**兜住：`create()` 会先检查目录是否存在。
std::string slugify(const std::string& name);

// ---- 助手根目录 / 全局配置文件路径 ----
std::string base_dir();          // %LOCALAPPDATA%\AudioTranslator
std::string assistants_root();   // <base>\assistants
std::string config_path();       // <base>\config.json

// 一个助手的元信息（assistant.json 的内容 + 由目录推出的路径）
struct Info {
    std::string name;          // 用户取的名字（显示用）
    std::string slug;          // 目录名
    std::string dir;           // 助手目录（绝对路径）
    std::string db_path;       // <dir>\data.db
    std::string out_dir;       // <dir>\deliverables
    std::string created_at;
    std::string source_lang = "auto";   // 这个助手自己的语言设置
    std::string target_lang = "zh";
    bool        cloud       = false;    // 这个助手的会话要不要上云
};

// 列出所有助手（按名字排序）。没有 assistants 目录时返回空 —— **不是错误**。
std::vector<Info> list();

// 新建一个助手。已存在同名（同 slug）时返回 false 并说明。
// 会真的建目录、写 assistant.json、建库（schema）。
bool create(const std::string& name, std::string* err = nullptr);

// 按名字或 slug 找一个（大小写不敏感）。找不到返回 false。
bool find(const std::string& name_or_slug, Info* out);

// 删除一个助手 = 把它的目录**改名**成 `<slug>.removed-<时间戳>`。
//
// ⚠️ **不做真正的删除**，理由和 §6.5「归档不删」是同一条：
//    那是用户的所有会议记录和知识。误删一次就再也回不来了。
//    改名之后它不再出现在列表里（等于"删掉了"），但文件还在，可以捞回来。
bool remove_soft(const std::string& name_or_slug, std::string* err = nullptr);

// ---- 全局设置（config.json）----
struct Settings {
    std::string whisper_model;
    std::string hunyuan_model;
    std::string last_assistant;        // 上次用的助手（slug）
    // API key **不以明文存**：这里存的是 DPAPI 加密后的 base64。
    std::string api_key_protected;
    // 没设置过时的兜底（让"没配置文件"和"配置文件里没这一项"行为一致）
    bool        loaded = false;
};

bool load_settings(Settings* out);
bool save_settings(const Settings& s, std::string* err = nullptr);

// ---- API key：DPAPI 加解密（§7 第 4 阶段 4.3）----
//
// 【为什么必须加密，不能明文】明文 key 放在 `%LOCALAPPDATA%` 下，
// 任何能读到这个文件的程序/人（包括他自己不小心把配置发出去）就拿到了账号。
// Windows 自带 DPAPI（`CryptProtectData`），用它做**当前用户**范围的加密：
//   · 不需要自己管密钥（密钥派生自用户的登录凭据）
//   · 换一台机器解不开 —— 这是**特性**不是缺陷
//
// ⚠️ 返回值是 base64（配置文件是 JSON，二进制塞不进去）。
// 失败时返回空串并填 err。**不要**在失败时退化成明文保存 ——
// 那会让"加密失败"变成"悄悄存了明文"，比不加密更坏。
std::string protect_key(const std::string& plain, std::string* err = nullptr);
std::string unprotect_key(const std::string& b64, std::string* err = nullptr);

// 当前是否已经存了 key（不解密，只看有没有）
bool has_stored_key();

}  // namespace assistant
