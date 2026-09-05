#include "TrimEditor.hpp"

#include "KeyBindings.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include <ftxui/dom/canvas.hpp>

using namespace ftxui;

namespace {

std::string formatPlaybackTime(double seconds)
{
    int centiseconds = std::max(0, (int)std::llround(seconds * 100.0));
    int total = centiseconds / 100;
    return std::format("{:02}:{:02}.{:02}",
                       total / 60,
                       total % 60,
                       centiseconds % 100);
}

std::string truncateEnd(const std::string& value, int width)
{
    if (width <= 0) {
        return "";
    }
    if ((int)value.size() <= width) {
        return value;
    }
    if (width <= 3) {
        return value.substr(0, width);
    }
    return value.substr(0, width - 3) + "...";
}

bool keyMatches(const Event& event, std::initializer_list<std::string> keys)
{
    return ui::keyMatches(event, keys);
}

Color colorFromRgb(std::uint32_t rgb)
{
    return Color::RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

}  // namespace

TrimEditor::TrimEditor(AppController& controller,
                       std::string& commandStatus,
                       std::atomic_bool& refreshActive)
    : controller_(controller),
      commandStatus_(commandStatus),
      refreshActive_(refreshActive)
{
    SliderOption<int> speed_options;
    speed_options.value = &speedValue_;
    speed_options.min = &speedMin_;
    speed_options.max = &speedMax_;
    speed_options.increment = &speedStep_;
    speed_options.on_change = [this] {
        setPlaybackRateFromValue(speedValue_);
    };
    speedSlider_ = Slider(speed_options);
}

bool TrimEditor::isOpen() const
{
    return open_;
}

bool TrimEditor::takeSplitRequest(Track& track,
                                  double& startSeconds,
                                  double& endSeconds)
{
    if (!splitRequested_) {
        return false;
    }
    splitRequested_ = false;
    track = track_;
    startSeconds = trimStart_;
    endSeconds = trimEnd_;
    return true;
}

double TrimEditor::duration() const
{
    if (waveform_.duration > 0.0) {
        return waveform_.duration;
    }
    return std::max(0.0, track_.duration);
}

void TrimEditor::clampState()
{
    double track_duration = duration();
    zoom_ = std::clamp(zoom_, 1.0, 1024.0);
    trimStart_ = std::clamp(trimStart_, 0.0, std::max(0.0, track_duration));
    trimEnd_ = std::clamp(trimEnd_,
                          startSet_ ? trimStart_ : 0.0,
                          std::max(0.0, track_duration));
    playhead_ = std::clamp(playhead_, 0.0, std::max(0.0, track_duration));
    double visible = track_duration > 0.0 ? track_duration / zoom_ : 0.0;
    if (track_duration <= 0.0 || visible >= track_duration) {
        viewStart_ = 0.0;
    } else {
        viewStart_ = std::clamp(viewStart_, 0.0, track_duration - visible);
    }
}

double TrimEditor::currentPosition()
{
    auto playback = controller_.previewPlaybackSnapshot();
    if (controller_.previewPlayingTrackId() == track_.id &&
        (playback.state == PlaybackState::Playing ||
         playback.state == PlaybackState::Paused)) {
        double track_duration = duration();
        // The audio backend already reports the source frame being rendered.
        // UI-side extrapolation made trim handles and playback drift apart.
        positionSample_ = playback.positionSeconds;
        positionState_ = playback.state;
        return std::clamp(playback.positionSeconds, 0.0, track_duration);
    }
    positionState_ = playback.state;
    return std::clamp(playhead_, 0.0, duration());
}

void TrimEditor::seekTo(double seconds)
{
    double track_duration = duration();
    playhead_ = std::clamp(seconds, 0.0, std::max(0.0, track_duration));
    positionSample_ = playhead_;
    positionSampleTime_ = std::chrono::steady_clock::now();
    if (track_duration > 0.0 && controller_.previewPlayingTrackId() == track_.id) {
        controller_.seekPreviewPlayback(playhead_ / track_duration);
    }
    clampState();
}

void TrimEditor::setPlaybackRateFromValue(int value)
{
    speedValue_ = std::clamp(value, 50, 200);
    controller_.setPreviewPlaybackRate((double)speedValue_ / 100.0);
}

void TrimEditor::setPreservePitch(bool preserve)
{
    preservePitch_ = preserve;
    controller_.setPreviewPreservePitch(preservePitch_);
}

void TrimEditor::updatePreviewLoopRange()
{
    if (loopEnabled_ &&
        startSet_ &&
        endSet_ &&
        trimEnd_ > trimStart_ + 0.1) {
        controller_.setPreviewLoopRange(trimStart_, trimEnd_);
    } else {
        controller_.clearPreviewLoopRange();
    }
}

void TrimEditor::playFrom(double seconds)
{
    double track_duration = duration();
    if (track_duration <= 0.0) {
        return;
    }
    playhead_ = std::clamp(seconds, 0.0, track_duration);
    auto playback = controller_.previewPlaybackSnapshot();
    bool same_track = controller_.previewPlayingTrackId() == track_.id;
    bool started_at_position = false;
    if (!same_track ||
        playback.state == PlaybackState::Stopped ||
        playback.state == PlaybackState::Error) {
        started_at_position = controller_.playPreviewTrack(track_, playhead_);
    }
    updatePreviewLoopRange();
    if (!started_at_position) {
        controller_.seekPreviewPlayback(playhead_ / track_duration);
    }
}

void TrimEditor::setStartAtPlayhead()
{
    double position = currentPosition();
    trimStart_ = position;
    startSet_ = true;
    if (endSet_ && position > trimEnd_) {
        endSet_ = false;
    }
    selected_ = 0;
    clampState();
    updatePreviewLoopRange();
}

void TrimEditor::setEndAtPlayhead()
{
    double position = currentPosition();
    trimEnd_ = startSet_ ? std::max(position, trimStart_) : position;
    endSet_ = true;
    selected_ = 1;
    clampState();
    updatePreviewLoopRange();
}

bool TrimEditor::save()
{
    if (!startSet_ || !endSet_) {
        commandStatus_ = "Trim error: set start and end";
        return false;
    }
    std::string error;
    if (!controller_.trimTrack(track_, trimStart_, trimEnd_, error)) {
        commandStatus_ = "Trim error: " + error;
        return false;
    }
    commandStatus_ = "Trim saved: " + track_.title;
    controller_.scanDirectory(controller_.currentPath(), true);
    return true;
}

bool TrimEditor::open(const Track& track,
                      const AutoCueFeatures* preparedWaveform,
                      const std::string* preparedError)
{
    if (track.type != EntryType::File || track.id.empty()) {
        commandStatus_ = "Select a track to trim";
        return false;
    }
    controller_.stopPlayback();
    controller_.stopPreviewPlayback();
    track_ = track;
    waveform_ = {};
    waveformLoading_ = true;
    selected_ = 0;
    zoom_ = 3.0;
    waveRows_ = 12;
    viewStart_ = 0.0;
    playhead_ = 0.0;
    trimStart_ = 0.0;
    trimEnd_ = std::max(0.0, track.duration);
    startSet_ = true;
    endSet_ = true;
    followPlayhead_ = false;
    loopEnabled_ = false;
    controller_.clearPreviewLoopRange();
    scrubbing_ = false;
    scrubPausedPlayback_ = false;
    draggingBoundary_ = -1;
    splitRequested_ = false;
    lastScrubSeekTime_ = std::chrono::steady_clock::time_point::min();
    positionSample_ = 0.0;
    speedValue_ = (int)std::round(controller_.previewPlaybackRate() * 100.0);
    preservePitch_ = controller_.previewPreservePitch();
    positionState_ = PlaybackState::Stopped;
    positionSampleTime_ = std::chrono::steady_clock::now();
    commandStatus_ = "Loading trim waveform: " + track_.title;

    std::string error;
    if (preparedWaveform != nullptr) {
        waveform_ = *preparedWaveform;
        if (preparedError != nullptr) {
            error = *preparedError;
        }
    } else {
        waveform_ = controller_.waveformForTrack(track_, error);
    }
    waveformLoading_ = false;
    if (waveform_.duration > 0.0) {
        trimEnd_ = waveform_.duration;
    }
    if (!error.empty()) {
        commandStatus_ = error;
    } else {
        commandStatus_ = "Trim: " + track_.title;
    }
    open_ = true;
    refreshActive_ = true;
    clampState();
    return true;
}

void TrimEditor::close()
{
    open_ = false;
    refreshActive_ = false;
    track_ = {};
    controller_.clearPreviewLoopRange();
    controller_.stopPreviewPlayback();
}

Element TrimEditor::renderOverlay(Element mainLayout, int contentWidth)
{
    closeBox_ = Box{};
    saveBox_ = Box{};
    splitBox_ = Box{};
    loopBox_ = Box{};
    followBox_ = Box{};
    startBox_ = Box{};
    endBox_ = Box{};
    waveBox_ = Box{};
    speedResetBox_ = Box{};
    speedSliderBox_ = Box{};
    pitchLockBox_ = Box{};

    int dialog_width = std::clamp(contentWidth - 4, 64, 118);
    int wave_rows = std::clamp(waveRows_, 10, 24);
    int wave_columns = std::max(24, dialog_width - 4);
    int canvas_width = wave_columns * 2;
    int canvas_height = wave_rows * 4;
    double track_duration = duration();
    double visible_seconds = track_duration > 0.0
        ? std::max(0.05, track_duration / zoom_)
        : 1.0;
    double view_start = std::clamp(viewStart_, 0.0,
                                   std::max(0.0, track_duration - visible_seconds));
    double view_end = std::min(track_duration, view_start + visible_seconds);
    double playhead = currentPosition();
    if (followPlayhead_ && track_duration > 0.0) {
        playhead_ = playhead;
        viewStart_ = playhead - visible_seconds * 0.5;
        clampState();
        view_start = std::clamp(viewStart_, 0.0,
                                std::max(0.0, track_duration - visible_seconds));
        view_end = std::min(track_duration, view_start + visible_seconds);
    }
    Color waveform_color = colorFromRgb(controller_.config().autoCue.manualWaveformColorRgb);
    Color playhead_color = colorFromRgb(controller_.config().autoCue.manualPlayheadColorRgb);
    const std::string& waveform_display = controller_.config().autoCue.waveformDisplay;
    Color start_color = Color::Green;
    Color end_color = Color::Red;

    auto secondToCanvasX = [&](double seconds) {
        if (view_end <= view_start) {
            return 0;
        }
        double ratio = (seconds - view_start) / (view_end - view_start);
        return (int)std::llround(std::clamp(ratio, 0.0, 1.0) *
                                 (double)(canvas_width - 1));
    };

    Element start_button = text("S " +
                                (startSet_ ? formatPlaybackTime(trimStart_) : "--:--.--")) |
        center | size(WIDTH, EQUAL, 16) | border | reflect(startBox_);
    Element end_button = text("E " +
                              (endSet_ ? formatPlaybackTime(trimEnd_) : "--:--.--")) |
        center | size(WIDTH, EQUAL, 16) | border | reflect(endBox_);
    if (selected_ == 0) start_button = start_button | inverted | color(start_color);
    else start_button = start_button | color(start_color);
    if (selected_ == 1) end_button = end_button | inverted | color(end_color);
    else end_button = end_button | color(end_color);

    Element waveform = canvas(canvas_width, canvas_height, [&](Canvas& c) {
        int mid = canvas_height / 2;
        c.DrawPointLine(0, mid, canvas_width - 1, mid, Color::GrayDark);
        const auto& energy = waveform_.energyCurve;
        if (!energy.empty() && waveform_.hopSeconds > 0.0) {
            float max_energy = 0.0f;
            for (float value : energy) {
                max_energy = std::max(max_energy, value);
            }
            if (max_energy > 0.0f) {
                int previous_upper = -1;
                int previous_lower = -1;
                for (int x = 0; x < canvas_width; ++x) {
                    double ratio = canvas_width <= 1 ? 0.0 :
                        (double)x / (double)(canvas_width - 1);
                    double seconds = view_start + ratio * (view_end - view_start);
                    int center = std::clamp(
                        (int)std::floor((seconds + waveform_.hopSeconds * 0.5) /
                                        waveform_.hopSeconds),
                        0,
                        std::max(0, (int)energy.size() - 1));
                    int radius = std::max(1, (int)std::ceil(
                        (view_end - view_start) /
                        std::max(1, canvas_width) /
                        waveform_.hopSeconds));
                    float value = 0.0f;
                    for (int frame = center - radius; frame <= center + radius; ++frame) {
                        value = std::max(value, energy[(size_t)std::clamp(
                            frame, 0, (int)energy.size() - 1)]);
                    }
                    int amp = (int)std::llround(
                        std::min(1.0f, (value / max_energy) * 1.35f) *
                        (double)(canvas_height / 2 - 2));
                    int upper = std::max(0, mid - amp);
                    int lower = std::min(canvas_height - 1, mid + amp);
                    if (waveform_display == "line") {
                        if (previous_upper >= 0) {
                            c.DrawPointLine(x - 1, previous_upper, x, upper,
                                            waveform_color);
                            c.DrawPointLine(x - 1, previous_lower, x, lower,
                                            waveform_color);
                        }
                    } else if (waveform_display == "bars") {
                        if (x % 2 == 0) {
                            c.DrawPointLine(x, upper, x, lower, waveform_color);
                        }
                    } else {
                        c.DrawPointLine(x, upper, x, lower, waveform_color);
                    }
                    previous_upper = upper;
                    previous_lower = lower;
                }
            }
        }
        if (startSet_ && trimStart_ >= view_start && trimStart_ <= view_end) {
            int x = secondToCanvasX(trimStart_);
            c.DrawPointLine(x, 0, x, canvas_height - 1, start_color);
            c.DrawText(std::clamp(x, 0, canvas_width - 4), 0, "[S]", start_color);
        }
        if (endSet_ && trimEnd_ >= view_start && trimEnd_ <= view_end) {
            int x = secondToCanvasX(trimEnd_);
            c.DrawPointLine(x, 0, x, canvas_height - 1, end_color);
            c.DrawText(std::clamp(x, 0, canvas_width - 4), 1, "[E]", end_color);
        }
        if (playhead >= view_start && playhead <= view_end) {
            int x = secondToCanvasX(playhead);
            c.DrawPointLine(x, 0, x, canvas_height - 1, playhead_color);
        }
    }) | size(WIDTH, EQUAL, wave_columns) |
        size(HEIGHT, EQUAL, wave_rows) |
        reflect(waveBox_);

    Element save_button = text("Trim") | center | size(WIDTH, EQUAL, 12) |
        border | bold | reflect(saveBox_);
    Element close_button = text("Close") | center | size(WIDTH, EQUAL, 12) |
        border | bold | reflect(closeBox_);
    if (selected_ == 5) save_button = save_button | inverted | color(Color::Yellow);
    else save_button = save_button | color(Color::Yellow);
    if (selected_ == 6) close_button = close_button | inverted | color(Color::Yellow);
    else close_button = close_button | color(Color::Yellow);
    Element split_button = text("Split -> stems") | center |
        size(WIDTH, EQUAL, 16) | border | reflect(splitBox_) | color(Color::Green);
    Element loop_button = text(loopEnabled_ ? "Loop on" : "Loop") | center |
        size(WIDTH, EQUAL, 9) | border | reflect(loopBox_) |
        color(loopEnabled_ ? Color::Green : Color::GrayLight);
    Element follow_button = text(followPlayhead_ ? "Follow on" : "Follow") |
        center | size(WIDTH, EQUAL, 11) | border | reflect(followBox_) |
        color(followPlayhead_ ? Color::Green : Color::GrayLight);

    std::string title_text = truncateEnd(track_.title, dialog_width - 4);
    std::string range_text = formatPlaybackTime(view_start) + " - " +
        formatPlaybackTime(view_end) + " / " + formatPlaybackTime(track_duration) +
        "  x" + std::to_string((int)zoom_);
    std::string helper = waveformLoading_
        ? "Loading waveform..."
        : "1 start | 2 end | L loop | Enter set/save | Space play | +/- zoom | [] height | S save";
    int speed_width = std::max(18, dialog_width - 18);
    Element speed_reset = text("x") | center | size(WIDTH, EQUAL, 3) |
        reflect(speedResetBox_);
    Element speed_slider = speedSlider_->Render() | size(WIDTH, EQUAL, speed_width) |
        reflect(speedSliderBox_);
    Element pitch_lock = text(preservePitch_ ? "▣ ♩" : "□ ♩") | center |
        size(WIDTH, EQUAL, 5) | reflect(pitchLockBox_);
    if (selected_ == 2) speed_reset = speed_reset | inverted;
    if (selected_ == 3) speed_slider = speed_slider | inverted;
    if (selected_ == 4) pitch_lock = pitch_lock | inverted;
    Element speed_box = hbox({
        speed_reset,
        speed_slider,
        text(std::format("{:.2f}x", (double)speedValue_ / 100.0)) |
            center | size(WIDTH, EQUAL, 6),
        pitch_lock,
    }) | size(HEIGHT, EQUAL, 1);

    Element dialog = vbox({
        text("trim track") | bold | center,
        text(title_text) | dim | center,
        separator(),
        hbox({start_button, text(" "), end_button}) | center,
        separator(),
        waveform | border,
        speed_box | center,
        separator(),
        hbox({
            text(formatPlaybackTime(playhead)) | bold,
            separator(),
            text(range_text) | dim,
            separator(),
            filler(),
            loop_button,
            text(" "),
            follow_button,
            text(" "),
            split_button,
            text(" "),
            save_button,
            text(" "),
            close_button,
        }),
        // text(helper) | dim | center,
    }) | size(WIDTH, EQUAL, dialog_width) |
        border | clear_under | center;

    return dbox({mainLayout, dialog}) | flex;
}

bool TrimEditor::handleEvent(Event event)
{
    auto trimKey = [&](const Event& event,
                       const std::string& action,
                       std::initializer_list<std::string> defaults) {
        return ui::bindingMatches(event, controller_.config().keybindsTrim,
                                  action, defaults);
    };

    double track_duration = duration();
    auto visibleSeconds = [&] {
        return track_duration > 0.0 ? std::max(0.05, track_duration / zoom_) : 1.0;
    };
    auto centerViewOnPlayhead = [&] {
        viewStart_ = playhead_ - visibleSeconds() * 0.5;
        clampState();
    };
    auto setSelectedBoundary = [&] {
        if (selected_ == 0) {
            setStartAtPlayhead();
        } else if (selected_ == 1) {
            setEndAtPlayhead();
        }
    };
    auto seekRelative = [&](double seconds) {
        seekTo(currentPosition() + seconds);
        centerViewOnPlayhead();
    };
    auto zoomManual = [&](double factor) {
        playhead_ = currentPosition();
        zoom_ = std::clamp(zoom_ * factor, 1.0, 1024.0);
        centerViewOnPlayhead();
    };
    auto secondsAtMouse = [&](const Mouse& mouse) {
        int width = std::max(1, waveBox_.x_max - waveBox_.x_min);
        double local = std::clamp((double)(mouse.x - waveBox_.x_min),
                                  0.0,
                                  (double)width);
        return std::clamp(viewStart_ + (local / (double)width) * visibleSeconds(),
                          0.0,
                          std::max(0.0, track_duration));
    };
    auto zoomAtMouse = [&](const Mouse& mouse, double factor) {
        if (followPlayhead_) {
            zoomManual(factor);
            return;
        }
        const double anchor = secondsAtMouse(mouse);
        const int width = std::max(1, waveBox_.x_max - waveBox_.x_min);
        const double ratio = std::clamp((double)(mouse.x - waveBox_.x_min) /
                                            (double)width, 0.0, 1.0);
        zoom_ = std::clamp(zoom_ * factor, 1.0, 1024.0);
        viewStart_ = anchor - ratio * visibleSeconds();
        clampState();
    };
    auto startScrub = [&](const Mouse& mouse) {
        auto playback = controller_.previewPlaybackSnapshot();
        scrubPausedPlayback_ = controller_.previewPlayingTrackId() == track_.id &&
            playback.state == PlaybackState::Playing;
        if (scrubPausedPlayback_) {
            controller_.togglePreviewPause();
        }
        scrubbing_ = true;
        scrubStartX_ = mouse.x;
        scrubStartPlayhead_ = secondsAtMouse(mouse);
        lastScrubSeekTime_ = std::chrono::steady_clock::now();
        seekTo(scrubStartPlayhead_);
        centerViewOnPlayhead();
    };
    auto setDraggedBoundary = [&](int boundary, double seconds) {
        constexpr double minimum_region = 0.05;
        if (boundary == 0) {
            trimStart_ = std::clamp(seconds, 0.0,
                                    std::max(0.0, trimEnd_ - minimum_region));
            startSet_ = true;
            selected_ = 0;
        } else {
            trimEnd_ = std::clamp(seconds, trimStart_ + minimum_region,
                                  std::max(trimStart_ + minimum_region, track_duration));
            endSet_ = true;
            selected_ = 1;
        }
        clampState();
        updatePreviewLoopRange();
    };
    auto boundaryAtMouse = [&](const Mouse& mouse) {
        const int width = std::max(1, waveBox_.x_max - waveBox_.x_min);
        const auto boundaryX = [&](double seconds) {
            const double ratio = std::clamp((seconds - viewStart_) / visibleSeconds(),
                                            0.0, 1.0);
            return waveBox_.x_min + (int)std::llround(ratio * width);
        };
        const int start_distance = startSet_ ? std::abs(mouse.x - boundaryX(trimStart_)) : 99;
        const int end_distance = endSet_ ? std::abs(mouse.x - boundaryX(trimEnd_)) : 99;
        if (std::min(start_distance, end_distance) > 3) return -1;
        return start_distance <= end_distance ? 0 : 1;
    };
    if (event.is_mouse()) {
        const Mouse& mouse = event.mouse();
        if (speedSliderBox_.Contain(mouse.x, mouse.y) &&
            speedSlider_ &&
            speedSlider_->OnEvent(event)) {
            selected_ = 3;
            return true;
        }
        if (mouse.button == Mouse::Left && mouse.motion == Mouse::Pressed) {
            if (speedResetBox_.Contain(mouse.x, mouse.y)) {
                setPlaybackRateFromValue(100);
                selected_ = 2;
                return true;
            }
            if (pitchLockBox_.Contain(mouse.x, mouse.y)) {
                setPreservePitch(!preservePitch_);
                selected_ = 4;
                return true;
            }
            if (loopBox_.Contain(mouse.x, mouse.y)) {
                loopEnabled_ = !loopEnabled_;
                updatePreviewLoopRange();
                if (loopEnabled_) {
                    playFrom(trimStart_);
                }
                return true;
            }
            if (followBox_.Contain(mouse.x, mouse.y)) {
                followPlayhead_ = !followPlayhead_;
                return true;
            }
            if (splitBox_.Contain(mouse.x, mouse.y)) {
                if (!startSet_ || !endSet_ || trimEnd_ <= trimStart_ + 0.05) {
                    commandStatus_ = "Split error: set a non-empty region";
                } else {
                    splitRequested_ = true;
                }
                return true;
            }
            if (closeBox_.Contain(mouse.x, mouse.y)) {
                close();
                return true;
            }
            if (saveBox_.Contain(mouse.x, mouse.y)) {
                if (save()) close();
                return true;
            }
            if (startBox_.Contain(mouse.x, mouse.y)) {
                setStartAtPlayhead();
                return true;
            }
            if (endBox_.Contain(mouse.x, mouse.y)) {
                setEndAtPlayhead();
                return true;
            }
            if (waveBox_.Contain(mouse.x, mouse.y)) {
                if (const int boundary = boundaryAtMouse(mouse); boundary >= 0) {
                    draggingBoundary_ = boundary;
                    setDraggedBoundary(boundary, secondsAtMouse(mouse));
                    return true;
                }
                startScrub(mouse);
                return true;
            }
        }
        if (draggingBoundary_ >= 0 && mouse.motion == Mouse::Moved) {
            setDraggedBoundary(draggingBoundary_, secondsAtMouse(mouse));
            return true;
        }
        if (draggingBoundary_ >= 0 && mouse.motion == Mouse::Released) {
            draggingBoundary_ = -1;
            return true;
        }
        if (scrubbing_ && mouse.motion == Mouse::Moved) {
            auto now = std::chrono::steady_clock::now();
            if (now - lastScrubSeekTime_ < std::chrono::milliseconds(16)) {
                return true;
            }
            lastScrubSeekTime_ = now;
            double visible = visibleSeconds();
            int width = std::max(1, waveBox_.x_max - waveBox_.x_min);
            double delta = (double)(mouse.x - scrubStartX_) / (double)width;
            seekTo(scrubStartPlayhead_ + delta * visible);
            centerViewOnPlayhead();
            return true;
        }
        if (scrubbing_ && mouse.motion == Mouse::Released) {
            double visible = visibleSeconds();
            int width = std::max(1, waveBox_.x_max - waveBox_.x_min);
            double delta = (double)(mouse.x - scrubStartX_) / (double)width;
            seekTo(scrubStartPlayhead_ + delta * visible);
            centerViewOnPlayhead();
            scrubbing_ = false;
            if (scrubPausedPlayback_) {
                controller_.togglePreviewPause();
            }
            scrubPausedPlayback_ = false;
            return true;
        }
        if (waveBox_.Contain(mouse.x, mouse.y) && mouse.button == Mouse::WheelUp) {
            zoomAtMouse(mouse, 1.4);
            return true;
        }
        if (waveBox_.Contain(mouse.x, mouse.y) && mouse.button == Mouse::WheelDown) {
            zoomAtMouse(mouse, 1.0 / 1.4);
            return true;
        }
        return true;
    }

    if (trimKey(event, "close", {"escape", "a", "A", "ф", "Ф"})) {
        close();
        return true;
    }
    if (trimKey(event, "stop", {"o", "O", "щ", "Щ"})) {
        controller_.stopPreviewPlayback();
        playhead_ = currentPosition();
        centerViewOnPlayhead();
        return true;
    }
    if (trimKey(event, "left", {"left"})) {
        if (selected_ == 3) {
            setPlaybackRateFromValue(speedValue_ - speedStep_);
        } else if (selected_ >= 5 && selected_ <= 6) {
            selected_ = std::max(5, selected_ - 1);
        } else if (selected_ <= 1) {
            selected_ = std::max(0, selected_ - 1);
        }
        return true;
    }
    if (trimKey(event, "right", {"right"})) {
        if (selected_ == 3) {
            setPlaybackRateFromValue(speedValue_ + speedStep_);
        } else if (selected_ >= 5 && selected_ <= 6) {
            selected_ = std::min(6, selected_ + 1);
        } else if (selected_ <= 1) {
            selected_ = std::min(1, selected_ + 1);
        }
        return true;
    }
    if (trimKey(event, "down", {"down"})) {
        if (selected_ <= 1) {
            selected_ = 2;
        } else if (selected_ >= 2 && selected_ < 6) {
            selected_++;
        }
        return true;
    }
    if (trimKey(event, "up", {"up"})) {
        if (selected_ > 2) {
            selected_--;
        } else if (selected_ == 2) {
            selected_ = 0;
        }
        return true;
    }
    if (trimKey(event, "focus_speed", {"?"})) {
        selected_ = 3;
        return true;
    }
    if (trimKey(event, "speed_reset", {":"})) {
        setPlaybackRateFromValue(100);
        selected_ = 2;
        return true;
    }
    if (trimKey(event, "pitch_lock", {"\""})) {
        setPreservePitch(!preservePitch_);
        selected_ = 4;
        return true;
    }
    if (trimKey(event, "start", {"1"})) {
        setStartAtPlayhead();
        return true;
    }
    if (trimKey(event, "end", {"2"})) {
        setEndAtPlayhead();
        return true;
    }
    if (trimKey(event, "enter", {"enter"})) {
        if (selected_ == 2) {
            setPlaybackRateFromValue(100);
        } else if (selected_ == 4) {
            setPreservePitch(!preservePitch_);
        } else if (selected_ == 5) {
            if (save()) close();
        } else if (selected_ == 6) {
            close();
        } else if (selected_ <= 1) {
            setSelectedBoundary();
        }
        return true;
    }
    if (trimKey(event, "save", {"s", "S", "ы", "Ы"})) {
        if (save()) close();
        return true;
    }
    if (trimKey(event, "split", {"x", "X", "ч", "Ч"})) {
        if (!startSet_ || !endSet_ || trimEnd_ <= trimStart_ + 0.05) {
            commandStatus_ = "Split error: set a non-empty region";
        } else {
            splitRequested_ = true;
        }
        return true;
    }
    if (trimKey(event, "loop", {"l", "L", "д", "Д"})) {
        loopEnabled_ = !loopEnabled_;
        updatePreviewLoopRange();
        if (loopEnabled_) {
            playFrom(trimStart_);
        }
        return true;
    }
    if (trimKey(event, "toggle_playback", {"space"})) {
        auto playback = controller_.previewPlaybackSnapshot();
        if (controller_.previewPlayingTrackId() == track_.id &&
            (playback.state == PlaybackState::Playing ||
             playback.state == PlaybackState::Paused)) {
            controller_.togglePreviewPause();
        } else {
            playFrom(currentPosition());
        }
        return true;
    }
    if (trimKey(event, "zoom_in", {"+", "="})) {
        zoomManual(1.4);
        return true;
    }
    if (trimKey(event, "zoom_out", {"-", "_"})) {
        zoomManual(1.0 / 1.4);
        return true;
    }
    if (trimKey(event, "seek_back_15", {"<", "Б"})) {
        seekRelative(-15.0);
        return true;
    }
    if (trimKey(event, "seek_forward_15", {">", "Ю"})) {
        seekRelative(15.0);
        return true;
    }
    if (trimKey(event, "seek_back_01", {",", "б"})) {
        seekRelative(-0.1);
        return true;
    }
    if (trimKey(event, "seek_forward_01", {".", "ю"})) {
        seekRelative(0.1);
        return true;
    }
    if (trimKey(event, "height_down", {"[", "х", "Х"})) {
        waveRows_ = std::max(10, waveRows_ - 2);
        return true;
    }
    if (trimKey(event, "height_up", {"]", "ъ", "Ъ"})) {
        waveRows_ = std::min(24, waveRows_ + 2);
        return true;
    }
    if (trimKey(event, "follow", {"t", "T", "е", "Е"})) {
        followPlayhead_ = !followPlayhead_;
        playhead_ = currentPosition();
        centerViewOnPlayhead();
        return true;
    }
    return true;
}
