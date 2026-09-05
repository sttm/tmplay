#pragma once

#include "../AutoCue/CuePreview.h"
#include "../core/AppController.hpp"
#include "../core/Track.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

#include <ftxui/component/event.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

class ManualCueEditor {
public:
    ManualCueEditor(AppController& controller,
                    std::string& commandStatus,
                    std::atomic_bool& refreshActive);

    bool open(const Track& track,
              const AutoCueFeatures* preparedWaveform = nullptr,
              const std::string* preparedError = nullptr);
    void close();
    bool isOpen() const;
    bool handleEvent(ftxui::Event event);
    ftxui::Element renderOverlay(ftxui::Element mainLayout, int contentWidth);

private:
    struct Slot {
        bool set = false;
        double seconds = 0.0;
        std::uint32_t colorRgb = 0xffffff;
    };

    AppController& controller_;
    std::string& commandStatus_;
    std::atomic_bool& refreshActive_;

    ftxui::Box saveBox_;
    ftxui::Box closeBox_;
    ftxui::Box autoCueBox_;
    ftxui::Box followBox_;
    ftxui::Box waveBox_;
    ftxui::Box speedResetBox_;
    ftxui::Box speedSliderBox_;
    ftxui::Box pitchLockBox_;
    ftxui::Component speedSlider_;
    std::array<ftxui::Box, 8> cueBoxes_{};

    bool open_ = false;
    bool dirty_ = false;
    bool waveformLoading_ = false;
    bool followPlayhead_ = false;
    bool closeSelected_ = false;
    int footerSelected_ = 1;
    int controlRow_ = 0;
    int speedSelected_ = 1;
    bool scrubbing_ = false;
    bool scrubPausedPlayback_ = false;
    int draggingCue_ = -1;
    int selected_ = 0;
    int scrubStartX_ = 0;
    int waveRows_ = 16;
    int speedValue_ = 100;
    int speedMin_ = 50;
    int speedMax_ = 200;
    int speedStep_ = 1;
    double zoom_ = 1.0;
    double viewStart_ = 0.0;
    double playhead_ = 0.0;
    double scrubStartPlayhead_ = 0.0;
    double positionSample_ = 0.0;
    bool preservePitch_ = true;
    PlaybackState positionState_ = PlaybackState::Stopped;
    std::chrono::steady_clock::time_point positionSampleTime_ =
        std::chrono::steady_clock::now();
    Track track_;
    AutoCueFeatures waveform_;
    std::array<Slot, 8> cues_{};
    std::string saveStatus_;

    double duration() const;
    void clampView();
    double currentPosition();
    void seekTo(double seconds);
    void setPlaybackRateFromValue(int value);
    void setPreservePitch(bool preserve);
    void setCue(int index);
    void playFrom(double seconds);
    void activateCue(int index);
    void deleteSelectedCue();
    bool save();
};
