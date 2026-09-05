#include "TelegramRepository.h"

#include "../Library/LibraryDatabase.h"

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

std::string textAt(sqlite3_stmt* stmt, int column)
{
    const unsigned char* text = sqlite3_column_text(stmt, column);
    return text != nullptr ? std::string((const char*)text) : std::string{};
}

TelegramAudioItem audioItemFromStatement(sqlite3_stmt* stmt)
{
    TelegramAudioItem item;
    item.chatId = textAt(stmt, 0);
    item.messageId = sqlite3_column_int(stmt, 1);
    item.fileId = textAt(stmt, 2);
    item.fileUniqueId = textAt(stmt, 3);
    item.fileName = textAt(stmt, 4);
    item.performer = textAt(stmt, 5);
    item.title = textAt(stmt, 6);
    item.mimeType = textAt(stmt, 7);
    item.duration = sqlite3_column_int(stmt, 8);
    item.fileSize = (std::uintmax_t)sqlite3_column_int64(stmt, 9);
    item.telegramDate = (std::int64_t)sqlite3_column_int64(stmt, 10);
    item.downloaded = sqlite3_column_int(stmt, 11) != 0;
    item.localPath = textAt(stmt, 12);
    item.importedTrackId = textAt(stmt, 13);
    item.createdAt = (std::int64_t)sqlite3_column_int64(stmt, 14);
    item.updatedAt = (std::int64_t)sqlite3_column_int64(stmt, 15);
    return item;
}

}  // namespace

TelegramRepository::TelegramRepository(LibraryDatabase& database)
    : database_(database)
{
}

bool TelegramRepository::upsertChat(const TelegramChat& chat, std::string& error)
{
    static constexpr const char* sql = R"sql(
INSERT INTO telegram_chats (
    chat_id, title, type, folder_path, last_seen_update_id, created_at, updated_at
) VALUES (?, ?, ?, ?, ?, ?, ?)
ON CONFLICT(chat_id) DO UPDATE SET
    title = excluded.title,
    type = excluded.type,
    folder_path = excluded.folder_path,
    last_seen_update_id = MAX(telegram_chats.last_seen_update_id,
                              excluded.last_seen_update_id),
    updated_at = excluded.updated_at;
)sql";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(database_.handle(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_.handle());
        return false;
    }
    std::int64_t created = chat.createdAt > 0 ? chat.createdAt : nowSeconds();
    std::int64_t updated = chat.updatedAt > 0 ? chat.updatedAt : created;
    bindText(stmt, 1, chat.chatId);
    bindText(stmt, 2, chat.title);
    bindText(stmt, 3, chat.type);
    bindText(stmt, 4, chat.folderPath.string());
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)chat.lastSeenUpdateId);
    sqlite3_bind_int64(stmt, 6, (sqlite3_int64)created);
    sqlite3_bind_int64(stmt, 7, (sqlite3_int64)updated);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) {
        error = sqlite3_errmsg(database_.handle());
    }
    sqlite3_finalize(stmt);
    return ok;
}

bool TelegramRepository::upsertAudioItem(const TelegramAudioItem& item,
                                         std::string& error)
{
    static constexpr const char* sql = R"sql(
INSERT INTO telegram_audio_items (
    chat_id, message_id, file_id, file_unique_id, file_name, performer, title,
    mime_type, duration, file_size, telegram_date, downloaded, local_path,
    imported_track_id, created_at, updated_at
) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
ON CONFLICT(chat_id, message_id, file_id) DO UPDATE SET
    file_unique_id = excluded.file_unique_id,
    file_name = excluded.file_name,
    performer = excluded.performer,
    title = excluded.title,
    mime_type = excluded.mime_type,
    duration = excluded.duration,
    file_size = excluded.file_size,
    telegram_date = excluded.telegram_date,
    downloaded = CASE
        WHEN telegram_audio_items.downloaded != 0 THEN telegram_audio_items.downloaded
        ELSE excluded.downloaded
    END,
    local_path = CASE
        WHEN telegram_audio_items.local_path != '' THEN telegram_audio_items.local_path
        ELSE excluded.local_path
    END,
    imported_track_id = CASE
        WHEN telegram_audio_items.imported_track_id != '' THEN telegram_audio_items.imported_track_id
        ELSE excluded.imported_track_id
    END,
    updated_at = excluded.updated_at;
)sql";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(database_.handle(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_.handle());
        return false;
    }
    std::int64_t created = item.createdAt > 0 ? item.createdAt : nowSeconds();
    std::int64_t updated = item.updatedAt > 0 ? item.updatedAt : created;
    bindText(stmt, 1, item.chatId);
    sqlite3_bind_int(stmt, 2, item.messageId);
    bindText(stmt, 3, item.fileId);
    bindText(stmt, 4, item.fileUniqueId);
    bindText(stmt, 5, item.fileName);
    bindText(stmt, 6, item.performer);
    bindText(stmt, 7, item.title);
    bindText(stmt, 8, item.mimeType);
    sqlite3_bind_int(stmt, 9, item.duration);
    sqlite3_bind_int64(stmt, 10, (sqlite3_int64)item.fileSize);
    sqlite3_bind_int64(stmt, 11, (sqlite3_int64)item.telegramDate);
    sqlite3_bind_int(stmt, 12, item.downloaded ? 1 : 0);
    bindText(stmt, 13, item.localPath.string());
    bindText(stmt, 14, item.importedTrackId);
    sqlite3_bind_int64(stmt, 15, (sqlite3_int64)created);
    sqlite3_bind_int64(stmt, 16, (sqlite3_int64)updated);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) {
        error = sqlite3_errmsg(database_.handle());
    }
    sqlite3_finalize(stmt);
    return ok;
}

