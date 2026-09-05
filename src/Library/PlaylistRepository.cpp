#include "PlaylistRepository.h"

#include "LibraryDatabase.h"

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

}  // namespace

PlaylistRepository::PlaylistRepository(LibraryDatabase& database)
    : database_(database)
{
}

bool PlaylistRepository::upsertPlaylist(const LibraryPlaylist& playlist,
                                        std::string& error)
{
    static constexpr const char* sql = R"sql(
INSERT INTO playlists (id, name, description, created_at, updated_at)
VALUES (?, ?, ?, ?, ?)
ON CONFLICT(id) DO UPDATE SET
    name = excluded.name,
    description = excluded.description,
    updated_at = excluded.updated_at;
)sql";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(database_.handle(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_.handle());
        return false;
    }

    std::int64_t created = playlist.createdAt > 0 ? playlist.createdAt : nowSeconds();
    std::int64_t updated = playlist.updatedAt > 0 ? playlist.updatedAt : created;
    bindText(stmt, 1, playlist.id);
    bindText(stmt, 2, playlist.name);
    bindText(stmt, 3, playlist.description);
    sqlite3_bind_int64(stmt, 4, created);
    sqlite3_bind_int64(stmt, 5, updated);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) {
        error = sqlite3_errmsg(database_.handle());
    }
    sqlite3_finalize(stmt);
    return ok;
}

bool PlaylistRepository::replacePlaylistTracks(const std::string& playlistId,
                                               const std::vector<std::string>& trackIds,
                                               std::string& error)
{
    if (!database_.exec("BEGIN IMMEDIATE;", error)) {
        return false;
    }

    sqlite3_stmt* delete_stmt = nullptr;
    if (sqlite3_prepare_v2(database_.handle(),
                           "DELETE FROM playlist_tracks WHERE playlist_id = ?;",
                           -1,
                           &delete_stmt,
                           nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_.handle());
        database_.exec("ROLLBACK;", error);
        return false;
    }
    bindText(delete_stmt, 1, playlistId);
    bool ok = sqlite3_step(delete_stmt) == SQLITE_DONE;
    if (!ok) {
        error = sqlite3_errmsg(database_.handle());
    }
    sqlite3_finalize(delete_stmt);
    if (!ok) {
        database_.exec("ROLLBACK;", error);
        return false;
    }

    static constexpr const char* insert_sql = R"sql(
INSERT INTO playlist_tracks (playlist_id, track_id, position, added_at)
VALUES (?, ?, ?, ?);
)sql";
    sqlite3_stmt* insert_stmt = nullptr;
    if (sqlite3_prepare_v2(database_.handle(), insert_sql, -1, &insert_stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_.handle());
        database_.exec("ROLLBACK;", error);
        return false;
    }

    std::int64_t now = nowSeconds();
    for (std::size_t i = 0; i < trackIds.size(); ++i) {
        sqlite3_reset(insert_stmt);
        sqlite3_clear_bindings(insert_stmt);
        bindText(insert_stmt, 1, playlistId);
        bindText(insert_stmt, 2, trackIds[i]);
        sqlite3_bind_int(insert_stmt, 3, (int)i);
        sqlite3_bind_int64(insert_stmt, 4, now);
        if (sqlite3_step(insert_stmt) != SQLITE_DONE) {
            error = sqlite3_errmsg(database_.handle());
            sqlite3_finalize(insert_stmt);
            database_.exec("ROLLBACK;", error);
            return false;
        }
    }

    sqlite3_finalize(insert_stmt);
    return database_.exec("COMMIT;", error);
}
