#include "CueRepository.h"

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

CueRepository::CueRepository(LibraryDatabase& database)
    : database_(database)
{
}

bool CueRepository::replaceCues(const std::string& trackId,
                                const std::vector<LibraryCue>& cues,
                                std::string& error)
{
    if (!database_.exec("BEGIN IMMEDIATE;", error)) {
        return false;
    }

    sqlite3_stmt* delete_stmt = nullptr;
    if (sqlite3_prepare_v2(database_.handle(),
                           "DELETE FROM cues WHERE track_id = ?;",
                           -1,
                           &delete_stmt,
                           nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_.handle());
        database_.exec("ROLLBACK;", error);
        return false;
    }
    bindText(delete_stmt, 1, trackId);
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
INSERT INTO cues (
    track_id, cue_index, name, type, position_seconds, color, created_at, updated_at
) VALUES (?, ?, ?, ?, ?, ?, ?, ?);
)sql";
    sqlite3_stmt* insert_stmt = nullptr;
    if (sqlite3_prepare_v2(database_.handle(), insert_sql, -1, &insert_stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_.handle());
        database_.exec("ROLLBACK;", error);
        return false;
    }

    std::int64_t now = nowSeconds();
    for (const auto& cue : cues) {
        sqlite3_reset(insert_stmt);
        sqlite3_clear_bindings(insert_stmt);
        bindText(insert_stmt, 1, trackId);
        sqlite3_bind_int(insert_stmt, 2, cue.index);
        bindText(insert_stmt, 3, cue.name);
        bindText(insert_stmt, 4, cue.type);
        sqlite3_bind_double(insert_stmt, 5, cue.positionSeconds);
        bindText(insert_stmt, 6, cue.color);
        sqlite3_bind_int64(insert_stmt, 7, now);
        sqlite3_bind_int64(insert_stmt, 8, now);
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

std::vector<LibraryCue> CueRepository::cuesForTrack(const std::string& trackId,
                                                    std::string& error) const
{
    static constexpr const char* sql = R"sql(
SELECT cue_index, name, type, position_seconds, color
FROM cues
WHERE track_id = ?
ORDER BY cue_index ASC;
)sql";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(database_.handle(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_.handle());
        return {};
    }
    bindText(stmt, 1, trackId);

    std::vector<LibraryCue> cues;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        auto textAt = [&](int column) {
            const unsigned char* text = sqlite3_column_text(stmt, column);
            return text != nullptr ? std::string((const char*)text) : std::string{};
        };
        cues.push_back({
            sqlite3_column_int(stmt, 0),
            textAt(1),
            textAt(2),
            sqlite3_column_double(stmt, 3),
            textAt(4),
        });
    }
    sqlite3_finalize(stmt);
    return cues;
}
