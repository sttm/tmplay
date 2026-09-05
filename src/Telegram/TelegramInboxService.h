#pragma once

#include "TelegramBotClient.h"
#include "TelegramModels.h"
#include "TelegramRepository.h"
#include "../core/Config.hpp"

#include <filesystem>
#include <optional>
#include <string>

class TelegramInboxService {
public:
    TelegramInboxService(const Config& config,
                         TelegramBotClient& client,
                         TelegramRepository& repository);

    bool sync(TelegramSyncSummary& summary, std::string& error);
    std::vector<TelegramChat> listChats(std::string& error) const;
    std::vector<TelegramAudioItem> listAudioItems(const std::string& chatId,
                                                  std::string& error) const;
    std::optional<TelegramAudioItem> findAudioItem(const std::string& chatId,
                                                   int messageId,
                                                   std::string& error) const;
    bool downloadItem(TelegramAudioItem& item, std::string& error);

private:
    const Config& config_;
    TelegramBotClient& client_;
    TelegramRepository& repository_;
};
