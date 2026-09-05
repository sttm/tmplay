#pragma once

#include "AutoCueMarker.h"
#include "../Serato/SeratoCueWriter.h"
#include "../core/Config.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

struct AutoCueProgress {
    int current = 0;
    int total = 0;
    int success = 0;
    int errors = 0;
    std::string currentFile;
    std::string status;
    bool running = false;
    bool done = false;
};

class FolderProcessor {
public:
    using ProgressCallback = std::function<void(const AutoCueProgress&)>;
    using ResultCallback = std::function<bool(
        const std::filesystem::path&,
        const AutoCueResult&,
        const std::vector<SeratoCue>&,
        std::string&)>;

    FolderProcessor(AutoCueMarker& marker, SeratoCueWriter& writer);

    void processFolder(const std::filesystem::path& folder,
                       bool writeJson,
                       bool writeSerato,
                       bool backupBeforeWrite,
                       bool overwriteExistingCues,
                       bool cleanupAfterWrite,
                       const std::vector<AutoCueConfig::Slot>& cueSlots,
                       ProgressCallback onProgress,
                       std::atomic_bool& cancel,
                       ResultCallback onResult = {});
    void processFiles(std::vector<std::filesystem::path> files,
                      bool writeJson,
                      bool writeSerato,
                      bool backupBeforeWrite,
                      bool overwriteExistingCues,
                      bool cleanupAfterWrite,
                      const std::vector<AutoCueConfig::Slot>& cueSlots,
                      ProgressCallback onProgress,
                      std::atomic_bool& cancel,
                      ResultCallback onResult = {});

private:
    AutoCueMarker& marker_;
    SeratoCueWriter& writer_;
};
