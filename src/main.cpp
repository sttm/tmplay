#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <cstdio>
#include <regex>
#include <thread>
#include <unordered_map>

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>

#include "core/AppController.hpp"
#include "core/Config.hpp"
#include "core/MacNowPlaying.hpp"
#include "core/ProcessRunner.hpp"
#include "Library/LibraryDatabase.h"
#include "Serato/SeratoCueWriter.h"
#include "Telegram/TelegramBotClient.h"
#include "Telegram/TelegramInboxService.h"
#include "Telegram/TelegramRepository.h"
#include "Traktor/TraktorMetadataWriter.h"
#include "ui/BrowserState.hpp"
#include "ui/KeyBindings.hpp"
#include "ui/ManualCueEditor.hpp"
#include "ui/TrimEditor.hpp"

using namespace ftxui;
namespace fs = std::filesystem;

std::string lowerExtension(const fs::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return ext;
}

std::string formatTime(double seconds)
{
    if (seconds <= 0.0) {
        return "--:--";
    }

    int total = (int)seconds;
    return std::format("{:02}:{:02}", total / 60, total % 60);
}

std::string formatPlaybackTime(double seconds)
{
    int total = std::max(0, (int)seconds);
    return std::format("{:02}:{:02}", total / 60, total % 60);
}

std::string formatBitrate(double kbps)
{
    return kbps > 0.0 ? std::to_string((int)(kbps + 0.5)) + "k" : "-";
}

std::string formatSampleRate(double hz)
{
    if (hz <= 0.0) {
        return "-";
    }
    double khz = hz / 1000.0;
    if (std::abs(khz - std::round(khz)) < 0.05) {
        return std::to_string((int)std::round(khz)) + "k";
    }
    return std::format("{:.1f}k", khz);
}

std::string missingExternalToolsWarning()
{
    std::vector<std::string> missing;
    for (const char* tool : {"yt-dlp", "ffmpeg", "ffprobe"}) {
        if (!ProcessRunner::findExecutable(tool)) {
            missing.emplace_back(tool);
        }
    }
    if (missing.empty()) {
        return {};
    }
    std::string result = "Missing required tool";
    if (missing.size() > 1) result += "s";
    result += ": ";
    for (size_t index = 0; index < missing.size(); ++index) {
        if (index > 0) result += ", ";
        result += missing[index];
    }
    result += " | reinstall the tmplay bundle or run scripts/bootstrap_macos.sh";
    return result;
}

std::string formatSize(std::uintmax_t bytes)
{
    if (bytes == 0) {
        return "-";
    }
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        return std::format("{:.1f}G", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    }
    if (bytes >= 1024ULL * 1024ULL) {
        return std::format("{:.1f}M", (double)bytes / (1024.0 * 1024.0));
    }
    if (bytes >= 1024ULL) {
        return std::format("{:.0f}K", (double)bytes / 1024.0);
    }
    return std::to_string(bytes) + "B";
}

std::string playbackStateToString(PlaybackState state)
{
    switch (state)
    {
    case PlaybackState::Stopped:
        return "⏹";
    case PlaybackState::Playing:
        return "▶";
    case PlaybackState::Paused:
        return "⏸";
    case PlaybackState::Error:
        return "!";
    }
    return "unknown";
}

std::string playbackModeToString(PlaybackMode mode)
{
    switch (mode)
    {
    case PlaybackMode::Shuffle:
        return "⇄";
    case PlaybackMode::RepeatOne:
        return "↻1";
    case PlaybackMode::RepeatAll:
        return "↻A";
    }
    return "↻A";
}

std::string truncateEnd(const std::string& value, int width)
{
    if (width <= 0) {
        return "";
    }
    if (string_width(value) <= width) {
        return value;
    }

    const int content_width = width <= 3 ? width : width - 3;
    std::string result;
    for (std::size_t offset = 0; offset < value.size();) {
        const unsigned char first = (unsigned char)value[offset];
        std::size_t length = 1;
        if ((first & 0xE0) == 0xC0) length = 2;
        else if ((first & 0xF0) == 0xE0) length = 3;
        else if ((first & 0xF8) == 0xF0) length = 4;
        if (offset + length > value.size()) length = 1;
        const std::string glyph = value.substr(offset, length);
        if (string_width(result) + string_width(glyph) > content_width) {
            break;
        }
        result += glyph;
        offset += length;
    }
    return width <= 3 ? result : result + "...";
}

std::string stripTrackNumberPrefix(std::string value)
{
    std::size_t offset = 0;
    while (offset < value.size() && std::isdigit((unsigned char)value[offset])) {
        ++offset;
    }
    if (offset == 0 || offset >= value.size() ||
        (value[offset] != '.' && value[offset] != '-')) {
        return value;
    }
    ++offset;
    while (offset < value.size() && std::isspace((unsigned char)value[offset])) {
        ++offset;
    }
    return value.substr(offset);
}

