#include "TelegramInboxService.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string_view>

namespace fs = std::filesystem;

namespace {

fs::path expandUserPath(fs::path path)
{
    std::string value = path.string();
    if (value == "~" || value.starts_with("~/")) {
        const char* home = std::getenv("HOME");
        if (home != nullptr && *home != '\0') {
            return fs::path(std::string(home) + value.substr(1));
        }
    }
    return path;
}

std::string lowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return value;
}

std::string sanitizedFolderName(std::string value)
{
    for (char& c : value) {
        if (c == '/' || c == '\\' || c == ':' || c == '\0') {
            c = '_';
        }
    }
    while (!value.empty() && std::isspace((unsigned char)value.back())) {
        value.pop_back();
    }
    while (!value.empty() && std::isspace((unsigned char)value.front())) {
        value.erase(value.begin());
    }
    return value.empty() ? std::string("Telegram Chat") : value;
}

std::string extensionOf(const std::string& fileName)
{
    fs::path path(fileName);
    std::string ext = path.extension().string();
    if (ext.starts_with(".")) {
        ext.erase(ext.begin());
    }
    return lowerCopy(ext);
}

bool extensionAllowed(const std::string& fileName,
                      const std::vector<std::string>& allowed)
{
    std::string ext = extensionOf(fileName);
    if (ext.empty()) {
        return false;
    }
    for (auto candidate : allowed) {
        candidate = lowerCopy(candidate);
        if (candidate.starts_with(".")) {
            candidate.erase(candidate.begin());
        }
        if (candidate == ext) {
            return true;
        }
    }
    return false;
}

std::string fallbackFileName(const TelegramAudioItem& item)
{
    if (!item.fileName.empty()) {
        return item.fileName;
    }
    std::string base;
    if (!item.performer.empty()) {
        base += item.performer + " - ";
    }
    base += item.title.empty() ? ("telegram_" + std::to_string(item.messageId))
                               : item.title;
    return sanitizedFolderName(base) + ".mp3";
}

std::string jsonStringField(std::string_view json, std::string_view key)
{
    std::string needle = "\"" + std::string(key) + "\":";
    std::size_t pos = json.find(needle);
    if (pos == std::string_view::npos) {
        return {};
    }
    pos += needle.size();
    while (pos < json.size() && std::isspace((unsigned char)json[pos])) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '"') {
        return {};
    }
    ++pos;
    std::string value;
    bool escaped = false;
    for (; pos < json.size(); ++pos) {
        char c = json[pos];
        if (escaped) {
            switch (c) {
            case 'n':
                value += '\n';
                break;
            case 'r':
                value += '\r';
                break;
            case 't':
                value += '\t';
                break;
            default:
                value += c;
                break;
            }
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') {
            break;
        }
        value += c;
    }
    return value;
}

double jsonNumberField(std::string_view json, std::string_view key)
{
    std::string needle = "\"" + std::string(key) + "\":";
    std::size_t pos = json.find(needle);
    if (pos == std::string_view::npos) {
        return 0.0;
    }
    pos += needle.size();
    while (pos < json.size() && std::isspace((unsigned char)json[pos])) {
        ++pos;
    }
    std::size_t end = pos;
    while (end < json.size() &&
           (std::isdigit((unsigned char)json[end]) || json[end] == '-' ||
            json[end] == '+' || json[end] == '.')) {
        ++end;
    }
    if (end <= pos) {
        return 0.0;
    }
    return std::stod(std::string(json.substr(pos, end - pos)));
}

std::optional<std::string_view> objectField(std::string_view json,
                                            std::string_view key)
{
    std::string needle = "\"" + std::string(key) + "\":";
    std::size_t pos = json.find(needle);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    pos += needle.size();
    while (pos < json.size() && std::isspace((unsigned char)json[pos])) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '{') {
        return std::nullopt;
    }

    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    std::size_t start = pos;
    for (; pos < json.size(); ++pos) {
        char c = json[pos];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0) {
                return json.substr(start, pos - start + 1);
            }
        }
    }
    return std::nullopt;
}

std::vector<std::string_view> arrayObjects(std::string_view json,
                                           std::string_view key)
{
    std::vector<std::string_view> objects;
    std::string needle = "\"" + std::string(key) + "\":[";
    std::size_t pos = json.find(needle);
    if (pos == std::string_view::npos) {
        return objects;
    }
    pos += needle.size();
    int depth = 0;
    std::size_t object_start = std::string_view::npos;
    bool in_string = false;
    bool escaped = false;
    for (; pos < json.size(); ++pos) {
        char c = json[pos];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            if (depth == 0) {
                object_start = pos;
            }
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0 && object_start != std::string_view::npos) {
                objects.push_back(json.substr(object_start, pos - object_start + 1));
                object_start = std::string_view::npos;
            }
        } else if (c == ']' && depth == 0) {
            break;
        }
    }
    return objects;
}

TelegramChat parseChat(std::string_view chatObject,
                       std::int64_t updateId,
                       const fs::path& downloadRoot)
{
    TelegramChat chat;
    std::ostringstream chat_id;
    chat_id << std::fixed << std::setprecision(0) << jsonNumberField(chatObject, "id");
    chat.chatId = chat_id.str();
    chat.title = jsonStringField(chatObject, "title");
    if (chat.title.empty()) {
        chat.title = jsonStringField(chatObject, "first_name");
    }
    if (chat.title.empty()) {
        chat.title = jsonStringField(chatObject, "username");
    }
    if (chat.title.empty()) {
        chat.title = "Telegram " + chat.chatId;
    }
    chat.type = jsonStringField(chatObject, "type");
    chat.folderPath = downloadRoot / sanitizedFolderName(chat.title);
    chat.lastSeenUpdateId = updateId;
    return chat;
}

