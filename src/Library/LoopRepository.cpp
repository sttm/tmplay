#include "LoopRepository.h"

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

LoopRepository::LoopRepository(LibraryDatabase& database)
    : database_(database)
{
}

bool LoopRepository::replaceLoops(const std::string& trackId,
                                  const std::vector<LibraryLoop>& loops,
                                  std::string& error)
{
    if (!database_.exec("BEGIN IMMEDIATE;", error)) {
        return false;
    }

    sqlite3_stmt* delete_stmt = nullptr;
    if (sqlite3_prepare_v2(database_.handle(),
                           "DELETE FROM loops WHERE track_id = ?;",
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
INSERT INTO loops (
    track_id, name, start_seconds, end_seconds, color, created_at, updated_at
) VALUES (?, ?, ?, ?, ?, ?, ?);
)sql";
    sqlite3_stmt* insert_stmt = nullptr;
    if (sqlite3_prepare_v2(database_.handle(), insert_sql, -1, &insert_stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_.handle());
        database_.exec("ROLLBACK;", error);
        return false;
    }

    std::int64_t now = nowSeconds();
    for (const auto& loop : loops) {
        sqlite3_reset(insert_stmt);
        sqlite3_clear_bindings(insert_stmt);
        bindText(insert_stmt, 1, trackId);
        bindText(insert_stmt, 2, loop.name);
        sqlite3_bind_double(insert_stmt, 3, loop.startSeconds);
        sqlite3_bind_double(insert_stmt, 4, loop.endSeconds);
        bindText(insert_stmt, 5, loop.color);
        sqlite3_bind_int64(insert_stmt, 6, now);
        sqlite3_bind_int64(insert_stmt, 7, now);
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
