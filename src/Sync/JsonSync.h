#pragma once

#include "../Library/LibraryModels.h"

#include <filesystem>
#include <optional>
#include <string>

class JsonSync {
public:
    bool exportTrack(const LibraryTrack& track,
                     const std::filesystem::path& folder,
                     std::string& error);
    std::optional<LibraryTrack> importTrack(const std::filesystem::path& file,
                                            std::string& error);

    bool exportLibrary(const std::filesystem::path& folder,
                       std::string& error);
    bool importLibrary(const std::filesystem::path& folder,
                       std::string& error);
};