std::vector<std::string> wrapText(const std::string& value, int width)
{
    std::vector<std::string> lines;
    if (width <= 0) {
        return lines;
    }

    auto flushParagraph = [&](std::string paragraph) {
        size_t first = paragraph.find_first_not_of(" \t\r");
        if (first == std::string::npos) {
            paragraph.clear();
        } else {
            size_t last = paragraph.find_last_not_of(" \t\r");
            paragraph = paragraph.substr(first, last - first + 1);
        }
        if (paragraph.empty()) {
            lines.emplace_back("");
            return;
        }

        std::string current;
        size_t start = 0;
        while (start < paragraph.size()) {
            while (start < paragraph.size() && paragraph[start] == ' ') {
                start++;
            }
            size_t end = paragraph.find(' ', start);
            std::string word = paragraph.substr(
                start,
                end == std::string::npos ? std::string::npos : end - start);
            start = end == std::string::npos ? paragraph.size() : end + 1;

            while ((int)word.size() > width) {
                if (!current.empty()) {
                    lines.push_back(current);
                    current.clear();
                }
                lines.push_back(word.substr(0, width));
                word.erase(0, width);
            }

            if (word.empty()) {
                continue;
            }
            if (current.empty()) {
                current = std::move(word);
            } else if ((int)(current.size() + 1 + word.size()) <= width) {
                current += " " + word;
            } else {
                lines.push_back(current);
                current = std::move(word);
            }
        }

        if (!current.empty()) {
            lines.push_back(current);
        }
    };

    size_t start = 0;
    while (start <= value.size()) {
        size_t end = value.find('\n', start);
        flushParagraph(value.substr(
            start,
            end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    if (lines.empty()) {
        lines.emplace_back("");
    }
    return lines;
}

std::optional<std::string> firstUrlInText(const std::string& value)
{
    size_t start = value.find("https://");
    if (start == std::string::npos) {
        start = value.find("http://");
    }
    if (start == std::string::npos) {
        return std::nullopt;
    }

    size_t end = start;
    while (end < value.size()) {
        char c = value[end];
        if (std::isspace((unsigned char)c) ||
            c == '"' || c == '\'' || c == '<' || c == '>') {
            break;
        }
        end++;
    }

    std::string url = value.substr(start, end - start);
    while (!url.empty() &&
           (url.back() == '.' || url.back() == ',' || url.back() == ';' ||
            url.back() == ':' || url.back() == ')' || url.back() == ']')) {
        url.pop_back();
    }
    if (url.empty()) {
        return std::nullopt;
    }
    return url;
}

bool looksLikeUrl(const std::string& value)
{
    return value.starts_with("http://") || value.starts_with("https://");
}

std::string trimInput(std::string value)
{
    size_t first = value.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return "";
    }
    size_t last = value.find_last_not_of(" \t");
    return value.substr(first, last - first + 1);
}

std::string downloadSourceFromInput(const std::string& input)
{
    if (looksLikeUrl(input)) {
        return input;
    }
    const std::string prefix = "download ";
    if (input.starts_with(prefix)) {
        return trimInput(input.substr(prefix.size()));
    }
    return "";
}

struct ParsedSearchCommand {
    std::string query;
    OnlineSearchOptions options;
};

std::optional<ParsedSearchCommand> parseSearchCommand(const std::string& input,
                                                       std::string& error)
{
    ParsedSearchCommand parsed;
    std::istringstream stream(input);
    std::vector<std::string> tokens;
    for (std::string token; stream >> token;) tokens.push_back(std::move(token));
    for (size_t index = 0; index < tokens.size(); ++index) {
        const std::string& token = tokens[index];
        auto readMinutes = [&](int fallback, int& seconds) -> bool {
            if (index + 1 >= tokens.size()) {
                seconds = fallback * 60;
                return true;
            }
            try {
                size_t consumed = 0;
                const int minutes = std::stoi(tokens[index + 1], &consumed);
                if (consumed != tokens[index + 1].size() || minutes <= 0) return false;
                seconds = minutes * 60;
                ++index;
                return true;
            } catch (...) {
                seconds = fallback * 60;
                return true;
            }
        };
        if (token == "-o") {
            if (index + 1 >= tokens.size()) {
                error = "-o requires a value from 1 to 500";
                return std::nullopt;
            }
            try {
                size_t consumed = 0;
                const int count = std::stoi(tokens[++index], &consumed);
                if (consumed != tokens[index].size() || count < 1 || count > 500) {
                    error = "-o must be between 1 and 500";
                    return std::nullopt;
                }
                parsed.options.maxTracks = count;
            } catch (...) {
                error = "-o must be between 1 and 500";
                return std::nullopt;
            }
        } else if (token == "-m") {
            if (!readMinutes(10, parsed.options.minDurationSeconds)) {
                error = "-m duration must be a positive number of minutes";
                return std::nullopt;
            }
        } else if (token == "-t") {
            if (!readMinutes(15, parsed.options.maxDurationSeconds)) {
                error = "-t duration must be a positive number of minutes";
                return std::nullopt;
            }
        } else {
            if (!parsed.query.empty()) parsed.query += " ";
            parsed.query += token;
        }
    }
    if (parsed.query.empty()) {
        error = "Enter a music search query";
        return std::nullopt;
    }
    if (parsed.options.minDurationSeconds > 0 &&
        parsed.options.maxDurationSeconds > 0 &&
        parsed.options.minDurationSeconds > parsed.options.maxDurationSeconds) {
        error = "-m cannot be longer than -t";
        return std::nullopt;
    }
    return parsed;
}

bool isPlaylistSource(const std::string& source)
{
    std::string normalized = source;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return normalized.find("list=") != std::string::npos ||
           normalized.find("/sets/") != std::string::npos;
}

std::optional<std::string> youtubeSingleVideoUrl(const std::string& source)
{
    std::smatch match;
    static const std::regex watch_regex(
        R"(^(https?://(?:www\.)?youtube\.com/watch\?)([^#]*?)(?:&|^)list=[^&#]+.*$)",
        std::regex::icase);
    if (std::regex_match(source, match, watch_regex)) {
        std::string query = match[2].str();
        std::smatch video;
        static const std::regex v_regex(R"((?:^|&)v=([^&#]+))",
                                        std::regex::icase);
        if (std::regex_search(query, video, v_regex)) {
            return std::string("https://www.youtube.com/watch?v=") + video[1].str();
        }
    }

    static const std::regex short_regex(
        R"(^https?://(?:www\.)?youtu\.be/([^?&#/]+)(?:[?&].*)?$)",
        std::regex::icase);
    if (std::regex_match(source, match, short_regex)) {
        return std::string("https://youtu.be/") + match[1].str();
    }
    return std::nullopt;
}

bool copyToClipboard(const std::string& value)
{
#ifdef __APPLE__
    FILE* pipe = popen("pbcopy", "w");
    if (pipe == nullptr) {
        return false;
    }
    fwrite(value.data(), 1, value.size(), pipe);
    return pclose(pipe) == 0;
#else
    (void)value;
    return false;
#endif
}

std::optional<std::string> readClipboard()
{
#ifdef __APPLE__
    FILE* pipe = popen("pbpaste", "r");
    if (pipe == nullptr) {
        return std::nullopt;
    }
    std::string value;
    std::array<char, 256> buffer{};
    while (fgets(buffer.data(), (int)buffer.size(), pipe) != nullptr) {
        value += buffer.data();
    }
    if (pclose(pipe) != 0) {
        return std::nullopt;
    }
    return value;
#else
    return std::nullopt;
#endif
}

fs::path activeConfigPath()
{
    fs::path config_path = fs::path(ProcessRunner::executableDirectory()) / "config.toml";
    std::error_code ec;
    if (!fs::is_regular_file(config_path, ec)) {
        config_path = fs::current_path() / "config.toml";
    }
    return config_path;
}

int handleTelegramCli(int argc, char** argv)
{
    auto print_help = [] {
        std::cout
            << "Usage:\n"
            << "  tmplay telegram sync\n"
            << "  tmplay telegram refresh\n"
            << "  tmplay telegram list-chats\n"
            << "  tmplay telegram list-items <chat_id>\n";
    };

    if (argc < 3) {
        print_help();
        return 1;
    }

    std::string subcommand = argv[2] != nullptr ? argv[2] : "";
    Config config;
    try {
        config = Config::load(activeConfigPath().string());
    } catch (const std::exception& ex) {
        std::cerr << "Config error: " << ex.what() << "\n";
        return 1;
    }

    LibraryDatabase database;
    std::string error;
    if (!database.open(config.library.databasePath, error) ||
        !database.initialize(error)) {
        std::cerr << "Library database error: " << error << "\n";
        return 1;
    }

    TelegramBotClient client(config.telegram.botToken);
    TelegramRepository repository(database);
    TelegramInboxService service(config, client, repository);

    if (subcommand == "sync" || subcommand == "refresh") {
        TelegramSyncSummary summary;
        if (!service.sync(summary, error)) {
            std::cerr << "Telegram sync error: " << error << "\n";
            return 1;
        }
        std::cout << "Telegram sync: updates " << summary.updates
                  << " | chats " << summary.chats
                  << " | audio " << summary.audioItems
                  << " | skipped " << summary.skipped
                  << " | last_update_id " << summary.lastUpdateId << "\n";
        return 0;
    }

    if (subcommand == "list-chats") {
        auto chats = service.listChats(error);
        if (!error.empty()) {
            std::cerr << "Telegram list error: " << error << "\n";
            return 1;
        }
        for (const auto& chat : chats) {
            std::cout << chat.title << " | " << chat.type
                      << " | " << chat.chatId
                      << " | " << chat.folderPath.string() << "\n";
        }
        if (chats.empty()) {
            std::cout << "No Telegram chats found. Run: tmplay telegram sync\n";
        }
        return 0;
    }

    if (subcommand == "list-items") {
        if (argc < 4) {
            std::cerr << "Usage: tmplay telegram list-items <chat_id>\n";
            return 1;
        }
        std::string chat_id = argv[3] != nullptr ? argv[3] : "";
        auto items = service.listAudioItems(chat_id, error);
        if (!error.empty()) {
            std::cerr << "Telegram list error: " << error << "\n";
            return 1;
        }
        for (const auto& item : items) {
            std::cout << (item.downloaded ? "downloaded" : "not downloaded")
                      << " | " << item.fileName
                      << " | " << item.title
                      << " | " << item.fileSize
                      << " | " << item.localPath.string() << "\n";
        }
        if (items.empty()) {
            std::cout << "No Telegram audio items found for chat " << chat_id << "\n";
        }
        return 0;
    }

    print_help();
    return 1;
}

std::string displayName(const std::string& path)
{
    if (path == "search://root") {
        return "Search";
    }
    if (path == "search://recent") {
        return "Recent search";
    }
    if (path == "search://recent/tracks") {
        return "Tracks";
    }
    if (path == "search://recent/albums") {
        return "Albums";
    }
    if (path.rfind("search://", 0) == 0) {
        std::string value = path.substr(std::string("search://").size());
        const std::size_t album = value.rfind("/albums/");
        if (album != std::string::npos) value = value.substr(album + 8);
        // Catalogue paths retain their full namespace internally (for
        // example online-albums/artist/Linkin Park). In the browser that
        // namespace is already represented by the preceding tree levels, so
        // show only the useful artist name.
        const std::size_t artist = value.rfind("/artist/");
        if (artist != std::string::npos) value = value.substr(artist + 8);
        return value.empty() ? "Search" : value;
    }
    if (path == "telegram://root") {
        return "Telegram";
    }
    if (path.rfind("telegram://chat/", 0) == 0) {
        return path.substr(std::string("telegram://chat/").size());
    }
    fs::path p(path);
    std::string name = p.filename().string();
    return name.empty() ? path : name;
}

enum class TrackSortColumn {
    Title,
    Time,
    Bpm,
    Key,
    Genre,
    Bitrate,
    SampleRate,
    Format,
    Size,
};

int trackColumnWidth(const std::string& column)
{
    if (column == "time") return 5;
    if (column == "bpm") return 4;
    if (column == "key") return 4;
    if (column == "kbps") return 5;
    if (column == "rate") return 5;
    if (column == "frmt") return 4;
    if (column == "size") return 6;
    if (column == "genre") return 7;
    return 0;
}

std::string trackColumnLabel(const std::string& column)
{
    if (column == "time") return "Time";
    if (column == "bpm") return "BPM";
    if (column == "key") return "Key";
    if (column == "kbps") return "Kbps";
    if (column == "rate") return "Rate";
    if (column == "frmt") return "Frmt";
    if (column == "size") return "Size";
    if (column == "genre") return "Genre";
    return column;
}

TrackSortColumn trackColumnSort(const std::string& column)
{
    if (column == "time") return TrackSortColumn::Time;
    if (column == "bpm") return TrackSortColumn::Bpm;
    if (column == "key") return TrackSortColumn::Key;
    if (column == "genre") return TrackSortColumn::Genre;
    if (column == "kbps") return TrackSortColumn::Bitrate;
    if (column == "rate") return TrackSortColumn::SampleRate;
    if (column == "frmt") return TrackSortColumn::Format;
    if (column == "size") return TrackSortColumn::Size;
    return TrackSortColumn::Title;
}

bool trackColumnEnabled(const ColumnVisibility& columns, const std::string& column)
{
    if (column == "time") return columns.time;
    if (column == "bpm") return columns.bpm;
    if (column == "key") return columns.key;
    if (column == "kbps") return columns.kbps;
    if (column == "rate") return columns.rate;
    if (column == "frmt") return columns.frmt;
    if (column == "size") return columns.size;
    if (column == "genre") return columns.genre;
    return false;
}

Element trackRow(const std::string& title,
                 const std::string& time,
                 const std::string& bpm,
                 const std::string& key,
                 const std::string& bitrate,
                 const std::string& sample_rate,
                 const std::string& format,
                 const std::string& file_size,
                 const std::string& genre,
                 const std::vector<std::string>& column_order,
                 Element marker)
{
    Elements columns = {
        std::move(marker) | size(WIDTH, EQUAL, 5),
        text(title) | flex,
    };
    for (const auto& column : column_order) {
        std::string value = "-";
        if (column == "time") value = time;
        else if (column == "bpm") value = bpm;
        else if (column == "key") value = key;
        else if (column == "kbps") value = bitrate;
        else if (column == "rate") value = sample_rate;
        else if (column == "frmt") value = format;
        else if (column == "size") value = file_size;
        else if (column == "genre") value = genre;
        int width = trackColumnWidth(column);
        if (width <= 0) {
            continue;
        }
        columns.push_back(separator());
        columns.push_back(text(value) | size(WIDTH, EQUAL, width));
    }
    return hbox(columns);
}

std::string lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return value;
}

void sortTracks(std::vector<Track>& tracks,
                TrackSortColumn column,
                bool ascending)
{
    auto compareText = [](const std::string& first, const std::string& second) {
        std::string left = lowercase(first);
        std::string right = lowercase(second);
        if (left == right) {
            return 0;
        }
        return left < right ? -1 : 1;
    };
    auto compareNumber = [](double first, double second) {
        if (first == second) {
            return 0;
        }
        return first < second ? -1 : 1;
    };

    std::stable_sort(tracks.begin(), tracks.end(),
                     [&](const Track& first, const Track& second) {
        bool first_has_value = true;
        bool second_has_value = true;
        int result = 0;
        switch (column) {
        case TrackSortColumn::Title:
            result = compareText(first.title, second.title);
            break;
        case TrackSortColumn::Time:
            first_has_value = first.duration > 0.0;
            second_has_value = second.duration > 0.0;
            result = compareNumber(first.duration, second.duration);
            break;
        case TrackSortColumn::Bpm:
            first_has_value = first.bpm > 0.0;
            second_has_value = second.bpm > 0.0;
            result = compareNumber(first.bpm, second.bpm);
            break;
        case TrackSortColumn::Key:
            first_has_value = !first.key.empty();
            second_has_value = !second.key.empty();
            result = compareText(first.key, second.key);
            break;
        case TrackSortColumn::Genre:
            first_has_value = !first.genre.empty();
            second_has_value = !second.genre.empty();
            result = compareText(first.genre, second.genre);
            break;
        case TrackSortColumn::Bitrate:
            first_has_value = first.bitrateKbps > 0.0;
            second_has_value = second.bitrateKbps > 0.0;
            result = compareNumber(first.bitrateKbps, second.bitrateKbps);
            break;
        case TrackSortColumn::SampleRate:
            first_has_value = first.sampleRateHz > 0.0;
            second_has_value = second.sampleRateHz > 0.0;
            result = compareNumber(first.sampleRateHz, second.sampleRateHz);
            break;
        case TrackSortColumn::Format: {
            std::string first_format = lowerExtension(first.id);
            std::string second_format = lowerExtension(second.id);
            first_has_value = !first_format.empty();
            second_has_value = !second_format.empty();
            result = compareText(first_format, second_format);
            break;
        }
        case TrackSortColumn::Size:
            first_has_value = first.sizeBytes > 0;
            second_has_value = second.sizeBytes > 0;
            result = compareNumber((double)first.sizeBytes, (double)second.sizeBytes);
            break;
        }

        if (first_has_value != second_has_value) {
            return first_has_value;
        }
        if (result == 0) {
            result = compareText(first.title, second.title);
        }
        return ascending ? result < 0 : result > 0;
    });
}

enum class HoverControl {
    None,
    VolumeDown,
    VolumeUp,
    Previous,
    PlayPause,
    Next,
    Mode,
    SpeedReset,
};

enum class PendingConfirmation {
    None,
    PlaylistDownload,
    StemSeparation,
    Normalization,
    Convert,
    Analysis,
    AutoCue,
    CueSync,
    Quit,
    DeleteEntry,
};

bool keyMatches(const Event& event, std::initializer_list<std::string> keys)
{
    return ui::keyMatches(event, keys);
}

int main(int argc, char** argv)
{
    if (argc > 1 && std::string(argv[1] != nullptr ? argv[1] : "") == "telegram") {
        return handleTelegramCli(argc, argv);
    }

    if (argc > 1) {
        AppController controller;
        std::string error;
        std::string command = argv[1] != nullptr ? argv[1] : "";
        if (command == "--search" || command == "--albums") {
            if (argc < 3) {
                std::cerr << "Usage: tmplay " << command << " <music query>\n";
                return 1;
            }
            std::string query;
            for (int index = 2; index < argc; ++index) {
                if (argv[index] == nullptr) continue;
                if (!query.empty()) query += " ";
                query += argv[index];
            }
            const bool album_search = command == "--albums";
            if (!controller.searchMusic(query, album_search, error)) {
                std::cerr << "Search error: " << error << "\n";
                return 1;
            }
            const auto tracks = controller.trackStore().getTracks();
            std::cout << error << "\n";
            for (const auto& track : tracks) {
                if (track.type == EntryType::File) {
                    std::cout << (track.isExplicit ? "[E] " : "")
                              << track.title << "\t" << track.id << "\n";
                } else if (album_search && track.type == EntryType::Album) {
                    std::cout << "ALBUM\t" << track.artist << "\t"
                              << track.title << "\t" << track.releaseDate << "\n";
                }
            }
            return 0;
        }
        if (command == "--export-library") {
            if (controller.exportLibrary(error, true)) {
                std::cout << error << "\n";
                return 0;
            }
            std::cerr << "Export error: " << error << "\n";
            return 1;
        }
        if (command == "--import-library-json") {
            if (controller.importChangedJson(error)) {
                std::cout << error << "\n";
                return 0;
            }
            std::cerr << "Import error: " << error << "\n";
            return 1;
        }
        if (command == "--import-serato-cues") {
            if (controller.importSeratoCues(error)) {
                std::cout << error << "\n";
                return 0;
            }
            std::cerr << "Serato import error: " << error << "\n";
            return 1;
        }
        if (command == "--validate-library-export") {
            if (controller.validateLibraryExport(error)) {
                std::cout << error << "\n";
                return 0;
            }
            std::cerr << "Validation error: " << error << "\n";
            return 1;
        }
        if (command == "--validate-serato-cues") {
            if (controller.validateSeratoCues(error)) {
                std::cout << error << "\n";
                return 0;
            }
            std::cerr << "Serato validation error: " << error << "\n";
            return 1;
        }
        if (command == "--validate-traktor-tags") {
            if (controller.validateTraktorEmbeddedCues(error)) {
                std::cout << error << "\n";
                return 0;
            }
            std::cerr << "Traktor embedded validation error: " << error << "\n";
            return 1;
        }
        if (command == "--dump-traktor-cues") {
            if (argc < 3 || argv[2] == nullptr) {
                std::cerr << "Usage: tmplay --dump-traktor-cues <audio-file>\n";
                return 1;
            }
            TraktorMetadataWriter traktor;
            fs::path file = argv[2];
            auto status = traktor.inspect(file, error);
            std::cout << "file=" << file << "\n"
                      << "supported=" << (status.supportedContainer ? "yes" : "no") << "\n"
                      << "has_traktor4=" << (status.hasTraktor4Tag ? "yes" : "no") << "\n"
                      << "tag_size=" << status.tagSize << "\n"
                      << "detail=" << status.detail << "\n";
            if (!error.empty()) {
                std::cerr << "inspect_error=" << error << "\n";
                return 1;
            }
            std::string read_error;
            auto cues = traktor.readCues(file, read_error);
            if (!read_error.empty()) {
                std::cerr << "read_error=" << read_error << "\n";
                return 1;
            }
            std::cout << "cue_count=" << cues.size() << "\n";
            for (const auto& cue : cues) {
                std::cout << cue.index << "|"
                          << cue.name << "|"
                          << std::format("{:.3f}", cue.seconds) << "\n";
            }
            return 0;
        }
        if (command == "--dump-serato-cues") {
            if (argc < 3 || argv[2] == nullptr) {
                std::cerr << "Usage: tmplay --dump-serato-cues <audio-file>\n";
                return 1;
            }
            SeratoCueWriter serato;
            fs::path file = argv[2];
            std::string read_error;
            auto cues = serato.readCues(file, read_error);
            std::cout << "file=" << file << "\n";
            if (!read_error.empty()) {
                std::cerr << "read_error=" << read_error << "\n";
                return 1;
            }
            std::cout << "cue_count=" << cues.size() << "\n";
            for (const auto& cue : cues) {
                std::cout << cue.index << "|"
                          << cue.name << "|"
                          << std::format("{:.3f}", cue.seconds) << "|"
                          << std::format("{:06x}", cue.colorRgb & 0xffffffu)
                          << "\n";
            }
            return 0;
        }
        if (command == "--make-serato-cue-test") {
            if (argc < 4 || argv[2] == nullptr || argv[3] == nullptr) {
                std::cerr << "Usage: tmplay --make-serato-cue-test <clean-input> <output>\n";
                return 1;
            }
            fs::path clean_input = argv[2];
            fs::path output = argv[3];
            if (!fs::is_regular_file(clean_input)) {
                std::cerr << "Clean input file does not exist\n";
                return 1;
            }
            std::error_code ec;
            fs::create_directories(output.parent_path(), ec);
            if (ec) {
                std::cerr << "Could not create output directory: " << ec.message() << "\n";
                return 1;
            }
            fs::copy_file(clean_input, output, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                std::cerr << "Could not copy input to output: " << ec.message() << "\n";
                return 1;
            }
            SeratoCueWriter serato;
            std::vector<SeratoCue> cues = {
                {0, "START", 5.0, 0x00ff00u},
                {1, "DROP 1", 15.0, 0x0080ffu},
                {2, "DROP 2", 25.0, 0xff8000u},
            };
            std::string write_error;
            if (!serato.writeCues(output, cues, write_error, false, true)) {
                std::cerr << "Serato write error: " << write_error << "\n";
                return 1;
            }
            std::cout << "Generated Serato cue test file: " << output << "\n"
                      << "Cues: 5.000s, 15.000s, 25.000s\n";
            return 0;
        }
        if (command == "--clear-serato-cues") {
            if (argc < 3 || argv[2] == nullptr) {
                std::cerr << "Usage: tmplay --clear-serato-cues <audio-file>\n";
                return 1;
            }
            SeratoCueWriter serato;
            std::string write_error;
            if (!serato.writeCues(argv[2], {}, write_error, false, true)) {
                std::cerr << "Serato clear error: " << write_error << "\n";
                return 1;
            }
            std::cout << "Cleared Serato cues: " << argv[2] << "\n";
            return 0;
        }
        if (command == "--clear-traktor-cues") {
            if (argc < 3 || argv[2] == nullptr) {
                std::cerr << "Usage: tmplay --clear-traktor-cues <audio-file>\n";
                return 1;
            }
            LibraryTrack track;
            track.id = argv[2];
            track.path = argv[2];
            track.title = fs::path(argv[2]).stem().string();
            TraktorMetadataWriter traktor;
            std::string write_error;
            if (!traktor.writeCues(track, write_error)) {
                std::cerr << "Traktor clear error: " << write_error << "\n";
                return 1;
            }
            std::cout << "Cleared Traktor cues: " << argv[2] << "\n";
            return 0;
        }
        if (command == "--sync-dual-cues") {
            if (argc < 3 || argv[2] == nullptr) {
                std::cerr << "Usage: tmplay --sync-dual-cues <audio-file>\n";
                return 1;
            }
            fs::path file = argv[2];
            SeratoCueWriter serato;
            TraktorMetadataWriter traktor;

            std::string serato_error;
            std::vector<SeratoCue> cues = serato.readCues(file, serato_error);
            std::string source = cues.empty() ? std::string() : std::string("Serato");

            if (cues.empty()) {
                std::string traktor_error;
                cues = traktor.readCues(file, traktor_error);
                if (!cues.empty()) {
                    source = "Traktor";
                    static constexpr std::uint32_t fallback_colors[] = {
                        0x00ff00u, 0xff0000u, 0x0080ffu, 0xff8000u,
                        0xffff00u, 0x8000ffu, 0x00ffffu, 0xffffffu,
                    };
                    for (auto& cue : cues) {
                        if ((cue.colorRgb & 0xffffffu) == 0xffffffu ||
                            (cue.colorRgb & 0xffffffu) == 0x000000u) {
                            int index = std::max(0, cue.index);
                            cue.colorRgb =
                                fallback_colors[(std::size_t)index %
                                                (sizeof(fallback_colors) /
                                                 sizeof(fallback_colors[0]))];
                        }
                    }
                }
            }

            if (cues.empty()) {
                std::cerr << "No embedded cues found to sync\n";
                return 1;
            }

            LibraryTrack track;
            track.id = file.string();
            track.path = file;
            track.title = file.stem().string();
            for (const auto& cue : cues) {
                track.cues.push_back({
                    cue.index,
                    cue.name,
                    "hotcue",
                    cue.seconds,
                    std::format("#{:06x}", cue.colorRgb & 0xffffffu),
                });
            }
            std::string write_error;
            auto writeSerato = [&]() {
                if (!serato.writeCues(file, cues, write_error, false, true)) {
                    std::cerr << "Serato sync error: " << write_error << "\n";
                    return false;
                }
                return true;
            };
            auto writeTraktor = [&]() {
                if (!traktor.writeCues(track, write_error)) {
                    std::cerr << "Traktor sync error: " << write_error << "\n";
                    return false;
                }
                return true;
            };
            if (!writeSerato() || !writeTraktor()) {
                return 1;
            }

            std::cout << "Synced " << cues.size()
                      << " cues from " << source
                      << " into Serato + Traktor metadata: " << file << "\n";
            return 0;
        }
        if (command == "--sync-cues-direction") {
            if (argc < 4 || argv[2] == nullptr || argv[3] == nullptr) {
                std::cerr << "Usage: tmplay --sync-cues-direction <serato-to-traktor|traktor-to-serato> <audio-file>\n";
                return 1;
            }
            std::string direction_arg = argv[2];
            CueSyncDirection direction = CueSyncDirection::Auto;
            if (direction_arg == "serato-to-traktor") {
                direction = CueSyncDirection::SeratoToTraktor;
            } else if (direction_arg == "traktor-to-serato") {
                direction = CueSyncDirection::TraktorToSerato;
            } else {
                std::cerr << "Unknown cue sync direction: " << direction_arg << "\n";
                return 1;
            }
            Track track;
            track.id = argv[3];
            track.title = fs::path(argv[3]).stem().string();
            track.type = EntryType::File;
            std::string result;
            if (!controller.syncCueMetadata(track, direction, result)) {
                std::cerr << "Cue sync error: " << result << "\n";
                return 1;
            }
            std::cout << result << "\n";
            return 0;
        }
        if (command == "--make-traktor-one-cue-test") {
            if (argc < 4 || argv[2] == nullptr || argv[3] == nullptr) {
                std::cerr << "Usage: tmplay --make-traktor-one-cue-test <clean-input> <output>\n";
                return 1;
            }
            fs::path clean_input = argv[2];
            fs::path output = argv[3];
            if (!fs::is_regular_file(clean_input)) {
                std::cerr << "Clean input file does not exist\n";
                return 1;
            }
            LibraryTrack track;
            track.id = output.string();
            track.path = output;
            track.title = output.stem().string();
            track.cues.push_back({0, "CUE 1", "hotcue", 5.0, "#00ff00"});
            std::error_code ec;
            fs::create_directories(output.parent_path(), ec);
            if (ec) {
                std::cerr << "Could not create output directory: " << ec.message() << "\n";
                return 1;
            }
            fs::copy_file(clean_input, output, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                std::cerr << "Could not copy input to output: " << ec.message() << "\n";
                return 1;
            }
            TraktorMetadataWriter traktor;
            std::string write_error;
            if (!traktor.writeCues(track, write_error)) {
                std::cerr << "Traktor write error: " << write_error << "\n";
                return 1;
            }
            std::cout << "Generated Traktor one-cue test file: " << output << "\n"
                      << "Cue: 5.000s\n";
            return 0;
        }
        if (command == "--make-both-one-cue-test") {
            if (argc < 4 || argv[2] == nullptr || argv[3] == nullptr) {
                std::cerr << "Usage: tmplay --make-both-one-cue-test <clean-input> <output>\n";
                return 1;
            }
            fs::path clean_input = argv[2];
            fs::path output = argv[3];
            if (!fs::is_regular_file(clean_input)) {
                std::cerr << "Clean input file does not exist\n";
                return 1;
            }
            std::error_code ec;
            fs::create_directories(output.parent_path(), ec);
            if (ec) {
                std::cerr << "Could not create output directory: " << ec.message() << "\n";
                return 1;
            }
            fs::copy_file(clean_input, output, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                std::cerr << "Could not copy input to output: " << ec.message() << "\n";
                return 1;
            }

            std::vector<SeratoCue> cues = {
                {0, "CUE 1", 5.0, 0x00ff00u},
            };
            SeratoCueWriter serato;
            std::string serato_error;
            if (!serato.writeCues(output, cues, serato_error, false, true)) {
                std::cerr << "Serato write error: " << serato_error << "\n";
                return 1;
            }

            LibraryTrack track;
            track.id = output.string();
            track.path = output;
            track.title = output.stem().string();
            track.cues.push_back({0, "CUE 1", "hotcue", 5.0, "#00ff00"});
            TraktorMetadataWriter traktor;
            std::string traktor_error;
            if (!traktor.writeCues(track, traktor_error)) {
                std::cerr << "Traktor write error: " << traktor_error << "\n";
                return 1;
            }
            std::cout << "Generated Serato+Traktor one-cue test file: " << output << "\n"
                      << "Cue: 5.000s\n";
            return 0;
        }
        if (command == "--make-traktor-cue-test") {
            if (argc < 4 || argv[2] == nullptr || argv[3] == nullptr) {
                std::cerr << "Usage: tmplay --make-traktor-cue-test <clean-input.mp3> <output.mp3> [template.mp3]\n";
                return 1;
            }
            fs::path clean_input = argv[2];
            fs::path output = argv[3];
            fs::path template_file = argc >= 5 && argv[4] != nullptr
                ? fs::path(argv[4])
                : fs::path();
            TraktorMetadataWriter traktor;
            std::vector<SeratoCue> cues = {
                {0, "n.n.", 5.0, 0xffffffu},
                {1, "n.n.", 15.0, 0xffffffu},
                {2, "n.n.", 25.0, 0xffffffu},
            };
            if (!traktor.makeCueTemplateTestFile(clean_input, template_file,
                                                 output, cues, error)) {
                std::cerr << "Traktor cue test generation error: " << error << "\n";
                return 1;
            }
            std::cout << "Generated Traktor cue test file: " << output << "\n"
                      << "Template: "
                      << (template_file.empty() ? std::string("embedded") : template_file.string())
                      << "\n"
                      << "Cues: 5.000s, 15.000s, 25.000s\n";
            return 0;
        }
        if (command == "--library-status") {
            if (controller.libraryStatus(error)) {
                std::cout << error << "\n";
                return 0;
            }
            std::cerr << "Library status error: " << error << "\n";
            return 1;
        }
        if (command == "--help") {
            std::cout << "Usage: tmplay [--library-status|--export-library|--import-library-json|--import-serato-cues|--validate-library-export|--validate-serato-cues|--validate-traktor-tags|--dump-traktor-cues <audio-file>|--make-traktor-cue-test <clean-input.mp3> <output.mp3> [template.mp3]]\n"
                      << "       tmplay telegram [sync|refresh|list-chats|list-items <chat_id>]\n";
            return 0;
        }
        std::cerr << "Unknown option: " << command << "\n";
        return 1;
    }

    // Release bundles keep these in Contents/MacOS/tools.  Developer builds
    // may use PATH instead, but never open a half-functional UI.
    if (const std::string missing_tools = missingExternalToolsWarning();
        !missing_tools.empty()) {
        std::cerr << "tmplay cannot start: " << missing_tools << "\n"
                  << "Reinstall the tmplay bundle, or for a developer build:\n"
                  << "  brew install ffmpeg yt-dlp\n";
        return 1;
    }

    std::cout << "\033[8;30;110t" << std::flush;

    AppController controller;
    BrowserState state;
    auto screen = ScreenInteractive::FullscreenAlternateScreen();
    std::atomic_bool refresh_running = true;

    std::vector<std::string> dir_entries;
    std::vector<std::string> dir_paths;
    std::vector<std::string> track_entries;
    std::vector<Track> visible_files;
    std::vector<std::string> visible_dir_entries;
    std::vector<std::string> visible_track_entries;
    int directory_preferred_width = 32;

    int browser_selector = 0;
    int root_selector = 0;
    int bottom_selector = 0;
    int speed_focus = 1;
    int progress_value = 0;
    int progress_min = 0;
    int progress_max = 1000;
    int progress_step = 5;
    bool progress_dragging = false;
    int speed_value = (int)std::round(controller.playbackRate() * 100.0);
    int speed_min = 50;
    int speed_max = 200;
    int speed_step = 1;
    bool preserve_pitch = controller.preservePitch();
    bool show_info = false;
    bool show_activity = true;
    bool show_download_panel = true;
    bool show_download_pool = false;
    bool show_stems_panel = true;
    bool show_audio_process_panel = true;
    bool show_auto_cue_panel = true;
    bool show_recording_panel = true;
    bool show_metadata_popup = false;
    bool show_eq_popup = false;
    bool show_context_menu = false;
    bool context_menu_for_track = false;
    int context_menu_selected = 0;
    std::vector<Box> context_menu_boxes;
    bool show_time_column = controller.config().columns.time;
    bool show_bpm_column = controller.config().columns.bpm;
    bool show_key_column = controller.config().columns.key;
    bool show_kbps_column = controller.config().columns.kbps;
    bool show_rate_column = controller.config().columns.rate;
    bool show_size_column = controller.config().columns.size;
    bool show_frmt_column = controller.config().columns.frmt;
    bool show_genre_column = controller.config().columns.genre;
    std::vector<std::string> visible_column_order;
    int visible_row_count = 1;
    int dir_view_selected = 0;
    int track_view_selected = 0;
    int last_click_pane = -1;
    int last_click_item = -1;
    auto last_click_time = std::chrono::steady_clock::time_point::min();
    HoverControl hovered_control = HoverControl::None;
    TrackSortColumn track_sort_column = TrackSortColumn::Title;
    bool track_sort_ascending = true;
    bool track_sort_requested = false;
    bool move_to_mode = false;
    Track move_track;
    std::string expanded_directory = controller.currentPath();
    std::string preferred_directory;
    std::string command_input;
    int command_cursor_position = 0;
    std::string command_status;
    std::string now_playing_copy_value;
    std::string pending_playlist_source;
    std::string pending_playlist_destination;
    int playlist_action_col = 0;
    PendingConfirmation pending_confirmation = PendingConfirmation::None;
    int confirmation_selected = 1;
    int stem_option_row = 0;
    int stem_option_col = 0;
    int normalize_option_row = 0;
    int normalize_option_col = 1;
    int convert_option_row = 0;
    int convert_option_col = 1;
    int analysis_option_row = 0;
    int auto_cue_option_col = 0;
    int cue_sync_option_row = 0;
    int cue_sync_direction_col = 0;
    int cue_sync_scope_col = 0;
    CueSyncDirection pending_cue_sync_direction = CueSyncDirection::SeratoToTraktor;
    Track pending_cue_sync_track;
    int eq_selected = 0;
    int eq_low = 0;
    int eq_mid = 0;
    int eq_high = 0;
    int eq_min = -60;
    int eq_max = 6;
    int eq_step = 1;
    int pending_normalize_lufs = -14;
    std::string pending_normalize_mode = "Short-Term Max";
    bool pending_process_all_folder = false;
    std::string pending_convert_format = "mp3";
    int metadata_offset = 0;
    int download_pool_scroll = 0;
    std::vector<std::pair<std::string, std::string>> metadata_details;
    std::string metadata_title;
    std::vector<Box> metadata_link_boxes;
    std::vector<std::string> metadata_link_urls;
    Track pending_stem_track;
    DemucsConfig pending_stem_config = controller.config().demucs;
    bool pending_stem_trim_region = false;
    double pending_stem_trim_start = 0.0;
    double pending_stem_trim_end = 0.0;
    Track pending_process_track;
    Track pending_analysis_track;
    Track pending_auto_cue_track;
    Track pending_delete_entry;
    std::string playing_track_id;
    Box directory_list_box;
    Box track_list_box;
    std::vector<Box> track_download_boxes;
    Box command_paste_box;
    Box command_clear_box;
    Box command_record_box;
    Box command_find_box;
    Box album_download_box;
    Box now_playing_box;
    Box volume_down_box;
    Box volume_up_box;
    Box previous_box;
    Box play_pause_box;
    Box next_box;
    Box mode_box;
    Box speed_reset_box;
    Box title_header_box;
    Box time_header_box;
    Box bpm_header_box;
    Box key_header_box;
    Box genre_header_box;
    Box bitrate_header_box;
    Box rate_header_box;
    Box format_header_box;
    Box size_header_box;
    Box confirm_yes_box;
    Box confirm_no_box;
    Box analysis_one_box;
    Box analysis_folder_box;
    Box analysis_cancel_box;
    Box playlist_show_box;
    Box playlist_download_box;
    Box playlist_cancel_box;
    Box stem_2_box;
    Box stem_4_box;
    Box stem_mp3_box;
    Box stem_wav_box;
    Box stem_flac_box;
    Box normalize_16_box;
    Box normalize_14_box;
    Box normalize_9_box;
    Box normalize_one_box;
    Box normalize_all_box;

    auto columnReservedWidth = [](const std::vector<std::string>& columns) {
        // Keep this in sync with trackRow(): five cells for the leading
        // download/play marker, then one separator and the fixed width of
        // every enabled metadata column.
        int width = 5;
        for (const auto& column : columns) {
            int column_width = trackColumnWidth(column);
            if (column_width > 0) {
                width += 1 + column_width;
            }
        }
        return width;
    };

    auto configuredTrackColumns = [&] {
        std::vector<std::string> columns;
        const auto& config_columns = controller.config().columns;
        for (const auto& column : config_columns.order) {
            if (trackColumnEnabled(config_columns, column)) {
                columns.push_back(column);
            }
        }
        return columns;
    };

    auto fitTrackColumns = [&](int right_width) {
        auto columns = configuredTrackColumns();
        while (!columns.empty() &&
               right_width - columnReservedWidth(columns) < 5) {
            columns.pop_back();
        }
        return columns;
    };
    Box convert_wav_box;
    Box convert_mp3_box;
    Box convert_m4a_box;
    Box convert_flac_box;
    Box convert_one_box;
    Box convert_all_box;
    Box auto_cue_track_box;
    Box auto_cue_folder_box;
    Box auto_cue_cancel_box;
    Box cue_sync_serato_to_traktor_box;
    Box cue_sync_traktor_to_serato_box;
    Box cue_sync_one_box;
    Box cue_sync_folder_box;
    Box cue_sync_cancel_box;
    Box metadata_close_box;
    Box metadata_list_box;
    Box eq_close_box;
    Box download_close_box;
    Box download_stop_box;
    Box download_pool_box;
    Box stems_close_box;
    Box audio_process_close_box;
    Box auto_cue_close_box;
    std::array<Box, 3> eq_reset_boxes{};
    std::atomic_bool manual_refresh_active = false;
    std::atomic_bool trim_refresh_active = false;
    std::atomic_bool editor_prepare_active = false;
    std::atomic_bool search_active = false;
    std::atomic_bool audd_find_active = false;
    std::atomic_bool dependency_update_active = false;
    std::jthread editor_prepare_worker;
    std::jthread search_worker;
    std::jthread audd_find_worker;
    std::jthread online_play_worker;
    std::jthread dependency_update_worker;
    std::atomic_bool online_play_starting = false;
    ManualCueEditor manual_cue_editor(controller, command_status, manual_refresh_active);
    TrimEditor trim_editor(controller, command_status, trim_refresh_active);

    auto syncBrowserData = [&](int left_width, int right_width, int row_count) {
        std::string current_path = controller.currentPath();
        auto tracks = controller.trackStore().getTracks();
        int dir_title_width = std::max(4, left_width - 2);
        int reserved_track_width = columnReservedWidth(visible_column_order);
        int track_title_width = std::max(5, right_width - reserved_track_width);

        dir_entries.clear();
        dir_paths.clear();
        track_entries.clear();
        std::string selected_track_id;
        if (state.selectedTrack >= 0 &&
            state.selectedTrack < (int)visible_files.size()) {
            selected_track_id = visible_files[state.selectedTrack].id;
        }
        visible_files.clear();

        std::unordered_map<std::string, bool> seen_dirs;
        int required_directory_width = 24;
        auto addDirectory = [&](const std::string& path, int depth, bool current) {
            if (path.empty() || seen_dirs[path]) {
                return;
            }
            seen_dirs[path] = true;

            std::string indent((size_t)std::max(0, depth) * 2, ' ');
            std::string connector = depth == 0 ? "" : (current ? "└ " : "├ ");
            std::string marker = current ? "▸ " : "  ";
            int available = std::max(4, dir_title_width - (int)indent.size() - (int)connector.size() - 2);
            const std::string virtual_name = controller.virtualPlaylistName(path);
            const std::string name = virtual_name.empty() ? displayName(path) : virtual_name;
            // Menu adds its own two-character selection marker to every row.
            required_directory_width = std::max(
                required_directory_width,
                (int)indent.size() + (int)connector.size() +
                    (int)marker.size() + (int)name.size() + 2);
            dir_entries.push_back(indent + connector + marker + truncateEnd(name, available));
            dir_paths.push_back(path);
        };

        const fs::path managed_search =
            fs::path(controller.config().rootFolder) / "Search";
        const fs::path downloads =
            fs::path(controller.config().rootFolder) / "Download";
        const fs::path separated =
            fs::path(controller.config().rootFolder) / "Separated";
        const fs::path records =
            fs::path(controller.config().rootFolder) / "Records";
        const fs::path managed_library_root =
            fs::path(controller.config().rootFolder).lexically_normal();
        auto isInside = [](const fs::path& path, const fs::path& root,
                           fs::path* relative_out = nullptr) {
            std::error_code ec;
            fs::path relative = fs::relative(path, root, ec);
            const bool inside = !ec && (relative == "." ||
                (!relative.empty() && !relative.string().starts_with("..")));
            if (inside && relative_out) {
                *relative_out = std::move(relative);
            }
            return inside;
        };
        const bool virtual_search_active =
            current_path.rfind("search://", 0) == 0;
        const bool physical_search_active =
            isInside(fs::path(current_path), managed_search);

        // Add a physical branch immediately after its root. This keeps every
        // child visually attached to the root it belongs to instead of adding
        // active-folder descendants after unrelated roots.
        auto addPhysicalBranch = [&](const fs::path& root, int first_depth) {
            fs::path relative;
            if (!isInside(fs::path(expanded_directory), root, &relative)) {
                return;
            }
            fs::path walk = root;
            int depth = first_depth;
            for (const auto& part : relative) {
                if (part == ".") {
                    continue;
                }
                walk /= part;
                addDirectory(walk.string(), depth,
                             walk == fs::path(expanded_directory));
                ++depth;
            }
            if (expanded_directory != current_path) {
                return;
            }
            for (const auto& track : tracks) {
                if (track.type == EntryType::Directory) {
                    // TPlay is represented by its own Search/Download/etc.
                    // roots above. Do not show the managed library itself as
                    // another ordinary folder under ~/Music.
                    if (fs::path(track.id).lexically_normal() == managed_library_root) {
                        continue;
                    }
                    addDirectory(track.id, depth, false);
                }
            }
        };

        addDirectory("search://root", 0,
                     virtual_search_active && current_path == "search://root");
        bool telegram_active = controller.isTelegramPath(expanded_directory) ||
                               controller.isTelegramPath(current_path);
        if (virtual_search_active && current_path != "search://root") {
            // Render every virtual ancestor, including album-search folders,
            // instead of assuming a fixed search-tree depth.
            std::string prefix = "search://";
            std::string tail = current_path.substr(prefix.size());
            int depth = 1;
            for (std::size_t start = 0; start < tail.size();) {
                const std::size_t slash = tail.find('/', start);
                const std::string part = tail.substr(start, slash - start);
                if (!part.empty()) {
                    prefix += part;
                    if (!controller.virtualPlaylistName(prefix).empty()) {
                        addDirectory(prefix, depth, prefix == current_path);
                        ++depth;
                    }
                    prefix += "/";
                }
                if (slash == std::string::npos) break;
                start = slash + 1;
            }
        }
        if (virtual_search_active) {
            const int virtual_child_depth =
                current_path == "search://root" ? 1 : 2;
            for (const auto& track : tracks) {
                if (track.type == EntryType::Directory) {
                    addDirectory(track.id, virtual_child_depth, false);
                }
            }
        }
        if (physical_search_active) {
            addPhysicalBranch(managed_search, 1);
        }

        std::error_code downloads_ec;
        if (fs::is_directory(downloads, downloads_ec)) {
            addDirectory(downloads.string(), 0,
                         isInside(fs::path(expanded_directory), downloads) &&
                         fs::path(expanded_directory) == downloads);
            addPhysicalBranch(downloads, 1);
        }

        std::error_code separated_ec;
        if (fs::is_directory(separated, separated_ec)) {
            addDirectory(separated.string(), 0,
                         isInside(fs::path(expanded_directory), separated) &&
                         fs::path(expanded_directory) == separated);
            addPhysicalBranch(separated, 1);
        }

        std::error_code records_ec;
        if (fs::is_directory(records, records_ec)) {
            addDirectory(records.string(), 0,
                         isInside(fs::path(expanded_directory), records) &&
                         fs::path(expanded_directory) == records);
            addPhysicalBranch(records, 1);
        }

        for (const auto& configured_root : controller.config().musicDirectories) {
            const fs::path root = configured_root;
            if (root == fs::path(controller.config().rootFolder) ||
                root == managed_search || root == downloads || root == separated || root == records) {
                continue;
            }
            // ~/Music can be both an external root and the parent of the
            // managed ~/Music/TPlay library. While browsing TPlay, keep that
            // external root collapsed; otherwise it would duplicate Search
            // and Separated under a second, incorrect branch.
            const bool root_contains_tplay = isInside(
                fs::path(controller.config().rootFolder), root);
            const bool browsing_tplay = isInside(
                fs::path(current_path), fs::path(controller.config().rootFolder));
            addDirectory(root.string(), 0,
                         !root_contains_tplay &&
                         isInside(fs::path(expanded_directory), root) &&
                         fs::path(expanded_directory) == root);
            if (!(root_contains_tplay && browsing_tplay)) {
                addPhysicalBranch(root, 1);
            }
        }

        if (controller.config().telegram.enabled) {
            std::string telegram_root = controller.telegramRootPath();
            addDirectory(telegram_root, 0, telegram_active && expanded_directory == telegram_root);
            if (telegram_active) {
                if (expanded_directory != telegram_root) {
                    addDirectory(expanded_directory, 1, true);
                }
                if (expanded_directory == current_path) {
                    int depth = expanded_directory == telegram_root ? 1 : 2;
                    for (const auto& track : tracks) {
                        if (track.type == EntryType::Directory) {
                            std::string indent((size_t)depth * 2, ' ');
                            int available = std::max(4, dir_title_width - (int)indent.size() - 4);
                            dir_entries.push_back(indent + "├   " + truncateEnd(track.title, available));
                            dir_paths.push_back(track.id);
                        }
                    }
                }
            }
        }

        for (const auto& track : tracks) {
            if (track.type == EntryType::File || track.type == EntryType::Album ||
                track.type == EntryType::Navigation) {
                visible_files.push_back(track);
            }
        }
        const bool direct_online_playlist = current_path == "search://recent" &&
            !visible_files.empty() &&
            std::all_of(visible_files.begin(), visible_files.end(),
                        [](const Track& track) {
                                   return track.id.starts_with("https://") ||
                                   track.id.starts_with("http://");
                       });
        const bool ranked_online_search = virtual_search_active &&
            current_path.find("/albums/") == std::string::npos &&
            !visible_files.empty() &&
            std::all_of(visible_files.begin(), visible_files.end(),
                        [](const Track& track) {
                            return track.type == EntryType::File &&
                                (track.id.starts_with("https://") ||
                                 track.id.starts_with("http://"));
                        });
        const bool album_track_order = virtual_search_active &&
            (current_path.find("/albums/") != std::string::npos ||
             direct_online_playlist);
        const bool album_search_results = !visible_files.empty() &&
            std::all_of(visible_files.begin(), visible_files.end(),
                        [](const Track& track) { return track.type == EntryType::Album; });
        // The Search root is a most-recent-first command history.  It is not
        // a media collection, so its chronological order is always retained.
        const bool search_history_order = current_path == "search://root" &&
            std::all_of(visible_files.begin(), visible_files.end(),
                        [](const Track& track) {
                            return track.type == EntryType::Navigation;
                        });
        const bool local_album_track_order =
            isInside(fs::path(current_path), downloads / "Albums") &&
            fs::path(current_path) != downloads / "Albums";
        if (!album_track_order && !album_search_results && !search_history_order &&
            (!ranked_online_search || track_sort_requested) &&
            (!local_album_track_order || track_sort_requested)) {
            sortTracks(visible_files, track_sort_column, track_sort_ascending);
        }
        track_download_boxes.assign(visible_files.size(), Box{});
        for (const auto& track : visible_files) {
            std::string label = track.status == TrackStatus::Downloading
                ? "[cloud] " + track.title
                : track.title;
            // A regular `search` is a flat list of online songs. Prefix it
            // with the artist so similarly named tracks are distinguishable,
            // while album playlists keep their numbered track titles clean.
            const bool flat_online_search = virtual_search_active &&
                current_path.find("/albums/") == std::string::npos &&
                track.type == EntryType::File &&
                (track.id.starts_with("https://") || track.id.starts_with("http://"));
            if (flat_online_search && !track.artist.empty()) {
                label = track.artist + " — " + label;
            }
            if (track.type == EntryType::Album) {
                label = track.artist.empty() ? track.title
                    : track.artist + " — " + track.title;
                if (!track.releaseDate.empty()) {
                    label += " (" + track.releaseDate + ")";
                }
            }
            if (local_album_track_order && track.type == EntryType::File) {
                label = stripTrackNumberPrefix(label);
            }
            if (track.isExplicit) label = "[E] " + label;
            track_entries.push_back(truncateEnd(label, track_title_width));
        }

        if (dir_entries.empty()) {
            state.selectedDirectory = 0;
        } else {
            state.selectedDirectory = std::clamp(
                state.selectedDirectory,
                0,
                (int)dir_entries.size() - 1);

            if (!preferred_directory.empty()) {
                auto selected = std::find(dir_paths.begin(), dir_paths.end(), preferred_directory);
                if (selected != dir_paths.end()) {
                    state.selectedDirectory = (int)std::distance(dir_paths.begin(), selected);
                }
                preferred_directory.clear();
            }
        }

        if (track_entries.empty()) {
            state.selectedTrack = 0;
        } else {
            if (!selected_track_id.empty()) {
                auto selected = std::find_if(
                    visible_files.begin(), visible_files.end(),
                    [&](const Track& track) { return track.id == selected_track_id; });
                if (selected != visible_files.end()) {
                    state.selectedTrack =
                        (int)std::distance(visible_files.begin(), selected);
                }
            }
            state.selectedTrack = std::clamp(
                state.selectedTrack,
                0,
                (int)track_entries.size() - 1);
        }

        auto buildViewport = [&](const std::vector<std::string>& source,
                                 int selected,
                                 int& offset,
                                 int& view_selected,
                                 std::vector<std::string>& output) {
            int count = (int)source.size();
            int capacity = std::max(1, row_count);
            int max_offset = std::max(0, count - capacity);
            offset = std::clamp(offset, 0, max_offset);
            if (selected < offset) {
                offset = selected;
            } else if (selected >= offset + capacity) {
                offset = selected - capacity + 1;
            }
            offset = std::clamp(offset, 0, max_offset);
            view_selected = count == 0 ? 0 : selected - offset;
            int end = std::min(count, offset + capacity);
            output.assign(source.begin() + std::min(offset, count), source.begin() + end);
        };

        buildViewport(dir_entries, state.selectedDirectory, state.dirOffset,
                      dir_view_selected, visible_dir_entries);
        buildViewport(track_entries, state.selectedTrack, state.trackOffset,
                      track_view_selected, visible_track_entries);
        directory_preferred_width = required_directory_width;
    };

    auto openSelectedDirectory = [&] {
        if (state.selectedDirectory < 0 ||
            state.selectedDirectory >= (int)dir_paths.size()) {
            return;
        }

        const std::string selected_path = dir_paths[state.selectedDirectory];
        if (!controller.isTelegramPath(selected_path) &&
            !controller.isVirtualPlaylistPath(selected_path) &&
            !fs::is_directory(selected_path)) {
            return;
        }

        // Mark the branch as expanded before scanning. A scan can publish a
        // refreshed browser list immediately, and doing this first prevents
        // the tree from briefly collapsing while focus moves to its playlist.
        expanded_directory = selected_path;
        preferred_directory = selected_path;
        state.selectedTrack = 0;
        state.focus = FocusPane::Directories;
        browser_selector = 0;
        if (selected_path != controller.currentPath()) {
            controller.scanDirectory(selected_path);
        }
    };

    auto goToParentDirectory = [&] {
        const std::string current = controller.currentPath();
        if (current.empty()) {
            return;
        }
        if (controller.isTelegramPath(current)) {
            if (current == controller.telegramRootPath()) {
                expanded_directory.clear();
                preferred_directory = controller.telegramRootPath();
                return;
            }
            controller.scanDirectory(controller.telegramRootPath());
            expanded_directory = controller.telegramRootPath();
            preferred_directory = current;
            state.selectedTrack = 0;
            return;
        }
        if (controller.isVirtualPlaylistPath(current)) {
            if (current == "search://root") {
                expanded_directory.clear();
                preferred_directory = "search://root";
                state.selectedTrack = 0;
                return;
            }
            if (current.rfind("search://", 0) == 0) {
                std::string parent = current;
                while (parent.size() > std::string("search://").size()) {
                    const std::size_t slash = parent.rfind('/');
                    if (slash == std::string::npos || slash < std::string("search://").size()) {
                        break;
                    }
                    parent.resize(slash);
                    if (!controller.virtualPlaylistName(parent).empty()) {
                        controller.scanDirectory(parent);
                        expanded_directory = parent;
                        preferred_directory = current;
                        state.selectedTrack = 0;
                        return;
                    }
                }
                controller.scanDirectory("search://root");
                expanded_directory = "search://root";
                preferred_directory = current;
                state.selectedTrack = 0;
                return;
            }
            expanded_directory.clear();
            state.selectedTrack = 0;
            return;
        }

        std::string active_root;
        for (const auto& root : controller.config().musicDirectories) {
            std::error_code ec;
            std::string relative = fs::relative(current, root, ec).string();
            if (!ec && !relative.starts_with("..")) {
                active_root = root;
                break;
            }
        }

        if (active_root.empty() || fs::path(current) == fs::path(active_root)) {
            expanded_directory.clear();
            preferred_directory = active_root;
            return;
        }

        std::string parent = fs::path(current).parent_path().string();
        controller.scanDirectory(parent);
        expanded_directory = parent;
        preferred_directory = current;
        state.selectedTrack = 0;
    };

    auto playSelectedTrack = [&] {
        if (state.selectedTrack < 0 ||
            state.selectedTrack >= (int)visible_files.size()) {
            return;
        }

        const Track selected = visible_files[state.selectedTrack];
        std::string history_query;
        bool history_album_search = false;
        if (controller.recentSearchRequest(selected, history_query,
                                           history_album_search)) {
            bool expected = false;
            if (!search_active.compare_exchange_strong(expected, true)) {
                command_status = "Search is already running";
                return;
            }
            if (search_worker.joinable()) {
                search_worker.join();
            }
            command_status = "Repeating " + std::string(history_album_search
                ? "album search: " : "search: ") + history_query;
            search_worker = std::jthread(
                [&, query = std::move(history_query), history_album_search]
                (std::stop_token stop_token) {
                    std::string result;
                    const bool succeeded = controller.searchMusic(
                        query, history_album_search, result);
                    if (stop_token.stop_requested()) {
                        search_active = false;
                        return;
                    }
                    screen.Post([&, succeeded, result = std::move(result)]() mutable {
                        search_active = false;
                        if (succeeded) {
                            expanded_directory = controller.currentPath();
                            preferred_directory = expanded_directory;
                            state.selectedTrack = 0;
                            state.focus = FocusPane::Tracks;
                            browser_selector = 1;
                            root_selector = 0;
                            bottom_selector = 0;
                            command_status = std::move(result);
                        } else {
                            command_status = "Search error: " + result;
                        }
                        screen.PostEvent(Event::Custom);
                    });
                });
            return;
        }
        if (selected.type == EntryType::Navigation) {
            if (!controller.isVirtualPlaylistPath(selected.id)) {
                command_status = "Album is no longer available";
                return;
            }
            controller.scanDirectory(selected.id);
            expanded_directory = selected.id;
            preferred_directory = selected.id;
            state.selectedTrack = 0;
            command_status = "Album results";
            return;
        }
        if (selected.type == EntryType::Album) {
            bool expected = false;
            if (!search_active.compare_exchange_strong(expected, true)) {
                command_status = "Search is already running";
                return;
            }
            if (search_worker.joinable()) {
                search_worker.join();
            }
            command_status = "Loading album: " + selected.title;
            search_worker = std::jthread(
                [&, selected](std::stop_token stop_token) {
                    std::string result;
                    const bool opened = controller.openOfficialAlbum(selected, result);
                    if (stop_token.stop_requested()) {
                        search_active = false;
                        return;
                    }
                    screen.Post([&, opened, result = std::move(result)]() mutable {
                        search_active = false;
                        if (opened) {
                            expanded_directory = controller.currentPath();
                            preferred_directory = expanded_directory;
                            state.selectedTrack = 0;
                            command_status = std::move(result);
                        } else {
                            command_status = "Album error: " + result;
                        }
                        screen.PostEvent(Event::Custom);
                    });
                });
            return;
        }
        const bool online = selected.id.starts_with("https://") ||
            selected.id.starts_with("http://");
        if (online) {
            bool expected = false;
            if (!online_play_starting.compare_exchange_strong(expected, true)) {
                command_status = "Opening online stream";
                return;
            }
            if (online_play_worker.joinable()) {
                online_play_worker.join();
            }
            command_status = "Opening online stream: " + selected.title;
            const std::vector<Track> ordered_tracks = visible_files;
            online_play_worker = std::jthread(
                [&, selected, ordered_tracks](std::stop_token stop_token) {
                    const bool started = controller.playTrack(selected, ordered_tracks);
                    if (stop_token.stop_requested()) {
                        return;
                    }
                    screen.Post([&, selected, started] {
                        online_play_starting = false;
                        if (started) {
                            playing_track_id = selected.id;
                            command_status.clear();
                        } else {
                            command_status = "Unable to open online stream";
                        }
                        screen.PostEvent(Event::Custom);
                    });
                });
            return;
        }

        if (controller.playTrack(selected, visible_files)) {
            playing_track_id = selected.id;
        }
    };

    auto finishKeyboardMove = [&] {
        move_to_mode = false;
        state.focus = FocusPane::Tracks;
        browser_selector = 1;
        root_selector = 0;
    };

    auto moveTrackToSelectedDirectory = [&] {
        if (!move_to_mode ||
            state.selectedDirectory < 0 ||
            state.selectedDirectory >= (int)dir_paths.size()) {
            return;
        }

        const std::string destination = dir_paths[state.selectedDirectory];
        std::string error;
        if (controller.moveTrack(move_track, destination, error)) {
            command_status = "Moved: " + move_track.title +
                " -> " + displayName(destination);
        } else {
            command_status = "Error: " + error;
        }
        finishKeyboardMove();
    };

    const std::string selection_style = controller.config().selectionStyle;
    auto applySelectionStyle = [&](Element row, bool active) {
        if (!active) return row;
        if (selection_style == "full") return row | inverted;
        if (selection_style == "underline") return row | underlined;
        return row | bold;
    };

    MenuOption dir_options = MenuOption::Vertical();
    dir_options.selected = &dir_view_selected;
    dir_options.focused_entry = &dir_view_selected;
    dir_options.entries = &visible_dir_entries;
    dir_options.on_change = [&] {
        state.selectedDirectory = state.dirOffset + dir_view_selected;
        state.focus = FocusPane::Directories;
        browser_selector = 0;
    };
    dir_options.on_enter = openSelectedDirectory;
    dir_options.entries_option.transform = [&](const EntryState& entry) {
        Element row = text((entry.active ? "> " : "  ") + entry.label);
        return applySelectionStyle(std::move(row), entry.active);
    };

    MenuOption track_options = MenuOption::Vertical();
    track_options.selected = &track_view_selected;
    track_options.focused_entry = &track_view_selected;
    track_options.entries = &visible_track_entries;
    track_options.on_change = [&] {
        state.selectedTrack = state.trackOffset + track_view_selected;
        state.focus = FocusPane::Tracks;
        browser_selector = 1;
    };
    track_options.on_enter = playSelectedTrack;
    track_options.entries_option.transform = [&](const EntryState& entry) {
        std::string time = "--:--";
        std::string bpm = "0";
        std::string key = "-";
        std::string bitrate = "-";
        std::string sample_rate = "-";
        std::string format = "-";
        std::string size = "-";
        std::string genre = "-";

        int track_index = state.trackOffset + entry.index;
        if (track_index >= 0 && track_index < (int)visible_files.size()) {
            const auto& track = visible_files[track_index];
            time = formatTime(track.duration);
            if (track.type == EntryType::Album) {
                time = track.releaseDate.empty() ? "-" : track.releaseDate;
            }
            bitrate = formatBitrate(track.bitrateKbps);
            sample_rate = formatSampleRate(track.sampleRateHz);
            format = track.format.empty() ? lowerExtension(track.id) : track.format;
            if (!format.empty() && format.front() == '.') {
                format.erase(format.begin());
            }
            if (format.empty()) {
                format = "-";
            }
            format = truncateEnd(format, trackColumnWidth("frmt"));
            size = formatSize(track.sizeBytes);
            // The compact table shows the most specific genre. Keep the full
            // hierarchy (for example "Rock / Nu Metal") everywhere else.
            std::string table_genre = track.genre;
            if (const std::size_t separator = table_genre.rfind('/');
                separator != std::string::npos) {
                const std::string specific = ProcessRunner::trim(
                    table_genre.substr(separator + 1));
                if (!specific.empty()) {
                    table_genre = specific;
                }
            }
            genre = table_genre.empty() ? "-" :
                truncateEnd(table_genre, trackColumnWidth("genre"));
            if (track.status == TrackStatus::Analyzing) {
                bpm = "...";
                key = "...";
            } else {
                bpm = track.bpm > 0.0 ? std::to_string((int)track.bpm) : "-";
                key = track.key.empty() ? "-" : track.key;
            }
        }

        bool playing = track_index >= 0 &&
            track_index < (int)visible_files.size() &&
            visible_files[track_index].id == playing_track_id;
        std::string marker = playing ? " ▶ " : (entry.active ? " > " : "   ");
        Element leading = text(marker);
        if (track_index >= 0 && track_index < (int)visible_files.size() &&
            (visible_files[track_index].id.starts_with("https://") ||
             visible_files[track_index].id.starts_with("http://"))) {
            const bool downloaded = controller.isOnlineTrackDownloaded(
                visible_files[(size_t)track_index]);
            Elements markers;
            markers.insert(markers.end(), {
                text(" "),
                text(downloaded ? "✓" : "↓") |
                    color(downloaded ? Color::Green : Color::Cyan) |
                    reflect(track_download_boxes[(size_t)track_index]),
                // A separate spacer element is preserved by FTXUI, unlike a
                // trailing space inside the coloured download icon.
                text(" "),
            });
            if (playing) {
                // The download action stays first; playback state is shown
                // immediately before the title as requested.
                markers.push_back(text("▶"));
                markers.push_back(text(" "));
            } else if (entry.active) {
                markers.push_back(text("> "));
            } else {
                // Reserve the play marker's width so every online title
                // starts at the same column.
                markers.push_back(text("  "));
            }
            leading = hbox(std::move(markers));
        }
        const std::string current_path = controller.currentPath();
        const fs::path local_albums_root =
            fs::path(controller.config().rootFolder) / "Download" / "Albums";
        std::error_code local_album_error;
        const fs::path local_relative = fs::relative(
            fs::path(current_path), local_albums_root, local_album_error);
        const bool local_album_collection = !local_album_error &&
            !local_relative.empty() && local_relative != "." &&
            !local_relative.string().starts_with("..");
        const bool ordered_collection = current_path.find("/albums/") != std::string::npos ||
            local_album_collection ||
            (current_path == "search://recent" && !visible_files.empty() &&
             std::all_of(visible_files.begin(), visible_files.end(), [](const Track& track) {
                 return track.id.starts_with("https://") || track.id.starts_with("http://");
             }));
        std::string title = entry.label;
        if (controller.config().showTrackNumbers && ordered_collection && track_index >= 0 &&
            visible_files[(size_t)track_index].type == EntryType::File) {
            const int number = track_index > 0 &&
                visible_files.front().type == EntryType::Navigation
                ? track_index : track_index + 1;
            title = std::format("{:02d}. ", number) + title;
        }
        Element row = trackRow(title, time, bpm, key, bitrate, sample_rate, format, size,
                               genre,
                               visible_column_order, std::move(leading));
        return applySelectionStyle(std::move(row), entry.active);
    };

    auto dir_menu = Menu(dir_options);
    auto track_menu = Menu(track_options);

    auto toggleTrackSort = [&](TrackSortColumn column) {
        const std::string path = controller.currentPath();
        if (path.starts_with("search://") &&
            path.find("/albums/") != std::string::npos) {
            command_status = "Album order follows the release tracklist";
            return;
        }
        if (track_sort_column == column) {
            track_sort_ascending = !track_sort_ascending;
        } else {
            track_sort_column = column;
            track_sort_ascending = true;
        }
        track_sort_requested = true;
        state.focus = FocusPane::Tracks;
        browser_selector = 1;
        root_selector = 0;
    };

    SliderOption<int> progress_options;
    progress_options.value = &progress_value;
    progress_options.min = &progress_min;
    progress_options.max = &progress_max;
    progress_options.increment = &progress_step;
    progress_options.on_change = [&] {
        progress_dragging = true;
        controller.seekPlayback((double)progress_value / (double)progress_max);
    };
    auto progress_slider = Slider(progress_options);

    auto setSpeedIfLocal = [&](double rate) {
        if (controller.isStreamingPlayback()) {
            speed_value = (int)std::round(controller.playbackRate() * 100.0);
            command_status = "Speed and pitch are available after the stream cache is ready";
            return false;
        }
        controller.setPlaybackRate(rate);
        return true;
    };

    auto setPitchIfLocal = [&](bool preserve) {
        if (controller.isStreamingPlayback()) {
            preserve_pitch = controller.preservePitch();
            command_status = "Speed and pitch are available after the stream cache is ready";
            return false;
        }
        controller.setPreservePitch(preserve);
        return true;
    };

    SliderOption<int> speed_options;
    speed_options.value = &speed_value;
    speed_options.min = &speed_min;
    speed_options.max = &speed_max;
    speed_options.increment = &speed_step;
    speed_options.on_change = [&] {
        setSpeedIfLocal((double)speed_value / 100.0);
    };
    auto speed_slider = Slider(speed_options);

    CheckboxOption pitch_lock_options = CheckboxOption::Simple();
    pitch_lock_options.label = "♩";
    pitch_lock_options.checked = &preserve_pitch;
    pitch_lock_options.on_change = [&] {
        setPitchIfLocal(preserve_pitch);
    };
    auto pitch_lock_checkbox = Checkbox(pitch_lock_options);

    auto applyEq = [&] {
        controller.setEqualizerGains(eq_low, eq_mid, eq_high);
    };
    SliderOption<int> eq_low_options;
    eq_low_options.value = &eq_low;
    eq_low_options.min = &eq_min;
    eq_low_options.max = &eq_max;
    eq_low_options.increment = &eq_step;
    eq_low_options.on_change = applyEq;
    auto eq_low_slider = Slider(eq_low_options);

    SliderOption<int> eq_mid_options = eq_low_options;
    eq_mid_options.value = &eq_mid;
    auto eq_mid_slider = Slider(eq_mid_options);

    SliderOption<int> eq_high_options = eq_low_options;
    eq_high_options.value = &eq_high;
    auto eq_high_slider = Slider(eq_high_options);

    auto seekPlaybackBySeconds = [&](double delta_seconds) {
        auto playback = controller.playbackSnapshot();
        double duration = std::max(0.0, playback.durationSeconds);
        if (duration <= 0.0) {
            return false;
        }

        double new_seconds = std::clamp(
            playback.positionSeconds + delta_seconds,
            0.0,
            duration);
        progress_value = (int)std::round(
            (new_seconds / duration) * progress_max);
        progress_dragging = true;
        controller.seekPlayback(
            (double)progress_value / (double)progress_max);
        return true;
    };

    auto pasteClipboardToInput = [&] {
        auto value = readClipboard();
        if (!value) {
            command_status = "Clipboard unavailable";
            return false;
        }
        command_input = *value;
        while (!command_input.empty() &&
               (command_input.back() == '\n' || command_input.back() == '\r')) {
            command_input.pop_back();
        }
        root_selector = 2;
        bottom_selector = 0;
        command_status = command_input.empty()
            ? "Clipboard is empty"
            : "Pasted from clipboard";
        return true;
    };

    auto toggleDesktopRecording = [&] {
        if (audd_find_active.load()) {
            command_status = "Audio FIND is running; use CANCEL";
            return;
        }
        const auto recording = controller.recordingSnapshot();
        if (recording.state == RecordingState::Recording) {
            controller.stopDesktopRecording();
            const auto saved = controller.recordingSnapshot();
            command_status = saved.message;
            return;
        }
        std::string result;
        if (controller.startDesktopRecording(result)) {
            show_recording_panel = true;
            command_status = "Desktop recording started";
        } else {
            command_status = "Recording error: " + result;
        }
    };

    auto startAudDFind = [&] {
        if (!controller.auddFindEnabled()) {
            command_status = "Audio FIND is unavailable: set [audd] api_key in config.toml";
            return;
        }
        if (audd_find_active.load()) {
            audd_find_worker.request_stop();
            command_status = "Audio FIND: canceling…";
            return;
        }
        if (controller.recordingSnapshot().state == RecordingState::Recording) {
            command_status = "Audio FIND: stop the current desktop recording first";
            return;
        }
        bool expected = false;
        if (!audd_find_active.compare_exchange_strong(expected, true)) {
            command_status = "Audio FIND is already running";
            return;
        }
        if (!search_active.compare_exchange_strong(expected, true)) {
            audd_find_active = false;
            command_status = "Search is already running";
            return;
        }

        std::string recording_error;
        if (!controller.startAudDRecording(recording_error)) {
            audd_find_active = false;
            search_active = false;
            command_status = "Audio FIND: recording error: " + recording_error;
            return;
        }

        show_recording_panel = true;
        const int listen_seconds = controller.auddListenSeconds();
        command_status = "Audio FIND: waiting for desktop audio…";
        if (audd_find_worker.joinable()) {
            audd_find_worker.join();
        }
        audd_find_worker = std::jthread(
            [&, listen_seconds](std::stop_token stop_token) {
                bool listening = false;
                std::chrono::steady_clock::time_point listen_started;
                while (true) {
                    if (stop_token.stop_requested()) {
                        controller.stopDesktopRecording();
                        controller.discardAudDRecording(
                            controller.recordingSnapshot().filePath);
                        audd_find_active = false;
                        search_active = false;
                        screen.Post([&] {
                            command_status = "Audio FIND canceled";
                            screen.PostEvent(Event::Custom);
                        });
                        return;
                    }
                    if (controller.recordingSnapshot().state != RecordingState::Recording) {
                        const RecordingSnapshot recording = controller.recordingSnapshot();
                        controller.discardAudDRecording(recording.filePath);
                        audd_find_active = false;
                        search_active = false;
                        screen.Post([&] {
                            command_status = "Audio FIND: desktop recording stopped before recognition";
                            screen.PostEvent(Event::Custom);
                        });
                        return;
                    }

                    if (!listening) {
                        if (!controller.auddRecordingHasAudio()) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                            continue;
                        }

                        // Discard the silent lead-in and begin a fresh clean
                        // capture once music is actually playing.
                        controller.stopDesktopRecording();
                        const RecordingSnapshot waiting = controller.recordingSnapshot();
                        controller.discardAudDRecording(waiting.filePath);
                        if (stop_token.stop_requested()) continue;
                        std::string restart_error;
                        if (!controller.startAudDRecording(restart_error)) {
                            audd_find_active = false;
                            search_active = false;
                            screen.Post([&, restart_error = std::move(restart_error)] {
                                command_status = "Audio FIND: recording error: " + restart_error;
                                screen.PostEvent(Event::Custom);
                            });
                            return;
                        }
                        listening = true;
                        listen_started = std::chrono::steady_clock::now();
                        screen.Post([&, listen_seconds] {
                            command_status = "Audio FIND: listening for " +
                                std::to_string(listen_seconds) + " seconds";
                            screen.PostEvent(Event::Custom);
                        });
                    }

                    if (listening && std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - listen_started).count()
                        >= listen_seconds) {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }

                controller.stopDesktopRecording();
                const RecordingSnapshot recording = controller.recordingSnapshot();
                if (stop_token.stop_requested()) {
                    audd_find_active = false;
                    search_active = false;
                    return;
                }

                screen.Post([&] {
                    command_status = "Audio FIND: recognizing with AudD…";
                    screen.PostEvent(Event::Custom);
                });
                AudDMatch match;
                std::string error;
                const bool recognized = controller.recognizeAudDRecording(
                    recording.filePath, match, error);
                controller.discardAudDRecording(recording.filePath);
                if (stop_token.stop_requested()) {
                    audd_find_active = false;
                    search_active = false;
                    return;
                }
                if (!recognized) {
                    audd_find_active = false;
                    search_active = false;
                    screen.Post([&, error = std::move(error)] {
                        command_status = "Audio FIND: " + error;
                        screen.PostEvent(Event::Custom);
                    });
                    return;
                }

                const std::string query = match.artist + " - " + match.title;
                screen.Post([&, query] {
                    command_status = "Audio FIND: " + query + " — searching YouTube…";
                    screen.PostEvent(Event::Custom);
                });
                std::string result;
                const bool found = controller.searchMusic(query, false, result);
                audd_find_active = false;
                if (stop_token.stop_requested()) {
                    search_active = false;
                    return;
                }
                screen.Post([&, query, found, result = std::move(result)]() mutable {
                    search_active = false;
                    if (found) {
                        expanded_directory = controller.currentPath();
                        preferred_directory = expanded_directory;
                        state.selectedTrack = 0;
                        state.focus = FocusPane::Tracks;
                        browser_selector = 1;
                        root_selector = 0;
                        bottom_selector = 0;
                        command_status = "Audio FIND: " + query;
                    } else {
                        command_status = "Audio FIND search error: " + result;
                    }
                    screen.PostEvent(Event::Custom);
                });
            });
    };

    auto beginDownload = [&](const std::string& source,
                             const std::string& destination) {
        if (!controller.downloadToDirectory(source, destination, [&, destination] {
            screen.Post([&, destination] {
                if (controller.currentPath() == destination) {
                    controller.scanDirectory(destination, true);
                }
            });
        })) {
            command_status = controller.downloadSnapshot().detail;
            return false;
        }
        show_download_panel = true;
        command_status.clear();
        return true;
    };

    auto beginSearchDownload = [&](const std::vector<Track>& tracks) {
        // Closing the panel does not create a new request. An album download
        // may continue after its progress panel has been dismissed.
        if (controller.downloadSnapshot().state == DownloadState::Running) {
            command_status = "Download already running";
            return false;
        }
        std::vector<Track> downloadable;
        std::copy_if(tracks.begin(), tracks.end(), std::back_inserter(downloadable),
                     [](const Track& track) {
                         return track.type == EntryType::File &&
                             (track.id.starts_with("https://") ||
                              track.id.starts_with("http://"));
                     });
        if (downloadable.empty()) {
            command_status = "Select an online track or album";
            return false;
        }
        if (!controller.downloadTracksToCurrentDirectory(downloadable, [&] {
                screen.Post([&screen] {
                    screen.PostEvent(Event::Custom);
                });
            })) {
            command_status = controller.downloadSnapshot().detail;
            return false;
        }
        show_download_panel = true;
        show_download_pool = downloadable.size() > 1;
        command_status = "Download started: " +
            std::to_string(downloadable.size()) + " track(s)";
        return true;
    };

    auto openOnlineAlbum = [&](const Track& track) {
        if (track.album.empty()) {
            command_status = "This online track has no album metadata";
            return;
        }
        bool expected = false;
        if (!search_active.compare_exchange_strong(expected, true)) {
            command_status = "Search is already running";
            return;
        }
        if (search_worker.joinable()) {
            search_worker.join();
        }
        command_status = "Opening album: " + track.album;
        search_worker = std::jthread([&, track](std::stop_token stop_token) {
            std::string result;
            const bool succeeded = controller.openAlbumFromSearchTrack(track, result);
            if (stop_token.stop_requested()) {
                search_active = false;
                return;
            }
            screen.Post([&, succeeded, result = std::move(result)]() mutable {
                search_active = false;
                if (succeeded) {
                    expanded_directory = controller.currentPath();
                    preferred_directory = expanded_directory;
                    state.selectedTrack = 0;
                    command_status = std::move(result);
                } else {
                    command_status = "Open album: " + result;
                }
                screen.PostEvent(Event::Custom);
            });
        });
    };

    auto clearConfirmation = [&] {
        pending_confirmation = PendingConfirmation::None;
        confirmation_selected = 1;
        pending_playlist_source.clear();
        pending_playlist_destination.clear();
        playlist_action_col = 0;
        pending_stem_track = Track{};
        pending_stem_config = controller.config().demucs;
        pending_stem_trim_region = false;
        pending_stem_trim_start = 0.0;
        pending_stem_trim_end = 0.0;
        stem_option_row = 0;
        stem_option_col = 0;
        normalize_option_row = 0;
        normalize_option_col = 1;
        convert_option_row = 0;
        convert_option_col = 1;
        analysis_option_row = 0;
        auto_cue_option_col = 0;
        cue_sync_option_row = 0;
        cue_sync_direction_col = 0;
        cue_sync_scope_col = 0;
        pending_cue_sync_direction = CueSyncDirection::SeratoToTraktor;
        pending_normalize_lufs = -14;
        pending_normalize_mode = "Short-Term Max";
        pending_process_all_folder = false;
        pending_convert_format = "mp3";
        pending_process_track = Track{};
        pending_analysis_track = Track{};
        pending_auto_cue_track = Track{};
        pending_cue_sync_track = Track{};
        pending_delete_entry = Track{};
    };

    auto rejectConfirmation = [&] {
        if (pending_confirmation == PendingConfirmation::PlaylistDownload) {
            clearConfirmation();
            command_status = "Playlist action cancelled";
            return;
        } else if (pending_confirmation == PendingConfirmation::StemSeparation) {
            command_status = "Demucs cancelled";
        } else if (pending_confirmation == PendingConfirmation::Normalization) {
            command_status = "Normalization cancelled";
        } else if (pending_confirmation == PendingConfirmation::Convert) {
            command_status = "Convert cancelled";
        } else if (pending_confirmation == PendingConfirmation::Analysis) {
            command_status = "Analysis cancelled";
        } else if (pending_confirmation == PendingConfirmation::AutoCue) {
            command_status = "Auto Cue cancelled";
        } else if (pending_confirmation == PendingConfirmation::CueSync) {
            command_status = "Cue sync cancelled";
        } else if (pending_confirmation == PendingConfirmation::Quit) {
            command_status = "Quit cancelled";
        } else if (pending_confirmation == PendingConfirmation::DeleteEntry) {
            command_status = "Delete cancelled";
        }
        clearConfirmation();
    };

    auto acceptConfirmation = [&] {
        if (pending_confirmation == PendingConfirmation::PlaylistDownload) {
            std::string source = std::move(pending_playlist_source);
            std::string destination = std::move(pending_playlist_destination);
            clearConfirmation();
            beginDownload(source, destination);
            return true;
        }

        if (pending_confirmation == PendingConfirmation::StemSeparation) {
            Track track = pending_stem_track;
            DemucsConfig config = pending_stem_config;
            const bool trim_region = pending_stem_trim_region;
            const double trim_start = pending_stem_trim_start;
            const double trim_end = pending_stem_trim_end;
            clearConfirmation();
            if (trim_region) {
                std::string trim_error;
                Track region_track;
                if (!controller.trimTrack(track, trim_start, trim_end,
                                          trim_error, &region_track)) {
                    command_status = "Split error: " + trim_error;
                    return true;
                }
                track = std::move(region_track);
            }
            if (controller.separateTrack(track, config)) {
                show_activity = true;
                show_stems_panel = true;
                command_status = "Demucs started: " + track.title;
            } else {
                command_status = "Error: " +
                    controller.stemSeparationSnapshot().detail;
            }
            return true;
        }

        if (pending_confirmation == PendingConfirmation::Normalization) {
            int lufs = pending_normalize_lufs;
            std::string mode = pending_normalize_mode;
            bool all_folder = pending_process_all_folder;
            Track track = pending_process_track;
            std::vector<Track> tracks = all_folder
                ? visible_files
                : std::vector<Track>{track};
            clearConfirmation();
            if (!all_folder && track.id.empty()) {
                command_status = "Select a track or choose all folder";
                return true;
            }
            if (controller.normalizeTracks(tracks, NormalizationOptions{lufs, mode})) {
                show_activity = true;
                show_audio_process_panel = true;
                std::string mode_suffix = mode.empty() ? "" : " " + mode;
                command_status = "Normalization started: " +
                    std::to_string(lufs) + " LUFS" + mode_suffix + " | " +
                    (all_folder ? "all folder" : track.title);
            } else {
                command_status = "Error: " +
                    controller.audioProcessSnapshot().detail;
            }
            return true;
        }

        if (pending_confirmation == PendingConfirmation::Convert) {
            std::string format = pending_convert_format;
            bool all_folder = pending_process_all_folder;
            Track track = pending_process_track;
            std::vector<Track> tracks = all_folder
                ? visible_files
                : std::vector<Track>{track};
            clearConfirmation();
            if (!all_folder && track.id.empty()) {
                command_status = "Select a track or choose all folder";
                return true;
            }
            if (controller.convertTracks(tracks, ConvertOptions{format})) {
                show_activity = true;
                show_audio_process_panel = true;
                command_status = "Convert started: " + format + " | " +
                    (all_folder ? "all folder" : track.title);
            } else {
                command_status = "Error: " +
                    controller.audioProcessSnapshot().detail;
            }
            return true;
        }

        if (pending_confirmation == PendingConfirmation::Analysis) {
            const int option = analysis_option_row;
            const Track track = pending_analysis_track;
            std::vector<Track> tracks = option == 1
                ? visible_files
                : std::vector<Track>{track};
            clearConfirmation();
            if (option == 2) {
                command_status = "Analysis cancelled";
                return true;
            }
            std::string error;
            if (!controller.analyzeTracks(tracks, error)) {
                command_status = "Analysis error: " + error;
                return true;
            }
            const size_t count = std::count_if(tracks.begin(), tracks.end(),
                [](const Track& item) {
                    return item.type == EntryType::File &&
                        !item.id.starts_with("https://") &&
                        !item.id.starts_with("http://");
                });
            command_status = option == 1
                ? "Analyzing folder: " + std::to_string(count) + " track(s)"
                : "Analyzing: " + track.title;
            return true;
        }

        if (pending_confirmation == PendingConfirmation::AutoCue) {
            int option = auto_cue_option_col;
            Track track = pending_auto_cue_track;
            clearConfirmation();
            show_activity = true;
            show_auto_cue_panel = true;
            if (option == 0) {
                if (track.id.empty()) {
                    command_status = "Select a track";
                } else if (controller.startAutoCueTrack(track)) {
                    command_status = "Auto Cue started: " + track.title;
                } else {
                    command_status = "Auto Cue: " +
                        controller.autoCueSnapshot().status;
                }
            } else if (option == 1) {
                if (controller.startAutoCueFolder()) {
                    command_status = "Auto Cue started: " +
                        displayName(controller.currentPath());
                } else {
                    command_status = "Auto Cue: " +
                        controller.autoCueSnapshot().status;
                }
            } else {
                command_status = "Auto Cue cancelled";
            }
            return true;
        }

        if (pending_confirmation == PendingConfirmation::CueSync) {
            int row = cue_sync_option_row;
            int col = cue_sync_scope_col;
            CueSyncDirection direction = pending_cue_sync_direction;
            Track track = pending_cue_sync_track;
            bool all_folder = row == 1 && col == 1;
            bool cancel = row == 1 && col == 2;
            std::vector<Track> tracks = all_folder
                ? visible_files
                : std::vector<Track>{track};
            clearConfirmation();
            if (cancel) {
                command_status = "Cue sync cancelled";
                return true;
            }
            if (!all_folder && track.id.empty()) {
                command_status = "Select a track to sync cues";
                return true;
            }
            int ok = 0;
            int failed = 0;
            std::string last_result;
            for (const auto& item : tracks) {
                if (item.type != EntryType::File) {
                    continue;
                }
                std::string result;
                if (controller.syncCueMetadata(item, direction, result)) {
                    ++ok;
                    last_result = result;
                } else {
                    ++failed;
                    if (last_result.empty()) {
                        last_result = result;
                    }
                }
            }
            if (all_folder) {
                command_status = "Cue sync folder: ok " + std::to_string(ok) +
                    " | errors " + std::to_string(failed);
            } else if (ok > 0) {
                command_status = last_result;
            } else {
                command_status = "Cue sync error: " + last_result;
            }
            return true;
        }

        if (pending_confirmation == PendingConfirmation::Quit) {
            clearConfirmation();
            refresh_running = false;
            screen.Exit();
            return true;
        }

        if (pending_confirmation == PendingConfirmation::DeleteEntry) {
            Track entry = pending_delete_entry;
            clearConfirmation();
            std::string error;
            if (controller.deleteEntry(entry, error)) {
                command_status = "Deleted: " + entry.title;
            } else {
                command_status = "Error: " + error;
            }
            return true;
        }

        return false;
    };

    auto showOnlinePlaylist = [&](std::string source) {
        bool expected = false;
        if (!search_active.compare_exchange_strong(expected, true)) {
            command_status = "Search is already running";
            return;
        }
        if (search_worker.joinable()) {
            search_worker.join();
        }
        command_status = "Opening playlist...";
        search_worker = std::jthread(
            [&, source = std::move(source)](std::stop_token stop_token) {
                std::string result;
                const bool succeeded = controller.openOnlinePlaylist(source, result);
                if (stop_token.stop_requested()) {
                    search_active = false;
                    return;
                }
                screen.Post([&, succeeded, result = std::move(result)]() mutable {
                    search_active = false;
                    if (succeeded) {
                        expanded_directory = controller.currentPath();
                        preferred_directory = expanded_directory;
                        state.selectedTrack = 0;
                        command_status = std::move(result);
                    } else {
                        command_status = "Playlist error: " + result;
                    }
                    screen.PostEvent(Event::Custom);
                });
            });
    };

    InputOption input_options = InputOption::Default();
    input_options.content = &command_input;
    input_options.cursor_position = &command_cursor_position;
    input_options.placeholder = "input";
    input_options.multiline = false;
    const bool input_underline_cursor =
        controller.config().input.cursorType == "underline";
    input_options.transform = [input_underline_cursor](InputState state) {
        // InputOption::Default inverts the entire focused line, which is the
        // white rectangle shown by the terminal. Keep the command line in
        // the same visual language as the rest of TPlay instead.
        Element element = std::move(state.element);
        if (state.is_placeholder) {
            element = element | dim;
        }
        // Input itself owns the caret position and supplies a blinking bar.
        // Wrapping it in a second bar decorator moves the terminal cursor out
        // of the text. Only override the shape for the optional underline.
        if (state.focused && input_underline_cursor) {
            element = focusCursorUnderlineBlinking(std::move(element));
        }
        return element;
    };
    input_options.on_enter = [&] {
        std::string input = trimInput(command_input);
        std::string download_source = downloadSourceFromInput(input);
        command_input.clear();
        command_cursor_position = 0;

        command_status.clear();

        if (input == "q" || input == "quit") {
            refresh_running = false;
            screen.Exit();
        } else if (input == "now") {
            const Track playing = controller.playingTrack();
            std::string result;
            if (controller.openNowPlayingLocation(result)) {
                expanded_directory = controller.currentPath();
                preferred_directory = expanded_directory;
                state.selectedTrack = 0;
                const auto entries = controller.trackStore().getTracks();
                int file_index = 0;
                for (const auto& entry : entries) {
                    if (entry.type != EntryType::File) continue;
                    if (entry.id == playing.id) {
                        state.selectedTrack = file_index;
                        break;
                    }
                    ++file_index;
                }
                command_status = result;
                state.focus = FocusPane::Tracks;
                browser_selector = 1;
                root_selector = 0;
                bottom_selector = 0;
            } else {
                command_status = result;
            }
        } else if (input == "stop") {
            controller.stopPlayback();
        } else if (input == "pause") {
            controller.togglePause();
        } else if (input == "update") {
            bool expected = false;
            if (!dependency_update_active.compare_exchange_strong(expected, true)) {
                command_status = "yt-dlp update is already running";
            } else if (dependency_update_worker.joinable()) {
                dependency_update_worker.join();
            }
            if (!expected) {
                command_status = "Updating bundled yt-dlp...";
                dependency_update_worker = std::jthread([&](std::stop_token stop_token) {
                    std::string result;
                    const auto yt_dlp = ProcessRunner::findExecutable("yt-dlp");
                    int exit_code = yt_dlp
                        ? ProcessRunner::runWithCombinedOutput({*yt_dlp, "-U"}, &result)
                        : -1;
                    if (exit_code == 0) {
#if defined(__APPLE__)
                        // The release bundle is ad-hoc signed. yt-dlp replaces
                        // its executable during -U, so renew that local bundle
                        // signature before the next launch.
                        const fs::path macos_dir = ProcessRunner::executableDirectory();
                        const fs::path app_dir = macos_dir.parent_path().parent_path();
                        if (app_dir.extension() == ".app") {
                            std::string signing_output;
                            const int signing_code = ProcessRunner::runWithCombinedOutput({
                                "/usr/bin/codesign", "--force", "--deep", "--sign", "-",
                                app_dir.string(),
                            }, &signing_output);
                            if (signing_code != 0) {
                                exit_code = signing_code;
                                result += "\nUpdated yt-dlp, but could not renew tmplay signature: " +
                                    ProcessRunner::trim(std::move(signing_output));
                            }
                        }
#endif
                    }
                    if (stop_token.stop_requested()) {
                        dependency_update_active = false;
                        return;
                    }
                    screen.Post([&, exit_code, result = ProcessRunner::trim(std::move(result))] {
                        dependency_update_active = false;
                        command_status = exit_code == 0
                            ? (result.empty() ? "yt-dlp is up to date" : result)
                            : (result.empty() ? "yt-dlp update failed" : "yt-dlp update: " + result);
                        screen.PostEvent(Event::Custom);
                    });
                });
            }
        } else if (input.starts_with("playlist ")) {
            std::string result;
            std::string playlist_path = trimInput(input.substr(9));
            if (controller.openPlaylistFile(playlist_path, result)) {
                expanded_directory = controller.currentPath();
                preferred_directory = expanded_directory;
                state.selectedTrack = 0;
                command_status = result;
            } else {
                command_status = "Playlist import error: " + result;
            }
        } else if (input.starts_with("search ")) {
            std::string query = trimInput(input.substr(7));
            if (query.empty()) {
                command_status = "Local search: enter artist, title, or file name";
                return;
            }
            bool expected = false;
            if (!search_active.compare_exchange_strong(expected, true)) {
                command_status = "Search is already running";
            } else {
                if (search_worker.joinable()) search_worker.join();
                command_status = "Searching local files: " + query;
                search_worker = std::jthread(
                    [&, query = std::move(query)](std::stop_token stop_token) {
                        std::string result;
                        const bool succeeded = controller.searchLocalMusic(query, result);
                        if (stop_token.stop_requested()) {
                            search_active = false;
                            return;
                        }
                        screen.Post([&, succeeded, result = std::move(result)]() mutable {
                            search_active = false;
                            if (succeeded) {
                                expanded_directory = controller.currentPath();
                                preferred_directory = expanded_directory;
                                state.selectedTrack = 0;
                                state.focus = FocusPane::Tracks;
                                browser_selector = 1;
                                root_selector = 0;
                                bottom_selector = 0;
                                command_status = std::move(result);
                            } else {
                                command_status = "Local search error: " + result;
                            }
                            screen.PostEvent(Event::Custom);
                        });
                    });
            }
        } else if (input.starts_with("track ") || input.starts_with("album ")) {
            const bool album_search = input.starts_with("album ");
            std::string parse_error;
            auto parsed = parseSearchCommand(
                trimInput(input.substr(6)), parse_error);
            if (!parsed) {
                command_status = "Search error: " + parse_error;
                return;
            }
            std::string query = std::move(parsed->query);
            OnlineSearchOptions search_options = parsed->options;
            bool expected = false;
            if (!search_active.compare_exchange_strong(expected, true)) {
                command_status = "Search is already running";
            } else {
                if (search_worker.joinable()) {
                    search_worker.join();
                }
                command_status = "Searching " + std::string(album_search ? "albums" : "music") +
                    ": " + query;
                // Leave the input as soon as the request starts so the
                // results pane is ready to receive arrows/Enter on arrival.
                state.focus = FocusPane::Tracks;
                browser_selector = 1;
                root_selector = 0;
                bottom_selector = 0;
                search_worker = std::jthread(
                    [&, query = std::move(query), album_search, search_options](std::stop_token stop_token) {
                        std::string result;
                        const bool succeeded = album_search
                            ? controller.searchMusic(query, true, result, search_options)
                            : controller.searchMusic(query, false, result, search_options);
                        if (stop_token.stop_requested()) {
                            search_active = false;
                            return;
                        }
                        screen.Post([&, succeeded, result = std::move(result)]() mutable {
                            search_active = false;
                            if (succeeded) {
                                expanded_directory = controller.currentPath();
                                preferred_directory = expanded_directory;
                                state.selectedTrack = 0;
                                state.focus = FocusPane::Tracks;
                                browser_selector = 1;
                                root_selector = 0;
                                bottom_selector = 0;
                                command_status = std::move(result);
                            } else {
                                command_status = "Search error: " + result;
                            }
                            screen.PostEvent(Event::Custom);
                        });
                    });
            }
        } else if (input == "find") {
            startAudDFind();
        } else if (input == "record" || input == "record start" || input == "record stop") {
            const auto recording = controller.recordingSnapshot();
            if (input == "record stop" && recording.state == RecordingState::Recording) {
                controller.stopDesktopRecording();
                command_status = controller.recordingSnapshot().message;
            } else if (input == "record start" && recording.state != RecordingState::Recording) {
                std::string result;
                if (controller.startDesktopRecording(result)) {
                    show_recording_panel = true;
                    command_status = "Desktop recording started";
                } else {
                    command_status = "Recording error: " + result;
                }
            } else if (input == "record") {
                toggleDesktopRecording();
            } else {
                command_status = recording.state == RecordingState::Recording
                    ? "Desktop recording is already running"
                    : "Desktop recording is not running";
            }
        } else if (!download_source.empty()) {
            std::string destination = controller.currentPath();
            if (isPlaylistSource(download_source)) {
                if (destination.starts_with("search://") &&
                    !controller.config().rootFolder.empty()) {
                    destination = (fs::path(controller.config().rootFolder) /
                                   "Download" / "Albums").string();
                }
                pending_playlist_source = download_source;
                pending_playlist_destination = destination;
                pending_confirmation = PendingConfirmation::PlaylistDownload;
                playlist_action_col = 0;
                command_status = "Playlist detected";
            } else {
                beginDownload(download_source, destination);
            }
        } else if (input.starts_with("mkdir ")) {
            std::string error;
            std::string name = trimInput(input.substr(6));
            command_status = controller.createFolder(name, error)
                ? "Folder created: " + name
                : "Error: " + error;
        } else if (input.starts_with("rename ")) {
            std::string error;
            std::string name = trimInput(input.substr(7));
            if (state.selectedDirectory < 0 ||
                state.selectedDirectory >= (int)dir_paths.size()) {
                command_status = "Error: Select a folder";
            } else if (controller.renameFolder(dir_paths[state.selectedDirectory], name, error)) {
                command_status = "Folder renamed: " + name;
                expanded_directory = controller.currentPath();
                preferred_directory = controller.currentPath();
            } else {
                command_status = "Error: " + error;
            }
        } else if (input == "refresh") {
            controller.scanDirectory(controller.currentPath(), true);
            command_status = "Library refreshed";
        } else if (!input.empty()) {
            command_status = "Unknown command";
        }
    };
    auto command = Input(input_options);

    auto browser = Container::Horizontal({
        dir_menu,
        track_menu,
    }, &browser_selector);

    auto bottom = Container::Horizontal({
        command,
        speed_slider,
        pitch_lock_checkbox,
    }, &bottom_selector);

    auto root_container = Container::Vertical({
        browser,
        progress_slider,
        bottom,
    }, &root_selector);

    enum class EditorPrepareTarget {
        ManualCues,
        Trim,
    };
    std::function<void(const Track&, EditorPrepareTarget)> prepareEditor;

    auto selectedContextTrackIsOnline = [&] {
        return context_menu_for_track && state.selectedTrack >= 0 &&
            state.selectedTrack < (int)visible_files.size() &&
            (visible_files[(size_t)state.selectedTrack].id.starts_with("https://") ||
             visible_files[(size_t)state.selectedTrack].id.starts_with("http://"));
    };
    auto contextMenuActions = [&] {
        if (!context_menu_for_track) {
            return std::vector<std::string>{
                "Enter", "Open folder", "Rename", "Delete", "Close"};
        }
        if (selectedContextTrackIsOnline()) {
            return std::vector<std::string>{"Play", "Open album", "Download", "Close"};
        }
        if (state.selectedTrack >= 0 &&
            state.selectedTrack < (int)visible_files.size() &&
            (visible_files[(size_t)state.selectedTrack].type == EntryType::Album ||
             visible_files[(size_t)state.selectedTrack].type == EntryType::Navigation)) {
            return std::vector<std::string>{"Open", "Close"};
        }
        return std::vector<std::string>{
            "Metadata", "Analyze", "Convert", "Separate stems", "Trim",
            "Hotcue", "Auto Cue", "Delete", "Open folder", "Close"};
    };

    auto root_renderer = Renderer(root_container, [&] {
        auto playback = controller.playbackSnapshot();
        auto download = controller.downloadSnapshot();
        auto stems = controller.stemSeparationSnapshot();
        auto audio_process = controller.audioProcessSnapshot();
        auto auto_cue = controller.autoCueSnapshot();
        auto recording = controller.recordingSnapshot();
        playing_track_id = controller.playingTrackId();
        int terminal_width = std::max(60, Terminal::Size().dimx);
        int content_width = std::max(40, terminal_width - 2);
        int left_width = state.focus == FocusPane::Directories
            ? std::clamp(directory_preferred_width, 24,
                         std::max(24, content_width - 24))
            : std::clamp(content_width / 5, 16, std::max(30, content_width / 3));
        int right_width = std::max(24, content_width - left_width - 1);
        show_time_column = controller.config().columns.time;
        show_bpm_column = controller.config().columns.bpm;
        show_key_column = controller.config().columns.key;
        show_kbps_column = controller.config().columns.kbps;
        show_rate_column = controller.config().columns.rate;
        show_size_column = controller.config().columns.size;
        show_frmt_column = controller.config().columns.frmt;
        show_genre_column = controller.config().columns.genre;
        const auto browser_tracks = controller.trackStore().getTracks();
        const bool album_search_results = !browser_tracks.empty() &&
            std::all_of(browser_tracks.begin(), browser_tracks.end(),
                        [](const Track& track) { return track.type == EntryType::Album; });
        visible_column_order = album_search_results
            ? std::vector<std::string>{"time"}
            : fitTrackColumns(right_width);
        const std::string columns_path = controller.currentPath();
        const bool search_history_view = columns_path == "search://root";
        const bool online_track_view =
            columns_path.rfind("search://", 0) == 0 &&
            columns_path != "search://root";
        if (search_history_view) {
            // A command history has no media metadata. Give it the entire
            // pane so previous queries remain readable.
            visible_column_order.clear();
        }
        if (online_track_view && !album_search_results) {
            // Search data is streamed metadata: BPM/key/genre are analysed
            // after download, so they do not belong in the online table.
            std::erase_if(visible_column_order, [](const std::string& column) {
                return column == "bpm" || column == "key" || column == "genre";
            });
        }
        auto expandedDownloadRows = [&] {
            int terminal_height = std::max(16, Terminal::Size().dimy);
            return std::clamp((terminal_height * 3) / 5,
                              8,
                              std::max(8, terminal_height - 12));
        };
        int download_auxiliary_rows =
            show_activity && show_download_panel && download.state != DownloadState::Idle
                ? (show_download_pool
                    ? expandedDownloadRows()
                    : 3)
                : 0;
        int auxiliary_rows = (show_info ? 2 : 0) +
            download_auxiliary_rows +
            (show_activity && show_stems_panel && stems.state != StemSeparationState::Idle ? 3 : 0) +
            (show_activity && show_audio_process_panel && audio_process.state != AudioProcessState::Idle ? 3 : 0) +
            (show_activity && show_auto_cue_panel && (auto_cue.running || auto_cue.done) ? 3 : 0) +
            (show_activity && show_recording_panel &&
             recording.state != RecordingState::Idle ? 3 : 0);
        visible_row_count = std::max(1, Terminal::Size().dimy - 11 - auxiliary_rows);

        syncBrowserData(left_width, right_width, visible_row_count);

        double duration = std::max(0.0, playback.durationSeconds);
        double position = std::clamp(playback.positionSeconds, 0.0, duration);
        if (!progress_dragging || playback.state == PlaybackState::Playing) {
            progress_value = duration > 0.0
                ? (int)std::clamp((position / duration) * progress_max, 0.0, (double)progress_max)
                : 0;
            progress_dragging = false;
        }

        std::string now_playing = playback.title.empty() ? "tmplay" : playback.title;
        std::string now_playing_tags;
        Track playing_track_details = controller.playingTrack();
        if (!playing_track_id.empty()) {
            auto playing = std::find_if(visible_files.begin(), visible_files.end(),
                                        [&](const Track& track) {
                                            return track.id == playing_track_id;
                                        });
            if (playing != visible_files.end()) {
                playing_track_details = *playing;
            }
            if (!playing_track_details.id.empty()) {
                const std::string track_title = playing_track_details.title.empty()
                    ? now_playing : playing_track_details.title;
                if (!playing_track_details.artist.empty()) {
                    now_playing = playing_track_details.artist + " - " + track_title;
                } else {
                    now_playing = track_title;
                }
                std::vector<std::string> tags;
                if (playing_track_details.bpm > 0.0) {
                    tags.push_back(std::to_string((int)playing_track_details.bpm));
                }
                if (!playing_track_details.key.empty()) {
                    tags.push_back(playing_track_details.key);
                }
                if (!tags.empty()) {
                    now_playing_tags = "  ";
                    for (size_t i = 0; i < tags.size(); ++i) {
                        if (i > 0) {
                            now_playing_tags += " | ";
                        }
                        now_playing_tags += tags[i];
                    }
                }
            }
        }
        now_playing_copy_value = now_playing + now_playing_tags;
        int title_total_width = std::max(16, content_width - 24);
        int title_text_width = std::max(1, title_total_width - (int)now_playing_tags.size());
        std::string title = truncateEnd(now_playing, title_text_width) + now_playing_tags;
        bool editor_loading = editor_prepare_active.load();
        bool searching = search_active.load();
        size_t spinner_frame = (size_t)(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count() /
            120);
        std::string playback_status = playback.state == PlaybackState::Error
            ? "error: " + truncateEnd(playback.errorMessage, 28)
            : playbackStateToString(playback.state);
        std::string status_line = command_status;
        if (status_line.empty() && download.state != DownloadState::Idle) {
            status_line = download.message;
        }
        if (status_line.empty() && stems.state != StemSeparationState::Idle) {
            status_line = stems.message;
        }
        if (status_line.empty() && audio_process.state != AudioProcessState::Idle) {
            status_line = audio_process.message;
        }
        if (status_line.empty() && (auto_cue.running || auto_cue.done)) {
            status_line = auto_cue.status;
        }
        if (status_line.empty() && recording.state != RecordingState::Idle) {
            status_line = recording.message;
        }
        if (status_line.empty() && controller.directoryScanBusy()) {
            status_line = "Scanning: " + displayName(controller.currentPath());
        }

        Element directory_list = visible_dir_entries.empty()
            ? text("(empty)") | dim
            : dir_menu->Render();
        directory_list = directory_list | flex | reflect(directory_list_box);

        std::string panel_directory;
        if (move_to_mode &&
            state.selectedDirectory >= 0 &&
            state.selectedDirectory < (int)dir_paths.size()) {
            panel_directory = dir_paths[state.selectedDirectory];
        } else if (!expanded_directory.empty()) {
            panel_directory = controller.currentPath();
        }
        const std::string virtual_folder_name =
            controller.virtualPlaylistName(panel_directory);
        std::string folder_name = panel_directory.empty()
            ? ""
            : (virtual_folder_name.empty() ? displayName(panel_directory)
                                           : virtual_folder_name);
        if (!move_to_mode && panel_directory == controller.config().rootFolder) {
            folder_name.clear();
        }
        std::string panel_prefix = move_to_mode ? "Move to " : "Folder ";
        std::string panel_title = truncateEnd(
            folder_name.empty() && !move_to_mode ? "Folders" : panel_prefix + folder_name,
            std::max(6, left_width - 1));

        Element left_panel = vbox({
            text(panel_title) | bold,
            separator(),
            directory_list,
        }) | size(WIDTH, EQUAL, left_width);

        if (state.focus == FocusPane::Directories) {
            left_panel = left_panel | color(Color::Yellow) | flex_shrink;
        } else {
            left_panel = left_panel | flex_shrink;
        }

        Element track_list = visible_track_entries.empty()
            ? text("(empty)") | dim
            : track_menu->Render();
        track_list = track_list | flex | reflect(track_list_box);

        const std::string active_path = controller.currentPath();
        const bool album_folder = active_path.starts_with("search://") &&
            active_path.find("/albums/") != std::string::npos;
        // A pasted playlist is displayed directly in search://recent rather
        // than beneath an /album/ path. Treat it as one collection as well:
        // its source order must stay intact and it needs Download all.
        const bool direct_online_playlist = active_path == "search://recent" &&
            !visible_files.empty() &&
            std::all_of(visible_files.begin(), visible_files.end(),
                        [](const Track& track) {
                            return track.id.starts_with("https://") ||
                                   track.id.starts_with("http://");
                        });
        const bool online_collection = album_folder || direct_online_playlist;
        const std::string local_album_prefix =
            (fs::path(controller.config().rootFolder) / "Download" / "Albums").string() + "/";
        const bool local_album_folder = active_path.starts_with(local_album_prefix);
        const bool preserve_album_order = online_collection ||
            (local_album_folder && !track_sort_requested) ||
            active_path == "search://root";
        auto sortLabel = [&](const std::string& label, TrackSortColumn column) {
            if (preserve_album_order) {
                return label;
            }
            if (column != track_sort_column) {
                return label;
            }
            return label + (track_sort_ascending ? "^" : "v");
        };
        auto headerCell = [&](const std::string& label,
                              TrackSortColumn column,
                              Box& box) {
            Element cell = text(sortLabel(label, column)) | reflect(box);
            if (!preserve_album_order && column == track_sort_column) {
                cell = cell | bold | underlined;
            }
            return cell;
        };
        album_download_box = Box{};
        Element title_header = headerCell(
            std::string(active_path == "search://root" ? "Last Search" :
                        (album_search_results ? "Albums" : "Title")) +
                " (" + std::to_string(track_entries.size()) + ")",
            TrackSortColumn::Title, title_header_box);
        if (online_collection) {
            const bool album_downloaded =
                controller.isOnlineAlbumDownloaded(visible_files);
            Elements collection_actions;
            collection_actions.insert(collection_actions.end(), {
                text(album_downloaded ? "✓" : "↓") |
                    color(album_downloaded ? Color::Green : Color::Cyan) | bold |
                    reflect(album_download_box),
                separator(),
                title_header | flex,
            });
            title_header = hbox(std::move(collection_actions)) | flex;
        } else {
            title_header = title_header | flex;
        }
        Elements header_columns = {
            text("") | size(WIDTH, EQUAL, 5),
            title_header,
        };
        time_header_box = Box{};
        bpm_header_box = Box{};
        key_header_box = Box{};
        genre_header_box = Box{};
        bitrate_header_box = Box{};
        rate_header_box = Box{};
        format_header_box = Box{};
        size_header_box = Box{};
        auto columnHeaderBox = [&](const std::string& column) -> Box& {
            if (column == "time") return time_header_box;
            if (column == "bpm") return bpm_header_box;
            if (column == "key") return key_header_box;
            if (column == "genre") return genre_header_box;
            if (column == "kbps") return bitrate_header_box;
            if (column == "rate") return rate_header_box;
            if (column == "frmt") return format_header_box;
            return size_header_box;
        };
        for (const auto& column : visible_column_order) {
            int width = trackColumnWidth(column);
            if (width <= 0) {
                continue;
            }
            header_columns.push_back(separator());
            header_columns.push_back(
                headerCell(album_search_results && column == "time" ? "Year"
                                                                  : trackColumnLabel(column),
                           trackColumnSort(column),
                           columnHeaderBox(column)) |
                size(WIDTH, EQUAL, width));
        }
        Element track_header = hbox(header_columns);
        Element right_panel = vbox({
            track_header,
            separator(),
            track_list,
        });

        if (state.focus == FocusPane::Tracks) {
            right_panel = right_panel | color(Color::Yellow) | flex;
        } else {
            right_panel = right_panel | flex;
        }

        auto controlCell = [&](const std::string& label,
                               int width,
                               Box& box,
                               HoverControl control) {
            Element cell = text(label) | center | size(WIDTH, EQUAL, width) | reflect(box);
            if (hovered_control == control) {
                cell = cell | bold | color(Color::Yellow);
            }
            return cell;
        };

        Element top_progress_bar = hbox({
            progress_slider->Render() | flex,
        });

        speed_reset_box = Box{};
        Element speed_reset = controlCell("x", 3, speed_reset_box,
                                          HoverControl::SpeedReset);
        Element speed_control = speed_slider->Render() | flex;
        Element pitch_lock = pitch_lock_checkbox->Render() | size(WIDTH, EQUAL, 4);
        if (root_selector == 2 && bottom_selector == 1 && speed_focus == 0) {
            speed_reset = speed_reset | inverted;
        }
        if (root_selector == 2 && bottom_selector == 1 && speed_focus == 1) {
            speed_control = speed_control | inverted;
        }
        if (root_selector == 2 && bottom_selector == 1 && speed_focus == 2) {
            pitch_lock = pitch_lock | inverted;
        }
        Element speed_box = hbox({
            speed_reset,
            speed_control,
            text(std::format("{:.2f}x", (double)speed_value / 100.0)) |
                size(WIDTH, EQUAL, 6) | center,
            pitch_lock,
        }) | size(HEIGHT, EQUAL, 1);

        Element control_bar = hbox({
            controlCell("-", 3, volume_down_box, HoverControl::VolumeDown),
            text("🔈 " + std::to_string(controller.volume()) + "%") |
                size(WIDTH, EQUAL, 7) | center,
            controlCell("+", 3, volume_up_box, HoverControl::VolumeUp),
            separator(),
            controlCell("⏮", 4, previous_box, HoverControl::Previous),
            controlCell(playback_status, 3, play_pause_box, HoverControl::PlayPause),
            controlCell("⏭", 3, next_box, HoverControl::Next),
            controlCell(playbackModeToString(controller.playbackMode()), 4,
                        mode_box, HoverControl::Mode),
            separator(),
            speed_box | flex,
            separator(),
            text(formatPlaybackTime(position) + " / " + formatPlaybackTime(duration)) | size(WIDTH, EQUAL, 13),
        });

        command_paste_box = Box{};
        command_clear_box = Box{};
        command_record_box = Box{};
        command_find_box = Box{};
        const bool command_focused = root_selector == 2 && bottom_selector == 0;
        const int command_width = command_focused
            ? std::max(30, (content_width * 2) / 3)
            : std::max(18, content_width / 3);
        Elements command_items = {
            text(recording.state == RecordingState::Recording && !audd_find_active.load()
                    ? "STOP REC" : "REC") |
                center | size(WIDTH, EQUAL, 8) | reflect(command_record_box) |
                (recording.state == RecordingState::Recording && !audd_find_active.load()
                    ? color(Color::Red) | bold
                    : color(Color::Yellow)),
        };
        if (controller.auddFindEnabled()) {
            command_items.insert(command_items.end(), {
                separator(),
                text(audd_find_active.load() ? "CANCEL" : "FIND") |
                    center | size(WIDTH, EQUAL, 8) | reflect(command_find_box) |
                    (audd_find_active.load() ? color(Color::Red) | bold
                                             : color(Color::Cyan)),
            });
        }
        command_items.insert(command_items.end(), {
            separator(),
            text("v") | center | size(WIDTH, EQUAL, 3) |
                reflect(command_paste_box) |
                color(Color::Yellow),
            separator(),
            command->Render() | flex,
            separator(),
            text("x") | center | size(WIDTH, EQUAL, 3) |
                reflect(command_clear_box) |
                (command_input.empty() ? dim : color(Color::Yellow)),
        });
        Element command_box = hbox(std::move(command_items)) |
            size(WIDTH, EQUAL, command_width) |
            size(HEIGHT, EQUAL, 1);

        Element bottom_bar = hbox({
            command_box,

            separator(),

            text(truncateEnd(status_line, std::max(10, content_width / 2))) |
                dim | flex,

            filler(),
        });

        Elements layout = {
            (hbox({
                filler(),
                text(" " + title + " ") | bold,
                editor_loading || searching
                    ? hbox({text(" "), spinner(12, spinner_frame) |
                                          color(Color::Yellow)})
                    : text("  "),
                filler(),
            }) | reflect(now_playing_box)),
            top_progress_bar,
            separator(),
            hbox({
                left_panel,
                separator(),
                right_panel,
            }) | flex,
            separator(),
            control_bar,
            separator(),
            bottom_bar,
        };

        if (show_info) {
            layout.push_back(separator());
            layout.push_back(text("track <text> [-o 1..500] [-m min] [-t max] | album <text> | search <artist> | now | playlist <file> | record | S stem | X auto | Z manual | A trim | H help | Q quit") | dim);
        }

        download_close_box = Box{};
        download_stop_box = Box{};
        download_pool_box = Box{};
        stems_close_box = Box{};
        audio_process_close_box = Box{};
        auto_cue_close_box = Box{};
        auto activityControl = [&](bool running, float progress, Box& close_box) {
            if (running) {
                return hbox({
                    gauge(progress) |
                        color(Color::Yellow) |
                        size(WIDTH, EQUAL, 20),
                    separator(),
                    text(std::to_string((int)(progress * 100.0f)) + "%") | dim,
                });
            }
            return hbox({
                text("x") |
                    color(Color::Yellow) |
                    bold |
                    center |
                    size(WIDTH, EQUAL, 3) |
                    reflect(close_box),
            });
        };

        if (show_activity && show_download_panel &&
            download.state != DownloadState::Idle) {
            layout.push_back(separator());
            if (show_download_pool) {
                int panel_rows = expandedDownloadRows();
                int list_rows = std::max(3, panel_rows - 4);
                int total_items = (int)download.items.size();
                download_pool_scroll = std::clamp(
                    download_pool_scroll,
                    0,
                    std::max(0, total_items - list_rows));

                Elements rows;
                if (download.items.empty()) {
                    rows.push_back(text("Preparing playlist...") | dim);
                } else {
                    int end = std::min(total_items, download_pool_scroll + list_rows);
                    for (int i = download_pool_scroll; i < end; ++i) {
                        const auto& item = download.items[(size_t)i];
                        std::string status = item.status.empty() ? "queued" : item.status;
                        std::string title = item.title.empty()
                            ? "Item " + std::to_string(item.index)
                            : item.title;
                        std::string line = std::format(
                            "{:>2}. {:<9} {:>3}%  {}",
                            item.index,
                            truncateEnd(status, 9),
                            (int)std::round(item.progress * 100.0f),
                            title);
                        Element row = text(truncateEnd(line, content_width - 2));
                        if (status == "running" || status == "analyzing" || status == "tagging") {
                            row = row | color(Color::Yellow);
                        } else if (status == "done") {
                            row = row | color(Color::Green);
                        } else if (status == "skipped") {
                            row = row | dim;
                        } else if (status == "error") {
                            row = row | color(Color::Red);
                        }
                        rows.push_back(row);
                    }
                }

                auto buttonText = [&](const std::string& label, Box& box, bool enabled) {
                    Element button = text(label) |
                        center |
                        size(WIDTH, EQUAL, (int)label.size() + 2) |
                        reflect(box);
                    return enabled ? (button | color(Color::Yellow) | bold) : (button | dim);
                };

                layout.push_back(hbox({
                    text("Download Pool: " + download.message) | color(Color::Yellow),
                    filler(),
                    download.state == DownloadState::Running
                        ? buttonText("stop", download_stop_box, true)
                        : text(""),
                    separator(),
                    buttonText("x", download_close_box, true),
                }));
                layout.push_back(
                    vbox(rows) |
                    size(HEIGHT, EQUAL, list_rows) |
                    reflect(download_pool_box));
                std::string footer = download.detail;
                if (total_items > list_rows) {
                    footer += " | " + std::to_string(download_pool_scroll + 1) +
                        "-" + std::to_string(std::min(total_items, download_pool_scroll + list_rows)) +
                        "/" + std::to_string(total_items);
                }
                layout.push_back(text(truncateEnd(footer, content_width)) | dim);
            } else {
                layout.push_back(hbox({
                    text("Download: " + download.message) | color(Color::Yellow),
                    filler(),
                    activityControl(download.state == DownloadState::Running,
                                    download.progress,
                                    download_close_box),
                }));
                layout.push_back(text(truncateEnd(download.detail, content_width)) | dim);
            }
        }

        if (show_activity && show_stems_panel &&
            stems.state != StemSeparationState::Idle) {
            layout.push_back(separator());
            layout.push_back(hbox({
                text("Demucs: " + stems.message) | color(Color::Yellow),
                filler(),
                activityControl(stems.state == StemSeparationState::Running,
                                stems.progress,
                                stems_close_box),
            }));
            std::string detail;
            if (stems.elapsedSeconds > 0.0) {
                detail = "time " + formatPlaybackTime(stems.elapsedSeconds);
            }
            if (!stems.detail.empty()) {
                if (!detail.empty()) {
                    detail += " | ";
                }
                detail += stems.detail;
            }
            if (!stems.outputDirectory.empty()) {
                detail += " | Output: " + stems.outputDirectory;
            }
            layout.push_back(text(truncateEnd(detail, content_width)) | dim);
        }

        if (show_activity && show_audio_process_panel &&
            audio_process.state != AudioProcessState::Idle) {
            layout.push_back(separator());
            std::string label = audio_process.kind == AudioProcessKind::Normalize
                ? "Normalization: "
                : "Convert: ";
            layout.push_back(hbox({
                text(label + audio_process.message) | color(Color::Yellow),
                filler(),
                activityControl(audio_process.state == AudioProcessState::Running,
                                audio_process.progress,
                                audio_process_close_box),
            }));
            std::string detail = audio_process.detail;
            if (!audio_process.outputDirectory.empty()) {
                detail += " | Output: " + audio_process.outputDirectory;
            }
            layout.push_back(text(truncateEnd(detail, content_width)) | dim);
        }

        if (show_activity && show_auto_cue_panel &&
            (auto_cue.running || auto_cue.done)) {
            layout.push_back(separator());
            float cue_progress = auto_cue.total > 0
                ? std::clamp((float)auto_cue.current / (float)auto_cue.total, 0.0f, 1.0f)
                : (auto_cue.done ? 1.0f : 0.0f);
            layout.push_back(hbox({
                text("Auto Cue: " + auto_cue.status) | color(Color::Yellow),
                filler(),
                auto_cue.running
                    ? hbox({
                        gauge(cue_progress) |
                            color(Color::Yellow) |
                            size(WIDTH, EQUAL, 20),
                        separator(),
                        text(std::to_string(auto_cue.current) + "/" +
                             std::to_string(auto_cue.total)) | dim,
                    })
                    : hbox({
                        text("x") |
                            color(Color::Yellow) |
                            bold |
                            center |
                            size(WIDTH, EQUAL, 3) |
                            reflect(auto_cue_close_box),
                    }),
            }));
            std::string detail = auto_cue.currentFile;
            if (!detail.empty()) {
                detail += " | ";
            }
            detail += "ok " + std::to_string(auto_cue.success) +
                " | errors " + std::to_string(auto_cue.errors);
            layout.push_back(text(truncateEnd(detail, content_width)) | dim);
        }

        if (show_activity && show_recording_panel &&
            recording.state != RecordingState::Idle) {
            layout.push_back(separator());
            const bool active = recording.state == RecordingState::Recording;
            layout.push_back(hbox({
                text("Desktop recording: " + recording.message) |
                    (active ? color(Color::Red) : color(Color::Yellow)),
                filler(),
                text(active ? "record" : "error") | dim,
            }));
            std::string detail = recording.filePath;
            if (active) {
                detail = "time " + formatPlaybackTime(recording.elapsedSeconds) +
                    " | " + detail;
            }
            layout.push_back(text(truncateEnd(detail, content_width)) | dim);
        }

        Element main_layout = vbox(layout) | border | flex;
        if (show_metadata_popup) {
            metadata_close_box = Box{};
            metadata_list_box = Box{};
            int dialog_width = std::clamp(content_width - 4, 44, 92);
            int max_rows = std::max(5, Terminal::Size().dimy - 12);
            int name_width = std::min(20, std::max(8, dialog_width / 4));
            int value_width = std::max(10, dialog_width - name_width - 8);
            std::vector<std::pair<std::string, std::string>> metadata_rows;
            for (const auto& [name, value] : metadata_details) {
                std::vector<std::string> wrapped = wrapText(value, value_width);
                if (wrapped.empty()) {
                    wrapped.emplace_back("");
                }
                for (size_t i = 0; i < wrapped.size(); ++i) {
                    metadata_rows.emplace_back(i == 0 ? name : "", wrapped[i]);
                }
            }

            int total_rows = (int)metadata_rows.size();
            metadata_offset = std::clamp(metadata_offset, 0,
                                         std::max(0, total_rows - max_rows));

            Elements rows;
            metadata_link_boxes.clear();
            metadata_link_urls.clear();
            metadata_link_boxes.reserve(max_rows);
            metadata_link_urls.reserve(max_rows);
            if (metadata_rows.empty()) {
                rows.push_back(text("No metadata") | dim | center);
            } else {
                int end = std::min(total_rows, metadata_offset + max_rows);
                for (int i = metadata_offset; i < end; ++i) {
                    const auto& [name, value] = metadata_rows[i];
                    Element value_element = text(value) |
                        dim |
                        size(WIDTH, EQUAL, value_width);
                    if (auto url = firstUrlInText(value)) {
                        metadata_link_boxes.emplace_back();
                        metadata_link_urls.push_back(*url);
                        value_element = text(value) |
                            color(Color::Cyan) |
                            underlined |
                            size(WIDTH, EQUAL, value_width) |
                            reflect(metadata_link_boxes.back());
                    }
                    rows.push_back(hbox({
                        text(truncateEnd(name, name_width)) |
                            bold |
                            size(WIDTH, EQUAL, name_width),
                        text(" "),
                        value_element,
                    }));
                }
            }

            std::string counter = total_rows == 0
                ? "0/0"
                : std::to_string(metadata_offset + 1) + "-" +
                    std::to_string(std::min(total_rows, metadata_offset + max_rows)) +
                    "/" + std::to_string(total_rows);
            Element close_button = text("Close") | center |
                size(WIDTH, EQUAL, 12) |
                border |
                bold |
                color(Color::Yellow) |
                reflect(metadata_close_box);
            Element dialog = vbox({
                text("meta data") | bold | center,
                text(truncateEnd(metadata_title, dialog_width - 4)) | dim | center,
                separator(),
                vbox(rows) | reflect(metadata_list_box),
                separator(),
                hbox({
                    text(counter) | dim,
                    filler(),
                    close_button,
                    filler(),
                    text("Esc/Enter") | dim,
                }),
            }) |
                size(WIDTH, EQUAL, dialog_width) |
                border |
                clear_under |
                center;

            return dbox({main_layout, dialog}) | flex;
        }

        if (show_eq_popup) {
            for (auto& box : eq_reset_boxes) {
                box = Box{};
            }
            eq_close_box = Box{};
            auto gainLabel = [](int value) {
                if (value <= -60) {
                    return std::string("-∞");
                }
                return value > 0 ? "+" + std::to_string(value) : std::to_string(value);
            };
            auto eqRow = [&](const std::string& label,
                             Component slider,
                             int value,
                             int index) {
                int slider_focus = index * 2;
                int reset_focus = index * 2 + 1;
                Element slider_element = slider->Render() | flex;
                Element reset_element = text("x") | center | size(WIDTH, EQUAL, 3) |
                    reflect(eq_reset_boxes[(size_t)index]);
                if (eq_selected == slider_focus) {
                    slider_element = slider_element | inverted;
                }
                if (eq_selected == reset_focus) {
                    reset_element = reset_element | inverted;
                }
                Element row = hbox({
                    text(label) | size(WIDTH, EQUAL, 4),
                    slider_element,
                    text(gainLabel(value)) | center | size(WIDTH, EQUAL, 4),
                    reset_element,
                });
                return row;
            };
            Element close_button = text("Close") | center |
                size(WIDTH, EQUAL, 12) |
                border |
                bold |
                color(Color::Yellow) |
                reflect(eq_close_box);
            if (eq_selected == 6) {
                close_button = close_button | inverted;
            }
            Element dialog = vbox({
                text("equalizer") | bold | center,
                separator(),
                eqRow("HI", eq_high_slider, eq_high, 0),
                eqRow("MID", eq_mid_slider, eq_mid, 1),
                eqRow("LOW", eq_low_slider, eq_low, 2),
                separator(),
                close_button | center,
            }) |
                size(WIDTH, EQUAL, std::clamp(content_width - 8, 44, 80)) |
                border |
                clear_under |
                center;
            return dbox({main_layout, dialog}) | flex;
        }

        if (manual_cue_editor.isOpen()) {
            return manual_cue_editor.renderOverlay(main_layout, content_width);
        }
        if (trim_editor.isOpen()) {
            return trim_editor.renderOverlay(main_layout, content_width);
        }

        if (show_context_menu) {
            const std::vector<std::string> actions = contextMenuActions();
            context_menu_selected = std::clamp(context_menu_selected, 0,
                                               (int)actions.size() - 1);
            context_menu_boxes.assign(actions.size(), Box{});
            Elements rows;
            rows.push_back(text(context_menu_for_track ? "Track actions" : "Folder actions") |
                           bold | center);
            rows.push_back(separator());
            const bool compact_offline_track_menu =
                context_menu_for_track && !selectedContextTrackIsOnline();
            if (compact_offline_track_menu) {
                constexpr int columns = 2;
                for (size_t start = 0; start < actions.size(); start += columns) {
                    Elements buttons;
                    for (int column = 0; column < columns; ++column) {
                        const size_t index = start + (size_t)column;
                        if (index >= actions.size()) {
                            buttons.push_back(filler());
                            continue;
                        }
                        Element button = text(actions[index]) | center |
                            size(WIDTH, EQUAL, 20) | border |
                            reflect(context_menu_boxes[index]);
                        if ((int)index == context_menu_selected) button = button | inverted;
                        buttons.push_back(button);
                    }
                    rows.push_back(hbox(buttons));
                }
            } else {
                for (size_t index = 0; index < actions.size(); ++index) {
                    Element row = text(actions[index]) | center |
                        size(WIDTH, EQUAL, 22) | border |
                        reflect(context_menu_boxes[index]);
                    if ((int)index == context_menu_selected) row = row | inverted;
                    rows.push_back(row);
                }
            }
            Element dialog = vbox(rows) |
                size(WIDTH, EQUAL, compact_offline_track_menu ? 44 : 28) |
                border | clear_under | center;
            return dbox({main_layout, dialog}) | flex;
        }

        if (pending_confirmation == PendingConfirmation::None) {
            return main_layout;
        }

        confirm_yes_box = Box{};
        confirm_no_box = Box{};
        std::string question;
        std::string detail;
        if (pending_confirmation == PendingConfirmation::PlaylistDownload) {
            question = "Playlist detected";
            detail = truncateEnd(pending_playlist_source, std::max(20, content_width - 18));
        } else if (pending_confirmation == PendingConfirmation::StemSeparation) {
            question = "Separate track with Demucs?";
            detail = truncateEnd(pending_stem_track.title, std::max(20, content_width - 18));
        } else if (pending_confirmation == PendingConfirmation::Normalization) {
            question = "Normalization";
            detail = pending_process_all_folder
                ? "Folder: " + displayName(controller.currentPath())
                : truncateEnd(pending_process_track.title, std::max(20, content_width - 18));
        } else if (pending_confirmation == PendingConfirmation::Convert) {
            question = "Convert";
            detail = pending_process_all_folder
                ? "Folder: " + displayName(controller.currentPath())
                : truncateEnd(pending_process_track.title, std::max(20, content_width - 18));
        } else if (pending_confirmation == PendingConfirmation::Analysis) {
            question = "Analyze";
            detail = pending_analysis_track.id.empty()
                ? "Folder: " + displayName(controller.currentPath())
                : truncateEnd(pending_analysis_track.title, std::max(20, content_width - 18));
        } else if (pending_confirmation == PendingConfirmation::AutoCue) {
            question = "Auto Cue";
            detail = pending_auto_cue_track.id.empty()
                ? "Folder: " + displayName(controller.currentPath())
                : truncateEnd(pending_auto_cue_track.title, std::max(20, content_width - 18));
        } else if (pending_confirmation == PendingConfirmation::CueSync) {
            question = "Cue Sync";
            detail = pending_cue_sync_track.id.empty()
                ? "Folder: " + displayName(controller.currentPath())
                : truncateEnd(pending_cue_sync_track.title,
                              std::max(20, content_width - 18));
        } else if (pending_confirmation == PendingConfirmation::DeleteEntry) {
            question = pending_delete_entry.type == EntryType::Directory
                ? "Delete folder?"
                : "Delete track?";
            detail = truncateEnd(pending_delete_entry.title, std::max(20, content_width - 18));
        } else {
            question = "Quit tmplay?";
            detail = "Playback and running jobs will stop.";
        }

        auto button = [&](const std::string& label, Box& box, int index) {
            Element element = text(label) | center |
                size(WIDTH, EQUAL, 10) |
                border |
                reflect(box);
            if (confirmation_selected == index) {
                element = element | bold | color(Color::Yellow);
            }
            return element;
        };

        auto optionButton = [&](const std::string& label,
                                Box& box,
                                int row,
                                int col,
                                int active_row,
                                int active_col,
                                bool selected) {
            Element element = text(label) | center |
                size(WIDTH, EQUAL, 12) |
                border |
                reflect(box);
            if (selected) {
                element = element | inverted;
            }
            if (active_row == row && active_col == col) {
                element = element | bold | color(Color::Yellow);
            }
            return element;
        };

        auto stemButton = [&](const std::string& label,
                              Box& box,
                              int row,
                              int col,
                              bool selected) {
            Element element = text(label) | center |
                size(WIDTH, EQUAL, 10) |
                border |
                reflect(box);
            if (selected) {
                element = element | inverted;
            }
            if (stem_option_row == row && stem_option_col == col) {
                element = element | bold | color(Color::Yellow);
            }
            return element;
        };

        auto autoCueButton = [&](const std::string& label,
                                 Box& box,
                                 int col,
                                 bool enabled = true) {
            Element element = text(label) | center |
                size(WIDTH, EQUAL, 16) |
                border |
                reflect(box);
            if (!enabled) {
                element = element | dim;
            }
            if (auto_cue_option_col == col) {
                element = element | bold | color(Color::Yellow);
            }
            return element;
        };

        auto analysisButton = [&](const std::string& label, Box& box, int row) {
            Element element = text(label) | center |
                size(WIDTH, EQUAL, 20) |
                border |
                reflect(box);
            if (analysis_option_row == row) {
                element = element | bold | color(Color::Yellow);
            }
            return element;
        };

        Elements dialog_items = {
            text(question) | bold | center,
            separator(),
            text(detail) | dim | center,
            separator(),
        };

        if (pending_confirmation == PendingConfirmation::PlaylistDownload) {
            playlist_show_box = Box{};
            playlist_download_box = Box{};
            playlist_cancel_box = Box{};
            auto playlistButton = [&](const std::string& label, Box& box, int column) {
                Element element = text(label) | center |
                    size(WIDTH, EQUAL, 24) | border | reflect(box);
                if (playlist_action_col == column) {
                    element = element | bold | color(Color::Yellow);
                }
                return element;
            };
            // Keep the choices one below another: long playlist URLs and
            // narrow terminals no longer squeeze the confirmation dialog.
            dialog_items.push_back(vbox({
                playlistButton("Show playlist / album", playlist_show_box, 0),
                playlistButton("Download all", playlist_download_box, 1),
                playlistButton("Cancel", playlist_cancel_box, 2),
            }) | center);
        } else if (pending_confirmation == PendingConfirmation::StemSeparation) {
            stem_2_box = Box{};
            stem_4_box = Box{};
            stem_mp3_box = Box{};
            stem_wav_box = Box{};
            stem_flac_box = Box{};
            dialog_items.push_back(hbox({
                filler(),
                stemButton("2stems", stem_2_box, 0, 0,
                           pending_stem_config.stems == 2),
                text("  "),
                stemButton("4stems", stem_4_box, 0, 1,
                           pending_stem_config.stems == 4),
                filler(),
            }));
            dialog_items.push_back(hbox({
                filler(),
                stemButton("mp3", stem_mp3_box, 1, 0,
                           pending_stem_config.outputFormat == "mp3"),
                text("  "),
                stemButton("wav", stem_wav_box, 1, 1,
                           pending_stem_config.outputFormat == "wav"),
                text("  "),
                stemButton("flac", stem_flac_box, 1, 2,
                           pending_stem_config.outputFormat == "flac"),
                filler(),
            }));
            dialog_items.push_back(separator());
            dialog_items.push_back(hbox({
                filler(),
                stemButton("Yes", confirm_yes_box, 2, 0, false),
                text("  "),
                stemButton("No", confirm_no_box, 2, 1, false),
                filler(),
            }));
        } else if (pending_confirmation == PendingConfirmation::Normalization) {
            normalize_16_box = Box{};
            normalize_14_box = Box{};
            normalize_9_box = Box{};
            normalize_one_box = Box{};
            normalize_all_box = Box{};
            dialog_items.push_back(text("-14 LUFS (Short-Term Max)") | center);
            dialog_items.push_back(text("Best for Club / DJ tracks") | dim | center);
            dialog_items.push_back(text("-16 LUFS (Integrated)") | center);
            dialog_items.push_back(text("Streaming standard (Spotify/Apple)") | dim | center);
            // dialog_items.push_back(text("-9 LUFS") | center);
            dialog_items.push_back(separator());
            dialog_items.push_back(hbox({
                filler(),
                optionButton("-16", normalize_16_box, 0, 0,
                             normalize_option_row, normalize_option_col,
                             pending_normalize_lufs == -16),
                text(" "),
                optionButton("-14", normalize_14_box, 0, 1,
                             normalize_option_row, normalize_option_col,
                             pending_normalize_lufs == -14),
                text(" "),
                // optionButton("-9", normalize_9_box, 0, 2,
                //              normalize_option_row, normalize_option_col,
                //              pending_normalize_lufs == -9),
                filler(),
            }));
            dialog_items.push_back(hbox({
                filler(),
                optionButton("one", normalize_one_box, 1, 0,
                             normalize_option_row, normalize_option_col,
                             !pending_process_all_folder),
                text(" "),
                optionButton("all folder", normalize_all_box, 1, 1,
                             normalize_option_row, normalize_option_col,
                             pending_process_all_folder),
                filler(),
            }));
            dialog_items.push_back(separator());
            dialog_items.push_back(hbox({
                filler(),
                optionButton("Yes", confirm_yes_box, 2, 0,
                             normalize_option_row, normalize_option_col, false),
                text(" "),
                optionButton("No", confirm_no_box, 2, 1,
                             normalize_option_row, normalize_option_col, false),
                filler(),
            }));
        } else if (pending_confirmation == PendingConfirmation::Convert) {
            convert_wav_box = Box{};
            convert_mp3_box = Box{};
            convert_m4a_box = Box{};
            convert_flac_box = Box{};
            convert_one_box = Box{};
            convert_all_box = Box{};
            dialog_items.push_back(hbox({
                filler(),
                optionButton("wav", convert_wav_box, 0, 0,
                             convert_option_row, convert_option_col,
                             pending_convert_format == "wav"),
                text(" "),
                optionButton("mp3", convert_mp3_box, 0, 1,
                             convert_option_row, convert_option_col,
                             pending_convert_format == "mp3"),
                text(" "),
                optionButton("m4a", convert_m4a_box, 0, 2,
                             convert_option_row, convert_option_col,
                             pending_convert_format == "m4a"),
                text(" "),
                optionButton("flac", convert_flac_box, 0, 3,
                             convert_option_row, convert_option_col,
                             pending_convert_format == "flac"),
                filler(),
            }));
            dialog_items.push_back(hbox({
                filler(),
                optionButton("one", convert_one_box, 1, 0,
                             convert_option_row, convert_option_col,
                             !pending_process_all_folder),
                text(" "),
                optionButton("all folder", convert_all_box, 1, 1,
                             convert_option_row, convert_option_col,
                             pending_process_all_folder),
                filler(),
            }));
            dialog_items.push_back(separator());
            dialog_items.push_back(hbox({
                filler(),
                optionButton("Yes", confirm_yes_box, 2, 0,
                             convert_option_row, convert_option_col, false),
                text(" "),
                optionButton("No", confirm_no_box, 2, 1,
                             convert_option_row, convert_option_col, false),
                filler(),
            }));
        } else if (pending_confirmation == PendingConfirmation::AutoCue) {
            auto_cue_track_box = Box{};
            auto_cue_folder_box = Box{};
            auto_cue_cancel_box = Box{};
            bool has_track = !pending_auto_cue_track.id.empty();
            dialog_items.push_back(hbox({
                filler(),
                autoCueButton("one", auto_cue_track_box, 0, has_track),
                text(" "),
                autoCueButton("all folder", auto_cue_folder_box, 1),
                text(" "),
                autoCueButton("cancel", auto_cue_cancel_box, 2),
                filler(),
            }));
        } else if (pending_confirmation == PendingConfirmation::Analysis) {
            analysis_one_box = Box{};
            analysis_folder_box = Box{};
            analysis_cancel_box = Box{};
            // Analysis is an execution choice, not a two-column settings
            // form. Keep its three options vertical and easy to scan.
            dialog_items.push_back(vbox({
                analysisButton("one", analysis_one_box, 0),
                analysisButton("all folder", analysis_folder_box, 1),
                analysisButton("cancel", analysis_cancel_box, 2),
            }) | center);
        } else if (pending_confirmation == PendingConfirmation::CueSync) {
            cue_sync_serato_to_traktor_box = Box{};
            cue_sync_traktor_to_serato_box = Box{};
            cue_sync_one_box = Box{};
            cue_sync_folder_box = Box{};
            cue_sync_cancel_box = Box{};
            bool has_track = !pending_cue_sync_track.id.empty();
            int cue_sync_active_col = cue_sync_option_row == 0
                ? cue_sync_direction_col
                : cue_sync_scope_col;
            dialog_items.push_back(hbox({
                filler(),
                optionButton("Serato cue to Traktor",
                             cue_sync_serato_to_traktor_box,
                             0, 0,
                             cue_sync_option_row,
                             cue_sync_active_col,
                             pending_cue_sync_direction ==
                                 CueSyncDirection::SeratoToTraktor),
                text(" "),
                optionButton("Traktor cue to Serato",
                             cue_sync_traktor_to_serato_box,
                             0, 1,
                             cue_sync_option_row,
                             cue_sync_active_col,
                             pending_cue_sync_direction ==
                                 CueSyncDirection::TraktorToSerato),
                filler(),
            }));
            dialog_items.push_back(hbox({
                filler(),
                optionButton("one", cue_sync_one_box, 1, 0,
                             cue_sync_option_row, cue_sync_active_col,
                             has_track && cue_sync_scope_col == 0),
                text(" "),
                optionButton("all folder", cue_sync_folder_box, 1, 1,
                             cue_sync_option_row, cue_sync_active_col, false),
                text(" "),
                optionButton("cancel", cue_sync_cancel_box, 1, 2,
                             cue_sync_option_row, cue_sync_active_col, false),
                filler(),
            }));
        } else {
            dialog_items.push_back(hbox({
                filler(),
                button("Yes", confirm_yes_box, 0),
                text("  "),
                button("No", confirm_no_box, 1),
                filler(),
            }));
        }

        Element dialog = vbox(dialog_items) |
            size(WIDTH, LESS_THAN, std::min(64, content_width - 4)) |
            border |
            clear_under |
            center;

        return dbox({main_layout, dialog}) | flex;
    });

    auto paneAtMouse = [&](const Mouse& mouse) {
        if (directory_list_box.Contain(mouse.x, mouse.y)) {
            return 0;
        }
        if (track_list_box.Contain(mouse.x, mouse.y)) {
            return 1;
        }
        return -1;
    };

    auto selectVisibleRowAtMouse = [&](const Mouse& mouse) -> std::pair<int, int> {
        int pane = paneAtMouse(mouse);
        if (pane == -1) {
            return {-1, -1};
        }
        int local_row = mouse.y -
            (pane == 0 ? directory_list_box.y_min : track_list_box.y_min);
        int count = pane == 0
            ? (int)visible_dir_entries.size()
            : (int)visible_track_entries.size();
        if (local_row >= count) {
            return {-1, -1};
        }

        root_selector = 0;
        browser_selector = pane;
        if (pane == 0) {
            dir_view_selected = local_row;
            state.selectedDirectory = state.dirOffset + local_row;
            state.focus = FocusPane::Directories;
            return {pane, state.selectedDirectory};
        }

        track_view_selected = local_row;
        state.selectedTrack = state.trackOffset + local_row;
        state.focus = FocusPane::Tracks;
        return {pane, state.selectedTrack};
    };

    auto moveBrowserSelection = [&](int direction, int pane) {
        int count = pane == 0 ? (int)dir_entries.size() : (int)track_entries.size();
        if (count == 0) {
            return false;
        }

        root_selector = 0;
        browser_selector = pane;
        state.focus = pane == 0 ? FocusPane::Directories : FocusPane::Tracks;
        int& selected = pane == 0 ? state.selectedDirectory : state.selectedTrack;
        int next = std::clamp(selected + direction, 0, count - 1);
        if (next == selected) {
            return false;
        }
        selected = next;
        return true;
    };

    auto controlAtMouse = [&](const Mouse& mouse) {
        if (volume_down_box.Contain(mouse.x, mouse.y)) {
            return HoverControl::VolumeDown;
        }
        if (volume_up_box.Contain(mouse.x, mouse.y)) {
            return HoverControl::VolumeUp;
        }
        if (previous_box.Contain(mouse.x, mouse.y)) {
            return HoverControl::Previous;
        }
        if (play_pause_box.Contain(mouse.x, mouse.y)) {
            return HoverControl::PlayPause;
        }
        if (next_box.Contain(mouse.x, mouse.y)) {
            return HoverControl::Next;
        }
        if (mode_box.Contain(mouse.x, mouse.y)) {
            return HoverControl::Mode;
        }
        if (speed_reset_box.Contain(mouse.x, mouse.y)) {
            return HoverControl::SpeedReset;
        }
        return HoverControl::None;
    };

    auto sortColumnAtMouse = [&](const Mouse& mouse) -> std::optional<TrackSortColumn> {
        if (title_header_box.Contain(mouse.x, mouse.y)) {
            return TrackSortColumn::Title;
        }
        if (time_header_box.Contain(mouse.x, mouse.y)) {
            return TrackSortColumn::Time;
        }
        if (bpm_header_box.Contain(mouse.x, mouse.y)) {
            return TrackSortColumn::Bpm;
        }
        if (key_header_box.Contain(mouse.x, mouse.y)) {
            return TrackSortColumn::Key;
        }
        if (genre_header_box.Contain(mouse.x, mouse.y)) {
            return TrackSortColumn::Genre;
        }
        if (bitrate_header_box.Contain(mouse.x, mouse.y)) {
            return TrackSortColumn::Bitrate;
        }
        if (rate_header_box.Contain(mouse.x, mouse.y)) {
            return TrackSortColumn::SampleRate;
        }
        if (format_header_box.Contain(mouse.x, mouse.y)) {
            return TrackSortColumn::Format;
        }
        if (size_header_box.Contain(mouse.x, mouse.y)) {
            return TrackSortColumn::Size;
        }
        return std::nullopt;
    };

    auto togglePlayback = [&] {
        auto playback = controller.playbackSnapshot();
        if (playback.state == PlaybackState::Stopped ||
            playback.state == PlaybackState::Error) {
            playSelectedTrack();
        } else {
            controller.togglePause();
        }
    };

    auto activateControl = [&](HoverControl control) {
        switch (control) {
        case HoverControl::VolumeDown:
            controller.volumeDown();
            return true;
        case HoverControl::VolumeUp:
            controller.volumeUp();
            return true;
        case HoverControl::Previous:
            controller.playPreviousTrack();
            return true;
        case HoverControl::PlayPause:
            togglePlayback();
            return true;
        case HoverControl::Next:
            controller.playNextTrack();
            return true;
        case HoverControl::Mode:
            controller.cyclePlaybackMode();
            return true;
        case HoverControl::SpeedReset:
            if (setSpeedIfLocal(1.0)) {
                speed_value = 100;
            }
            return true;
        case HoverControl::None:
            return false;
        }
        return false;
    };

    auto mainKey = [&](const Event& event,
                       const std::string& action,
                       std::initializer_list<std::string> defaults) {
        return ui::bindingMatches(event, controller.config().keybinds,
                                  action, defaults);
    };

    auto runContextMenuAction = [&] {
        const int action = context_menu_selected;
        const bool is_track = context_menu_for_track;
        show_context_menu = false;
        if (!is_track) {
            if (action == 0) {
                openSelectedDirectory();
            } else if (action == 1) {
                if (state.selectedDirectory < 0 ||
                    state.selectedDirectory >= (int)dir_paths.size()) {
                    return;
                }
                std::string error;
                if (controller.openFolderExternally(
                        dir_paths[(size_t)state.selectedDirectory], error)) {
                    command_status = "Opened folder";
                } else {
                    command_status = "Open folder error: " + error;
                }
            } else if (action == 2) {
                if (state.selectedDirectory < 0 ||
                    state.selectedDirectory >= (int)dir_paths.size() ||
                    controller.isVirtualPlaylistPath(dir_paths[state.selectedDirectory]) ||
                    controller.isTelegramPath(dir_paths[state.selectedDirectory])) {
                    command_status = "Virtual folders cannot be renamed";
                    return;
                }
                command_input = "rename ";
                root_selector = 2;
                bottom_selector = 0;
                command_status = "Enter a new folder name";
            } else if (action == 3 && state.selectedDirectory >= 0 &&
                       state.selectedDirectory < (int)dir_paths.size()) {
                if (controller.isVirtualPlaylistPath(dir_paths[state.selectedDirectory]) ||
                    controller.isTelegramPath(dir_paths[state.selectedDirectory])) {
                    command_status = "Virtual folders cannot be deleted";
                    return;
                }
                pending_delete_entry = Track{};
                pending_delete_entry.id = dir_paths[state.selectedDirectory];
                pending_delete_entry.title = displayName(pending_delete_entry.id);
                pending_delete_entry.type = EntryType::Directory;
                pending_confirmation = PendingConfirmation::DeleteEntry;
            }
            return;
        }
        if (state.selectedTrack < 0 || state.selectedTrack >= (int)visible_files.size()) {
            return;
        }
        const Track track = visible_files[state.selectedTrack];
        if (track.type == EntryType::Album || track.type == EntryType::Navigation) {
            if (action == 0) {
                playSelectedTrack();
            }
            return;
        }
        const bool online_track = track.id.starts_with("https://") ||
            track.id.starts_with("http://");
        if (online_track) {
            if (action == 0) {
                playSelectedTrack();
                return;
            }
            if (action == 1) {
                openOnlineAlbum(track);
                return;
            }
            if (action == 2) {
                if (controller.isOnlineTrackDownloaded(track)) {
                    command_status = "Track is already downloaded";
                    return;
                }
                beginSearchDownload({track});
            }
            return;
        }

        if (action == 0) {
            metadata_title = track.title;
            metadata_details = controller.metadataDetails(track);
            metadata_offset = 0;
            show_metadata_popup = true;
            command_status = "Metadata: " + track.title;
        } else if (action == 1) {
            pending_analysis_track = track;
            analysis_option_row = 0;
            pending_confirmation = PendingConfirmation::Analysis;
            command_status = "Analyze";
        } else if (action == 2) {
            pending_process_track = track;
            pending_process_all_folder = false;
            pending_convert_format = "mp3";
            convert_option_row = 0;
            convert_option_col = 1;
            pending_confirmation = PendingConfirmation::Convert;
        } else if (action == 3) {
            pending_stem_track = track;
            pending_stem_config = controller.config().demucs;
            stem_option_row = 0;
            stem_option_col = pending_stem_config.stems == 4 ? 1 : 0;
            pending_confirmation = PendingConfirmation::StemSeparation;
        } else if (action == 4) {
            prepareEditor(track, EditorPrepareTarget::Trim);
        } else if (action == 5) {
            prepareEditor(track, EditorPrepareTarget::ManualCues);
        } else if (action == 6) {
            pending_auto_cue_track = track;
            auto_cue_option_col = 0;
            pending_confirmation = PendingConfirmation::AutoCue;
            command_status = "Auto Cue";
        } else if (action == 7) {
            if (track.id.rfind("https://", 0) == 0 ||
                track.id.rfind("http://", 0) == 0) {
                command_status = "Search tracks are temporary; no local file to delete";
                return;
            }
            pending_delete_entry = track;
            pending_confirmation = PendingConfirmation::DeleteEntry;
        } else if (action == 8) {
            const std::string folder = fs::path(track.id).parent_path().string();
            std::string error;
            if (folder.empty()) {
                command_status = "Open folder error: track folder is unavailable";
            } else if (controller.openFolderExternally(folder, error)) {
                command_status = "Opened folder: " + displayName(folder);
            } else {
                command_status = "Open folder error: " + error;
            }
        }
    };

    prepareEditor = [&](const Track& track, EditorPrepareTarget target) {
        if (editor_prepare_active.load()) {
            command_status = "Waveform is loading";
            return;
        }
        if (track.type != EntryType::File || track.id.empty()) {
            command_status = target == EditorPrepareTarget::ManualCues
                ? "Select a track for manual cues"
                : "Select a track to trim";
            return;
        }
        std::string local_reason;
        if (!controller.canEditTrack(track, local_reason)) {
            command_status = local_reason;
            return;
        }
        if (editor_prepare_worker.joinable()) {
            editor_prepare_worker.request_stop();
            editor_prepare_worker.join();
        }
        controller.stopPlayback();
        controller.stopPreviewPlayback();
        editor_prepare_active = true;
        command_status = target == EditorPrepareTarget::ManualCues
            ? "Loading waveform: " + track.title
            : "Loading trim waveform: " + track.title;
        editor_prepare_worker = std::jthread(
            [&, track, target](std::stop_token token) {
                std::string error;
                AutoCueFeatures waveform = controller.waveformForTrack(track, error);
                if (token.stop_requested()) {
                    return;
                }
                screen.Post([&, track, target, waveform = std::move(waveform),
                             error = std::move(error)]() mutable {
                    editor_prepare_active = false;
                    if (!error.empty()) {
                        command_status = error;
                        return;
                    }
                    if (target == EditorPrepareTarget::ManualCues) {
                        manual_cue_editor.open(track, &waveform, &error);
                    } else {
                        trim_editor.open(track, &waveform, &error);
                    }
                });
            });
    };

    auto component = CatchEvent(root_renderer, [&](Event e) {
        if (root_selector == 2 && bottom_selector == 0 && e == Event::Tab) {
            static const std::vector<std::string> commands = {
                "track ", "album ", "search ", "find", "playlist ", "download ",
                "now", "pause", "stop", "update",
                "record ", "record start", "record stop", "mkdir ",
                "rename ", "refresh", "quit",
            };
            const std::string typed = lowercase(command_input);
            std::vector<std::string> matches;
            for (const auto& command : commands) {
                if (lowercase(command).starts_with(typed)) {
                    matches.push_back(command);
                }
            }
            if (matches.size() == 1) {
                command_input = matches.front();
                command_cursor_position = (int)command_input.size();
                command_status = "Complete: " + command_input;
            } else if (!matches.empty()) {
                command_status = "Commands: ";
                for (size_t index = 0; index < matches.size(); ++index) {
                    if (index > 0) command_status += ", ";
                    command_status += matches[index];
                }
            } else if (!command_input.empty()) {
                command_status = "No command completion";
            }
            return true;
        }
        if (e == Event::Custom) {
            MacNowPlaying::pumpSystemRunLoop();
            std::string active_track_id = controller.playingTrackId();
            if (!active_track_id.empty() && active_track_id != playing_track_id) {
                auto playing = std::find_if(visible_files.begin(), visible_files.end(),
                                            [&](const Track& track) {
                                                return track.id == active_track_id;
                                            });
                if (playing != visible_files.end()) {
                    state.selectedTrack = (int)std::distance(visible_files.begin(), playing);
                }
                playing_track_id = active_track_id;
            }
            return true;
        }

        if (manual_cue_editor.isOpen()) {
            return manual_cue_editor.handleEvent(e);
        }
        if (trim_editor.isOpen()) {
            const bool handled = trim_editor.handleEvent(e);
            Track track;
            double start_seconds = 0.0;
            double end_seconds = 0.0;
            if (trim_editor.takeSplitRequest(track, start_seconds, end_seconds)) {
                trim_editor.close();
                pending_stem_track = std::move(track);
                pending_stem_config = controller.config().demucs;
                pending_stem_trim_region = true;
                pending_stem_trim_start = start_seconds;
                pending_stem_trim_end = end_seconds;
                stem_option_row = 0;
                stem_option_col = pending_stem_config.stems == 4 ? 1 : 0;
                pending_confirmation = PendingConfirmation::StemSeparation;
                command_status = "Choose 2 or 4 stems for selected region";
            }
            return handled;
        }

        if (show_metadata_popup) {
            int visible_rows = std::max(5, Terminal::Size().dimy - 12);
            int terminal_width = std::max(60, Terminal::Size().dimx);
            int dialog_width = std::clamp(terminal_width - 6, 44, 92);
            int name_width = std::min(20, std::max(8, dialog_width / 4));
            int value_width = std::max(10, dialog_width - name_width - 8);
            int visual_rows = 0;
            for (const auto& [name, value] : metadata_details) {
                visual_rows += std::max(1, (int)wrapText(value, value_width).size());
            }
            int max_offset = std::max(0, visual_rows - visible_rows);
            auto scrollMetadata = [&](int delta) {
                metadata_offset = std::clamp(metadata_offset + delta, 0, max_offset);
            };

            if (e.is_mouse()) {
                if (e.mouse().button == Mouse::Left &&
                    e.mouse().motion == Mouse::Pressed) {
                    if (metadata_close_box.Contain(e.mouse().x, e.mouse().y)) {
                        show_metadata_popup = false;
                        return true;
                    }
                    for (size_t i = 0; i < metadata_link_boxes.size(); ++i) {
                        if (metadata_link_boxes[i].Contain(e.mouse().x, e.mouse().y)) {
                            std::string error;
                            if (controller.openExternalUrl(metadata_link_urls[i], error)) {
                                command_status = "Opened: " + metadata_link_urls[i];
                            } else {
                                command_status = "Error: " + error;
                            }
                            return true;
                        }
                    }
                }
                if (e.mouse().button == Mouse::WheelUp) {
                    scrollMetadata(-1);
                    return true;
                }
                if (e.mouse().button == Mouse::WheelDown) {
                    scrollMetadata(1);
                    return true;
                }
                return true;
            }

            if (mainKey(e, "metadata_close",
                        {"escape", "enter", "m", "M", "ь", "Ь"})) {
                show_metadata_popup = false;
                return true;
            }
            if (mainKey(e, "up", {"up"})) {
                scrollMetadata(-1);
                return true;
            }
            if (mainKey(e, "down", {"down"})) {
                scrollMetadata(1);
                return true;
            }
            if (mainKey(e, "page_up", {"page_up"})) {
                scrollMetadata(-visible_rows);
                return true;
            }
            if (mainKey(e, "page_down", {"page_down"})) {
                scrollMetadata(visible_rows);
                return true;
            }
            return true;
        }

        if (show_eq_popup) {
            auto resetEq = [&](int index) {
                if (index == 0) eq_high = 0;
                if (index == 1) eq_mid = 0;
                if (index == 2) eq_low = 0;
                applyEq();
            };
            auto adjustEq = [&](int delta) {
                int row = eq_selected / 2;
                if (eq_selected % 2 != 0 || row > 2) {
                    return;
                }
                if (row == 0) eq_high = std::clamp(eq_high + delta, eq_min, eq_max);
                if (row == 1) eq_mid = std::clamp(eq_mid + delta, eq_min, eq_max);
                if (row == 2) eq_low = std::clamp(eq_low + delta, eq_min, eq_max);
                applyEq();
            };
            if (e.is_mouse() &&
                e.mouse().button == Mouse::Left &&
                e.mouse().motion == Mouse::Pressed) {
                if (eq_close_box.Contain(e.mouse().x, e.mouse().y)) {
                    show_eq_popup = false;
                    return true;
                }
                for (int i = 0; i < 3; ++i) {
                    if (eq_reset_boxes[(size_t)i].Contain(e.mouse().x, e.mouse().y)) {
                        eq_selected = i * 2 + 1;
                        resetEq(i);
                        return true;
                    }
                }
                return true;
            }
            if (mainKey(e, "equalizer_close",
                        {"escape", "e", "E", "у", "У"})) {
                show_eq_popup = false;
                return true;
            }
            if (mainKey(e, "up", {"up"})) {
                eq_selected = std::max(0, eq_selected - 1);
                return true;
            }
            if (mainKey(e, "down", {"down"})) {
                eq_selected = std::min(6, eq_selected + 1);
                return true;
            }
            if (mainKey(e, "left", {"left"})) {
                adjustEq(-1);
                return true;
            }
            if (mainKey(e, "right", {"right"})) {
                adjustEq(1);
                return true;
            }
            if (mainKey(e, "equalizer_reset", {"x", "X", "ч", "Ч"})) {
                if (eq_selected < 6) {
                    resetEq(eq_selected / 2);
                }
                return true;
            }
            if (mainKey(e, "play", {"enter"})) {
                if (eq_selected == 6) {
                    show_eq_popup = false;
                } else if (eq_selected % 2 == 1) {
                    resetEq(eq_selected / 2);
                }
                return true;
            }
            return true;
        }

        if (show_context_menu) {
            const int count = (int)contextMenuActions().size();
            if (e.is_mouse() && e.mouse().button == Mouse::Left &&
                e.mouse().motion == Mouse::Pressed) {
                for (int index = 0; index < count &&
                     index < (int)context_menu_boxes.size(); ++index) {
                    if (context_menu_boxes[(size_t)index].Contain(e.mouse().x, e.mouse().y)) {
                        context_menu_selected = index;
                        runContextMenuAction();
                        return true;
                    }
                }
                return true;
            }
            if (mainKey(e, "context_close", {"escape"})) {
                show_context_menu = false;
                return true;
            }
            if (mainKey(e, "up", {"up"})) {
                context_menu_selected = std::max(0, context_menu_selected - 1);
                return true;
            }
            if (mainKey(e, "down", {"down"})) {
                context_menu_selected = std::min(count - 1, context_menu_selected + 1);
                return true;
            }
            if (mainKey(e, "play", {"enter"})) {
                runContextMenuAction();
                return true;
            }
            return true;
        }

        if (pending_confirmation != PendingConfirmation::None) {
            auto maxProcessCol = [&](int row) {
                if (pending_confirmation == PendingConfirmation::Convert && row == 0) {
                    return 3;
                }
                if (pending_confirmation == PendingConfirmation::Normalization && row == 0) {
                    return 2;
                }
                return 1;
            };
            auto clampProcessCol = [&](int& row, int& col) {
                col = std::clamp(col, 0, maxProcessCol(row));
            };
            auto chooseNormalizationOption = [&](int row, int col) {
                normalize_option_row = row;
                normalize_option_col = col;
                if (row == 0) {
                    if (col == 0) {
                        pending_normalize_lufs = -16;
                        pending_normalize_mode = "Integrated";
                    } else if (col == 1) {
                        pending_normalize_lufs = -14;
                        pending_normalize_mode = "Short-Term Max";
                    } else {
                        pending_normalize_lufs = -9;
                        pending_normalize_mode = "";
                    }
                } else if (row == 1) {
                    pending_process_all_folder = col == 1;
                }
            };
            auto chooseConvertOption = [&](int row, int col) {
                convert_option_row = row;
                convert_option_col = col;
                if (row == 0) {
                    static const std::array<std::string, 4> formats = {
                        "wav", "mp3", "m4a", "flac",
                    };
                    pending_convert_format = formats[std::clamp(col, 0, 3)];
                } else if (row == 1) {
                    pending_process_all_folder = col == 1;
                }
            };
            auto chooseAutoCueOption = [&](int col) {
                bool has_track = !pending_auto_cue_track.id.empty();
                if (col == 0 && !has_track) {
                    col = 1;
                }
                auto_cue_option_col = std::clamp(col, 0, 2);
            };
            auto chooseCueSyncOption = [&](int row, int col) {
                cue_sync_option_row = std::clamp(row, 0, 1);
                if (cue_sync_option_row == 0) {
                    cue_sync_direction_col = std::clamp(col, 0, 1);
                    pending_cue_sync_direction = cue_sync_direction_col == 0
                        ? CueSyncDirection::SeratoToTraktor
                        : CueSyncDirection::TraktorToSerato;
                } else {
                    cue_sync_scope_col = std::clamp(col, 0, 2);
                }
            };

            if (pending_confirmation == PendingConfirmation::PlaylistDownload) {
                auto show = [&] {
                    std::string source = std::move(pending_playlist_source);
                    clearConfirmation();
                    showOnlinePlaylist(std::move(source));
                };
                if (e.is_mouse() && e.mouse().button == Mouse::Left &&
                    e.mouse().motion == Mouse::Pressed) {
                    if (playlist_show_box.Contain(e.mouse().x, e.mouse().y)) {
                        playlist_action_col = 0;
                        show();
                        return true;
                    }
                    if (playlist_download_box.Contain(e.mouse().x, e.mouse().y)) {
                        playlist_action_col = 1;
                        acceptConfirmation();
                        return true;
                    }
                    if (playlist_cancel_box.Contain(e.mouse().x, e.mouse().y)) {
                        playlist_action_col = 2;
                        rejectConfirmation();
                        return true;
                    }
                    return true;
                }
                if (mainKey(e, "left", {"left", "up", "shift_tab"})) {
                    playlist_action_col = std::max(0, playlist_action_col - 1);
                    return true;
                }
                if (mainKey(e, "right", {"right", "down", "tab"})) {
                    playlist_action_col = std::min(2, playlist_action_col + 1);
                    return true;
                }
                if (mainKey(e, "play", {"enter"})) {
                    if (playlist_action_col == 0) {
                        show();
                    } else if (playlist_action_col == 1) {
                        acceptConfirmation();
                    } else {
                        rejectConfirmation();
                    }
                    return true;
                }
                if (mainKey(e, "confirm_no", {"escape", "n", "N", "т", "Т"})) {
                    rejectConfirmation();
                    return true;
                }
                return true;
            }

            if (pending_confirmation == PendingConfirmation::CueSync) {
                if (e.is_mouse() &&
                    e.mouse().button == Mouse::Left &&
                    e.mouse().motion == Mouse::Pressed) {
                    if (cue_sync_serato_to_traktor_box.Contain(e.mouse().x, e.mouse().y)) {
                        chooseCueSyncOption(0, 0);
                        return true;
                    }
                    if (cue_sync_traktor_to_serato_box.Contain(e.mouse().x, e.mouse().y)) {
                        chooseCueSyncOption(0, 1);
                        return true;
                    }
                    if (cue_sync_one_box.Contain(e.mouse().x, e.mouse().y)) {
                        chooseCueSyncOption(1, 0);
                        acceptConfirmation();
                        return true;
                    }
                    if (cue_sync_folder_box.Contain(e.mouse().x, e.mouse().y)) {
                        chooseCueSyncOption(1, 1);
                        acceptConfirmation();
                        return true;
                    }
                    if (cue_sync_cancel_box.Contain(e.mouse().x, e.mouse().y)) {
                        chooseCueSyncOption(1, 2);
                        rejectConfirmation();
                        return true;
                    }
                    return true;
                }

                if (mainKey(e, "left", {"left", "shift_tab"})) {
                    chooseCueSyncOption(cue_sync_option_row,
                                        cue_sync_option_row == 0
                                            ? cue_sync_direction_col - 1
                                            : cue_sync_scope_col - 1);
                    return true;
                }
                if (mainKey(e, "right", {"right", "tab"})) {
                    chooseCueSyncOption(cue_sync_option_row,
                                        cue_sync_option_row == 0
                                            ? cue_sync_direction_col + 1
                                            : cue_sync_scope_col + 1);
                    return true;
                }
                if (mainKey(e, "up", {"up"})) {
                    cue_sync_option_row = 0;
                    return true;
                }
                if (mainKey(e, "down", {"down"})) {
                    cue_sync_option_row = 1;
                    return true;
                }
                if (mainKey(e, "play", {"enter"})) {
                    if (cue_sync_option_row == 1) {
                        if (cue_sync_scope_col == 2) {
                            rejectConfirmation();
                        } else {
                            acceptConfirmation();
                        }
                    }
                    return true;
                }
                if (mainKey(e, "confirm_no", {"escape", "n", "N", "т", "Т"})) {
                    rejectConfirmation();
                    return true;
                }
                return true;
            };

            if (pending_confirmation == PendingConfirmation::AutoCue) {
                if (e.is_mouse() &&
                    e.mouse().button == Mouse::Left &&
                    e.mouse().motion == Mouse::Pressed) {
                    if (auto_cue_track_box.Contain(e.mouse().x, e.mouse().y)) {
                        chooseAutoCueOption(0);
                        if (!pending_auto_cue_track.id.empty()) {
                            acceptConfirmation();
                        }
                        return true;
                    }
                    if (auto_cue_folder_box.Contain(e.mouse().x, e.mouse().y)) {
                        chooseAutoCueOption(1);
                        acceptConfirmation();
                        return true;
                    }
                    if (auto_cue_cancel_box.Contain(e.mouse().x, e.mouse().y)) {
                        chooseAutoCueOption(2);
                        rejectConfirmation();
                        return true;
                    }
                    return true;
                }

                if (mainKey(e, "left", {"left", "shift_tab"})) {
                    chooseAutoCueOption(auto_cue_option_col - 1);
                    return true;
                }
                if (mainKey(e, "right", {"right", "tab"})) {
                    chooseAutoCueOption(auto_cue_option_col + 1);
                    return true;
                }
                if (mainKey(e, "play", {"enter"})) {
                    if (auto_cue_option_col == 2) {
                        rejectConfirmation();
                    } else {
                        acceptConfirmation();
                    }
                    return true;
                }
                if (mainKey(e, "confirm_no", {"escape", "n", "N", "т", "Т"})) {
                    rejectConfirmation();
                    return true;
                }
                if (mainKey(e, "confirm_yes", {"y", "Y", "н", "Н"})) {
                    acceptConfirmation();
                    return true;
                }
                return true;
            }

            if (pending_confirmation == PendingConfirmation::Analysis) {
                auto chooseAnalysisOption = [&](int row) {
                    analysis_option_row = std::clamp(row, 0, 2);
                };
                if (e.is_mouse() && e.mouse().button == Mouse::Left &&
                    e.mouse().motion == Mouse::Pressed) {
                    if (analysis_one_box.Contain(e.mouse().x, e.mouse().y)) {
                        chooseAnalysisOption(0);
                        acceptConfirmation();
                        return true;
                    }
                    if (analysis_folder_box.Contain(e.mouse().x, e.mouse().y)) {
                        chooseAnalysisOption(1);
                        acceptConfirmation();
                        return true;
                    }
                    if (analysis_cancel_box.Contain(e.mouse().x, e.mouse().y)) {
                        chooseAnalysisOption(2);
                        rejectConfirmation();
                        return true;
                    }
                    return true;
                }
                if (mainKey(e, "up", {"up", "left", "shift_tab"})) {
                    chooseAnalysisOption(analysis_option_row - 1);
                    return true;
                }
                if (mainKey(e, "down", {"down", "right", "tab"})) {
                    chooseAnalysisOption(analysis_option_row + 1);
                    return true;
                }
                if (mainKey(e, "play", {"enter"})) {
                    if (analysis_option_row == 2) {
                        rejectConfirmation();
                    } else {
                        acceptConfirmation();
                    }
                    return true;
                }
                if (mainKey(e, "confirm_no", {"escape", "n", "N", "т", "Т"})) {
                    rejectConfirmation();
                    return true;
                }
                if (mainKey(e, "confirm_yes", {"y", "Y", "н", "Н"})) {
                    acceptConfirmation();
                    return true;
                }
                return true;
            }

            if (e.is_mouse() &&
                e.mouse().button == Mouse::Left &&
                e.mouse().motion == Mouse::Pressed) {
                if (pending_confirmation == PendingConfirmation::StemSeparation) {
                    if (stem_2_box.Contain(e.mouse().x, e.mouse().y)) {
                        pending_stem_config.stems = 2;
                        stem_option_row = 0;
                        stem_option_col = 0;
                        return true;
                    }
                    if (stem_4_box.Contain(e.mouse().x, e.mouse().y)) {
                        pending_stem_config.stems = 4;
                        stem_option_row = 0;
                        stem_option_col = 1;
                        return true;
                    }
                    if (stem_mp3_box.Contain(e.mouse().x, e.mouse().y)) {
                        pending_stem_config.outputFormat = "mp3";
                        stem_option_row = 1;
                        stem_option_col = 0;
                        return true;
                    }
                    if (stem_wav_box.Contain(e.mouse().x, e.mouse().y)) {
                        pending_stem_config.outputFormat = "wav";
                        stem_option_row = 1;
                        stem_option_col = 1;
                        return true;
                    }
                    if (stem_flac_box.Contain(e.mouse().x, e.mouse().y)) {
                        pending_stem_config.outputFormat = "flac";
                        stem_option_row = 1;
                        stem_option_col = 2;
                        return true;
                    }
                }

                if (pending_confirmation == PendingConfirmation::Normalization) {
                    if (normalize_16_box.Contain(e.mouse().x, e.mouse().y)) {
                        chooseNormalizationOption(0, 0);
                        return true;
                    }
                    if (normalize_14_box.Contain(e.mouse().x, e.mouse().y)) {
                        chooseNormalizationOption(0, 1);
                        return true;
                    }
                    if (normalize_9_box.Contain(e.mouse().x, e.mouse().y)) {
                        chooseNormalizationOption(0, 2);
                        return true;
                    }
                    if (normalize_one_box.Contain(e.mouse().x, e.mouse().y)) {
                        chooseNormalizationOption(1, 0);
                        return true;
                    }
                    if (normalize_all_box.Contain(e.mouse().x, e.mouse().y)) {
                        chooseNormalizationOption(1, 1);
                        return true;
                    }
                }

                if (pending_confirmation == PendingConfirmation::Convert) {
                    if (convert_wav_box.Contain(e.mouse().x, e.mouse().y)) {
                        chooseConvertOption(0, 0);
                        return true;
                    }
                    if (convert_mp3_box.Contain(e.mouse().x, e.mouse().y)) {
                        chooseConvertOption(0, 1);
                        return true;
                    }
                    if (convert_m4a_box.Contain(e.mouse().x, e.mouse().y)) {
                        chooseConvertOption(0, 2);
                        return true;
                    }
                    if (convert_flac_box.Contain(e.mouse().x, e.mouse().y)) {
                        chooseConvertOption(0, 3);
                        return true;
                    }
                    if (convert_one_box.Contain(e.mouse().x, e.mouse().y)) {
                        chooseConvertOption(1, 0);
                        return true;
                    }
                    if (convert_all_box.Contain(e.mouse().x, e.mouse().y)) {
                        chooseConvertOption(1, 1);
                        return true;
                    }
                }

                if (confirm_yes_box.Contain(e.mouse().x, e.mouse().y)) {
                    confirmation_selected = 0;
                    stem_option_row = 2;
                    stem_option_col = 0;
                    normalize_option_row = 2;
                    normalize_option_col = 0;
                    convert_option_row = 2;
                    convert_option_col = 0;
                    acceptConfirmation();
                    return true;
                }
                if (confirm_no_box.Contain(e.mouse().x, e.mouse().y)) {
                    confirmation_selected = 1;
                    stem_option_row = 2;
                    stem_option_col = 1;
                    normalize_option_row = 2;
                    normalize_option_col = 1;
                    convert_option_row = 2;
                    convert_option_col = 1;
                    rejectConfirmation();
                    return true;
                }
                return true;
            }

            if (pending_confirmation == PendingConfirmation::StemSeparation) {
                auto maxStemCol = [&] {
                    return stem_option_row == 0 ? 1 : (stem_option_row == 1 ? 2 : 1);
                };
                if (mainKey(e, "left", {"left", "shift_tab"})) {
                    stem_option_col = std::max(0, stem_option_col - 1);
                    return true;
                }
                if (mainKey(e, "right", {"right", "tab"})) {
                    stem_option_col = std::min(maxStemCol(), stem_option_col + 1);
                    return true;
                }
                if (mainKey(e, "up", {"up"})) {
                    stem_option_row = std::max(0, stem_option_row - 1);
                    stem_option_col = std::min(maxStemCol(), stem_option_col);
                    return true;
                }
                if (mainKey(e, "down", {"down"})) {
                    stem_option_row = std::min(2, stem_option_row + 1);
                    stem_option_col = std::min(maxStemCol(), stem_option_col);
                    return true;
                }
                if (mainKey(e, "play", {"enter"})) {
                    if (stem_option_row == 0) {
                        if (stem_option_col == 0) {
                            pending_stem_config.stems = 2;
                        } else {
                            pending_stem_config.stems = 4;
                        }
                    } else if (stem_option_row == 1) {
                        if (stem_option_col == 0) {
                            pending_stem_config.outputFormat = "mp3";
                        } else if (stem_option_col == 1) {
                            pending_stem_config.outputFormat = "wav";
                        } else {
                            pending_stem_config.outputFormat = "flac";
                        }
                    } else if (stem_option_col == 0) {
                        acceptConfirmation();
                    } else {
                        rejectConfirmation();
                    }
                    return true;
                }
            }

            if (pending_confirmation == PendingConfirmation::Normalization ||
                pending_confirmation == PendingConfirmation::Convert) {
                int& row = pending_confirmation == PendingConfirmation::Normalization
                    ? normalize_option_row
                    : convert_option_row;
                int& col = pending_confirmation == PendingConfirmation::Normalization
                    ? normalize_option_col
                    : convert_option_col;
                if (mainKey(e, "left", {"left", "shift_tab"})) {
                    col = std::max(0, col - 1);
                    return true;
                }
                if (mainKey(e, "right", {"right", "tab"})) {
                    col = std::min(maxProcessCol(row), col + 1);
                    return true;
                }
                if (mainKey(e, "up", {"up"})) {
                    row = std::max(0, row - 1);
                    clampProcessCol(row, col);
                    return true;
                }
                if (mainKey(e, "down", {"down"})) {
                    row = std::min(2, row + 1);
                    clampProcessCol(row, col);
                    return true;
                }
                if (mainKey(e, "play", {"enter"})) {
                    if (pending_confirmation == PendingConfirmation::Normalization) {
                        chooseNormalizationOption(row, col);
                    } else {
                        chooseConvertOption(row, col);
                    }
                    if (row == 2 && col == 0) {
                        acceptConfirmation();
                    } else if (row == 2) {
                        rejectConfirmation();
                    }
                    return true;
                }
            }

            if (mainKey(e, "left", {"left", "shift_tab"}) ||
                mainKey(e, "right", {"right", "tab"})) {
                confirmation_selected = confirmation_selected == 0 ? 1 : 0;
                return true;
            }

            if (mainKey(e, "play", {"enter"})) {
                if (confirmation_selected == 0) {
                    acceptConfirmation();
                } else {
                    rejectConfirmation();
                }
                return true;
            }

            if (mainKey(e, "confirm_yes", {"y", "Y", "н", "Н"})) {
                confirmation_selected = 0;
                acceptConfirmation();
                return true;
            }

            if (mainKey(e, "confirm_no", {"escape", "n", "N", "т", "Т"})) {
                confirmation_selected = 1;
                rejectConfirmation();
                return true;
            }
            return true;
        }

        if (e.is_mouse() && e.mouse().motion == Mouse::Moved) {
            HoverControl next_hover = controlAtMouse(e.mouse());
            bool hover_changed = hovered_control != next_hover;
            hovered_control = next_hover;
            if (next_hover != HoverControl::None) {
                return true;
            }

            if (move_to_mode && paneAtMouse(e.mouse()) != 0) {
                return hover_changed;
            }
            // Hover must not steal keyboard focus from the other pane or a
            // bottom control. Selection and focus are changed on click only.
            return hover_changed;
        }

        if (e.is_mouse() &&
            (e.mouse().button == Mouse::WheelUp ||
             e.mouse().button == Mouse::WheelDown)) {
            if (download_pool_box.Contain(e.mouse().x, e.mouse().y)) {
                auto download = controller.downloadSnapshot();
                int max_scroll = std::max(0, (int)download.items.size() - 3);
                int delta = e.mouse().button == Mouse::WheelDown ? 3 : -3;
                download_pool_scroll = std::clamp(
                    download_pool_scroll + delta,
                    0,
                    max_scroll);
                return true;
            }
            int pane = paneAtMouse(e.mouse());
            if (pane != -1) {
                moveBrowserSelection(
                    e.mouse().button == Mouse::WheelDown ? 1 : -1,
                    pane);
                return true;
            }
        }

        if (e.is_mouse() && e.mouse().button == Mouse::Right &&
            e.mouse().motion == Mouse::Pressed) {
            const auto [pane, selected] = selectVisibleRowAtMouse(e.mouse());
            if (pane != -1 && selected >= 0) {
                context_menu_for_track = pane == 1;
                context_menu_selected = 0;
                show_context_menu = true;
                return true;
            }
        }

        if (e.is_mouse() &&
            e.mouse().button == Mouse::Left &&
            e.mouse().motion == Mouse::Pressed) {
            if (download_close_box.Contain(e.mouse().x, e.mouse().y)) {
                show_download_panel = false;
                return true;
            }
            if (download_stop_box.Contain(e.mouse().x, e.mouse().y)) {
                controller.cancelDownload();
                command_status = "Stopping download";
                return true;
            }
            if (stems_close_box.Contain(e.mouse().x, e.mouse().y)) {
                show_stems_panel = false;
                return true;
            }
            if (audio_process_close_box.Contain(e.mouse().x, e.mouse().y)) {
                show_audio_process_panel = false;
                return true;
            }
            if (auto_cue_close_box.Contain(e.mouse().x, e.mouse().y)) {
                show_auto_cue_panel = false;
                return true;
            }

            if (album_download_box.Contain(e.mouse().x, e.mouse().y)) {
                if (controller.isOnlineAlbumDownloaded(visible_files)) {
                    command_status = "Album is already downloaded";
                    return true;
                }
                beginSearchDownload(visible_files);
                return true;
            }
            for (size_t index = 0; index < track_download_boxes.size() &&
                                   index < visible_files.size(); ++index) {
                if (track_download_boxes[index].Contain(e.mouse().x, e.mouse().y)) {
                    if (controller.isOnlineTrackDownloaded(visible_files[index])) {
                        command_status = "Track is already downloaded";
                        return true;
                    }
                    beginSearchDownload({visible_files[index]});
                    return true;
                }
            }

            HoverControl control = controlAtMouse(e.mouse());
            if (activateControl(control)) {
                hovered_control = control;
                return true;
            }

            if (command_paste_box.Contain(e.mouse().x, e.mouse().y)) {
                pasteClipboardToInput();
                return true;
            }

            if (command_clear_box.Contain(e.mouse().x, e.mouse().y)) {
                command_input.clear();
                root_selector = 2;
                bottom_selector = 0;
                return true;
            }

            if (command_record_box.Contain(e.mouse().x, e.mouse().y)) {
                toggleDesktopRecording();
                return true;
            }

            if (controller.auddFindEnabled() &&
                command_find_box.Contain(e.mouse().x, e.mouse().y)) {
                startAudDFind();
                return true;
            }

            if (now_playing_box.Contain(e.mouse().x, e.mouse().y)) {
                const std::string value = now_playing_copy_value.empty()
                    ? std::string("tmplay") : now_playing_copy_value;
                command_status = copyToClipboard(value)
                    ? "Copied: " + truncateEnd(value, 40)
                    : "Clipboard unavailable";
                return true;
            }

            if (!move_to_mode) {
                if (auto column = sortColumnAtMouse(e.mouse())) {
                    toggleTrackSort(*column);
                    return true;
                }
            }

            if (move_to_mode && paneAtMouse(e.mouse()) != 0) {
                return true;
            }

            auto selected = selectVisibleRowAtMouse(e.mouse());
            if (selected.first == -1) {
                return false;
            }
            int click_pane = selected.first;
            int click_item = selected.second;
            auto now = std::chrono::steady_clock::now();
            bool is_double_click =
                click_pane == last_click_pane &&
                click_item == last_click_item &&
                now - last_click_time <= std::chrono::milliseconds(450);

            last_click_pane = click_pane;
            last_click_item = click_item;
            last_click_time = now;

            if (is_double_click) {
                if (click_pane == 0) {
                    state.focus = FocusPane::Directories;
                    browser_selector = 0;
                    openSelectedDirectory();
                } else {
                    state.focus = FocusPane::Tracks;
                    browser_selector = 1;
                    playSelectedTrack();
                }
            }
            return true;
        }

        if (move_to_mode && mainKey(e, "cancel", {"escape"})) {
            command_status = "Move cancelled";
            finishKeyboardMove();
            return true;
        }

        bool command_active = root_selector == 2 && bottom_selector == 0;
        if (command_active) {
            if (mainKey(e, "cancel", {"escape"})) {
                root_selector = 0;
                return true;
            }
            return false;
        }

        if (mainKey(e, "focus_progress", {"/"})) {
            root_selector = 1;
            return true;
        }
        if (mainKey(e, "focus_speed", {"?"})) {
            root_selector = 2;
            bottom_selector = 1;
            speed_focus = 1;
            return true;
        }
        if (mainKey(e, "focus_input", {"i", "I", "ш", "Ш"})) {
            root_selector = 2;
            bottom_selector = 0;
            return true;
        }
        if (mainKey(e, "paste_input", {"v", "V", "м", "М"})) {
            pasteClipboardToInput();
            return true;
        }
        if (mainKey(e, "speed_reset", {":"})) {
            if (setSpeedIfLocal(1.0)) {
                speed_value = 100;
            }
            root_selector = 2;
            bottom_selector = 1;
            speed_focus = 0;
            return true;
        }
        if (mainKey(e, "pitch_lock", {"\""})) {
            setPitchIfLocal(!preserve_pitch);
            root_selector = 2;
            bottom_selector = 1;
            speed_focus = 2;
            return true;
        }
        if (mainKey(e, "help", {"h", "H", "р", "Р"})) {
            const DownloadSnapshot active_download = controller.downloadSnapshot();
            if (active_download.state == DownloadState::Running) {
                show_activity = true;
                show_download_panel = true;
                show_download_pool = !show_download_pool;
                command_status = show_download_pool
                    ? "Download pool expanded"
                    : "Download pool collapsed";
            }
            return true;
        }
        if (mainKey(e, "equalizer", {"e", "E", "у", "У"})) {
            show_eq_popup = true;
            eq_selected = 0;
            return true;
        }
        if (!move_to_mode && mainKey(e, "sort_title", {"1"})) {
            toggleTrackSort(TrackSortColumn::Title);
            return true;
        }

        if (!move_to_mode && mainKey(e, "telegram", {"t", "T", "е", "Е"})) {
            if (!controller.config().telegram.enabled) {
                command_status = "Telegram disabled in config";
                return true;
            }
            std::string telegram_root = controller.telegramRootPath();
            controller.scanDirectory(telegram_root, true);
            expanded_directory = telegram_root;
            preferred_directory = telegram_root;
            state.selectedTrack = 0;
            state.focus = FocusPane::Directories;
            browser_selector = 0;
            root_selector = 0;
            command_status = "Telegram";
            return true;
        }

        if (!move_to_mode && mainKey(e, "sort_time", {"2"})) {
            toggleTrackSort(TrackSortColumn::Time);
            return true;
        }

        if (!move_to_mode && mainKey(e, "sort_bpm", {"3"})) {
            toggleTrackSort(TrackSortColumn::Bpm);
            return true;
        }

        if (!move_to_mode && mainKey(e, "sort_key", {"4"})) {
            toggleTrackSort(TrackSortColumn::Key);
            return true;
        }

        if (!move_to_mode && mainKey(e, "sort_kbps", {"5"})) {
            toggleTrackSort(TrackSortColumn::Bitrate);
            return true;
        }
        
        if (!move_to_mode && mainKey(e, "sort_size", {"6"})) {
            toggleTrackSort(TrackSortColumn::Size);
            return true;
        }

        if (!move_to_mode && mainKey(e, "sort_rate", {"7"})) {
            toggleTrackSort(TrackSortColumn::SampleRate);
            return true;
        }

        if (root_selector == 1 && mainKey(e, "left", {"left"})) {
            return seekPlaybackBySeconds(-5.0);
        }
        if (root_selector == 1 && mainKey(e, "right", {"right"})) {
            return seekPlaybackBySeconds(5.0);
        }
        if (mainKey(e, "seek_back_15", {",", "б"})) {
            return seekPlaybackBySeconds(-15.0);
        }
        if (mainKey(e, "seek_forward_15", {".", "ю"})) {
            return seekPlaybackBySeconds(15.0);
        }
        if (mainKey(e, "seek_back_30", {"<", "Б"})) {
            return seekPlaybackBySeconds(-30.0);
        }
        if (mainKey(e, "seek_forward_30", {">", "Ю"})) {
            return seekPlaybackBySeconds(30.0);
        }

        if (root_selector == 2 && bottom_selector == 1) {
            if (mainKey(e, "left", {"left"})) {
                if (speed_focus == 1) {
                    int next = std::max(speed_min, speed_value - speed_step);
                    if (setSpeedIfLocal((double)next / 100.0)) {
                        speed_value = next;
                    }
                }
                return true;
            }
            if (mainKey(e, "right", {"right"})) {
                if (speed_focus == 1) {
                    int next = std::min(speed_max, speed_value + speed_step);
                    if (setSpeedIfLocal((double)next / 100.0)) {
                        speed_value = next;
                    }
                }
                return true;
            }
            if (mainKey(e, "up", {"up"})) {
                if (speed_focus > 0) {
                    speed_focus--;
                } else {
                    root_selector = 0;
                }
                return true;
            }
            if (mainKey(e, "down", {"down"})) {
                if (speed_focus < 2) {
                    speed_focus++;
                } else {
                    root_selector = 0;
                }
                return true;
            }
            if (mainKey(e, "play", {"enter"})) {
                if (speed_focus == 0) {
                    if (setSpeedIfLocal(1.0)) {
                        speed_value = 100;
                    }
                } else if (speed_focus == 2) {
                    setPitchIfLocal(!preserve_pitch);
                }
                return true;
            }
        }

        if (root_selector == 1 && mainKey(e, "down", {"down"})) {
            root_selector = 0;
            return true;
        }

        if (mainKey(e, "left", {"left"}) && root_selector == 0) {
            if (browser_selector == 1) {
                state.focus = FocusPane::Directories;
                browser_selector = 0;
            } else {
                goToParentDirectory();
            }
            root_selector = 0;
            return true;
        }

        if (mainKey(e, "right", {"right"}) && root_selector == 0) {
            if (browser_selector == 0) {
                bool selected_directory_is_open =
                    !expanded_directory.empty() &&
                    state.selectedDirectory >= 0 &&
                    state.selectedDirectory < (int)dir_paths.size() &&
                    dir_paths[state.selectedDirectory] == controller.currentPath() &&
                    dir_paths[state.selectedDirectory] == expanded_directory;
                if (selected_directory_is_open && !move_to_mode) {
                    state.focus = FocusPane::Tracks;
                    browser_selector = 1;
                } else {
                    openSelectedDirectory();
                }
            } else if (browser_selector == 1 && !move_to_mode) {
                playSelectedTrack();
            }
            return true;
        }

        if (mainKey(e, "switch_pane", {"tab"}) && root_selector == 0) {
            if (move_to_mode) {
                return true;
            }
            browser_selector = browser_selector == 0 ? 1 : 0;
            state.focus = browser_selector == 0 ? FocusPane::Directories : FocusPane::Tracks;
            return true;
        }

        if (mainKey(e, "play", {"enter"})) {
            if (root_selector == 0 && browser_selector == 0) {
                if (move_to_mode) {
                    moveTrackToSelectedDirectory();
                    return true;
                }
                openSelectedDirectory();
                return true;
            }
            if (root_selector == 0 && browser_selector == 1) {
                playSelectedTrack();
                return true;
            }
        }

        if (!move_to_mode && mainKey(e, "context_menu", {"menu"}) &&
            root_selector == 0 &&
            ((browser_selector == 0 && state.selectedDirectory >= 0 &&
              state.selectedDirectory < (int)dir_paths.size()) ||
             (browser_selector == 1 && state.selectedTrack >= 0 &&
              state.selectedTrack < (int)visible_files.size()))) {
            context_menu_for_track = browser_selector == 1;
            context_menu_selected = 0;
            show_context_menu = true;
            return true;
        }

        if (mainKey(e, "move", {"d", "D", "в", "В"}) &&
            root_selector == 0 &&
            browser_selector == 1 &&
            state.selectedTrack >= 0 &&
            state.selectedTrack < (int)visible_files.size()) {
            const Track& selected_track = visible_files[state.selectedTrack];
            const bool online_track = selected_track.id.starts_with("https://") ||
                selected_track.id.starts_with("http://");
            if (online_track) {
                if (controller.isOnlineTrackDownloaded(selected_track)) {
                    command_status = "Track is already downloaded";
                    return true;
                }
                beginSearchDownload({selected_track});
                return true;
            }
            move_track = selected_track;
            move_to_mode = true;
            command_status = "Move: " + move_track.title +
                " | Enter place | Esc cancel";
            state.focus = FocusPane::Directories;
            browser_selector = 0;
            root_selector = 0;
            return true;
        }

        if (!move_to_mode &&
            mainKey(e, "drag", {"g", "G", "п", "П"}) &&
            root_selector == 0 &&
            browser_selector == 1 &&
            state.selectedTrack >= 0 &&
            state.selectedTrack < (int)visible_files.size()) {
            std::string error;
            const Track& track = visible_files[state.selectedTrack];
            if (controller.startExternalDrag(track, error)) {
                command_status = "Drag ready: " + track.title;
            } else {
                command_status = "Error: " + error;
            }
            return true;
        }

        if (!move_to_mode &&
            mainKey(e, "open_folder", {"o", "O", "щ", "Щ"}) &&
            root_selector == 0) {
            std::string folder = controller.currentPath();
            if (browser_selector == 1 &&
                state.selectedTrack >= 0 &&
                state.selectedTrack < (int)visible_files.size()) {
                const Track& track = visible_files[(size_t)state.selectedTrack];
                if (track.id.starts_with("https://") || track.id.starts_with("http://")) {
                    command_status = "Open folder: online tracks have no local folder";
                    return true;
                }
                folder = fs::path(track.id).parent_path().string();
            } else if (browser_selector == 0 &&
                state.selectedDirectory >= 0 &&
                state.selectedDirectory < (int)dir_paths.size() &&
                fs::is_directory(dir_paths[state.selectedDirectory])) {
                folder = dir_paths[state.selectedDirectory];
            }

            std::string error;
            if (controller.openFolderExternally(folder, error)) {
                command_status = "Opened folder: " + displayName(folder);
            } else {
                command_status = "Error: " + error;
            }
            return true;
        }

        if (!move_to_mode &&
            mainKey(e, "find", {"f", "F", "а", "А"}) &&
            root_selector == 0) {
            startAudDFind();
            return true;
        }

        if (!move_to_mode &&
            mainKey(e, "demucs", {"s", "S", "ы", "Ы"}) &&
            root_selector == 0 &&
            browser_selector == 1 &&
            state.selectedTrack >= 0 &&
            state.selectedTrack < (int)visible_files.size()) {
            const Track& track = visible_files[state.selectedTrack];
            pending_stem_track = track;
            pending_stem_config = controller.config().demucs;
            stem_option_row = 0;
            stem_option_col = pending_stem_config.stems == 4 ? 1 : 0;
            pending_confirmation = PendingConfirmation::StemSeparation;
            command_status = "Confirm Demucs separation";
            return true;
        }

        if (!move_to_mode &&
            mainKey(e, "normalize", {"n", "N", "т", "Т"}) &&
            root_selector == 0) {
            pending_process_track = Track{};
            pending_process_all_folder = true;
            if (browser_selector == 1 &&
                state.selectedTrack >= 0 &&
                state.selectedTrack < (int)visible_files.size()) {
                pending_process_track = visible_files[state.selectedTrack];
                pending_process_all_folder = false;
            }
            pending_normalize_lufs = -14;
            pending_normalize_mode = "Short-Term Max";
            normalize_option_row = 0;
            normalize_option_col = 1;
            pending_confirmation = PendingConfirmation::Normalization;
            command_status = "Confirm normalization";
            return true;
        }

        if (!move_to_mode &&
            mainKey(e, "convert", {"c", "C", "с", "С"}) &&
            root_selector == 0) {
            pending_process_track = Track{};
            pending_process_all_folder = true;
            if (browser_selector == 1 &&
                state.selectedTrack >= 0 &&
                state.selectedTrack < (int)visible_files.size()) {
                pending_process_track = visible_files[state.selectedTrack];
                pending_process_all_folder = false;
            }
            pending_convert_format = "mp3";
            convert_option_row = 0;
            convert_option_col = 1;
            pending_confirmation = PendingConfirmation::Convert;
            command_status = "Confirm convert";
            return true;
        }

        if (!move_to_mode &&
            mainKey(e, "auto_cue", {"x", "X", "ч", "Ч"}) &&
            root_selector == 0) {
            pending_auto_cue_track = Track{};
            if (browser_selector == 1 &&
                state.selectedTrack >= 0 &&
                state.selectedTrack < (int)visible_files.size()) {
                pending_auto_cue_track = visible_files[state.selectedTrack];
            }
            auto_cue_option_col = pending_auto_cue_track.id.empty() ? 1 : 0;
            pending_confirmation = PendingConfirmation::AutoCue;
            command_status = "Auto Cue";
            return true;
        }

        if (!move_to_mode &&
            mainKey(e, "manual_cues", {"z", "Z", "я", "Я"}) &&
            root_selector == 0 &&
            browser_selector == 1) {
            if (state.selectedTrack >= 0 &&
                state.selectedTrack < (int)visible_files.size()) {
                prepareEditor(visible_files[state.selectedTrack],
                              EditorPrepareTarget::ManualCues);
            } else {
                command_status = "Select a track for manual cues";
            }
            return true;
        }

        if (!move_to_mode &&
            mainKey(e, "sync_cues", {"w", "W", "ц", "Ц"}) &&
            root_selector == 0 &&
            browser_selector == 1) {
            if (state.selectedTrack >= 0 &&
                state.selectedTrack < (int)visible_files.size()) {
                pending_cue_sync_track = visible_files[state.selectedTrack];
                pending_cue_sync_direction = CueSyncDirection::SeratoToTraktor;
                cue_sync_option_row = 0;
                cue_sync_direction_col = 0;
                cue_sync_scope_col = 0;
                pending_confirmation = PendingConfirmation::CueSync;
                command_status = "Cue Sync";
            } else {
                command_status = "Select a track to sync cues";
            }
            return true;
        }

        if (!move_to_mode &&
            mainKey(e, "trim", {"a", "A", "ф", "Ф"}) &&
            root_selector == 0 &&
            browser_selector == 1) {
            if (state.selectedTrack >= 0 &&
                state.selectedTrack < (int)visible_files.size()) {
                prepareEditor(visible_files[state.selectedTrack],
                              EditorPrepareTarget::Trim);
            } else {
                command_status = "Select a track to trim";
            }
            return true;
        }

        if (!move_to_mode &&
            mainKey(e, "metadata", {"m", "M", "ь", "Ь"}) &&
            root_selector == 0 &&
            browser_selector == 1 &&
            state.selectedTrack >= 0 &&
            state.selectedTrack < (int)visible_files.size()) {
            const Track& track = visible_files[state.selectedTrack];
            metadata_title = track.title;
            metadata_details = controller.metadataDetails(track);
            metadata_offset = 0;
            show_metadata_popup = true;
            command_status = "Metadata: " + track.title;
            return true;
        }

        if (mainKey(e, "toggle_playback", {"space"})) {
            togglePlayback();
            return true;
        }

        if (mainKey(e, "previous", {"[", "х", "Х"})) {
            controller.playPreviousTrack();
            return true;
        }

        if (mainKey(e, "next", {"]", "ъ", "Ъ"})) {
            controller.playNextTrack();
            return true;
        }
        if (mainKey(e, "record", {"F8"})) {
            toggleDesktopRecording();
            return true;
        }
        if (mainKey(e, "refresh", {"r", "R", "к", "К"})) {
            controller.scanDirectory(controller.currentPath(), true);
            return true;
        }

        if (!move_to_mode && mainKey(e, "export_library", {"l", "L", "д", "Д"})) {
            std::string result;
            command_status = "Exporting library";
            if (controller.exportLibrary(result, true)) {
                command_status = result;
            } else {
                command_status = "Export error: " + result;
            }
            return true;
        }

        if (!move_to_mode && mainKey(e, "import_library", {"u", "U", "г", "Г"})) {
            std::string result;
            command_status = "Importing JSON sync";
            if (controller.importChangedJson(result)) {
                command_status = result;
                controller.scanDirectory(controller.currentPath(), true);
            } else {
                command_status = "Import error: " + result;
            }
            return true;
        }

        if (!move_to_mode &&
            mainKey(e, "validate_library_export", {"k", "K", "л", "Л"})) {
            std::string result;
            if (controller.validateLibraryExport(result)) {
                command_status = result;
            } else {
                command_status = "Export check error: " + result;
            }
            return true;
        }

        if (mainKey(e, "playback_mode", {"p", "P", "з", "З"})) {
            controller.cyclePlaybackMode();
            return true;
        }

        if (mainKey(e, "stop", {"o", "O", "щ", "Щ"})) {
            controller.stopPlayback();
            return true;
        }

        if (mainKey(e, "delete", {"backspace", "delete"})) {
            if (root_selector != 0) {
                return false;
            }

            if (browser_selector == 1 &&
                state.selectedTrack >= 0 &&
                state.selectedTrack < (int)visible_files.size()) {
                pending_delete_entry = visible_files[state.selectedTrack];
            } else if (browser_selector == 0 &&
                       state.selectedDirectory >= 0 &&
                       state.selectedDirectory < (int)dir_paths.size()) {
                pending_delete_entry = Track{};
                pending_delete_entry.id = dir_paths[state.selectedDirectory];
                pending_delete_entry.title = displayName(pending_delete_entry.id);
                pending_delete_entry.type = EntryType::Directory;
            } else {
                command_status = "Select a track or folder to delete";
                return true;
            }

            pending_confirmation = PendingConfirmation::DeleteEntry;
            command_status = "Confirm delete: " + pending_delete_entry.title;
            return true;
        }

        if (mainKey(e, "volume_down", {"-", "_"})) {
            controller.volumeDown();
            return true;
        }

        if (mainKey(e, "volume_up", {"=", "+"})) {
            controller.volumeUp();
            return true;
        }

        if (mainKey(e, "toggle_activity", {"y", "Y", "н", "Н"})) {
            show_activity = true;
            show_download_panel = true;
            show_download_pool = !show_download_pool;
            return true;
        }

        if (mainKey(e, "quit", {"q", "Q", "й", "Й"})) {
            pending_confirmation = PendingConfirmation::Quit;
            command_status = "Confirm quit";
            return true;
        }

        if (mainKey(e, "down", {"down"})) {
            if (move_to_mode) {
                moveBrowserSelection(1, 0);
                return true;
            }
            if (root_selector == 0) {
                if (moveBrowserSelection(1, browser_selector)) {
                    return true;
                }
                return true;
            }
        }

        if (mainKey(e, "up", {"up"})) {
            if (move_to_mode) {
                moveBrowserSelection(-1, 0);
                return true;
            }
            if (root_selector == 0) {
                if (moveBrowserSelection(-1, browser_selector)) {
                    return true;
                }
                return true;
            }
        }

        return false;
    });

    std::thread refresh_thread([&]() {
        bool playback_was_active = false;
        bool stems_was_active = false;
        bool audio_process_was_active = false;
        bool auto_cue_was_active = false;
        bool recording_was_active = false;
        auto last_metadata_refresh = std::chrono::steady_clock::now() -
            std::chrono::seconds(1);
        int ui_fps = std::clamp(controller.config().fps, 1, 30);
        auto manual_frame_delay = std::chrono::milliseconds(
            std::max(1, 1000 / ui_fps));
        while (refresh_running.load()) {
            bool manual_active =
                manual_refresh_active.load() || trim_refresh_active.load();
            std::this_thread::sleep_for(manual_active
                ? manual_frame_delay
                : std::chrono::milliseconds(250));
            if (!refresh_running.load()) {
                break;
            }
            auto snapshot = controller.playbackSnapshot();
            auto download = controller.downloadSnapshot();
            auto stems = controller.stemSeparationSnapshot();
            auto audio_process = controller.audioProcessSnapshot();
            auto auto_cue = controller.autoCueSnapshot();
            auto recording = controller.recordingSnapshot();
            bool playback_active =
                snapshot.state == PlaybackState::Playing ||
                snapshot.state == PlaybackState::Paused;
            bool stems_active = stems.state == StemSeparationState::Running;
            bool audio_process_active =
                audio_process.state == AudioProcessState::Running;
            bool auto_cue_active = auto_cue.running;
            bool recording_active = recording.state == RecordingState::Recording;
            bool editor_prepare_running = editor_prepare_active.load();
            bool search_running = search_active.load();
            bool metadata_active = controller.metadataBusy();
            auto now = std::chrono::steady_clock::now();
            bool metadata_refresh_due =
                metadata_active &&
                now - last_metadata_refresh >= std::chrono::seconds(1);
            bool stems_finished =
                stems_was_active &&
                !stems_active &&
                (stems.state == StemSeparationState::Done ||
                 stems.state == StemSeparationState::Error);
            if (stems_finished) {
                controller.scanDirectory(controller.currentPath(), true);
            }
            if (playback_active ||
                playback_was_active ||
                download.state == DownloadState::Running ||
                stems_active ||
                stems_was_active ||
                audio_process_active ||
                audio_process_was_active ||
                auto_cue_active ||
                auto_cue_was_active ||
                recording_active ||
                recording_was_active ||
                manual_active ||
                editor_prepare_running ||
                search_running ||
                controller.directoryScanBusy() ||
                metadata_refresh_due) {
                if (metadata_refresh_due) {
                    last_metadata_refresh = now;
                }
                screen.Post([&screen] {
                    screen.PostEvent(Event::Custom);
                });
            } else {
                // Keep the native macOS event loop alive even when the
                // terminal UI is idle, so remote commands can wake TPlay.
                screen.Post([&screen] {
                    screen.PostEvent(Event::Custom);
                });
            }
            playback_was_active = playback_active;
            stems_was_active = stems_active;
            audio_process_was_active = audio_process_active;
            auto_cue_was_active = auto_cue_active;
            recording_was_active = recording_active;
        }
    });

    screen.Loop(component);
    if (editor_prepare_worker.joinable()) {
        editor_prepare_worker.request_stop();
        editor_prepare_worker.join();
    }
    if (search_worker.joinable()) {
        search_worker.request_stop();
        search_worker.join();
    }
    if (audd_find_worker.joinable()) {
        audd_find_worker.request_stop();
        controller.stopDesktopRecording();
        audd_find_worker.join();
    }
    if (online_play_worker.joinable()) {
        online_play_worker.request_stop();
        online_play_worker.join();
    }
    if (dependency_update_worker.joinable()) {
        dependency_update_worker.request_stop();
        dependency_update_worker.join();
    }
    refresh_running = false;
    refresh_thread.join();
    return 0;
}
