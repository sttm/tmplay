#pragma once

#include "LibraryModels.h"

#include <string>

class LibraryDatabase;

class PlaylistRepository {
public:
    explicit PlaylistRepository(LibraryDatabase& database);

    bool upsertPlaylist(const LibraryPlaylist& playlist, std::string& error);
    bool replacePlaylistTracks(const std::string& playlistId,
                               const std::vector<std::string>& trackIds,
                               std::string& error);

private:
    LibraryDatabase& database_;
};
