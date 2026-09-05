#pragma once

#include "LibraryModels.h"

#include <string>
#include <vector>

class LibraryDatabase;

class LoopRepository {
public:
    explicit LoopRepository(LibraryDatabase& database);

    bool replaceLoops(const std::string& trackId,
                      const std::vector<LibraryLoop>& loops,
                      std::string& error);

private:
    LibraryDatabase& database_;
};
