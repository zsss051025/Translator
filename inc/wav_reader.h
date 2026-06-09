#pragma once
#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <cstring>

static std::vector<float> read_wav(const std::string& filepath, int expected_rate) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file) {
        std::cerr << "无法打开WAV文件: " << filepath << std::endl;
        return {};
    }

    // 读整个文件
    std::vector<char> buffer((std::istreambuf_iterator<char>(file)), {});

    if (buffer.size() < 44 || memcmp(buffer.data(), "RIFF", 4) != 0 || memcmp(buffer.data() + 8, "WAVE", 4) != 0) {
        std::cerr << "无效的WAV文件" << std::endl;
        return {};
    }

    // 遍历 chunk 找到 fmt 和 data
    int16_t channels = 1, bits = 16;
    int32_t sample_rate = 16000;
    char* data_ptr = nullptr;
    uint32_t data_size = 0;

    size_t pos = 12;
    while (pos + 8 <= buffer.size()) {
        char id[5] = {};
        memcpy(id, buffer.data() + pos, 4);
        uint32_t size = *reinterpret_cast<uint32_t*>(buffer.data() + pos + 4);

        if (memcmp(id, "fmt ", 4) == 0) {
            channels    = *reinterpret_cast<int16_t*>(buffer.data() + pos + 10);
            sample_rate = *reinterpret_cast<int32_t*>(buffer.data() + pos + 12);
            bits        = *reinterpret_cast<int16_t*>(buffer.data() + pos + 22);
        } else if (memcmp(id, "data", 4) == 0) {
            data_ptr  = buffer.data() + pos + 8;
            data_size = size;
            break;
        }
        pos += 8 + size;
    }

    if (!data_ptr || data_size == 0) {
        std::cerr << "未找到data块" << std::endl;
        return {};
    }

    std::cout << "[WAV] " << sample_rate << "Hz, " << channels << "ch, " << bits << "bit" << std::endl;

    size_t num_samples = data_size / (bits / 8);
    std::vector<float> result;

    for (size_t i = 0; i < num_samples; i += channels) {
        float sample = 0;
        if (bits == 16) {
            sample = *reinterpret_cast<int16_t*>(data_ptr + i * 2) / 32768.0f;
        } else if (bits == 32) {
            sample = *reinterpret_cast<float*>(data_ptr + i * 4);
        }
        result.push_back(sample);
    }

    return result;
}





