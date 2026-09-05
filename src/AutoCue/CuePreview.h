#pragma once

#include <array>
#include <string>
#include <vector>

struct CuePoint {
    std::string name;
    double positionSeconds = 0.0;
};

struct AutoCueResult {
    double duration = 0.0;
    CuePoint start{"START", 0.0};
    CuePoint drop1{"VERSE", 0.0};
    CuePoint breakdown{"CHORUS", 0.0};
    CuePoint drop2{"BRIDGE", 0.0};
};

struct AutoCueFeatures {
    double duration = 0.0;
    std::vector<float> beats;
    std::vector<float> energyCurve;
    std::vector<float> onsetCurve;
    std::vector<float> spectralFluxCurve;
    std::vector<float> spectralCentroidCurve;
    std::vector<std::array<float, 12>> chromaCurve;
    double hopSeconds = 0.0;
};

std::string renderCuePreview(const AutoCueResult& cues,
                             double duration,
                             int width);
std::string renderCuePreview(const std::vector<CuePoint>& cues,
                             double duration,
                             int width);
