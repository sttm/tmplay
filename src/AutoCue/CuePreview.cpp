#include "CuePreview.h"

#include <algorithm>

std::string renderCuePreview(const AutoCueResult& cues,
                             double duration,
                             int width)
{
    if (duration <= 0.0 || width < 12) {
        return {};
    }

    std::string line((size_t)width, '-');
    auto place = [&](const CuePoint& cue, char marker) {
        int column = (int)((cue.positionSeconds / duration) * (double)(width - 1));
        column = std::clamp(column, 0, width - 1);
        line[(size_t)column] = marker;
    };

    place(cues.start, 'S');
    place(cues.drop1, '1');
    place(cues.breakdown, 'B');
    place(cues.drop2, '2');
    return line;
}

std::string renderCuePreview(const std::vector<CuePoint>& cues,
                             double duration,
                             int width)
{
    if (duration <= 0.0 || width < 12 || cues.empty()) {
        return {};
    }

    std::string line((size_t)width, '-');
    constexpr char markers[] = {'S', '1', 'B', '2', '3', '4', '5', '6'};
    constexpr std::size_t marker_count = sizeof(markers) / sizeof(markers[0]);
    for (std::size_t i = 0; i < cues.size(); ++i) {
        char marker = markers[std::min<std::size_t>(i, marker_count - 1)];
        int column = (int)((cues[i].positionSeconds / duration) * (double)(width - 1));
        column = std::clamp(column, 0, width - 1);
        line[(size_t)column] = marker;
    }
    return line;
}
