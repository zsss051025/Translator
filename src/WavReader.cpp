#include "WavReader.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>

namespace {

// 小端读取，**逐字节拼**而不是 reinterpret_cast。
// 原因：data 块可能落在任意字节偏移上，reinterpret_cast<uint32_t*> 是未对齐访问
// （x86 上侥幸能跑，ARM 上直接崩），而且违反严格别名规则。
uint32_t rd_u32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t rd_u16(const unsigned char* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

bool tag_is(const unsigned char* p, const char* tag) {
    return std::memcmp(p, tag, 4) == 0;
}

// 把一个采样点转成 float，取值范围 [-1, 1]。
// 8bit WAV 是**无符号**的（128 为中心），这点最容易搞错。
float decode_sample(const unsigned char* p, int bits, int format, bool* ok) {
    *ok = true;
    if (format == 3) {                       // IEEE float
        if (bits != 32) { *ok = false; return 0.0f; }
        const uint32_t raw = rd_u32(p);
        float f = 0.0f;
        std::memcpy(&f, &raw, sizeof(f));
        return f;
    }
    if (format != 1) { *ok = false; return 0.0f; }   // 只支持 PCM 整数与 IEEE float

    switch (bits) {
    case 8:
        return (static_cast<int>(p[0]) - 128) / 128.0f;
    case 16: {
        const int16_t v = static_cast<int16_t>(rd_u16(p));
        return v / 32768.0f;
    }
    case 24: {
        // 24bit 小端有符号：把最高字节当符号扩展位
        int32_t v = static_cast<int32_t>(p[0]) |
                    (static_cast<int32_t>(p[1]) << 8) |
                    (static_cast<int32_t>(p[2]) << 16);
        if (v & 0x00800000) v |= static_cast<int32_t>(0xFF000000);   // 符号扩展
        return v / 8388608.0f;
    }
    case 32: {
        const int32_t v = static_cast<int32_t>(rd_u32(p));
        return v / 2147483648.0f;
    }
    default:
        // 关键：**不支持就报错，不要静默输出 0**。
        // 原 wav_reader.h 在这里让 sample 保持 0，整段变成静音且不报错。
        *ok = false;
        return 0.0f;
    }
}

// 线性插值重采样。
//
// 【刻意不做抗混叠滤波】测试素材是人声（能量集中在 4kHz 以下），
// 而且主要用例是本来就 16kHz 的素材（那条路径直接原样返回，不走这里）。
// 需要高保真时再换成带滤波的重采样器 —— 但那样就该引第三方库了。
std::vector<float> resample_linear(const std::vector<float>& in, int src_rate, int dst_rate) {
    if (src_rate == dst_rate || in.empty() || src_rate <= 0 || dst_rate <= 0) return in;

    const double ratio = static_cast<double>(src_rate) / static_cast<double>(dst_rate);
    const size_t out_len = static_cast<size_t>(std::llround(in.size() / ratio));
    std::vector<float> out;
    out.reserve(out_len);

    for (size_t i = 0; i < out_len; ++i) {
        const double pos = i * ratio;
        const size_t i0 = static_cast<size_t>(pos);
        const double frac = pos - i0;
        const size_t i1 = (i0 + 1 < in.size()) ? i0 + 1 : in.size() - 1;
        const float a = in[i0];
        const float b = in[i1];
        out.push_back(static_cast<float>(a + (b - a) * frac));
    }
    return out;
}

}  // namespace

WavReader::Result WavReader::parse(const std::vector<unsigned char>& bytes, int target_rate) {
    Result r;

    if (bytes.size() < 44) {
        r.error = "文件太小，不是合法的 WAV";
        return r;
    }
    if (!tag_is(bytes.data(), "RIFF") || !tag_is(bytes.data() + 8, "WAVE")) {
        r.error = "缺少 RIFF/WAVE 标记（不是 WAV 文件？）";
        return r;
    }

    int      fmt_format   = 0;
    int      fmt_channels = 0;
    int      fmt_rate     = 0;
    int      fmt_bits     = 0;
    bool     has_fmt      = false;
    const unsigned char* data_ptr = nullptr;
    size_t   data_size    = 0;

    // 遍历 chunk。每一步都做边界检查：size 来自文件，不能直接信。
    size_t pos = 12;
    while (pos + 8 <= bytes.size()) {
        const unsigned char* id = bytes.data() + pos;
        const uint32_t size = rd_u32(bytes.data() + pos + 4);
        const size_t body = pos + 8;

        // 越界保护：size 可能被损坏的文件写成天文数字
        if (body + size > bytes.size()) {
            // fmt 已经拿到、data 也拿到时，允许提前结束（有些工具不写正确的 RIFF 长度）
            if (has_fmt && data_ptr != nullptr) break;
            r.error = "chunk 长度越界，文件可能已损坏";
            return r;
        }

        if (tag_is(id, "fmt ")) {
            if (size < 16) { r.error = "fmt 块过短"; return r; }
            fmt_format   = rd_u16(bytes.data() + body + 0);
            fmt_channels = rd_u16(bytes.data() + body + 2);
            fmt_rate     = static_cast<int>(rd_u32(bytes.data() + body + 4));
            fmt_bits     = rd_u16(bytes.data() + body + 14);
            has_fmt = true;
        } else if (tag_is(id, "data")) {
            data_ptr  = bytes.data() + body;
            data_size = size;
        }

        pos = body + size;
        if (size & 1) ++pos;        // chunk 按偶数字节对齐
    }

    if (!has_fmt)   { r.error = "没有找到 fmt 块";  return r; }
    if (!data_ptr)  { r.error = "没有找到 data 块"; return r; }
    if (fmt_channels <= 0) { r.error = "声道数为 0"; return r; }
    if (fmt_rate <= 0)     { r.error = "采样率为 0"; return r; }
    if (fmt_format != 1 && fmt_format != 3) {
        r.error = "只支持 PCM 整数(1) 与 IEEE float(3)，本文件格式为 " + std::to_string(fmt_format);
        return r;
    }

    const int bytes_per_sample = fmt_bits / 8;
    if (bytes_per_sample <= 0) { r.error = "位深非法"; return r; }

    const size_t frame_bytes = static_cast<size_t>(bytes_per_sample) * fmt_channels;
    const size_t frames = data_size / frame_bytes;
    if (frames == 0) { r.error = "data 块里没有采样点"; return r; }

    // ① 解码 + 混单声道
    std::vector<float> mono;
    mono.reserve(frames);
    for (size_t f = 0; f < frames; ++f) {
        const unsigned char* base = data_ptr + f * frame_bytes;
        float acc = 0.0f;
        for (int c = 0; c < fmt_channels; ++c) {
            bool ok = false;
            const float v = decode_sample(base + static_cast<size_t>(c) * bytes_per_sample,
                                          fmt_bits, fmt_format, &ok);
            if (!ok) {
                r.error = "不支持的位深/格式组合: " + std::to_string(fmt_bits) + "bit fmt=" +
                          std::to_string(fmt_format);
                return r;
            }
            acc += v;
        }
        mono.push_back(acc / static_cast<float>(fmt_channels));   // 平均，不是相加（相加会削顶）
    }

    // ② 重采样到目标采样率
    r.samples = resample_linear(mono, fmt_rate, target_rate);
    if (r.samples.empty()) { r.error = "重采样后为空"; return r; }

    r.src_rate     = fmt_rate;
    r.src_channels = fmt_channels;
    r.src_bits     = fmt_bits;
    r.src_format   = fmt_format;
    r.src_seconds  = static_cast<double>(frames) / fmt_rate;
    r.ok = true;
    return r;
}

WavReader::Result WavReader::read_file(const std::string& path, int target_rate) {
    Result r;
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        r.error = "无法打开文件: " + path;
        return r;
    }
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        r.error = "文件为空: " + path;
        return r;
    }
    return parse(bytes, target_rate);
}

