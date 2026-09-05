#pragma once

#include "LibraryModels.h"

#include <string>
#include <vector>

class LibraryDatabase;

class CueRepository {
public:
    explicit CueRepository(LibraryDatabase& database);

    bool replaceCues(const std::string& trackId,
                     const std::vector<LibraryCue>& cues,
                     std::string& error);
    std::vector<LibraryCue> cuesForTrack(const std::string& trackId,
                                         std::string& error) const;

private:
    LibraryDatabase& database_;
};
