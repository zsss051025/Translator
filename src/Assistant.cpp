#include "Assistant.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "json.hpp"
#include "SessionStore.h"
#include "Utf8.h"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <wincrypt.h>
#endif

namespace fs = std::filesystem;
using nlohmann::json;

namespace assistant {

namespace {

std::string env_or_empty(const char* k) {
    const char* v = std::getenv(k);
    return v ? std::string(v) : std::string();
}

// base64：DPAPI 的输出是二进制，配置文件是 JSON，中间必须过一道编码。
// 自己写而不是引第三方库 —— 二十行，而且这个项目已经在避免多引依赖。
const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64_encode(const std::vector<unsigned char>& in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 2 < in.size()) {
        const unsigned v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out += kB64[(v >> 18) & 63]; out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];  out += kB64[v & 63];
        i += 3;
    }
    if (i + 1 == in.size()) {
        const unsigned v = in[i] << 16;
        out += kB64[(v >> 18) & 63]; out += kB64[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == in.size()) {
        const unsigned v = (in[i] << 16) | (in[i + 1] << 8);
        out += kB64[(v >> 18) & 63]; out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];  out += '=';
    }
    return out;
}

std::vector<unsigned char> b64_decode(const std::string& s) {
    int rev[256];
    for (int i = 0; i < 256; ++i) rev[i] = -1;
    for (int i = 0; i < 64; ++i) rev[static_cast<unsigned char>(kB64[i])] = i;

    std::vector<unsigned char> out;
    unsigned buf = 0;
    int bits = 0;
    for (unsigned char c : s) {
        const int v = rev[c];
        if (v < 0) continue;              // '=' 与换行都跳过
        buf = (buf << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

// 统一入口：UTF-8 路径字符串 → fs::path。
// **本文件里所有 filesystem / fstream 操作都必须过它**，不要直接写 fs::path(utf8)。
fs::path P(const std::string& utf8_path) { return fs::path(utf8::to_wide(utf8_path)); }

// 反方向：fs::path → UTF-8 字符串。
//
// ⚠️ **不能用 `path::string()`**：它把宽路径转窄时也按 **ANSI 代码页**，
//    于是 `英语课助手` 会变成一串 GBK 字节。那些字节再喂给 `to_wide()`
//    （它按 UTF-8 解）就解不开 → 读不到文件 → 这个助手被**静默跳过**。
//    实测症状：建助手成功了、目录也在，但 `--assistants` 列表里没有它。
//
// 这一对（P / U8）就是 Windows 上窄宽转换的**两个方向**，缺一个都会出事：
// 只补 P 会崩，只补 U8 会静默丢数据，两个都补才真的对。
std::string U8(const fs::path& p) { return utf8::from_wide(p.wstring().c_str()); }

std::string read_file(const std::string& p) {
    // ⚠️ **路径必须先转宽字符**：Windows 上 `std::ifstream` 从 `std::string`
    // 构造时按 ANSI 代码页解释，UTF-8 的中文路径会被解错
    //（轻则找错文件，重则转换失败抛异常 → terminate → 0xC0000409）。
    // 详见 inc/Utf8.h 里 `to_wide` 的说明 —— 这个崩溃是实测出来的。
    const std::wstring wp = utf8::to_wide(p);
    if (wp.empty()) return {};
    std::ifstream f(wp.c_str(), std::ios::binary);
    if (!f) return {};
    return std::string((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
}

bool write_file_atomic(const std::string& p, const std::string& data, std::string* err) {
    if (utf8::to_wide(p).empty()) {
        if (err) *err = u8"路径不是合法 UTF-8：" + p;
        return false;
    }
    const std::string tmp = p + ".tmp";
    {
        const std::wstring wt = utf8::to_wide(tmp);
        std::ofstream f(wt.c_str(), std::ios::binary | std::ios::trunc);
        if (!f) { if (err) *err = u8"写不了 " + tmp; return false; }
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!f) { if (err) *err = u8"写入失败 " + tmp; return false; }
    }
    // 先写临时文件再改名：配置文件被写到一半时断电/被杀，不会留下半个 JSON。
    std::error_code ec;
    fs::rename(P(tmp), P(p), ec);
    if (ec) {
        // Windows 上 rename 到已存在的文件会失败 —— 先删再改
        fs::remove(P(p), ec);
        fs::rename(P(tmp), P(p), ec);
        if (ec) { if (err) *err = u8"改名失败: " + ec.message(); return false; }
    }
    return true;
}

std::string now_string() { return SessionStore::now_string(); }

}  // namespace

std::string slugify(const std::string& name) {
    std::string out;
    bool last_underscore = false;
    size_t chars = 0;

    for (size_t i = 0; i < name.size() && chars < 48;) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        size_t len = 1;
        if      ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (i + len > name.size()) break;

        bool keep = false;
        if (c < 0x80) {
            keep = std::isalnum(c) != 0;
        } else {
            // 非 ASCII 一律保留（中文/日文/韩文都能当目录名）。
            // ⚠️ 但**要挡住路径穿越**：`/` `\` `:` `*` `?` `"` `<` `>` `|`
            //    全是 ASCII，走不到这一支 —— 那是这个分支安全的理由。
            keep = true;
        }

        if (keep) {
            out.append(name, i, len);
            last_underscore = false;
        } else if (!last_underscore && !out.empty()) {
            out += '_';
            last_underscore = true;
        }
        if (keep) ++chars;
        i += len;
    }

    // 去尾部下划线
    while (!out.empty() && out.back() == '_') out.pop_back();
    // 空结果 / 全是点的（`.` `..` 是目录穿越的经典写法）→ 兜底
    if (out.empty()) return "assistant";
    if (out == "." || out == "..") return "assistant";
    return out;
}

std::string base_dir() {
    // 允许用环境变量覆盖 —— 自检要靠它把配置写到临时目录，
    // 绝不能碰用户真实的 %LOCALAPPDATA%（那是在动用户的真实数据）。
    const std::string forced = env_or_empty("AT_HOME");
    if (!forced.empty()) return forced;
    const std::string local = env_or_empty("LOCALAPPDATA");
    if (!local.empty()) return local + "\\AudioTranslator";
    return "AudioTranslator";    // 极端兜底：当前目录（不该发生）
}

std::string assistants_root() { return base_dir() + "\\assistants"; }
std::string config_path()     { return base_dir() + "\\config.json"; }

std::vector<Info> list() {
    std::vector<Info> out;
    std::error_code ec;
    const std::string root = assistants_root();
    if (!fs::exists(P(root), ec)) return out;      // 没有 = 还没建过助手，不是错误

    for (const auto& e : fs::directory_iterator(P(root), ec)) {
        if (ec) break;
        if (!e.is_directory()) continue;
        const std::string dir = U8(e.path());
        // 被软删除的助手（改名为 *.removed-*）不再出现在列表里
        if (dir.find(".removed-") != std::string::npos) continue;

        const std::string meta = read_file(dir + "\\assistant.json");
        if (meta.empty()) continue;             // 不是助手目录（没有元信息）
        Info info;
        try {
            const auto j = json::parse(meta);
            info.name        = j.value("name", std::string());
            info.created_at  = j.value("created_at", std::string());
            info.source_lang = j.value("source_lang", std::string("auto"));
            info.target_lang = j.value("target_lang", std::string("zh"));
            info.cloud       = j.value("cloud", false);
        } catch (const std::exception&) {
            continue;                            // 元信息坏了：跳过而不是崩
        }
        info.slug     = U8(e.path().filename());
        info.dir      = dir;
        info.db_path  = dir + "\\data.db";
        info.out_dir  = dir + "\\deliverables";
        if (info.name.empty()) info.name = info.slug;
        out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(),
              [](const Info& a, const Info& b) { return a.name < b.name; });
    return out;
}

bool find(const std::string& name_or_slug, Info* out) {
    if (name_or_slug.empty()) return false;
    const std::string want = slugify(name_or_slug);
    for (const auto& a : list()) {
        if (a.slug == want || a.name == name_or_slug ||
            slugify(a.name) == want) {
            if (out) *out = a;
            return true;
        }
    }
    return false;
}

bool create(const std::string& name, std::string* err) {
    auto set_err = [&](const std::string& m) { if (err) *err = m; };
    if (name.empty()) { set_err(u8"助手名不能为空"); return false; }

    const std::string slug = slugify(name);
    const std::string dir  = assistants_root() + "\\" + slug;

    std::error_code ec;
    if (fs::exists(P(dir), ec)) {
        set_err(u8"已经有一个叫「" + slug + u8"」的助手了（目录已存在）");
        return false;
    }
    fs::create_directories(P(dir + "\\deliverables"), ec);
    if (ec) { set_err(u8"建目录失败: " + ec.message()); return false; }

    json j;
    j["name"]        = name;
    j["created_at"]  = now_string();
    j["source_lang"] = "auto";
    j["target_lang"] = "zh";
    j["cloud"]       = false;
    std::string werr;
    if (!write_file_atomic(dir + "\\assistant.json", j.dump(2), &werr)) {
        set_err(werr);
        return false;
    }

    // ⚠️ **这里刻意不预建数据库。**
    //
    // 本来想"建助手时顺手建库，免得第一批命令报打不开数据库" —— 但 `SessionStore`
    // 是**单例**（`SessionStore::instance()`），为了建一个新库去 `init()` 会把
    // **当前正在用的那个库**切走。那是在一个"建目录"的函数里偷偷换掉全局状态，
    // 属于最难查的一类副作用。
    //
    // 而它本来也不需要：`SessionStore::init()` 每次都会跑 `ensure_schema()`，
    // 所以**第一次真的用到这个助手时库会自动建好**。
    // 目录可写性已经由上面写 `assistant.json` 验证过了。
    return true;
}

bool remove_soft(const std::string& name_or_slug, std::string* err) {
    auto set_err = [&](const std::string& m) { if (err) *err = m; };
    Info info;
    if (!find(name_or_slug, &info)) {
        set_err(u8"没有叫「" + name_or_slug + u8"」的助手");
        return false;
    }
    std::string ts = now_string();
    for (char& c : ts) if (c == ':' || c == ' ' || c == '.') c = '-';
    const std::string dst = info.dir + ".removed-" + ts;

    std::error_code ec;
    fs::rename(P(info.dir), P(dst), ec);
    if (ec) { set_err(u8"改名失败: " + ec.message()); return false; }
    return true;
}

bool load_settings(Settings* out) {
    if (out == nullptr) return false;
    *out = Settings{};
    const std::string txt = read_file(config_path());
    if (txt.empty()) return false;              // 没配置文件 = 全部用默认值
    try {
        const auto j = json::parse(txt);
        out->whisper_model       = j.value("whisper_model", std::string());
        out->hunyuan_model       = j.value("hunyuan_model", std::string());
        out->last_assistant      = j.value("last_assistant", std::string());
        out->api_key_protected   = j.value("api_key_protected", std::string());
        out->loaded              = true;
    } catch (const std::exception& e) {
        // ⚠️ **坏配置不能让程序起不来**：它是用户的文件，可能被手改坏。
        //    报一句然后当"没有配置"用默认值 —— 比拒绝启动好得多。
        std::cerr << "[Config] 配置文件读不动，按默认配置继续: " << e.what() << std::endl;
        return false;
    }
    return true;
}

bool save_settings(const Settings& s, std::string* err) {
    std::error_code ec;
    fs::create_directories(P(base_dir()), ec);
    json j;
    j["whisper_model"]     = s.whisper_model;
    j["hunyuan_model"]     = s.hunyuan_model;
    j["last_assistant"]    = s.last_assistant;
    j["api_key_protected"] = s.api_key_protected;
    return write_file_atomic(config_path(), j.dump(2), err);
}

std::string protect_key(const std::string& plain, std::string* err) {
    auto set_err = [&](const std::string& m) { if (err) *err = m; };
    if (plain.empty()) { set_err(u8"key 是空的"); return {}; }
#ifdef _WIN32
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()));
    in.cbData = static_cast<DWORD>(plain.size());
    DATA_BLOB out{};
    // 第三个参数是"额外熵"：留空表示"只绑定当前用户"。
    // 不额外加熵，是为了让同一用户下的本程序各版本都能解开（升级不丢 key）。
    if (!CryptProtectData(&in, L"AudioTranslator", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        set_err(u8"DPAPI 加密失败（错误码 " + std::to_string(GetLastError()) + u8"）");
        return {};
    }
    std::vector<unsigned char> bytes(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return b64_encode(bytes);
#else
    set_err(u8"这个平台没有 DPAPI —— 不保存 key（绝不明文落盘）");
    return {};
#endif
}

std::string unprotect_key(const std::string& b64, std::string* err) {
    auto set_err = [&](const std::string& m) { if (err) *err = m; };
    if (b64.empty()) { set_err(u8"没有存过 key"); return {}; }
#ifdef _WIN32
    const auto raw = b64_decode(b64);
    if (raw.empty()) { set_err(u8"密文不是合法 base64"); return {}; }
    DATA_BLOB in{};
    in.pbData = const_cast<BYTE*>(raw.data());
    in.cbData = static_cast<DWORD>(raw.size());
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        // 换了机器/换了用户就会走到这里 —— 那是 DPAPI 的设计，不是坏了。
        set_err(u8"解不开（换机器或换用户了？DPAPI 的密钥跟登录凭据绑定）");
        return {};
    }
    std::string plain(reinterpret_cast<char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return plain;
#else
    set_err(u8"这个平台没有 DPAPI");
    return {};
#endif
}

bool has_stored_key() {
    Settings s;
    load_settings(&s);
    return !s.api_key_protected.empty();
}

}  // namespace assistant
