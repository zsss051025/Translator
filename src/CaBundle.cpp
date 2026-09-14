#include "CaBundle.h"

#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

fs::path executable_dir() {
#ifdef _WIN32
    char buf[MAX_PATH] = {0};
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        return fs::path(std::string(buf, static_cast<size_t>(n))).parent_path();
    }
#endif
    std::error_code ec;
    return fs::current_path(ec);
}

bool is_file(const char* p) {
    std::error_code ec;
    return fs::is_regular_file(fs::path(p), ec);
}

bool is_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

}  // namespace

std::string find_ca_bundle() {
    // 1) 环境变量优先
    for (const char* k : {"SSL_CERT_FILE", "CURL_CA_BUNDLE"}) {
        const char* v = std::getenv(k);
        if (v != nullptr && *v != '\0' && is_file(v)) return v;
    }

    // 2) 从可执行文件目录逐级向上找
    //    （exe 在 build/RelWithDebInfo/，证书在项目根的 certs/，向上两级即可命中）
    static const char* kRel[] = {
        "certs/ca-bundle.crt",
        "ca-bundle.crt",
        "certs/cacert.pem",
        "cacert.pem",
    };
    fs::path cur = executable_dir();
    for (int depth = 0; depth < 6; ++depth) {
        for (const char* r : kRel) {
            const fs::path cand = cur / r;
            if (is_file(cand)) return cand.string();
        }
        if (!cur.has_parent_path()) break;
        const fs::path parent = cur.parent_path();
        if (parent == cur) break;
        cur = parent;
    }

    // 3) 常见系统位置（Git for Windows 自带一份完整证书包）
    static const char* kSys[] = {
        "C:/Program Files/Git/mingw64/etc/ssl/certs/ca-bundle.crt",
        "C:/Program Files/Git/usr/ssl/certs/ca-bundle.crt",
    };
    for (const char* p : kSys) {
        if (is_file(p)) return p;
    }

    return {};
}