std::vector<unsigned char> WavReader::make_test_wav(const std::vector<float>& mono,
                                                    int rate, int channels) {
    const int    bits = 16;
    const size_t frame_bytes = static_cast<size_t>(bits / 8) * channels;
    const uint32_t data_size = static_cast<uint32_t>(mono.size() * frame_bytes);

    std::vector<unsigned char> out;
    auto put32 = [&out](uint32_t v) {
        out.push_back(static_cast<unsigned char>(v & 0xFF));
        out.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
        out.push_back(static_cast<unsigned char>((v >> 16) & 0xFF));
        out.push_back(static_cast<unsigned char>((v >> 24) & 0xFF));
    };
    auto put16 = [&out](uint16_t v) {
        out.push_back(static_cast<unsigned char>(v & 0xFF));
        out.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
    };
    auto tag = [&out](const char* t) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<unsigned char>(t[i]));
    };

    tag("RIFF");
    put32(static_cast<uint32_t>(36 + data_size));
    tag("WAVE");
    tag("fmt ");
    put32(16);
    put16(1);                                   // PCM
    put16(static_cast<uint16_t>(channels));
    put32(static_cast<uint32_t>(rate));
    put32(static_cast<uint32_t>(rate * static_cast<int>(frame_bytes)));   // byte rate
    put16(static_cast<uint16_t>(frame_bytes));  // block align
    put16(static_cast<uint16_t>(bits));
    tag("data");
    put32(data_size);

    for (float s : mono) {
        float clamped = s;
        if (clamped > 1.0f)  clamped = 1.0f;
        if (clamped < -1.0f) clamped = -1.0f;
        const int16_t v = static_cast<int16_t>(std::lround(clamped * 32767.0f));
        for (int c = 0; c < channels; ++c) put16(static_cast<uint16_t>(v));
    }
    return out;
}
