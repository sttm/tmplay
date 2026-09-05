#pragma once

#include <vector>
#include <string>

enum class FocusPane {
    Directories,
    Tracks
};

struct BrowserState {
    FocusPane focus = FocusPane::Directories;
    int dirOffset = 0;
    int trackOffset = 0;
    int selectedDirectory = 0;
    int selectedTrack = 0;

    // 🔥 стек навигации по папкам
    std::vector<std::string> pathStack;
};