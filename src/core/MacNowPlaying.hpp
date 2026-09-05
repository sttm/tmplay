#pragma once

#include "AudioEngine.hpp"

#include <string>
#include <functional>

// Publishes TPlay's primary deck to macOS Control Center / Now Playing.
// The implementation is native Objective-C++ and intentionally has no UI
// dependency, so the terminal player stays usable without an app bundle.
class MacNowPlaying {
public:
    MacNowPlaying();
    ~MacNowPlaying();

    MacNowPlaying(const MacNowPlaying&) = delete;
    MacNowPlaying& operator=(const MacNowPlaying&) = delete;

    void publish(const std::string& title,
                 const std::string& artist,
                 const std::string& artworkUrl,
                 const std::string& mediaPath,
                 const PlaybackSnapshot& playback);
    void updatePlayback(const PlaybackSnapshot& playback);
    void clear();
    void setRemoteCommandHandlers(std::function<void()> play,
                                  std::function<void()> pause,
                                  std::function<void()> togglePlayPause,
                                  std::function<void()> previous,
                                  std::function<void()> next);

    // FTXUI owns the terminal event loop. Call this from its main thread so
    // macOS can deliver MediaPlayer / remote-command events as well.
    static void pumpSystemRunLoop();

private:
    void* bridge_ = nullptr;
};
