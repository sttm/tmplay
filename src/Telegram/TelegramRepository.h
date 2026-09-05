#pragma once

#include "TelegramModels.h"

#include <optional>
#include <string>
#include <vector>

class LibraryDatabase;

class TelegramRepository {
public:
    explicit TelegramRepository(LibraryDatabase& database);

    bool upsertChat(const TelegramChat& chat, std::string& error);
    bool upsertAudioItem(const TelegramAudioItem& item, std::string& error);
    std::optional<TelegramAudioItem> findAudioItem(const std::string& chatId,
                                                   int messageId,
                                                   std::string& error) const;
    std::vector<TelegramChat> listChats(std::string& error) const;
    std::vector<TelegramAudioItem> listAudioItems(const std::string& chatId,
                                                  std::string& error) const;
    std::int64_t lastUpdateId(std::string& error) const;
    bool setLastUpdateId(std::int64_t updateId, std::string& error);

private:
    LibraryDatabase& database_;
};
