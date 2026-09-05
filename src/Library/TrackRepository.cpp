#include "TrackRepository.h"

#include "LibraryDatabase.h"
#include "TrackId.h"
#include "../core/Track.hpp"

#include <ctime>

#include <sqlite3.h>

namespace {

std::int64_t nowSeconds()
{
    return (std::int64_t)std::time(nullptr);
}

void bindText(sqlite3_stmt* stmt, int index, const std::string& value)
{
    sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT);
}

LibraryTrack trackFromStatement(sqlite3_stmt* stmt)
{
    auto textAt = [&](int column) {
        const unsigned char* text = sqlite3_column_text(stmt, column);
        return text != nullptr ? std::string((const char*)text) : std::string{};
    };
    LibraryTrack track;
    track.id = textAt(0);
    track.path = textAt(1);
    track.title = textAt(2);
    track.artist = textAt(3);
    track.album = textAt(4);
    track.duration = sqlite3_column_double(stmt, 5);
    track.bpm = sqlite3_column_double(stmt, 6);
    track.key = textAt(7);
    track.camelotKey = textAt(8);
    track.genre = textAt(9);
    track.rating = sqlite3_column_int(stmt, 10);
    track.comments = textAt(11);
    track.fileSize = (std::uintmax_t)sqlite3_column_int64(stmt, 12);
    track.fileMtime = (std::int64_t)sqlite3_column_int64(stmt, 13);
    track.contentHash = textAt(14);
    track.waveformPath = textAt(15);
    track.createdAt = (std::int64_t)sqlite3_column_int64(stmt, 16);
    track.updatedAt = (std::int64_t)sqlite3_column_int64(stmt, 17);
    return track;
}

int countRows(sqlite3* db, const char* table, std::string& error)
{
    std::string sql = "SELECT COUNT(*) FROM ";
    sql += table;
    sql += ";";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        return 0;
    }
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    } else {
        error = sqlite3_errmsg(db);
    }
    sqlite3_finalize(stmt);
    return count;
}

}  // namespace

TrackRepository::TrackRepository(LibraryDatabase& database)
    : database_(database)
{
}

bool TrackRepository::upsertTrack(const LibraryTrack& track, std::string& error)
{
    static constexpr const char* sql = R"sql(
INSERT INTO tracks (
    id, path, title, artist, album, duration, bpm, musical_key, camelot_key,
    genre, rating, comments, file_size, file_mtime, content_hash,
    waveform_path, created_at, updated_at
) VALUES (
    ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?
)
ON CONFLICT(id) DO UPDATE SET
    path = excluded.path,
    title = excluded.title,
    artist = excluded.artist,
    album = excluded.album,
    duration = excluded.duration,
    bpm = excluded.bpm,
    musical_key = excluded.musical_key,
    camelot_key = excluded.camelot_key,
    genre = excluded.genre,
    rating = excluded.rating,
    comments = excluded.comments,
    file_size = excluded.file_size,
    file_mtime = excluded.file_mtime,
    content_hash = excluded.content_hash,
    waveform_path = excluded.waveform_path,
    updated_at = excluded.updated_at;
)sql";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(database_.handle(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_.handle());
        return false;
    }

    std::int64_t created = track.createdAt > 0 ? track.createdAt : nowSeconds();
    std::int64_t updated = track.updatedAt > 0 ? track.updatedAt : created;

    bindText(stmt, 1, track.id);
    bindText(stmt, 2, track.path.string());
    bindText(stmt, 3, track.title);
    bindText(stmt, 4, track.artist);
    bindText(stmt, 5, track.album);
    sqlite3_bind_double(stmt, 6, track.duration);
    sqlite3_bind_double(stmt, 7, track.bpm);
    bindText(stmt, 8, track.key);
    bindText(stmt, 9, track.camelotKey);
    bindText(stmt, 10, track.genre);
    sqlite3_bind_int(stmt, 11, track.rating);
    bindText(stmt, 12, track.comments);
    sqlite3_bind_int64(stmt, 13, (sqlite3_int64)track.fileSize);
    sqlite3_bind_int64(stmt, 14, (sqlite3_int64)track.fileMtime);
    bindText(stmt, 15, track.contentHash);
    bindText(stmt, 16, track.waveformPath);
    sqlite3_bind_int64(stmt, 17, (sqlite3_int64)created);
    sqlite3_bind_int64(stmt, 18, (sqlite3_int64)updated);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) {
        error = sqlite3_errmsg(database_.handle());
    }
    sqlite3_finalize(stmt);
    return ok;
}

std::optional<LibraryTrack> TrackRepository::findById(const std::string& id,
                                                      std::string& error) const
{
    static constexpr const char* sql = R"sql(
SELECT id, path, title, artist, album, duration, bpm, musical_key, camelot_key,
       genre, rating, comments, file_size, file_mtime, content_hash,
       waveform_path, created_at, updated_at
FROM tracks WHERE id = ? LIMIT 1;
)sql";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(database_.handle(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_.handle());
        return std::nullopt;
    }
    bindText(stmt, 1, id);

    LibraryTrack track;
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }
    track = trackFromStatement(stmt);
    sqlite3_finalize(stmt);
    return track;
}

std::optional<LibraryTrack> TrackRepository::findByPath(const std::string& path,
                                                        std::string& error) const
{
    static constexpr const char* sql = R"sql(
SELECT id, path, title, artist, album, duration, bpm, musical_key, camelot_key,
       genre, rating, comments, file_size, file_mtime, content_hash,
       waveform_path, created_at, updated_at
FROM tracks WHERE path = ? LIMIT 1;
)sql";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(database_.handle(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_.handle());
        return std::nullopt;
    }
    bindText(stmt, 1, path);

    LibraryTrack track;
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }
    track = trackFromStatement(stmt);
    sqlite3_finalize(stmt);
    return track;
}

std::vector<LibraryTrack> TrackRepository::listTracks(std::string& error) const
{
    static constexpr const char* sql = R"sql(
SELECT id, path, title, artist, album, duration, bpm, musical_key, camelot_key,
       genre, rating, comments, file_size, file_mtime, content_hash,
       waveform_path, created_at, updated_at
FROM tracks
ORDER BY path ASC;
)sql";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(database_.handle(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_.handle());
        return {};
    }

    std::vector<LibraryTrack> tracks;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        tracks.push_back(trackFromStatement(stmt));
    }
    sqlite3_finalize(stmt);
    return tracks;
}

LibraryStats TrackRepository::stats(std::string& error) const
{
    LibraryStats stats;
    stats.tracks = countRows(database_.handle(), "tracks", error);
    if (!error.empty()) {
        return stats;
    }
    stats.cues = countRows(database_.handle(), "cues", error);
    if (!error.empty()) {
        return stats;
    }
    stats.loops = countRows(database_.handle(), "loops", error);
    if (!error.empty()) {
        return stats;
    }
    stats.playlists = countRows(database_.handle(), "playlists", error);
    return stats;
}

LibraryTrack libraryTrackFromTrack(const Track& track)
{
    LibraryTrack library_track;
    auto identity = generateTrackIdentity(track.id, track.duration);
    library_track.id = identity.id;
    library_track.path = track.id;
    library_track.title = track.title;
    library_track.duration = track.duration;
    library_track.bpm = track.bpm;
    library_track.key = track.key;
    library_track.genre = track.genre;
    library_track.fileSize = identity.fileSize > 0 ? identity.fileSize : track.sizeBytes;
    library_track.fileMtime = identity.fileMtime;
    library_track.contentHash = identity.contentHash;
    library_track.updatedAt = nowSeconds();
    return library_track;
}
