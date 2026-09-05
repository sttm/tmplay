#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct SeratoCue {
    int index = 0;
    std::string name;
    double seconds = 0.0;
    std::uint32_t colorRgb = 0xffffff;
};

class SeratoCueWriter {
public:
    std::vector<SeratoCue> readCues(const std::filesystem::path& file,
                                    std::string& error) const;
    bool writeCues(const std::filesystem::path& file,
                   const std::vector<SeratoCue>& cues,
                   std::string& error);
    bool writeCues(const std::filesystem::path& file,
                   const std::vector<SeratoCue>& cues,
                   std::string& error,
                   bool backupBeforeWrite,
                   bool overwriteExistingCues);
};
