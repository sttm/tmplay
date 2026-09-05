#pragma once

#include "Config.hpp"
#include "Track.hpp"
#include <functional>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

enum class StemSeparationState {
    Idle,
    Running,
    Done,
    Error
};

struct StemSeparationSnapshot {
    StemSeparationState state = StemSeparationState::Idle;
    std::string message;
    std::string detail;
    std::string outputDirectory;
    double elapsedSeconds = 0.0;
    float progress = 0.0f;
};

class StemSeparator {
public:
    StemSeparator() = default;
    ~StemSeparator();

    StemSeparator(const StemSeparator&) = delete;
    StemSeparator& operator=(const StemSeparator&) = delete;

    bool start(const Track& track, const DemucsConfig& config);
    StemSeparationSnapshot snapshot() const;
    void setOnFinished(std::function<void()> callback);
private:
    struct Request {
        Track track;
        DemucsConfig config;
    };

    void run(Request request);
    void setState(StemSeparationState state,
                  std::string message,
                  std::string detail = {},
                  std::string outputDirectory = {},
                  float progress = 0.0f);
    mutable std::mutex mutex_;
    std::thread worker_;
    std::atomic_bool running_{false};
    std::chrono::steady_clock::time_point startedAt_ =
        std::chrono::steady_clock::now();
    StemSeparationSnapshot snapshot_;
    std::function<void()> onFinished_;
};
