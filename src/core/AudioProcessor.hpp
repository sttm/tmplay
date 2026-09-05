#pragma once

#include "Track.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

enum class AudioProcessState {
    Idle,
    Running,
    Done,
    Error
};

enum class AudioProcessKind {
    Normalize,
    Convert
};

struct AudioProcessSnapshot {
    AudioProcessState state = AudioProcessState::Idle;
    AudioProcessKind kind = AudioProcessKind::Normalize;
    std::string message;
    std::string detail;
    std::string outputDirectory;
    float progress = 0.0f;
};

struct NormalizationOptions {
    int targetLufs = -14;
    std::string mode = "Short-Term Max";
};

struct ConvertOptions {
    std::string format = "mp3";
};

class AudioProcessor {
public:
    using FinishedCallback = std::function<void()>;

    AudioProcessor() = default;
    ~AudioProcessor();

    AudioProcessor(const AudioProcessor&) = delete;
    AudioProcessor& operator=(const AudioProcessor&) = delete;

    bool normalize(const std::vector<Track>& tracks,
                   const std::string& sourceDirectory,
                   const NormalizationOptions& options,
                   FinishedCallback on_finished = {});
    bool convert(const std::vector<Track>& tracks,
                 const std::string& sourceDirectory,
                 const ConvertOptions& options,
                 FinishedCallback on_finished = {});

    AudioProcessSnapshot snapshot() const;

private:
    struct Request {
        AudioProcessKind kind = AudioProcessKind::Normalize;
        std::vector<Track> tracks;
        std::string sourceDirectory;
        NormalizationOptions normalization;
        ConvertOptions convert;
        FinishedCallback onFinished;
    };

    void run(Request request);
    bool processOne(const std::string& ffmpeg,
                    const Request& request,
                    const Track& track,
                    const std::filesystem::path& outputDirectory,
                    std::string& error) const;
    void setState(AudioProcessState state,
                  AudioProcessKind kind,
                  std::string message,
                  std::string detail = {},
                  std::string outputDirectory = {},
                  float progress = 0.0f);

    mutable std::mutex mutex_;
    std::thread worker_;
    std::atomic_bool running_{false};
    AudioProcessSnapshot snapshot_;
};
