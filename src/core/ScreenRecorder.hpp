#pragma once

#include <chrono>
#include <mutex>
#include <string>

enum class RecordingState {
    Idle,
    Recording,
    Error
};

struct RecordingSnapshot {
    RecordingState state = RecordingState::Idle;
    std::string filePath;
    std::string message;
    double elapsedSeconds = 0.0;
};

// Owns the ScreenCaptureKit companion process. ScreenCaptureKit is used rather
// than an input-device API so this records the macOS desktop output.
class ScreenRecorder {
public:
    ~ScreenRecorder();
    bool start(const std::string& outputDirectory,
               std::string& error,
               const std::string& fileName = {});
    void stop();
    RecordingSnapshot snapshot() const;

private:
    mutable std::mutex mutex_;
    mutable int pid_ = -1;
    mutable RecordingSnapshot snapshot_;
    std::chrono::steady_clock::time_point startedAt_{};
};