std::optional<TelegramAudioItem> parseAudioItem(std::string_view message,
                                                const TelegramChat& chat)
{
    auto media = objectField(message, "audio");
    bool from_document = false;
    if (!media) {
        media = objectField(message, "document");
        from_document = true;
    }
    if (!media) {
        return std::nullopt;
    }

    TelegramAudioItem item;
    item.chatId = chat.chatId;
    item.messageId = (int)jsonNumberField(message, "message_id");
    item.telegramDate = (std::int64_t)jsonNumberField(message, "date");
    item.fileId = jsonStringField(*media, "file_id");
    item.fileUniqueId = jsonStringField(*media, "file_unique_id");
    item.fileName = jsonStringField(*media, "file_name");
    item.performer = jsonStringField(*media, "performer");
    item.title = jsonStringField(*media, "title");
    item.mimeType = jsonStringField(*media, "mime_type");
    item.duration = (int)jsonNumberField(*media, "duration");
    item.fileSize = (std::uintmax_t)jsonNumberField(*media, "file_size");
    item.fileName = fallbackFileName(item);
    item.localPath = chat.folderPath / sanitizedFolderName(item.fileName);

    if (item.fileId.empty()) {
        return std::nullopt;
    }
    if (from_document && item.mimeType.find("audio/") == std::string::npos &&
        extensionOf(item.fileName).empty()) {
        return std::nullopt;
    }
    return item;
}

}  // namespace

TelegramInboxService::TelegramInboxService(const Config& config,
                                           TelegramBotClient& client,
                                           TelegramRepository& repository)
    : config_(config)
    , client_(client)
    , repository_(repository)
{
}

bool TelegramInboxService::sync(TelegramSyncSummary& summary, std::string& error)
{
    if (!config_.telegram.enabled) {
        error = "Telegram is disabled in config";
        return false;
    }

    std::int64_t last_update = repository_.lastUpdateId(error);
    if (!error.empty()) {
        return false;
    }

    std::string json;
    if (!client_.getUpdates(last_update > 0 ? last_update + 1 : 0, json, error)) {
        return false;
    }

    if (json.find("\"ok\":true") == std::string::npos) {
        error = "Telegram getUpdates returned an error";
        return false;
    }

    fs::path download_root = expandUserPath(config_.telegram.downloadRoot);
    std::uintmax_t max_bytes =
        (std::uintmax_t)std::max(0, config_.telegram.maxFileSizeMb) *
        1024ULL * 1024ULL;

    for (auto update : arrayObjects(json, "result")) {
        std::int64_t update_id = (std::int64_t)jsonNumberField(update, "update_id");
        summary.lastUpdateId = std::max(summary.lastUpdateId, update_id);
        summary.updates++;

        auto message = objectField(update, "message");
        if (!message) {
            message = objectField(update, "channel_post");
        }
        if (!message) {
            summary.skipped++;
            continue;
        }

        auto chat_object = objectField(*message, "chat");
        if (!chat_object) {
            summary.skipped++;
            continue;
        }

        TelegramChat chat = parseChat(*chat_object, update_id, download_root);
        if (chat.chatId.empty() || !repository_.upsertChat(chat, error)) {
            return false;
        }
        summary.chats++;

        auto item = parseAudioItem(*message, chat);
        if (!item) {
            continue;
        }
        if (max_bytes > 0 && item->fileSize > max_bytes) {
            summary.skipped++;
            continue;
        }
        if (!extensionAllowed(item->fileName, config_.telegram.allowedExtensions)) {
            summary.skipped++;
            continue;
        }
        if (!repository_.upsertAudioItem(*item, error)) {
            return false;
        }
        summary.audioItems++;
    }

    if (summary.lastUpdateId > last_update &&
        !repository_.setLastUpdateId(summary.lastUpdateId, error)) {
        return false;
    }
    return true;
}

std::vector<TelegramChat> TelegramInboxService::listChats(std::string& error) const
{
    return repository_.listChats(error);
}

std::vector<TelegramAudioItem>
TelegramInboxService::listAudioItems(const std::string& chatId, std::string& error) const
{
    return repository_.listAudioItems(chatId, error);
}

std::optional<TelegramAudioItem>
TelegramInboxService::findAudioItem(const std::string& chatId,
                                    int messageId,
                                    std::string& error) const
{
    return repository_.findAudioItem(chatId, messageId, error);
}

bool TelegramInboxService::downloadItem(TelegramAudioItem& item, std::string& error)
{
    if (item.fileId.empty()) {
        error = "Telegram item has no file_id";
        return false;
    }

    if (!item.localPath.empty()) {
        std::error_code ec;
        if (fs::is_regular_file(item.localPath, ec)) {
            item.downloaded = true;
            return repository_.upsertAudioItem(item, error);
        }
    }

    std::string response;
    if (!client_.getFile(item.fileId, response, error)) {
        return false;
    }
    if (response.find("\"ok\":true") == std::string::npos) {
        error = "Telegram getFile returned an error";
        return false;
    }
    std::string file_path = jsonStringField(response, "file_path");
    if (file_path.empty()) {
        error = "Telegram getFile response has no file_path";
        return false;
    }
    if (item.localPath.empty()) {
        item.localPath = expandUserPath(config_.telegram.downloadRoot) /
            item.chatId /
            fallbackFileName(item);
    }
    if (!client_.downloadFile(file_path, item.localPath, error)) {
        return false;
    }
    item.downloaded = true;
    return repository_.upsertAudioItem(item, error);
}
