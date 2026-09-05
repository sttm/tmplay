#pragma once

#include "ExportProvider.h"
#include "../Serato/SeratoCueWriter.h"

class SeratoExportProvider : public ExportProvider {
public:
    SeratoExportProvider(SeratoCueWriter& writer,
                         bool backupBeforeWrite,
                         bool overwriteExistingCues);

    std::string name() const override { return "Serato"; }
    bool exportTrack(const LibraryTrack& track, std::string& error) override;
    bool exportPlaylist(const LibraryPlaylist& playlist,
                        const std::vector<LibraryTrack>& tracks,
                        std::string& error) override;

private:
    SeratoCueWriter& writer_;
    bool backupBeforeWrite_ = true;
    bool overwriteExistingCues_ = true;
};
