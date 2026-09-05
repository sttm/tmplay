#include "TelegramBotClient.h"

#include "../core/ProcessRunner.hpp"

#include <filesystem>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

bool runCurl(const std::vector<std::string>& args,
             std::string& output,
             std::string& error,
             const std::string& safeDescription)
{
    auto curl = ProcessRunner::findExecutable("curl");
    if (!curl) {
        error = "curl not found; Telegram Bot API needs curl in PATH";
        return false;
    }

    std::vector<std::string> command{*curl};
    command.insert(command.end(), args.begin(), args.end());
    int code = ProcessRunner::run(command, &output);
    if (code != 0) {
        error = safeDescription + " failed";
        return false;
    }
    return true;
}

}  // namespace

TelegramBotClient::TelegramBotClient(std::string botToken)
    : botToken_(std::move(botToken))
{
}

bool TelegramBotClient::configured() const
{
    return !botToken_.empty() && botToken_.find("123456:") != 0;
}

std::string TelegramBotClient::apiUrl(const std::string& method) const
{
    return "https://api.telegram.org/bot" + botToken_ + "/" + method;
}

std::string TelegramBotClient::fileUrl(const std::string& filePath) const
{
    return "https://api.telegram.org/file/bot" + botToken_ + "/" + filePath;
}

bool TelegramBotClient::getUpdates(std::int64_t offset,
                                   std::string& json,
                                   std::string& error) const
{
    if (!configured()) {
        error = "Telegram bot token is not configured";
        return false;
    }
    std::string url = apiUrl("getUpdates") + "?timeout=0&allowed_updates="
        "%5B%22message%22%2C%22channel_post%22%5D";
    if (offset > 0) {
        url += "&offset=" + std::to_string(offset);
    }
    return runCurl({"-fsSL", "--max-time", "30", url},
                   json,
                   error,
                   "Telegram getUpdates");
}

bool TelegramBotClient::getFile(const std::string& fileId,
                                std::string& json,
                                std::string& error) const
{
    if (!configured()) {
        error = "Telegram bot token is not configured";
        return false;
    }
    std::string url = apiUrl("getFile") + "?file_id=" + fileId;
    return runCurl({"-fsSL", "--max-time", "30", url},
                   json,
                   error,
                   "Telegram getFile");
}

bool TelegramBotClient::downloadFile(const std::string& filePath,
                                     const fs::path& output,
                                     std::string& error) const
{
    if (!configured()) {
        error = "Telegram bot token is not configured";
        return false;
    }

    std::error_code ec;
    fs::create_directories(output.parent_path(), ec);
    if (ec) {
        error = ec.message();
        return false;
    }

    std::string ignored;
    return runCurl({"-fsSL", "--max-time", "120", "-o", output.string(),
                    fileUrl(filePath)},
                   ignored,
                   error,
                   "Telegram file download");
}
