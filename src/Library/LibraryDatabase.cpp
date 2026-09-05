#include "LibraryDatabase.h"

#include <cstdlib>
#include <filesystem>

#include <sqlite3.h>

namespace fs = std::filesystem;

namespace {

fs::path expandUserPath(fs::path path)
{
    std::string value = path.string();
    if (value == "~" || value.starts_with("~/")) {
        const char* home = std::getenv("HOME");
        if (home != nullptr && *home != '\0') {
            value = std::string(home) + value.substr(1);
            return fs::path(value);
        }
    }
    return path;
}

}  // namespace

LibraryDatabase::~LibraryDatabase()
{
    if (db_ != nullptr) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool LibraryDatabase::open(const fs::path& path, std::string& error)
{
    if (db_ != nullptr) {
        return true;
    }

    path_ = expandUserPath(path);
    std::error_code ec;
    fs::create_directories(path_.parent_path(), ec);
    if (ec) {
        error = ec.message();
        return false;
    }

    if (sqlite3_open(path_.string().c_str(), &db_) != SQLITE_OK) {
        error = db_ != nullptr ? sqlite3_errmsg(db_) : "Unable to open SQLite database";
        return false;
    }
    exec("PRAGMA busy_timeout = 5000;", error);
    exec("PRAGMA foreign_keys = ON;", error);
    exec("PRAGMA journal_mode = WAL;", error);
    return true;
}

bool LibraryDatabase::initialize(std::string& error)
{
    return exec(R"sql(
CREATE TABLE IF NOT EXISTS tracks (
    id TEXT PRIMARY KEY,
    path TEXT NOT NULL,
    title TEXT,
    artist TEXT,
    album TEXT,
    duration REAL,
    bpm REAL,
    musical_key TEXT,
    camelot_key TEXT,
    genre TEXT,
    rating INTEGER DEFAULT 0,
    comments TEXT,
    file_size INTEGER,
    file_mtime INTEGER,
    content_hash TEXT,
    waveform_path TEXT,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS cues (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    track_id TEXT NOT NULL,
    cue_index INTEGER NOT NULL,
    name TEXT NOT NULL,
    type TEXT NOT NULL,
    position_seconds REAL NOT NULL,
    color TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    FOREIGN KEY(track_id) REFERENCES tracks(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS loops (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    track_id TEXT NOT NULL,
    name TEXT,
    start_seconds REAL NOT NULL,
    end_seconds REAL NOT NULL,
    color TEXT,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    FOREIGN KEY(track_id) REFERENCES tracks(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS playlists (
    id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    description TEXT,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS playlist_tracks (
    playlist_id TEXT NOT NULL,
    track_id TEXT NOT NULL,
    position INTEGER NOT NULL,
    added_at INTEGER NOT NULL,
    PRIMARY KEY(playlist_id, track_id),
    FOREIGN KEY(playlist_id) REFERENCES playlists(id) ON DELETE CASCADE,
    FOREIGN KEY(track_id) REFERENCES tracks(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS sync_state (
    key TEXT PRIMARY KEY,
    value TEXT
);

CREATE TABLE IF NOT EXISTS telegram_chats (
    chat_id TEXT PRIMARY KEY,
    title TEXT,
    type TEXT,
    folder_path TEXT,
    last_seen_update_id INTEGER,
    created_at INTEGER,
    updated_at INTEGER
);

CREATE TABLE IF NOT EXISTS telegram_audio_items (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    chat_id TEXT NOT NULL,
    message_id INTEGER NOT NULL,
    file_id TEXT NOT NULL,
    file_unique_id TEXT,
    file_name TEXT,
    performer TEXT,
    title TEXT,
    mime_type TEXT,
    duration INTEGER,
    file_size INTEGER,
    telegram_date INTEGER,
    downloaded INTEGER DEFAULT 0,
    local_path TEXT,
    imported_track_id TEXT,
    created_at INTEGER,
    updated_at INTEGER,
    UNIQUE(chat_id, message_id, file_id),
    FOREIGN KEY(chat_id) REFERENCES telegram_chats(chat_id) ON DELETE CASCADE
);
)sql", error);
}

bool LibraryDatabase::exec(const std::string& sql, std::string& error)
{
    char* message = nullptr;
    int result = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &message);
    if (result != SQLITE_OK) {
        error = message != nullptr ? message : sqlite3_errmsg(db_);
        sqlite3_free(message);
        return false;
    }
    return true;
}

sqlite3* LibraryDatabase::handle() const
{
    return db_;
}

bool LibraryDatabase::isOpen() const
{
    return db_ != nullptr;
}

const fs::path& LibraryDatabase::path() const
{
    return path_;
}
