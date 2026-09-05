#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "../AutoCue/CuePreview.h"

struct AudioMetadata {
    std::string title;
    std::string artist;
    std::string album;
    double duration = 0.0;
    double bpm = 0.0;
    double bitrateKbps = 0.0;
    double sampleRateHz = 0.0;
    std::uintmax_t sizeBytes = 0;
    std::string key;
    std::string genre;
    bool succeeded = false;
    std::string error;
};

class AudioAnalyzer {
public:
    AudioMetadata readEmbeddedMetadata(const std::string& path) const;
    AudioMetadata analyzeWithEssentia(const std::string& path) const;
    AutoCueFeatures extractAutoCueFeatures(const std::filesystem::path& file) const;
    AutoCueFeatures extractWaveformFeatures(const std::filesystem::path& file) const;
};
