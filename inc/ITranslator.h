#pragma once
#include <string>

class ITranslator{
public:
    virtual ~ITranslator() = default;
    virtual void push_text(const std::string& text) = 0;
    virtual void stop() = 0;
    virtual long long get_last_api_ms() = 0;
};
