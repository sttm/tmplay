#pragma once

#include "AudioAnalyzer.hpp"

#include <string>

class MetadataWriter {
public:
    bool write(const std::string& path,
               const AudioMetadata& metadata,
               std::string& error) const;
};
