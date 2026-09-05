#pragma once

#include "../AutoCue/CuePreview.h"
#include "../core/AppController.hpp"
#include "../core/Track.hpp"

#include <chrono>
#include <atomic>
#include <string>

#include <ftxui/component/event.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

class TrimEditor {
public:
    TrimEditor(AppController& controller,
               std::string& commandStatus,
               std::atomic_bool& refreshActive);

    bool open(const Track& track,
              const AutoCueFeatures* preparedWaveform = nullptr,
              const std::string* preparedError = nullptr);
    void close();
    bool isOpen() const;
    bool takeSplitRequest(Track& track, double& startSeconds, double& endSeconds);
    bool handleEvent(ftxui::Event event);
    ftxui::Element renderOverlay(ftxui::Element mainLayout, int contentWidth);

private:
    AppController& controller_;
    std::string& commandStatus_;
    std::atomic_bool& refreshActive_;

    ftxui::Box closeBox_;
    ftxui::Box saveBox_;
    ftxui::Box splitBox_;
    ftxui::Box loopBox_;
    ftxui::Box followBox_;
    ftxui::Box startBox_;
    ftxui::Box endBox_;
    ftxui::Box waveBox_;
    ftxui::Box speedResetBox_;
    ftxui::Box speedSliderBox_;
    ftxui::Box pitchLockBox_;
    ftxui::Component speedSlider_;

    bool open_ = false;
    bool waveformLoading_ = false;
    bool followPlayhead_ = false;
    bool loopEnabled_ = false;
    bool startSet_ = true;
    bool endSet_ = true;
    bool scrubbing_ = false;
    bool scrubPausedPlayback_ = false;
    int draggingBoundary_ = -1;
    bool splitRequested_ = false;
    int selected_ = 0;
    int waveRows_ = 16;
    int scrubStartX_ = 0;
    int speedValue_ = 100;
    int speedMin_ = 50;
    int speedMax_ = 200;
    int speedStep_ = 1;
    double zoom_ = 3.0;
    double viewStart_ = 0.0;
    double playhead_ = 0.0;
    double trimStart_ = 0.0;
    double trimEnd_ = 0.0;
    double scrubStartPlayhead_ = 0.0;
    double positionSample_ = 0.0;
    bool preservePitch_ = true;
    PlaybackState positionState_ = PlaybackState::Stopped;
    std::chrono::steady_clock::time_point lastScrubSeekTime_ =
        std::chrono::steady_clock::time_point::min();
    std::chrono::steady_clock::time_point positionSampleTime_ =
        std::chrono::steady_clock::now();
    Track track_;
    AutoCueFeatures waveform_;

    double duration() const;
    void clampState();
    double currentPosition();
    void seekTo(double seconds);
    void setPlaybackRateFromValue(int value);
    void setPreservePitch(bool preserve);
    void updatePreviewLoopRange();
    void playFrom(double seconds);
    void setStartAtPlayhead();
    void setEndAtPlayhead();
    bool save();
};
