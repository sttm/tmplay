#pragma once

#include "LibraryModels.h"

#include <optional>
#include <string>
#include <vector>

class LibraryDatabase;
struct Track;

struct LibraryStats {
    int tracks = 0;
    int cues = 0;
    int loops = 0;
    int playlists = 0;
};

class TrackRepository {
public:
    explicit TrackRepository(LibraryDatabase& database);

    bool upsertTrack(const LibraryTrack& track, std::string& error);
    std::optional<LibraryTrack> findById(const std::string& id,
                                         std::string& error) const;
    std::optional<LibraryTrack> findByPath(const std::string& path,
                                           std::string& error) const;
    std::vector<LibraryTrack> listTracks(std::string& error) const;
    LibraryStats stats(std::string& error) const;

private:
    LibraryDatabase& database_;
};

LibraryTrack libraryTrackFromTrack(const Track& track);
