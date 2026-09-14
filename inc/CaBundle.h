#pragma once
#include <string>

// 找到一份可用的 CA 证书包路径；找不到返回空串。
//
// 背景：OpenSSL 在 Windows 上没有默认的 CA 路径，而 cpp-httplib 不会自动
// 使用系统证书库。结果是所有 HTTPS 请求都报
//     "SSL server verification failed"
// 实测确认 DeepSeek 云端后端因此从未成功过（而它是"双后端"卖点的一半）。
//
// 查找顺序：
//   1. 环境变量 SSL_CERT_FILE / CURL_CA_BUNDLE
//   2. 可执行文件所在目录及其上层的 certs/ca-bundle.crt 等常见命名
//   3. 常见系统位置（Git for Windows 自带的证书包）
std::string find_ca_bundle();
