#pragma once

#include "ExportProvider.h"

class RekordboxExportProvider : public ExportProvider {
public:
    std::string name() const override { return "Rekordbox"; }
    bool exportTrack(const LibraryTrack& track, std::string& error) override;
    bool exportPlaylist(const LibraryPlaylist& playlist,
                        const std::vector<LibraryTrack>& tracks,
                        std::string& error) override;
};