std::optional<TelegramAudioItem>
TelegramRepository::findAudioItem(const std::string& chatId,
                                  int messageId,
                                  std::string& error) const
{
    static constexpr const char* sql = R"sql(
SELECT chat_id, message_id, file_id, file_unique_id, file_name, performer, title,
       mime_type, duration, file_size, telegram_date, downloaded, local_path,
       imported_track_id, created_at, updated_at
FROM telegram_audio_items
WHERE chat_id = ? AND message_id = ?
ORDER BY id ASC
LIMIT 1;
)sql";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(database_.handle(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_.handle());
        return std::nullopt;
    }
    bindText(stmt, 1, chatId);
    sqlite3_bind_int(stmt, 2, messageId);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }
    TelegramAudioItem item = audioItemFromStatement(stmt);
    sqlite3_finalize(stmt);
    return item;
}

std::vector<TelegramChat> TelegramRepository::listChats(std::string& error) const
{
    static constexpr const char* sql = R"sql(
SELECT chat_id, title, type, folder_path, last_seen_update_id, created_at, updated_at
FROM telegram_chats
ORDER BY title COLLATE NOCASE ASC;
)sql";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(database_.handle(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_.handle());
        return {};
    }

    std::vector<TelegramChat> chats;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        TelegramChat chat;
        chat.chatId = textAt(stmt, 0);
        chat.title = textAt(stmt, 1);
        chat.type = textAt(stmt, 2);
        chat.folderPath = textAt(stmt, 3);
        chat.lastSeenUpdateId = (std::int64_t)sqlite3_column_int64(stmt, 4);
        chat.createdAt = (std::int64_t)sqlite3_column_int64(stmt, 5);
        chat.updatedAt = (std::int64_t)sqlite3_column_int64(stmt, 6);
        chats.push_back(std::move(chat));
    }
    sqlite3_finalize(stmt);
    return chats;
}

std::vector<TelegramAudioItem>
TelegramRepository::listAudioItems(const std::string& chatId, std::string& error) const
{
    static constexpr const char* sql = R"sql(
SELECT chat_id, message_id, file_id, file_unique_id, file_name, performer, title,
       mime_type, duration, file_size, telegram_date, downloaded, local_path,
       imported_track_id, created_at, updated_at
FROM telegram_audio_items
WHERE chat_id = ?
ORDER BY telegram_date DESC, message_id DESC;
)sql";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(database_.handle(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_.handle());
        return {};
    }
    bindText(stmt, 1, chatId);

    std::vector<TelegramAudioItem> items;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        items.push_back(audioItemFromStatement(stmt));
    }
    sqlite3_finalize(stmt);
    return items;
}

std::int64_t TelegramRepository::lastUpdateId(std::string& error) const
{
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(database_.handle(),
                           "SELECT value FROM sync_state WHERE key = 'telegram.last_update_id';",
                           -1,
                           &stmt,
                           nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_.handle());
        return 0;
    }
    std::int64_t value = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        value = std::stoll(textAt(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return value;
}

bool TelegramRepository::setLastUpdateId(std::int64_t updateId, std::string& error)
{
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(database_.handle(),
                           R"sql(
INSERT INTO sync_state (key, value)
VALUES ('telegram.last_update_id', ?)
ON CONFLICT(key) DO UPDATE SET value = excluded.value;
)sql",
                           -1,
                           &stmt,
                           nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_.handle());
        return false;
    }
    bindText(stmt, 1, std::to_string(updateId));
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) {
        error = sqlite3_errmsg(database_.handle());
    }
    sqlite3_finalize(stmt);
    return ok;
}
