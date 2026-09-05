#include "LibraryExporter.h"

#include "RekordboxExportProvider.h"
#include "SeratoExportProvider.h"
#include "TraktorExportProvider.h"
#include "../Sync/JsonSync.h"

#include <filesystem>

namespace fs = std::filesystem;

namespace {

fs::path defaultOutputFolder(const std::vector<LibraryTrack>& tracks)
{
    for (const auto& track : tracks) {
        if (!track.path.empty()) {
            return track.path.parent_path();
        }
    }
    return fs::current_path();
}

}  // namespace

LibraryExporter::LibraryExporter(TrackRepository& tracks,
                                 CueRepository& cues,
                                 SeratoCueWriter& seratoWriter)
    : tracks_(tracks)
    , cues_(cues)
    , seratoWriter_(seratoWriter)
{
}

bool LibraryExporter::exportAll(const LibraryExportOptions& options,
                                std::string& error)
{
    std::vector<LibraryTrack> tracks = tracks_.listTracks(error);
    if (!error.empty()) {
        return false;
    }
    for (auto& track : tracks) {
        track.cues = cues_.cuesForTrack(track.id, error);
        if (!error.empty()) {
            return false;
        }
    }

    fs::path output_folder = options.outputFolder.empty()
        ? defaultOutputFolder(tracks)
        : options.outputFolder;
    std::error_code ec;
    fs::create_directories(output_folder, ec);
    if (ec) {
        error = ec.message();
        return false;
    }

    if (!options.exportRekordbox) {
        fs::remove(output_folder / "tmplay_library.rekordbox.xml", ec);
        ec.clear();
    }
    if (!options.exportTraktor) {
        fs::remove(output_folder / "tmplay_library.traktor.nml", ec);
        ec.clear();
    }

    if (options.exportSerato) {
        SeratoExportProvider serato(seratoWriter_,
                                    options.backupBeforeSeratoWrite,
                                    options.overwriteExistingSeratoCues);
        for (const auto& track : tracks) {
            if (!track.cues.empty() && !serato.exportTrack(track, error)) {
                return false;
            }
        }
    }

    LibraryPlaylist all_tracks;
    all_tracks.id = (output_folder / "tmplay_library").string();
    all_tracks.name = "tmplay Library";
    for (const auto& track : tracks) {
        all_tracks.trackIds.push_back(track.id);
    }

    if (options.exportRekordbox) {
        RekordboxExportProvider rekordbox;
        if (!rekordbox.exportPlaylist(all_tracks, tracks, error)) {
            return false;
        }
    }

    if (options.exportTraktor) {
        TraktorExportProvider traktor;
        if (!traktor.exportPlaylist(all_tracks, tracks, error)) {
            return false;
        }
    }

    if (options.exportJson) {
        JsonSync sync;
        fs::path sync_folder = options.syncFolder.empty()
            ? (output_folder / "sync")
            : options.syncFolder;
        for (const auto& track : tracks) {
            if (!sync.exportTrack(track, sync_folder, error)) {
                return false;
            }
        }
    }

    return true;
}
