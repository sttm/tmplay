#include "AudDRecognizer.hpp"

#include "ProcessRunner.hpp"

#include <filesystem>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string jsonStringField(std::string_view json, std::string_view key)
{
    const std::string needle = "\"" + std::string(key) + "\":\"";
    std::size_t pos = json.find(needle);
    if (pos == std::string_view::npos) {
        return {};
    }
    pos += needle.size();

    std::string value;
    bool escaped = false;
    for (; pos < json.size(); ++pos) {
        const char character = json[pos];
        if (escaped) {
            switch (character) {
            case 'n': value += '\n'; break;
            case 'r': value += '\r'; break;
            case 't': value += '\t'; break;
            default: value += character; break;
            }
            escaped = false;
        } else if (character == '\\') {
            escaped = true;
        } else if (character == '\"') {
            break;
        } else {
            value += character;
        }
    }
    return value;
}

bool hasSuccessStatus(std::string_view response)
{
    return response.find("\"status\":\"success\"") != std::string_view::npos;
}

}  // namespace

AudDRecognizer::AudDRecognizer(std::string apiKey)
    : apiKey_(ProcessRunner::trim(std::move(apiKey)))
{
}

bool AudDRecognizer::configured() const
{
    return !apiKey_.empty();
}

bool AudDRecognizer::recognize(const std::string& audioPath,
                                AudDMatch& match,
                                std::string& error) const
{
    match = {};
    if (!configured()) {
        error = "AudD API key is not configured";
        return false;
    }
    std::error_code filesystem_error;
    if (!fs::is_regular_file(audioPath, filesystem_error) ||
        fs::file_size(audioPath, filesystem_error) < 4096) {
        error = "Desktop recording is empty";
        return false;
    }

    const auto curl = ProcessRunner::findExecutable("curl");
    if (!curl) {
        error = "curl is required for AudD FIND";
        return false;
    }

    std::string response;
    const std::vector<std::string> command{
        *curl,
        "-fsS", "--connect-timeout", "10", "--max-time", "45",
        "-X", "POST", "https://api.audd.io/",
        "-F", "api_token=" + apiKey_,
        "-F", "return=timecode",
        "-F", "file=@" + audioPath,
    };
    if (ProcessRunner::runWithCombinedOutput(command, &response) != 0) {
        error = "AudD request failed";
        return false;
    }

    match.artist = ProcessRunner::trim(jsonStringField(response, "artist"));
    match.title = ProcessRunner::trim(jsonStringField(response, "title"));
    if (!match.artist.empty() && !match.title.empty() && hasSuccessStatus(response)) {
        return true;
    }

    const std::string audd_error = ProcessRunner::trim(jsonStringField(response, "error"));
    error = audd_error.empty()
        ? "AudD did not recognize this audio; record clear music for 12–20 seconds"
        : "AudD: " + audd_error;
    return false;
}
