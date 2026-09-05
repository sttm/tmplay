#pragma once

#include <string>

class MacActivity {
public:
    explicit MacActivity(const std::string& reason);
    ~MacActivity();

    MacActivity(const MacActivity&) = delete;
    MacActivity& operator=(const MacActivity&) = delete;

private:
    void* token_ = nullptr;
};
