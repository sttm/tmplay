#pragma once

#include "ExportProvider.h"

class TraktorExportProvider : public ExportProvider {
public:
    std::string name() const override { return "Traktor"; }
    bool exportTrack(const LibraryTrack& track, std::string& error) override;
    bool exportPlaylist(const LibraryPlaylist& playlist,
                        const std::vector<LibraryTrack>& tracks,
                        std::string& error) override;
};

