#pragma once

#include <cstdint>
#include <mutex>
#include <string>

enum class PlaybackState {
    Stopped,
    Playing,
    Paused,
    Error
};

struct PlaybackSnapshot {
    PlaybackState state = PlaybackState::Stopped;
    std::string title;
    double positionSeconds = 0.0;
    double durationSeconds = 0.0;
    std::string errorMessage;
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool play(const std::string& path, const std::string& title, int volume,
              double expectedDurationSeconds = 0.0,
              double startSeconds = 0.0);
    void pause();
    void resume();
    void togglePause();
    void stop();
    void setVolume(int volume);
    void setPlaybackRate(double rate);
    void setPreservePitch(bool preserve);
    void setEqualizerGains(double lowDb, double midDb, double highDb);
    void setLoopRange(double startSeconds, double endSeconds);
    void clearLoopRange();
    void seekToRatio(double ratio);
    void followSystemAudioOutput();

    double readDuration(const std::string& path);
    double playbackRate() const;
    bool preservePitch() const;
    PlaybackSnapshot snapshot() const;
    bool consumeFinishedNaturally();

private:
    void setErrorLocked(const std::string& message);
    void refreshStateLocked() const;
    void preventIdleSleepLocked();
    void allowIdleSleepLocked();

    mutable std::mutex mutex_;
    void* backend_ = nullptr;
    bool ready_ = true;
    mutable PlaybackState state_ = PlaybackState::Stopped;
    std::string currentTitle_;
    double currentDurationSeconds_ = 0.0;
    double playbackRate_ = 1.0;
    bool preservePitch_ = true;
    mutable std::string errorMessage_;
    mutable bool finishedNaturally_ = false;
#ifdef __APPLE__
    std::uint32_t sleepAssertion_ = 0;
    std::uint32_t systemOutputDevice_ = 0;
#endif
};
