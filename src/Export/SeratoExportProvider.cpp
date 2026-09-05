#include "SeratoExportProvider.h"

#include <cstdint>
#include <filesystem>

namespace {

std::uint32_t parseColor(const std::string& color)
{
    std::string value = color;
    if (value.starts_with("#")) {
        value.erase(0, 1);
    } else if (value.starts_with("0x") || value.starts_with("0X")) {
        value.erase(0, 2);
    }
    try {
        return (std::uint32_t)std::stoul(value, nullptr, 16);
    } catch (...) {
        return 0xffffffu;
    }
}

std::vector<SeratoCue> toSeratoCues(const LibraryTrack& track)
{
    std::vector<SeratoCue> cues;
    cues.reserve(track.cues.size());
    for (const auto& cue : track.cues) {
        cues.push_back({
            cue.index,
            cue.name,
            cue.positionSeconds,
            parseColor(cue.color),
        });
    }
    return cues;
}

}  // namespace

SeratoExportProvider::SeratoExportProvider(SeratoCueWriter& writer,
                                           bool backupBeforeWrite,
                                           bool overwriteExistingCues)
    : writer_(writer)
    , backupBeforeWrite_(backupBeforeWrite)
    , overwriteExistingCues_(overwriteExistingCues)
{
}

bool SeratoExportProvider::exportTrack(const LibraryTrack& track, std::string& error)
{
    if (track.path.empty()) {
        error = "Serato export skipped: empty track path";
        return false;
    }
    return writer_.writeCues(track.path,
                             toSeratoCues(track),
                             error,
                             backupBeforeWrite_,
                             overwriteExistingCues_);
}

bool SeratoExportProvider::exportPlaylist(const LibraryPlaylist&,
                                          const std::vector<LibraryTrack>&,
                                          std::string& error)
{
    error = "Serato playlist export is not needed for embedded cue export";
    return false;
}
