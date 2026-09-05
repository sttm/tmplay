#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Decodes an HTTP audio stream with libavformat/libavcodec into a small PCM
// ring buffer. It deliberately has no UI or AVFoundation dependency, so WebM
// and Opus support stays isolated from local-file playback.
class OnlineStreamDecoder {
public:
    static constexpr int kSampleRate = 44100;
    static constexpr int kChannels = 2;

    explicit OnlineStreamDecoder(std::string url);
    ~OnlineStreamDecoder();

    OnlineStreamDecoder(const OnlineStreamDecoder&) = delete;
    OnlineStreamDecoder& operator=(const OnlineStreamDecoder&) = delete;

    void start();
    void stop();
    void setPaused(bool paused);
    bool isPaused() const;
    // Requests a seek on the decoder thread. HTTP range-capable sources (such
    // as YouTube's direct media URLs) are reopened at the requested time.
    void seekToSeconds(double seconds);
    // Called from the audio render callback. Missing samples are silence.
    void read(float* output, std::uint32_t frames);
    bool finished() const;
    bool failed() const;
    std::string error() const;

private:
    static int interruptCallback(void* opaque);
    void decodeLoop();
    void push(const float* samples, std::size_t frames);
    void setError(std::string error);

    std::string url_;
    mutable std::mutex mutex_;
    std::condition_variable spaceAvailable_;
    std::vector<float> ring_;
    std::size_t readFrame_ = 0;
    std::size_t writeFrame_ = 0;
    std::size_t bufferedFrames_ = 0;
    bool paused_ = false;
    std::string error_;
    std::atomic_bool stopRequested_{false};
    std::atomic_bool finished_{false};
    std::atomic_bool failed_{false};
    std::atomic<std::int64_t> seekRequestedMs_{-1};
    std::thread worker_;
};
