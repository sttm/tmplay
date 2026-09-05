#pragma once

#include "../Library/LibraryModels.h"

class ConflictResolver {
public:
    enum class Strategy {
        LastWriteWins,
    };

    LibraryTrack resolve(const LibraryTrack& local,
                         const LibraryTrack& incoming,
                         Strategy strategy = Strategy::LastWriteWins) const
    {
        if (strategy == Strategy::LastWriteWins &&
            incoming.updatedAt >= local.updatedAt) {
            return incoming;
        }
        return local;
    }
};
