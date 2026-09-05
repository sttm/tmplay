#pragma once

#include "Track.hpp"

#include <string>
#include <vector>

struct PlaylistImportResult {
    std::string name;
    std::vector<Track> tracks;
    int missingFiles = 0;
};

// Imports local playlist references without copying or moving the audio files.
// Supported formats mirror ProducersCenter: Serato .crate, M3U/M3U8, Traktor
// .nml and Rekordbox .xml.
class PlaylistImporter {
public:
    static bool importFile(const std::string& playlistPath,
                           PlaylistImportResult& result,
                           std::string& error);
};
