#pragma once

#include "../Library/CueRepository.h"
#include "../Library/TrackRepository.h"
#include "../Serato/SeratoCueWriter.h"

#include <filesystem>
#include <string>

struct LibraryExportOptions {
    bool exportSerato = true;
    bool exportRekordbox = true;
    bool exportTraktor = true;
    bool exportJson = true;
    bool backupBeforeSeratoWrite = true;
    bool overwriteExistingSeratoCues = true;
    std::filesystem::path outputFolder;
    std::filesystem::path syncFolder;
};

class LibraryExporter {
public:
    LibraryExporter(TrackRepository& tracks,
                    CueRepository& cues,
                    SeratoCueWriter& seratoWriter);

    bool exportAll(const LibraryExportOptions& options, std::string& error);

private:
    TrackRepository& tracks_;
    CueRepository& cues_;
    SeratoCueWriter& seratoWriter_;
};
