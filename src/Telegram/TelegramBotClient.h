#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

class TelegramBotClient {
public:
    explicit TelegramBotClient(std::string botToken);

    bool configured() const;
    bool getUpdates(std::int64_t offset, std::string& json, std::string& error) const;
    bool getFile(const std::string& fileId, std::string& json, std::string& error) const;
    bool downloadFile(const std::string& filePath,
                      const std::filesystem::path& output,
                      std::string& error) const;

private:
    std::string apiUrl(const std::string& method) const;
    std::string fileUrl(const std::string& filePath) const;

    std::string botToken_;
};
