#pragma once

#include "CuePreview.h"

#include <filesystem>
#include <string>

class AudioAnalyzer;

class AutoCueMarker {
public:
    explicit AutoCueMarker(AudioAnalyzer& analyzer);

    AutoCueResult analyze(const std::filesystem::path& file,
                          std::string& error) const;
    bool writeDebugJson(const std::filesystem::path& file,
                        const AutoCueResult& cues,
                        std::string& error) const;

private:
    AudioAnalyzer& analyzer_;
};
