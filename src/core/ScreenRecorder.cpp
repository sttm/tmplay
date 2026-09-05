#include "ScreenRecorder.hpp"

#include "ProcessRunner.hpp"

#include <csignal>
#include <chrono>
#include <filesystem>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

ScreenRecorder::~ScreenRecorder()
{
    stop();
}

bool ScreenRecorder::start(const std::string& outputDirectory,
                           std::string& error,
                           const std::string& fileName)
{
#if !defined(__APPLE__)
    error = "Desktop recording is available on macOS only";
    return false;
#else
    std::lock_guard<std::mutex> lock(mutex_);
    if (pid_ > 0) {
        error = "Desktop recording is already running";
        return false;
    }
    auto recorder = ProcessRunner::findExecutable("tmplay-recorder");
    if (!recorder) {
        error = "tmplay-recorder is missing; rebuild tmplay";
        return false;
    }
    std::error_code ec;
    fs::create_directories(outputDirectory, ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    fs::path output = fs::path(outputDirectory) /
        (fileName.empty() ? "desktop-" + std::to_string(timestamp) + ".wav"
                          : fileName);
    const std::string output_argument = output.string();
    pid_t child = fork();
    if (child < 0) {
        error = "Could not start desktop recorder";
        return false;
    }
    if (child == 0) {
        execl(recorder->c_str(), recorder->c_str(), output_argument.c_str(), nullptr);
        _exit(127);
    }
    pid_ = (int)child;
    startedAt_ = std::chrono::steady_clock::now();
    snapshot_ = {RecordingState::Recording, output.string(), "Recording desktop audio", 0.0};
    error.clear();
    return true;
#endif
}

void ScreenRecorder::stop()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (pid_ <= 0) return;
    kill((pid_t)pid_, SIGINT);
    waitpid((pid_t)pid_, nullptr, 0);
    pid_ = -1;
    snapshot_.state = RecordingState::Idle;
    snapshot_.message = "Recording saved: " + snapshot_.filePath;
}

RecordingSnapshot ScreenRecorder::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (pid_ > 0) {
        int status = 0;
        const pid_t result = waitpid((pid_t)pid_, &status, WNOHANG);
        if (result == pid_) {
            pid_ = -1;
            snapshot_.state = RecordingState::Error;
            snapshot_.message = "Desktop recorder stopped unexpectedly";
            if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
                snapshot_.message += " (check Screen Recording permission)";
            }
        }
    }
    RecordingSnapshot result = snapshot_;
    if (result.state == RecordingState::Recording) {
        result.elapsedSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - startedAt_).count();
    }
    return result;
}
