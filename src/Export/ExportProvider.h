#pragma once

#include "../Library/LibraryModels.h"

#include <string>
#include <vector>

class ExportProvider {
public:
    virtual ~ExportProvider() = default;

    virtual std::string name() const = 0;

    virtual bool exportTrack(const LibraryTrack& track,
                             std::string& error) = 0;

    virtual bool exportPlaylist(const LibraryPlaylist& playlist,
                                const std::vector<LibraryTrack>& tracks,
                                std::string& error) = 0;
};
