#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct TelegramChat {
    std::string chatId;
    std::string title;
    std::string type;
    std::filesystem::path folderPath;
    std::int64_t lastSeenUpdateId = 0;
    std::int64_t createdAt = 0;
    std::int64_t updatedAt = 0;
};

struct TelegramAudioItem {
    std::string chatId;
    int messageId = 0;
    std::string fileId;
    std::string fileUniqueId;
    std::string fileName;
    std::string performer;
    std::string title;
    std::string mimeType;
    int duration = 0;
    std::uintmax_t fileSize = 0;
    std::int64_t telegramDate = 0;
    bool downloaded = false;
    std::filesystem::path localPath;
    std::string importedTrackId;
    std::int64_t createdAt = 0;
    std::int64_t updatedAt = 0;
};

struct TelegramSyncSummary {
    int updates = 0;
    int chats = 0;
    int audioItems = 0;
    int skipped = 0;
    std::int64_t lastUpdateId = 0;
};

