#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

struct TrackIdentity {
    std::string id;
    std::string contentHash;
    std::uintmax_t fileSize = 0;
    std::int64_t fileMtime = 0;
};

TrackIdentity generateTrackIdentity(const std::filesystem::path& path,
                                    double durationSeconds = 0.0);

