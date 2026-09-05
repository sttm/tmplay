#pragma once

#include "../Library/LibraryModels.h"
#include "../Serato/SeratoCueWriter.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

struct TraktorEmbeddedMetadataStatus {
    bool supportedContainer = false;
    bool hasTraktor4Tag = false;
    std::size_t tagSize = 0;
    std::string detail;
};

class TraktorMetadataWriter {
public:
    TraktorEmbeddedMetadataStatus inspect(const std::filesystem::path& file,
                                          std::string& error) const;
    std::vector<SeratoCue> readCues(const std::filesystem::path& file,
                                    std::string& error) const;

    bool writeCues(const LibraryTrack& track,
                   std::string& error,
                   TraktorEmbeddedMetadataStatus* status = nullptr) const;

    bool makeCueTemplateTestFile(const std::filesystem::path& cleanInput,
                                 const std::filesystem::path& templateFile,
                                 const std::filesystem::path& output,
                                 const std::vector<SeratoCue>& cues,
                                 std::string& error) const;
};
