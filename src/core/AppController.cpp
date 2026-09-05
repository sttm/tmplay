#include "AppController.hpp"

#include "ProcessRunner.hpp"
#include "../Sync/JsonSync.h"
#include "../Sync/ConflictResolver.h"
#include "../Export/ExportValidator.h"
#include "../Library/LibraryJson.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <thread>
#include <utility>

#include <sqlite3.h>

namespace fs = std::filesystem;

namespace {

constexpr const char* kTelegramRoot = "telegram://root";
constexpr const char* kTelegramChatPrefix = "telegram://chat/";
constexpr const char* kTelegramItemPrefix = "telegram://item/";
constexpr const char* kPlaylistPrefix = "playlist://";
constexpr const char* kSearchRoot = "search://root";
constexpr const char* kRecentSearch = "search://recent";
constexpr const char* kSearchHistoryPrefix = "search://history/";
constexpr size_t kSearchHistoryLimit = 20;

bool startsWith(const std::string& value, const std::string& prefix)
{
    return value.rfind(prefix, 0) == 0;
}

bool youtubeNeedsCookies(const std::string& output)
{
    std::string normalized = output;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return normalized.find("sign in to confirm") != std::string::npos ||
           normalized.find("not a bot") != std::string::npos ||
           normalized.find("use --cookies-from-browser") != std::string::npos ||
           normalized.find("--cookies for the authentication") != std::string::npos;
}

bool hasCookiesFile(const std::string& path)
{
    std::error_code error;
    return !path.empty() && fs::is_regular_file(path, error);
}

void appendCookiesFile(std::vector<std::string>& arguments,
                       const std::string& path)
{
    const auto separator = std::find(arguments.begin(), arguments.end(), "--");
    arguments.insert(separator, {"--cookies", path});
}

int runYtDlpWithCookiesFileFallback(std::vector<std::string> arguments,
                                    const std::string& cookies_path,
                                    std::string* output)
{
    int exit_code = ProcessRunner::runWithCombinedOutput(arguments, output);
    if (exit_code != 0 && output != nullptr && youtubeNeedsCookies(*output) &&
        hasCookiesFile(cookies_path)) {
        appendCookiesFile(arguments, cookies_path);
        output->clear();
        exit_code = ProcessRunner::runWithCombinedOutput(arguments, output);
    }
    return exit_code;
}

std::optional<std::string> preferredYtDlp()
{
    if (auto system = ProcessRunner::findSystemExecutable("yt-dlp")) {
        return system;
    }
    return ProcessRunner::findExecutable("yt-dlp");
}

bool hasUsableGenre(const std::string& value)
{
    std::string normalized = ProcessRunner::trim(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    // YouTube frequently writes the generic "Music" tag.  It is not genre
    // metadata, so allow the Discogs classifier to replace it in the normal
    // background scan instead of requiring the user to press Analyze.
    return !normalized.empty() && normalized != "-" && normalized != "unknown" &&
           normalized != "none" && normalized != "n/a" && normalized != "music";
}

std::string lowerExtension(const fs::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return ext;
}

std::string cueSafeExtensionForCueWrite(const fs::path& path)
{
    std::string ext = lowerExtension(path);
    if (ext == ".wav") {
        return ".flac";
    }
    if (ext == ".m4a" || ext == ".mp4" ||
        ext == ".aac" || ext == ".alac") {
        return ".mp3";
    }
    return {};
}

bool isCueSafeExtension(const fs::path& path)
{
    std::string ext = lowerExtension(path);
    return ext == ".mp3" || ext == ".flac";
}

std::string compactProcessOutput(std::string value)
{
    value = ProcessRunner::trim(std::move(value));
    std::replace(value.begin(), value.end(), '\n', ' ');
    std::replace(value.begin(), value.end(), '\r', ' ');
    while (value.find("  ") != std::string::npos) {
        value.erase(value.find("  "), 1);
    }
    if (value.size() > 260) {
        value = "..." + value.substr(value.size() - 257);
    }
    return value;
}

std::string percentEncodeQuery(const std::string& value)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (unsigned char character : value) {
        if (std::isalnum(character) || character == '-' || character == '_' ||
            character == '.' || character == '~') {
            encoded.push_back((char)character);
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[character >> 4]);
            encoded.push_back(hex[character & 0x0f]);
        }
    }
    return encoded;
}

std::string mediumThumbnailUrl(std::string url)
{
    // YouTube's default URLs expose stable size names.  hqdefault is the
    // practical middle ground: suitable for embedded covers without saving a
    // max-resolution image for every downloaded track.
    const auto replace = [&](const std::string& from, const std::string& to) {
        std::size_t position = url.find(from);
        if (position != std::string::npos) {
            url.replace(position, from.size(), to);
        }
    };
    replace("maxresdefault.jpg", "hqdefault.jpg");
    replace("sddefault.jpg", "hqdefault.jpg");
    replace("/default.jpg", "/hqdefault.jpg");

    // YTMusic song cards often contain only a 120×120 thumbnail while album
    // cards use 544×544. Control Center may discard the tiny artwork, so ask
    // Google's image endpoint for the same cover at the album-card size.
    if (url.find("googleusercontent.com") != std::string::npos) {
        const std::size_t width = url.find("=w");
        const std::size_t height = width == std::string::npos
            ? std::string::npos : url.find("-h", width + 2);
        const std::size_t suffix = height == std::string::npos
            ? std::string::npos : url.find('-', height + 2);
        if (width != std::string::npos && height != std::string::npos &&
            suffix != std::string::npos) {
            url.replace(width, suffix - width, "=w544-h544");
        }
    }
    return url;
}

std::string decodeBase64(const std::string& value)
{
    static constexpr unsigned char kInvalid = 0xff;
    static const std::array<unsigned char, 256> table = [] {
        std::array<unsigned char, 256> result{};
        result.fill(kInvalid);
        for (unsigned char index = 0; index < 26; ++index) {
            result[(unsigned char)('A' + index)] = index;
            result[(unsigned char)('a' + index)] = index + 26;
        }
        for (unsigned char index = 0; index < 10; ++index) {
            result[(unsigned char)('0' + index)] = index + 52;
        }
        result[(unsigned char)'+'] = 62;
        result[(unsigned char)'/'] = 63;
        return result;
    }();

    std::string decoded;
    int buffer = 0;
    int bits = -8;
    for (unsigned char character : value) {
        if (character == '=') break;
        const unsigned char part = table[character];
        if (part == kInvalid) return {};
        buffer = (buffer << 6) | part;
        bits += 6;
        if (bits >= 0) {
            decoded.push_back((char)((buffer >> bits) & 0xff));
            bits -= 8;
        }
    }
    return decoded;
}

std::vector<std::string> bridgeFields(const std::string& line)
{
    std::vector<std::string> fields;
    std::istringstream stream(line);
    for (std::string field; std::getline(stream, field, '\t');) {
        fields.push_back(std::move(field));
    }
    return fields;
}

std::optional<fs::path> ytmusicBridgePath()
{
    const std::array<fs::path, 2> candidates = {
        fs::path(ProcessRunner::executableDirectory()) / "scripts" / "ytmusic_bridge.py",
        fs::current_path() / "scripts" / "ytmusic_bridge.py",
    };
    for (const auto& candidate : candidates) {
        std::error_code error;
        if (fs::is_regular_file(candidate, error)) return candidate;
    }
    return std::nullopt;
}

std::optional<std::vector<std::string>> ytmusicBridgeCommand(
    const std::string& action,
    const std::string& value,
    std::string& error,
    int limit = 0)
{
    // Release bundles contain this self-contained executable.  Keep the
    // Python bridge fallback for developer builds and for existing users.
    if (auto bundled = ProcessRunner::findExecutable("tmplay-ytmusic")) {
        std::vector<std::string> command{*bundled, action};
        if (limit > 0) command.push_back(std::to_string(limit));
        command.push_back(value);
        return command;
    }

    auto python = ProcessRunner::findExecutable("python3");
    auto bridge = ytmusicBridgePath();
    if (!python || !bridge) {
        error = !python
            ? "Bundled YouTube Music bridge is missing (python3 was not found)"
            : "Bundled YouTube Music bridge is missing";
        return std::nullopt;
    }
    std::vector<std::string> command{*python, bridge->string(), action};
    if (limit > 0) command.push_back(std::to_string(limit));
    command.push_back(value);
    return command;
}

struct OfficialAlbumSearchResult {
    std::string id;
    std::string title;
    std::string artist;
    std::string year;
    std::string thumbnailUrl;
    std::vector<Track> tracks;
};

bool searchOfficialAlbums(const std::string& query,
                          std::vector<OfficialAlbumSearchResult>& albums,
                          std::string& error)
{
    auto command = ytmusicBridgeCommand("search-albums", query, error);
    if (!command) return false;
    std::string output;
    const int exit_code = ProcessRunner::run(*command, &output);

    std::map<std::string, size_t> albums_by_id;
    std::istringstream lines(output);
    for (std::string line; std::getline(lines, line);) {
        const auto fields = bridgeFields(line);
        if (fields.empty()) continue;
        if (fields[0] == "ERROR" && fields.size() == 2) {
            error = decodeBase64(fields[1]);
            continue;
        }
        if (fields[0] == "ALBUM" && fields.size() == 6) {
            OfficialAlbumSearchResult album;
            album.id = decodeBase64(fields[1]);
            album.title = decodeBase64(fields[2]);
            album.artist = decodeBase64(fields[3]);
            album.year = decodeBase64(fields[4]);
            album.thumbnailUrl = decodeBase64(fields[5]);
            if (album.id.empty() || album.title.empty()) continue;
            albums_by_id.emplace(album.id, albums.size());
            albums.push_back(std::move(album));
            continue;
        }
        if (fields[0] != "TRACK" || fields.size() != 11) continue;
        const std::string album_id = decodeBase64(fields[1]);
        const auto album = albums_by_id.find(album_id);
        if (album == albums_by_id.end()) continue;
        Track track;
        try {
            track.duration = std::stod(decodeBase64(fields[6]));
        } catch (...) {
            track.duration = 0.0;
        }
        const std::string video_id = decodeBase64(fields[10]);
        if (video_id.empty()) continue;
        track.id = "https://www.youtube.com/watch?v=" + video_id;
        track.title = decodeBase64(fields[3]);
        track.artist = decodeBase64(fields[4]);
        track.album = decodeBase64(fields[5]);
        track.isExplicit = decodeBase64(fields[7]) == "1";
        track.genre = decodeBase64(fields[8]);
        track.thumbnailUrl = mediumThumbnailUrl(decodeBase64(fields[9]));
        track.type = EntryType::File;
        track.status = TrackStatus::Ready;
        albums[album->second].tracks.push_back(std::move(track));
    }
    if (!albums.empty()) return true;
    if (error.empty()) {
        error = exit_code == 0 ? "No official albums with verified Art Tracks found"
                               : "YouTube Music search failed";
    }
    return false;
}

bool searchOfficialTracks(const std::string& query,
                          int limit,
                          std::vector<Track>& tracks,
                          std::string& error)
{
    auto command = ytmusicBridgeCommand(
        "search-tracks", query, error, std::clamp(limit, 1, 50));
    if (!command) return false;
    std::string output;
    const int exit_code = ProcessRunner::run(*command, &output);
    std::set<std::string> seen_ids;
    std::istringstream lines(output);
    for (std::string line; std::getline(lines, line);) {
        const auto fields = bridgeFields(line);
        if (fields.empty()) continue;
        if (fields[0] == "ERROR" && fields.size() == 2) {
            error = decodeBase64(fields[1]);
            continue;
        }
        // SONG: video id, title, artist, album, album browse id, duration,
        // explicit, genre, artwork, year. Accept the prior ten-field bridge
        // too, so a developer build with an older copied script still shows
        // its search results (only exact Open album needs the new browse id).
        if (fields[0] != "SONG" ||
            (fields.size() != 11 && fields.size() != 10)) continue;
        const bool has_album_id = fields.size() == 11;
        const std::string video_id = decodeBase64(fields[1]);
        if (video_id.empty() || !seen_ids.insert(video_id).second) continue;
        Track track;
        track.id = "https://www.youtube.com/watch?v=" + video_id;
        track.title = decodeBase64(fields[2]);
        track.artist = decodeBase64(fields[3]);
        track.album = decodeBase64(fields[4]);
        try {
            track.duration = std::stod(decodeBase64(fields[has_album_id ? 6 : 5]));
        } catch (...) {
            track.duration = 0.0;
        }
        if (has_album_id) track.sourceId = decodeBase64(fields[5]);
        track.isExplicit = decodeBase64(fields[has_album_id ? 7 : 6]) == "1";
        track.genre = decodeBase64(fields[has_album_id ? 8 : 7]);
        track.thumbnailUrl = mediumThumbnailUrl(
            decodeBase64(fields[has_album_id ? 9 : 8]));
        track.releaseDate = decodeBase64(fields[has_album_id ? 10 : 9]);
        track.type = EntryType::File;
        track.status = TrackStatus::Ready;
        tracks.push_back(std::move(track));
    }
    if (!tracks.empty()) return true;
    if (error.empty()) {
        error = exit_code == 0 ? "No YouTube Music song results found"
                               : "YouTube Music song search failed";
    }
    return false;
}

bool loadOfficialAlbumTracks(const std::string& browse_id,
                             OfficialAlbumSearchResult& album,
                             std::string& error)
{
    auto command = ytmusicBridgeCommand("album-tracks", browse_id, error);
    if (!command) return false;
    std::string output;
    const int exit_code = ProcessRunner::run(*command, &output);

    std::istringstream lines(output);
    for (std::string line; std::getline(lines, line);) {
        const auto fields = bridgeFields(line);
        if (fields.empty()) continue;
        if (fields[0] == "ERROR" && fields.size() == 2) {
            error = decodeBase64(fields[1]);
            continue;
        }
        if (fields[0] == "ALBUM" && fields.size() == 6) {
            album.id = decodeBase64(fields[1]);
            album.title = decodeBase64(fields[2]);
            album.artist = decodeBase64(fields[3]);
            album.year = decodeBase64(fields[4]);
            album.thumbnailUrl = decodeBase64(fields[5]);
            continue;
        }
        if (fields[0] != "TRACK" || fields.size() != 11) continue;
        const std::string video_id = decodeBase64(fields[10]);
        if (video_id.empty()) continue;
        Track track;
        track.id = "https://www.youtube.com/watch?v=" + video_id;
        track.title = decodeBase64(fields[3]);
        track.artist = decodeBase64(fields[4]);
        track.album = decodeBase64(fields[5]);
        try {
            track.duration = std::stod(decodeBase64(fields[6]));
        } catch (...) {
            track.duration = 0.0;
        }
        track.isExplicit = decodeBase64(fields[7]) == "1";
        track.genre = decodeBase64(fields[8]);
        track.thumbnailUrl = mediumThumbnailUrl(decodeBase64(fields[9]));
        track.type = EntryType::File;
        track.status = TrackStatus::Ready;
        album.tracks.push_back(std::move(track));
    }
    if (!album.tracks.empty()) return true;
    if (error.empty()) {
        error = exit_code == 0 ? "No verified YouTube Music Art Tracks in this album"
                               : "YouTube Music album lookup failed";
    }
    return false;
}

std::string searchDuplicateKey(std::string value)
{
    value = ProcessRunner::trim(std::move(value));
    if (value.empty() || value == "NA" || value == "None" || value == "null") {
        return {};
    }
    std::string result;
    bool in_annotation = false;
    for (unsigned char character : value) {
        if (character == '(' || character == '[') {
            in_annotation = true;
            continue;
        }
        if (character == ')' || character == ']') {
            in_annotation = false;
            continue;
        }
        if (!in_annotation && std::isalnum(character)) {
            result.push_back((char)std::tolower(character));
        }
    }
    static constexpr std::string_view noise[] = {
        "official", "audio", "lyrics", "lyric", "video", "musicvideo", "hd",
    };
    for (const auto word : noise) {
        std::size_t position = 0;
        while ((position = result.find(word, position)) != std::string::npos) {
            result.erase(position, word.size());
        }
    }
    return result;
}

bool isTopicAutoGeneratedName(const std::string& value)
{
    std::string normalized = ProcessRunner::trim(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char character) {
                       return (char)std::tolower(character);
                   });
    static constexpr std::string_view suffix = "- topic";
    return normalized == "topic" ||
        (normalized.size() >= suffix.size() &&
         normalized.compare(normalized.size() - suffix.size(), suffix.size(),
                            suffix) == 0);
}

struct StreamUrl {
    std::string url;
    std::string format;
    double durationSeconds = 0.0;
    double bitrateKbps = 0.0;
    double sampleRateHz = 0.0;
    std::uintmax_t sizeBytes = 0;
};

std::optional<StreamUrl> resolveStreamUrl(
    const std::string& source,
    const std::vector<std::string>& cookieBrowsers,
    const std::string& cookiesPath)
{
    auto yt_dlp = preferredYtDlp();
    if (!yt_dlp) return std::nullopt;

    std::string last_output;
    auto resolve = [&](const std::optional<std::string>& browser,
                       const std::optional<std::string>& cookies_path) {
        std::vector<std::string> arguments = {
            *yt_dlp,
            "--ignore-config", "--no-plugin-dirs",
            "--no-playlist",
            "--no-warnings",
            // The C++ FFmpeg online decoder accepts the actual best source,
            // including YouTube WebM/Opus.
            "-f", "bestaudio",
            // Some YouTube Music search cards have no duration. yt-dlp has
            // already resolved the item here, so read its authoritative
            // duration in the same invocation for the transport UI.
            "--print", "%(ext)s\t%(abr)s\t%(filesize_approx)s\t%(asr)s\t%(duration)s",
            "-g",
        };
        if (browser && !browser->empty()) {
            arguments.push_back("--cookies-from-browser");
            arguments.push_back(*browser);
        }
        if (cookies_path && !cookies_path->empty()) {
            arguments.push_back("--cookies");
            arguments.push_back(*cookies_path);
        }
        arguments.push_back("--");
        arguments.push_back(source);
        std::string output;
        if (ProcessRunner::runWithCombinedOutput(arguments, &output) != 0) {
            last_output = std::move(output);
            return std::optional<StreamUrl>{};
        }
        StreamUrl stream;
        std::istringstream lines(output);
        for (std::string line; std::getline(lines, line);) {
            line = ProcessRunner::trim(line);
            if (startsWith(line, "https://") || startsWith(line, "http://")) {
                stream.url = std::move(line);
                continue;
            }
            std::vector<std::string> fields;
            std::istringstream fieldStream(line);
            for (std::string field; std::getline(fieldStream, field, '\t');) {
                fields.push_back(std::move(field));
            }
            if (fields.size() >= 4) {
                stream.format = fields[0] == "NA" ? "" : fields[0];
                try { stream.bitrateKbps = std::stod(fields[1]); } catch (...) {}
                try { stream.sizeBytes = (std::uintmax_t)std::stoull(fields[2]); } catch (...) {}
                try { stream.sampleRateHz = std::stod(fields[3]); } catch (...) {}
                if (fields.size() >= 5) {
                    try { stream.durationSeconds = std::stod(fields[4]); } catch (...) {}
                }
            }
        }
        return stream.url.empty()
            ? std::optional<StreamUrl>{}
            : std::optional<StreamUrl>{std::move(stream)};
    };

    if (auto stream = resolve(std::nullopt, std::nullopt)) return stream;
    if (!youtubeNeedsCookies(last_output)) return std::nullopt;
    if (hasCookiesFile(cookiesPath)) {
        if (auto stream = resolve(std::nullopt, cookiesPath)) return stream;
    }
    for (const auto& browser : cookieBrowsers) {
        if (auto stream = resolve(browser, std::nullopt)) return stream;
    }
    return std::nullopt;
}

bool convertForCueSafeWrite(const fs::path& input,
                            const fs::path& output,
                            std::string& error)
{
    auto ffmpeg = ProcessRunner::findExecutable("ffmpeg");
    if (!ffmpeg) {
        error = "ffmpeg not found";
        return false;
    }

    std::string target_ext = lowerExtension(output);
    std::vector<std::string> args = {
        *ffmpeg,
        "-nostdin",
        "-hide_banner",
        "-loglevel",
        "error",
        "-y",
        "-i",
        input.string(),
        "-map",
        "0:a:0",
        "-map_metadata",
        "0",
        "-vn",
    };
    if (target_ext == ".flac") {
        args.insert(args.end(), {"-c:a", "flac"});
    } else if (target_ext == ".mp3") {
        args.insert(args.end(), {"-c:a", "libmp3lame", "-b:a", "320k"});
    } else {
        args.insert(args.end(), {"-c:a", "copy"});
    }
    args.push_back(output.string());

    std::string output_text;
    int exit_code = ProcessRunner::runWithCombinedOutput(args, &output_text);
    if (exit_code != 0 || !fs::exists(output)) {
        std::error_code ec;
        fs::remove(output, ec);
        error = compactProcessOutput(output_text);
        if (error.empty()) {
            error = "cue-safe conversion failed";
        }
        return false;
    }
    error.clear();
    return true;
}

std::string telegramChatPath(const std::string& chatId)
{
    return std::string(kTelegramChatPrefix) + chatId;
}

std::string telegramItemPath(const std::string& chatId, int messageId)
{
    return std::string(kTelegramItemPrefix) + chatId + "/" + std::to_string(messageId);
}

std::optional<std::pair<std::string, int>> parseTelegramItemPath(const std::string& path)
{
    if (!startsWith(path, kTelegramItemPrefix)) {
        return std::nullopt;
    }
    std::string rest = path.substr(std::string(kTelegramItemPrefix).size());
    std::size_t slash = rest.rfind('/');
    if (slash == std::string::npos) {
        return std::nullopt;
    }
    std::string chat_id = rest.substr(0, slash);
    int message_id = 0;
    try {
        message_id = std::stoi(rest.substr(slash + 1));
    } catch (...) {
        return std::nullopt;
    }
    return std::pair<std::string, int>{chat_id, message_id};
}

}  // namespace

std::string AppController::currentPath() const {
    std::lock_guard<std::mutex> lock(currentPathMutex_);
    return currentPath_;
}

void AppController::setCurrentPath(const std::string& path) {
    std::lock_guard<std::mutex> lock(currentPathMutex_);
    currentPath_ = path;
}

std::string AppController::virtualPlaylistName(const std::string& path) const {
    std::lock_guard<std::mutex> lock(directoryCacheMutex_);
    const auto found = virtualPlaylists_.find(path);
    return found == virtualPlaylists_.end() ? std::string{} : found->second.name;
}
// ---------------- utils ----------------

bool AppController::isAllowedFormat(const std::string& ext) const {
    std::string normalized = ext;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });

    for (const auto& f : config_.formats) {
        std::string format = f;
        std::transform(format.begin(), format.end(), format.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });

        if (normalized == "." + format)
            return true;
    }
    return false;
}

bool AppController::isTelegramPath(const std::string& path) const
{
    return path == kTelegramRoot ||
           startsWith(path, kTelegramChatPrefix) ||
           startsWith(path, kTelegramItemPrefix);
}

std::string AppController::telegramRootPath() const
{
    return kTelegramRoot;
}

bool AppController::scanTelegramDirectory(const std::string& path,
                                          std::vector<Track>& tracks,
                                          std::string& error)
{
    if (!telegramInbox_) {
        error = "Telegram is not configured";
        return false;
    }

    if (path == kTelegramRoot) {
        if (config_.telegram.syncOnOpenFolder) {
            TelegramSyncSummary summary;
            std::string sync_error;
            telegramInbox_->sync(summary, sync_error);
        }

        auto chats = telegramInbox_->listChats(error);
        if (!error.empty()) {
            return false;
        }
        for (const auto& chat : chats) {
            Track t;
            t.id = telegramChatPath(chat.chatId);
            t.title = chat.title.empty() ? chat.chatId : chat.title;
            t.type = EntryType::Directory;
            t.status = TrackStatus::Ready;
            tracks.push_back(std::move(t));
        }
        return true;
    }

    if (startsWith(path, kTelegramChatPrefix)) {
        std::string chat_id = path.substr(std::string(kTelegramChatPrefix).size());
        auto items = telegramInbox_->listAudioItems(chat_id, error);
        if (!error.empty()) {
            return false;
        }
        for (const auto& item : items) {
            Track t;
            t.id = telegramItemPath(item.chatId, item.messageId);
            t.title = item.fileName.empty()
                ? (item.title.empty() ? ("Telegram " + std::to_string(item.messageId))
                                      : item.title)
                : item.fileName;
            t.duration = item.duration;
            t.sizeBytes = item.fileSize;
            t.type = EntryType::File;
            t.status = item.downloaded ? TrackStatus::Ready : TrackStatus::Downloading;
            if (item.downloaded && !item.localPath.empty()) {
                t.id = item.localPath.string();
                t.status = TrackStatus::Ready;
            }
            tracks.push_back(std::move(t));
        }
        return true;
    }

    error = "Unsupported Telegram path";
    return false;
}

bool AppController::needsMetadataScan(const Track& track) {
    if (track.type != EntryType::File || isOnlineMediaUrl(track.id)) {
        return false;
    }
    return track.bpm <= 0.0 || track.key.empty() || !hasUsableGenre(track.genre);
}

void AppController::mergeMetadataIntoTrack(Track& track,
                                           const AudioMetadata& metadata) {
    if (!metadata.title.empty()) {
        track.title = metadata.title;
    }
    if (!metadata.artist.empty()) {
        track.artist = metadata.artist;
    }
    if (!metadata.album.empty()) {
        track.album = metadata.album;
    }
    if (metadata.duration > 0.0) {
        track.duration = metadata.duration;
    }
    if (metadata.bpm > 0.0) {
        track.bpm = metadata.bpm;
    }
    if (metadata.bitrateKbps > 0.0) {
        track.bitrateKbps = metadata.bitrateKbps;
    }
    if (metadata.sampleRateHz > 0.0) {
        track.sampleRateHz = metadata.sampleRateHz;
    }
    if (metadata.sizeBytes > 0) {
        track.sizeBytes = metadata.sizeBytes;
    }
    if (!metadata.key.empty()) {
        track.key = metadata.key;
    }
    if (!metadata.genre.empty()) {
        track.genre = metadata.genre;
    }
}

std::string cueColorHex(std::uint32_t color)
{
    std::ostringstream stream;
    stream << "#" << std::hex << std::setfill('0') << std::setw(6)
           << (color & 0xffffffu);
    return stream.str();
}

std::uint32_t cueColorFromString(std::string color)
{
    if (color.starts_with("#")) {
        color.erase(color.begin());
    } else if (color.starts_with("0x") || color.starts_with("0X")) {
        color.erase(0, 2);
    }
    std::uint32_t value = 0xffffffu;
    if (color.size() == 6) {
        std::from_chars(color.data(), color.data() + color.size(), value, 16);
    }
    return value;
}

std::vector<SeratoCue> seratoCuesFromLibraryTrack(const LibraryTrack& track)
{
    std::vector<SeratoCue> cues;
    cues.reserve(track.cues.size());
    for (const auto& cue : track.cues) {
        if (cue.index < 0 || cue.index > 7 || cue.positionSeconds < 0.0) {
            continue;
        }
        cues.push_back({
            cue.index,
            cue.name,
            cue.positionSeconds,
            cueColorFromString(cue.color),
        });
    }
    std::sort(cues.begin(), cues.end(), [](const auto& a, const auto& b) {
        if (a.index != b.index) {
            return a.index < b.index;
        }
        return a.seconds < b.seconds;
    });
    return cues;
}

bool cuePositionsMatch(std::vector<SeratoCue> expected,
                       std::vector<SeratoCue> actual)
{
    auto sort_cues = [](std::vector<SeratoCue>& cues) {
        std::sort(cues.begin(), cues.end(), [](const auto& a, const auto& b) {
            if (a.index != b.index) {
                return a.index < b.index;
            }
            return a.seconds < b.seconds;
        });
    };
    sort_cues(expected);
    sort_cues(actual);
    if (expected.size() != actual.size()) {
        return false;
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (expected[i].index != actual[i].index ||
            std::abs(expected[i].seconds - actual[i].seconds) > 0.02) {
            return false;
        }
    }
    return true;
}

std::vector<SeratoCue> sortedCues(std::vector<SeratoCue> cues)
{
    std::sort(cues.begin(), cues.end(), [](const auto& a, const auto& b) {
        if (a.index != b.index) {
            return a.index < b.index;
        }
        return a.seconds < b.seconds;
    });
    return cues;
}

std::uint32_t fallbackCueColorForIndex(const AutoCueConfig& autoCue, int index)
{
    for (const auto& slot : autoCue.cues) {
        if (slot.index == index) {
            return slot.colorRgb;
        }
    }
    static constexpr std::uint32_t kFallbackColors[] = {
        0x00ff00u, 0xff0000u, 0x0080ffu, 0xff8000u,
        0xffff00u, 0x8000ffu, 0x00ffffu, 0xffffffu,
    };
    if (index < 0) {
        index = 0;
    }
    return kFallbackColors[(std::size_t)index %
                           (sizeof(kFallbackColors) / sizeof(kFallbackColors[0]))];
}

std::vector<SeratoCue> colorizeImportedTraktorCues(
    std::vector<SeratoCue> cues,
    const AutoCueConfig& autoCue)
{
    for (auto& cue : cues) {
        if ((cue.colorRgb & 0xffffffu) == 0xffffffu ||
            (cue.colorRgb & 0xffffffu) == 0x000000u) {
            cue.colorRgb = fallbackCueColorForIndex(autoCue, cue.index);
        }
    }
    return cues;
}

std::vector<SeratoCue> chooseCueSyncSource(
    const std::vector<SeratoCue>& serato,
    const std::vector<SeratoCue>& traktor,
    const std::string& prefer)
{
    if (serato.empty()) {
        return sortedCues(traktor);
    }
    if (traktor.empty()) {
        return sortedCues(serato);
    }
    if (cuePositionsMatch(serato, traktor)) {
        return sortedCues(serato);
    }
    if (prefer == "serato") {
        return sortedCues(serato);
    }
    if (prefer == "traktor") {
        return sortedCues(traktor);
    }
    return sortedCues(serato);
}

std::int64_t nowUnixSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string cueHash(std::vector<SeratoCue> cues)
{
    cues = sortedCues(std::move(cues));
    std::uint64_t hash = 1469598103934665603ull;
    auto mix = [&](unsigned char byte) {
        hash ^= byte;
        hash *= 1099511628211ull;
    };
    auto mixString = [&](const std::string& value) {
        for (unsigned char c : value) {
            mix(c);
        }
        mix(0xff);
    };
    for (const auto& cue : cues) {
        mix((unsigned char)std::clamp(cue.index, 0, 255));
        auto millis = (std::int64_t)std::llround(std::max(0.0, cue.seconds) * 1000.0);
        for (int i = 0; i < 8; ++i) {
            mix((unsigned char)((millis >> (i * 8)) & 0xff));
        }
        mixString(cue.name);
    }
    std::ostringstream stream;
    stream << std::hex << hash;
    return stream.str();
}

std::string syncStateKey(const std::string& trackId,
                         const std::string& source,
                         const std::string& field)
{
    return "cue_sync:" + trackId + ":" + source + ":" + field;
}

std::string syncStateGet(sqlite3* db, const std::string& key)
{
    if (db == nullptr) {
        return {};
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
                           "SELECT value FROM sync_state WHERE key = ? LIMIT 1;",
                           -1,
                           &stmt,
                           nullptr) != SQLITE_OK) {
        return {};
    }
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    std::string value;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* text = sqlite3_column_text(stmt, 0);
        if (text != nullptr) {
            value = (const char*)text;
        }
    }
    sqlite3_finalize(stmt);
    return value;
}

void syncStateSet(sqlite3* db, const std::string& key, const std::string& value)
{
    if (db == nullptr) {
        return;
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
                           "INSERT INTO sync_state(key, value) VALUES(?, ?) "
                           "ON CONFLICT(key) DO UPDATE SET value = excluded.value;",
                           -1,
                           &stmt,
                           nullptr) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::int64_t syncStateTime(sqlite3* db,
                           const std::string& trackId,
                           const std::string& source)
{
    std::string value = syncStateGet(db, syncStateKey(trackId, source, "time"));
    if (value.empty()) {
        return 0;
    }
    try {
        return std::stoll(value);
    } catch (...) {
        return 0;
    }
}

void syncStateTouch(sqlite3* db,
                    const std::string& trackId,
                    const std::string& source,
                    const std::string& hash,
                    std::int64_t timestamp)
{
    syncStateSet(db, syncStateKey(trackId, source, "hash"), hash);
    syncStateSet(db, syncStateKey(trackId, source, "time"), std::to_string(timestamp));
}

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

std::string safeFolderName(std::string value)
{
    value = ProcessRunner::trim(std::move(value));
    for (char& character : value) {
        if (character == '/' || character == '\\' || character == ':' ||
            character == '\0') {
            character = '-';
        }
    }
    while (!value.empty() && (value.back() == '.' || value.back() == ' ')) {
        value.pop_back();
    }
    return value.empty() || value == "." || value == ".." ? "Album" : value;
}

std::string downloadNameKey(std::string value)
{
    value = ProcessRunner::trim(std::move(value));
    // Downloaded album files are named "01. Track" to preserve release
    // order. The number is not part of the online title used for download
    // badges, so omit it while comparing names.
    size_t prefix_end = 0;
    while (prefix_end < value.size() &&
           std::isdigit((unsigned char)value[prefix_end])) {
        ++prefix_end;
    }
    if (prefix_end > 0 && prefix_end < value.size() &&
        (value[prefix_end] == '.' || value[prefix_end] == '-' ||
         std::isspace((unsigned char)value[prefix_end]))) {
        while (prefix_end < value.size() &&
               (value[prefix_end] == '.' || value[prefix_end] == '-' ||
                std::isspace((unsigned char)value[prefix_end]))) {
            ++prefix_end;
        }
        value.erase(0, prefix_end);
    }
    for (char& character : value) {
        if (character == '/' || character == '\\' || character == ':' ||
            character == '\0') {
            character = '-';
        }
        character = (char)std::tolower((unsigned char)character);
    }
    while (!value.empty() && (value.back() == '.' || value.back() == ' ')) {
        value.pop_back();
    }
    return value.empty() ? "audio" : value;
}

// ---------------- ctor ----------------

AppController::AppController() {
    fs::path config_path = fs::path(ProcessRunner::executableDirectory()) / "config.toml";
    std::error_code ec;
    if (!fs::is_regular_file(config_path, ec)) {
        config_path = fs::current_path() / "config.toml";
    }
    config_ = Config::load(config_path.string());
    auddRecognizer_ = AudDRecognizer(config_.audd.apiKey);
    if (config_.rootFolder.empty()) {
        const char* home = std::getenv("HOME");
        config_.rootFolder = (home != nullptr && *home != '\0')
            ? (fs::path(home) / "Music" / "tmplay").string()
            : (fs::current_path() / "tmplay").string();
    }
    config_.rootFolder = expandUserPath(config_.rootFolder).lexically_normal().string();
    if (std::find(config_.musicDirectories.begin(), config_.musicDirectories.end(),
                  config_.rootFolder) == config_.musicDirectories.end()) {
        config_.musicDirectories.insert(config_.musicDirectories.begin(), config_.rootFolder);
    }
    {
        std::error_code root_error;
        const fs::path root = config_.rootFolder;
        fs::create_directories(root / "Download" / "Albums", root_error);
        if (!root_error) {
            fs::create_directories(root / "Download" / "Downloads", root_error);
        }
        if (!root_error) {
            fs::create_directories(root / "Separated", root_error);
        }
        if (!root_error) {
            fs::create_directories(root / "Records", root_error);
        }
        if (root_error) {
            std::cerr << "tmplay library folder error: " << root_error.message() << "\n";
        }
    }
    {
        std::lock_guard<std::mutex> lock(directoryCacheMutex_);
        PlaylistImportResult searchRoot;
        searchRoot.name = "Search";
        PlaylistImportResult recentSearch;
        recentSearch.name = "Recent search";

        // Search contains the ten most recent search and album commands.
        // Downloads live in the physical Download folder beside Search.
        virtualPlaylists_[kSearchRoot] = std::move(searchRoot);
        virtualPlaylists_[kRecentSearch] = std::move(recentSearch);
    }
    restoreSearchCache();
    restoreSearchHistory();
    initializeLibrary();
    volume_ = std::clamp(config_.volume, 0, 100);
    audioEngine_.setVolume(volume_);
    previewAudioEngine_.setVolume(volume_);
    nowPlaying_.setRemoteCommandHandlers(
        [this] {
            if (audioEngine_.snapshot().state == PlaybackState::Paused) {
                togglePause();
            }
        },
        [this] {
            if (audioEngine_.snapshot().state == PlaybackState::Playing) {
                togglePause();
            }
        },
        [this] { togglePause(); },
        [this] { playPreviousTrack(); },
        [this] { playNextTrack(); });
    metadataWorker_ = std::jthread([this](std::stop_token stop_token) {
        metadataLoop(stop_token);
    });
    playbackWorker_ = std::jthread([this](std::stop_token stop_token) {
        playbackLoop(stop_token);
    });

    if (!config_.musicDirectories.empty()) {
        std::string initial_path = config_.musicDirectories[0];
        setCurrentPath(initial_path);
        initialScanWorker_ = std::jthread(
            [this, initial_path](std::stop_token stop_token) {
                if (!stop_token.stop_requested() &&
                    currentPath() == initial_path) {
                    scanDirectory(initial_path);
                }
            });
    }
    stemSeparator_.setOnFinished([this] {
        scanDirectory(currentPath(), true);
    });
}

AppController::~AppController() {
    autoCueCancel_ = true;
    if (initialScanWorker_.joinable()) {
        initialScanWorker_.request_stop();
    }
    if (autoCueWorker_.joinable()) {
        autoCueWorker_.request_stop();
    }
    playbackWorker_.request_stop();
    metadataWorker_.request_stop();
    metadataCv_.notify_all();
    stopPreviewPlayback();
    stopPlayback();
    if (autoCueWorker_.joinable()) {
        autoCueWorker_.join();
    }
    if (playbackWorker_.joinable()) {
        playbackWorker_.join();
    }
    if (metadataWorker_.joinable()) {
        metadataWorker_.join();
    }
    if (initialScanWorker_.joinable()) {
        initialScanWorker_.join();
    }
}

fs::path AppController::searchCachePath() const
{
    return fs::path(config_.rootFolder) / "Search" / "current-results.jsonl";
}

fs::path AppController::searchHistoryPath() const
{
    return fs::path(config_.rootFolder) / "Search" / "recent-queries.jsonl";
}

void AppController::restoreSearchCache()
{
    std::ifstream input(searchCachePath());
    if (!input) return;

    PlaylistImportResult recent;
    recent.name = "Recent search";
    for (std::string line; std::getline(input, line);) {
        auto cached = LibraryJson::trackFromJson(line);
        if (!cached || !isOnlineMediaUrl(cached->path.string())) continue;
        Track track;
        track.id = cached->path.string();
        track.title = cached->title.empty() ? track.id : cached->title;
        track.artist = cached->artist;
        track.album = cached->album;
        track.duration = cached->duration;
        track.genre = cached->genre;
        track.type = EntryType::File;
        track.status = TrackStatus::Ready;
        recent.tracks.push_back(std::move(track));
    }
    std::lock_guard<std::mutex> lock(directoryCacheMutex_);
    // The Recent search folder is always present, including on a first run.
    // Restoring the cache only fills it with the last online results.
    virtualPlaylists_[kRecentSearch] = std::move(recent);
}

void AppController::restoreSearchHistory()
{
    std::ifstream input(searchHistoryPath());
    if (!input) return;

    std::vector<Track> restored;
    std::set<std::pair<std::string, std::string>> seen;
    for (std::string line; std::getline(input, line);) {
        auto cached = LibraryJson::trackFromJson(line);
        if (!cached || (cached->id != "search" && cached->id != "track" &&
                        cached->id != "album")) {
            continue;
        }
        // `search` was the old on-line command. Keep old history readable,
        // but migrate its visible and saved mode to the new `track` command.
        const std::string mode = cached->id == "search" ? "track" : cached->id;
        const std::string query = ProcessRunner::trim(cached->title);
        if (query.empty() || !seen.emplace(mode, query).second) {
            continue;
        }
        Track entry;
        entry.id = std::string(kSearchHistoryPrefix) +
            std::to_string(restored.size());
        entry.title = mode + ": " + query;
        entry.album = query;
        entry.sourceId = mode;
        entry.type = EntryType::Navigation;
        entry.status = TrackStatus::Ready;
        restored.push_back(std::move(entry));
        if (restored.size() == kSearchHistoryLimit) break;
    }

    std::lock_guard<std::mutex> lock(directoryCacheMutex_);
    auto root = virtualPlaylists_.find(kSearchRoot);
    if (root != virtualPlaylists_.end()) {
        root->second.tracks = std::move(restored);
    }
}

void AppController::saveSearchCache(const std::vector<Track>& tracks) const
{
    const fs::path cache = searchCachePath();
    const fs::path temporary = cache.string() + ".tmp";
    std::error_code ec;
    fs::create_directories(cache.parent_path(), ec);
    if (ec) return;

    std::ofstream output(temporary, std::ios::trunc);
    if (!output) return;
    for (const auto& track : tracks) {
        if (track.type != EntryType::File || !isOnlineMediaUrl(track.id)) continue;
        LibraryTrack cached;
        cached.id = track.id;
        cached.path = track.id;
        cached.title = track.title;
        cached.artist = track.artist;
        cached.album = track.album;
        cached.duration = track.duration;
        cached.bpm = track.bpm;
        cached.key = track.key;
        cached.genre = track.genre;
        output << LibraryJson::trackToJson(cached) << "\n";
    }
    output.close();
    if (!output) {
        fs::remove(temporary, ec);
        return;
    }
    fs::rename(temporary, cache, ec);
    if (ec) {
        fs::remove(cache, ec);
        ec.clear();
        fs::rename(temporary, cache, ec);
    }
}

void AppController::saveSearchHistory() const
{
    std::vector<LibraryTrack> history;
    {
        std::lock_guard<std::mutex> lock(directoryCacheMutex_);
        const auto root = virtualPlaylists_.find(kSearchRoot);
        if (root == virtualPlaylists_.end()) return;
        for (const auto& entry : root->second.tracks) {
            if (entry.type != EntryType::Navigation ||
                !startsWith(entry.id, kSearchHistoryPrefix) ||
                (entry.sourceId != "track" && entry.sourceId != "album") ||
                entry.album.empty()) {
                continue;
            }
            LibraryTrack cached;
            cached.id = entry.sourceId;
            cached.path = entry.sourceId;
            cached.title = entry.album;
            history.push_back(std::move(cached));
        }
    }

    const fs::path cache = searchHistoryPath();
    const fs::path temporary = cache.string() + ".tmp";
    std::error_code ec;
    fs::create_directories(cache.parent_path(), ec);
    if (ec) return;
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) return;
    for (const auto& entry : history) {
        output << LibraryJson::trackToJson(entry) << "\n";
    }
    output.close();
    if (!output) {
        fs::remove(temporary, ec);
        return;
    }
    fs::rename(temporary, cache, ec);
    if (ec) {
        fs::remove(cache, ec);
        ec.clear();
        fs::rename(temporary, cache, ec);
    }
}

void AppController::rememberSearch(const std::string& query, bool groupAlbums)
{
    const std::string trimmed = ProcessRunner::trim(query);
    if (trimmed.empty()) return;
    const std::string mode = groupAlbums ? "album" : "track";
    {
        std::lock_guard<std::mutex> lock(directoryCacheMutex_);
        auto root = virtualPlaylists_.find(kSearchRoot);
        if (root == virtualPlaylists_.end()) return;
        auto& entries = root->second.tracks;
        entries.erase(std::remove_if(entries.begin(), entries.end(),
            [&](const Track& entry) {
                return entry.type == EntryType::Navigation &&
                    startsWith(entry.id, kSearchHistoryPrefix) &&
                    entry.sourceId == mode && entry.album == trimmed;
            }), entries.end());
        Track entry;
        entry.title = mode + ": " + trimmed;
        entry.album = trimmed;
        entry.sourceId = mode;
        entry.type = EntryType::Navigation;
        entry.status = TrackStatus::Ready;
        entries.insert(entries.begin(), std::move(entry));
        if (entries.size() > kSearchHistoryLimit) {
            entries.resize(kSearchHistoryLimit);
        }
        for (size_t index = 0; index < entries.size(); ++index) {
            entries[index].id = std::string(kSearchHistoryPrefix) +
                std::to_string(index);
        }
    }
    saveSearchHistory();
}

bool AppController::recentSearchRequest(const Track& entry,
                                        std::string& query,
                                        bool& groupAlbums) const
{
    if (entry.type != EntryType::Navigation ||
        !startsWith(entry.id, kSearchHistoryPrefix) ||
        (entry.sourceId != "search" && entry.sourceId != "track" &&
         entry.sourceId != "album")) {
        return false;
    }
    query = ProcessRunner::trim(entry.album);
    if (query.empty()) return false;
    groupAlbums = entry.sourceId == "album";
    return true;
}

void AppController::setCurrentSearchLabel(const std::string& query)
{
    const std::string label = ProcessRunner::trim(query);
    if (label.empty()) return;
    // Search labels include artist and title when the source provides them.
    const std::string title = label.starts_with("Found: ")
        ? label : "Track: " + label;
    std::lock_guard<std::mutex> lock(directoryCacheMutex_);
    auto recent = virtualPlaylists_.find(kRecentSearch);
    if (recent != virtualPlaylists_.end()) {
        recent->second.name = title;
    }
}


TrackStore& AppController::trackStore() {
    return trackStore_;
}

const Config& AppController::config() const {
    return config_;
}

void AppController::initializeLibrary() {
    if (!config_.library.enabled) {
        return;
    }

    std::string error;
    if (!libraryDatabase_.open(config_.library.databasePath, error)) {
        return;
    }
    if (!libraryDatabase_.initialize(error)) {
        return;
    }
    trackRepository_ = std::make_unique<TrackRepository>(libraryDatabase_);
    cueRepository_ = std::make_unique<CueRepository>(libraryDatabase_);
    if (config_.telegram.enabled && config_.telegram.mode == "bot") {
        telegramRepository_ = std::make_unique<TelegramRepository>(libraryDatabase_);
        telegramClient_ = std::make_unique<TelegramBotClient>(config_.telegram.botToken);
        telegramInbox_ = std::make_unique<TelegramInboxService>(
            config_, *telegramClient_, *telegramRepository_);
    }
}

void AppController::upsertLibraryTrack(const Track& track) {
    if (!trackRepository_ || track.type != EntryType::File || track.id.empty()) {
        return;
    }
    std::string error;
    LibraryTrack library_track = libraryTrackFromTrack(track);
    if (trackRepository_->upsertTrack(library_track, error)) {
        importSeratoCuesIfLibraryEmpty(track, library_track);
    }
}

void AppController::importSeratoCuesIfLibraryEmpty(
    const Track& track,
    const LibraryTrack& libraryTrack)
{
    if (!cueRepository_ || track.id.empty() || libraryTrack.id.empty()) {
        return;
    }

    std::string error;
    auto existing = cueRepository_->cuesForTrack(libraryTrack.id, error);
    if (!error.empty() || !existing.empty()) {
        return;
    }

    std::string serato_error;
    auto serato_cues = seratoCueWriter_.readCues(track.id, serato_error);
    std::vector<SeratoCue> embedded_cues = serato_cues;
    if (embedded_cues.empty()) {
        std::string traktor_error;
        embedded_cues = traktorMetadataWriter_.readCues(track.id, traktor_error);
    }
    if (embedded_cues.empty()) {
        return;
    }

    LibraryTrack imported = libraryTrackForCues(track, embedded_cues);
    if (imported.id != libraryTrack.id) {
        imported.id = libraryTrack.id;
    }
    cueRepository_->replaceCues(libraryTrack.id, imported.cues, error);
}

void AppController::replaceLibraryCues(const Track& track,
                                       const std::vector<SeratoCue>& cues) {
    if (!trackRepository_ || !cueRepository_ ||
        track.type != EntryType::File || track.id.empty()) {
        return;
    }

    LibraryTrack library_track = libraryTrackForCues(track, cues);
    std::string error;
    if (!trackRepository_->upsertTrack(library_track, error)) {
        return;
    }

    cueRepository_->replaceCues(library_track.id, library_track.cues, error);
}

LibraryTrack AppController::libraryTrackForCues(
    const Track& track,
    const std::vector<SeratoCue>& cues) const
{
    LibraryTrack library_track = libraryTrackFromTrack(track);
    library_track.cues.reserve(cues.size());
    for (const auto& cue : cues) {
        library_track.cues.push_back({
            cue.index,
            cue.name,
            "hotcue",
            cue.seconds,
            cueColorHex(cue.colorRgb),
        });
    }
    return library_track;
}

Track AppController::cueSafeTrackForRead(const Track& source) const
{
    // The selected audio file remains the cue owner.  SQLite is the
    // canonical store for every container, so no companion conversion is
    // needed merely to edit hot cues.
    return source;
}

bool AppController::cueSafeTrackForWrite(const Track& source,
                                         Track& target,
                                         std::string& error) const
{
    target = source;
    (void)error;
    return true;
}

bool AppController::exportLibraryCollection(std::string& error)
{
    if (!trackRepository_ || !cueRepository_) {
        error = "Library database is disabled";
        return false;
    }
    if (!config_.library.exportRekordbox && !config_.library.exportTraktor) {
        return true;
    }

    LibraryExporter exporter(*trackRepository_, *cueRepository_, seratoCueWriter_);
    LibraryExportOptions options;
    options.exportSerato = false;
    options.exportRekordbox = config_.library.exportRekordbox;
    options.exportTraktor = config_.library.exportTraktor;
    options.exportJson = false;
    options.syncFolder = expandUserPath(config_.library.syncFolder);
    options.outputFolder = options.syncFolder / "exports";
    return exporter.exportAll(options, error);
}

bool AppController::exportLibraryCues(const LibraryTrack& track,
                                      std::string& error,
                                      bool updateCollectionExport)
{
    const auto expected_cues = seratoCuesFromLibraryTrack(track);
    const bool can_write_embedded_cues = isCueSafeExtension(track.path);

    if (config_.autoCue.writeSerato && can_write_embedded_cues) {
        SeratoExportProvider serato(seratoCueWriter_,
                                    false,
                                    config_.autoCue.overwriteExistingCues);
        if (!serato.exportTrack(track, error)) {
            return false;
        }
    }

    if (config_.autoCue.writeTraktor && can_write_embedded_cues) {
        TraktorEmbeddedMetadataStatus status;
        if (!traktorMetadataWriter_.writeCues(track, error, &status)) {
            return false;
        }
    }

    if (config_.autoCue.writeSerato && can_write_embedded_cues) {
        std::string verify_error;
        auto written = seratoCueWriter_.readCues(track.path, verify_error);
        if (!verify_error.empty() || !cuePositionsMatch(expected_cues, written)) {
            error = "Serato cue verification failed";
            if (!verify_error.empty()) {
                error += ": " + verify_error;
            } else {
                error += ": expected " + std::to_string(expected_cues.size()) +
                    ", read " + std::to_string(written.size());
            }
            return false;
        }
    }

    if (config_.autoCue.writeTraktor && can_write_embedded_cues) {
        std::string verify_error;
        auto written = traktorMetadataWriter_.readCues(track.path, verify_error);
        if (!verify_error.empty() || !cuePositionsMatch(expected_cues, written)) {
            error = "Traktor cue verification failed";
            if (!verify_error.empty()) {
                error += ": " + verify_error;
            } else {
                error += ": expected " + std::to_string(expected_cues.size()) +
                    ", read " + std::to_string(written.size());
            }
            return false;
        }
    }

    if (libraryDatabase_.isOpen()) {
        std::int64_t timestamp = nowUnixSeconds();
        std::string hash = cueHash(expected_cues);
        sqlite3* db = libraryDatabase_.handle();
        if (config_.autoCue.writeSerato && can_write_embedded_cues) {
            syncStateTouch(db, track.id, "serato", hash, timestamp);
        }
        if (config_.autoCue.writeTraktor && can_write_embedded_cues) {
            syncStateTouch(db, track.id, "traktor", hash, timestamp);
        }
    }

    if (config_.library.exportJson) {
        JsonSync sync;
        if (!sync.exportTrack(track, expandUserPath(config_.library.syncFolder), error)) {
            return false;
        }
    }
    (void)updateCollectionExport;
    return true;
}

bool AppController::saveAutoCueResult(const fs::path& file,
                                      const AutoCueResult& result,
                                      const std::vector<SeratoCue>& cues,
                                      std::string& error,
                                      bool updateCollectionExport)
{
    Track track;
    track.id = file.string();
    track.title = file.stem().string();
    track.type = EntryType::File;
    track.duration = result.duration;

    try {
        mergeMetadataIntoTrack(track, audioAnalyzer_.readEmbeddedMetadata(track.id));
    } catch (...) {
    }

    Track cue_track;
    if (!cueSafeTrackForWrite(track, cue_track, error)) {
        return false;
    }
    if (cue_track.duration <= 0.0) {
        cue_track.duration = result.duration;
    }

    LibraryTrack library_track = libraryTrackForCues(cue_track, cues);
    if (trackRepository_ && cueRepository_) {
        if (trackRepository_->upsertTrack(library_track, error)) {
            if (!cueRepository_->replaceCues(library_track.id, library_track.cues, error)) {
                return false;
            }
        } else {
            return false;
        }
    }
    return exportLibraryCues(library_track, error, updateCollectionExport);
}

// ---------------- scanner ----------------

void AppController::scanDirectory(const std::string& path, bool forceRefresh) {
    struct ScanBusyGuard {
        std::atomic_int& count;
        explicit ScanBusyGuard(std::atomic_int& value) : count(value) {
            count.fetch_add(1);
        }
        ~ScanBusyGuard() {
            count.fetch_sub(1);
        }
    } scan_busy(directoryScansInFlight_);

    std::uint64_t scan_generation =
        directoryScanGeneration_.fetch_add(1) + 1;
    setCurrentPath(path);
    auto scanIsCurrent = [&] {
        return directoryScanGeneration_.load() == scan_generation &&
               currentPath() == path;
    };

    if (isVirtualPlaylistPath(path)) {
        std::vector<Track> tracks;
        {
            std::lock_guard<std::mutex> cache_lock(directoryCacheMutex_);
            auto found = virtualPlaylists_.find(path);
            if (found == virtualPlaylists_.end()) {
                trackStore_.clear();
                return;
            }
            tracks = found->second.tracks;
            directoryCache_[path] = tracks;
        }
        if (!scanIsCurrent()) return;
        trackStore_.setTracks(tracks);
        std::vector<Track> playable;
        playable.reserve(tracks.size());
        for (const auto& track : tracks) {
            if (track.type == EntryType::File) {
                playable.push_back(track);
            }
        }
        {
            std::lock_guard<std::mutex> lock(playbackMutex_);
            displayedTracks_ = std::move(playable);
        }
        queueMetadataScan(path, tracks);
        return;
    }

    if (!forceRefresh) {
        std::lock_guard<std::mutex> cache_lock(directoryCacheMutex_);
        auto cached = directoryCache_.find(path);
        if (cached != directoryCache_.end()) {
            if (!scanIsCurrent()) {
                return;
            }
            trackStore_.setTracks(cached->second);
            std::vector<Track> files;
            for (const auto& track : cached->second) {
                if (track.type == EntryType::File) {
                    files.push_back(track);
                }
            }
            {
                std::lock_guard<std::mutex> lock(playbackMutex_);
                displayedTracks_ = files;
            }
            std::vector<Track> metadata_files;
            for (const auto& file : files) {
                if (!isTelegramPath(file.id)) {
                    metadata_files.push_back(file);
                }
            }
            queueMetadataScan(path, metadata_files);
            return;
        }
    }

    std::vector<Track> dirs;
    std::vector<Track> files;

    if (isTelegramPath(path)) {
        std::vector<Track> tracks;
        std::string error;
        scanTelegramDirectory(path, tracks, error);
        for (const auto& track : tracks) {
            if (track.type == EntryType::Directory) {
                dirs.push_back(track);
            } else {
                files.push_back(track);
            }
        }
    } else
    try {
        for (const auto& entry : fs::directory_iterator(path)) {

            Track t;
            t.id = entry.path().string();
            t.title = entry.path().filename().string();

            // Skip hidden files
            if (t.title.empty() || t.title[0] == '.')
                continue;

            // DIRECTORY
            if (entry.is_directory()) {
                t.type = EntryType::Directory;
                t.status = TrackStatus::Ready;
                t.duration = 0;
                t.bpm = 0;
                t.key = "";
                dirs.push_back(t);
            }
            // FILE
            else {
                auto ext = entry.path().extension().string();

                if (!isAllowedFormat(ext))
                    continue;

                t.type = EntryType::File;
                t.status = TrackStatus::Ready;
                t.duration = 0;
                std::error_code ec;
                t.sizeBytes = entry.file_size(ec);
                if (ec) {
                    t.sizeBytes = 0;
                }
                files.push_back(t);
            }
        }
    } catch (const std::exception& e) {
        // Silently handle permission errors etc
    }

    // Sort directories by name, then add files
    std::sort(dirs.begin(), dirs.end(), 
              [](const Track& a, const Track& b) { return a.title < b.title; });
    std::sort(files.begin(), files.end(),
              [](const Track& a, const Track& b) { return a.title < b.title; });
    {
        std::lock_guard<std::mutex> lock(playbackMutex_);
        if (scanIsCurrent()) {
            displayedTracks_ = files;
        }
    }

    std::vector<Track> tracks;
    tracks.reserve(dirs.size() + files.size());
    tracks.insert(tracks.end(), dirs.begin(), dirs.end());
    tracks.insert(tracks.end(), files.begin(), files.end());
    {
        std::lock_guard<std::mutex> cache_lock(directoryCacheMutex_);
        directoryCache_[path] = tracks;
    }

    if (!scanIsCurrent()) {
        return;
    }
    trackStore_.setTracks(tracks);
    std::vector<Track> metadata_files;
    for (const auto& file : files) {
        if (!isTelegramPath(file.id)) {
            metadata_files.push_back(file);
        }
    }
    queueMetadataScan(path, metadata_files);
}

void AppController::updateCachedTrack(const std::string& directory,
                                      const Track& track) {
    std::lock_guard<std::mutex> cache_lock(directoryCacheMutex_);
    auto cached = directoryCache_.find(directory);
    if (cached == directoryCache_.end()) {
        return;
    }
    for (auto& existing : cached->second) {
        if (existing.id == track.id) {
            existing = track;
            return;
        }
    }
}

void AppController::removeCachedEntry(const std::string& directory,
                                      const std::string& id) {
    std::lock_guard<std::mutex> cache_lock(directoryCacheMutex_);
    auto cached = directoryCache_.find(directory);
    if (cached == directoryCache_.end()) {
        return;
    }
    cached->second.erase(
        std::remove_if(cached->second.begin(), cached->second.end(),
                       [&](const Track& track) { return track.id == id; }),
        cached->second.end());
}

void AppController::invalidateDirectoryCache(const std::string& path) {
    std::lock_guard<std::mutex> cache_lock(directoryCacheMutex_);
    directoryCache_.erase(path);
}

void AppController::queueMetadataScan(const std::string& path,
                                      const std::vector<Track>& tracks) {
    std::vector<Track> pending;
    pending.reserve(tracks.size());
    for (const auto& track : tracks) {
        if (needsMetadataScan(track)) {
            pending.push_back(track);
        }
    }

    {
        std::lock_guard<std::mutex> lock(metadataMutex_);
        pendingMetadata_ = std::move(pending);
        pendingMetadataPath_ = path;
        metadataGeneration_++;
        metadataBusy_ = !pendingMetadata_.empty();
    }
    metadataCv_.notify_one();
}

bool AppController::analyzeTrack(const Track& track, std::string& error)
{
    return analyzeTracks({track}, error);
}

bool AppController::analyzeTracks(const std::vector<Track>& tracks, std::string& error)
{
    std::vector<Track> local_tracks;
    local_tracks.reserve(tracks.size());
    for (const auto& track : tracks) {
        if (track.type != EntryType::File || track.id.empty() ||
            isOnlineMediaUrl(track.id)) {
            continue;
        }
        std::error_code ec;
        if (fs::is_regular_file(track.id, ec)) {
            local_tracks.push_back(track);
        }
    }
    if (local_tracks.empty()) {
        error = "Analysis is available only for local audio tracks";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(metadataMutex_);
        // A deliberate menu action takes precedence over the background scan.
        pendingMetadata_ = std::move(local_tracks);
        pendingMetadataPath_ = currentPath();
        for (const auto& pending : pendingMetadata_) {
            forcedMetadataAnalysis_.insert(pending.id);
        }
        ++metadataGeneration_;
        metadataBusy_ = true;
    }
    metadataCv_.notify_one();
    return true;
}

void AppController::metadataLoop(std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
        std::vector<Track> batch;
        std::unordered_set<std::string> forced_ids;
        std::string directory;
        unsigned long generation = 0;

        {
            std::unique_lock<std::mutex> lock(metadataMutex_);
            metadataCv_.wait(lock, stop_token, [&] {
                return !pendingMetadata_.empty();
            });
            if (stop_token.stop_requested()) {
                return;
            }
            batch = std::move(pendingMetadata_);
            pendingMetadata_.clear();
            for (const auto& track : batch) {
                if (forcedMetadataAnalysis_.erase(track.id) > 0) {
                    forced_ids.insert(track.id);
                }
            }
            directory = pendingMetadataPath_;
            generation = metadataGeneration_;
        }

        auto is_current_scan = [&] {
            std::lock_guard<std::mutex> lock(metadataMutex_);
            return generation == metadataGeneration_ &&
                   directory == pendingMetadataPath_ &&
                   directory == currentPath();
        };

        auto publish_track = [&](const Track& updated) {
            if (is_current_scan()) {
                trackStore_.updateTrack(updated);
            }
            updateCachedTrack(directory, updated);
            upsertLibraryTrack(updated);
            std::lock_guard<std::mutex> playback_lock(playbackMutex_);
            if (playingTrackId_ == updated.id) {
                playingTrack_ = updated;
            }
            for (auto& queued : playbackQueue_) {
                if (queued.id == updated.id) {
                    queued = updated;
                }
            }
        };

        std::vector<Track> analysis_batch;
        analysis_batch.reserve(batch.size());

        for (auto& track : batch) {
            if (stop_token.stop_requested() || !is_current_scan()) {
                break;
            }

            AudioMetadata metadata = audioAnalyzer_.readEmbeddedMetadata(track.id);
            if (metadata.succeeded) {
                mergeMetadataIntoTrack(track, metadata);
            }

            if (forced_ids.contains(track.id) || needsMetadataScan(track)) {
                analysis_batch.push_back(track);
            } else {
                track.status = TrackStatus::Ready;
                publish_track(track);
            }
        }

        for (auto& track : analysis_batch) {
            if (stop_token.stop_requested() || !is_current_scan()) {
                break;
            }

            const bool missing_bpm = track.bpm <= 0.0;
            const bool missing_key = track.key.empty();
            const bool missing_genre = !hasUsableGenre(track.genre);
            const bool force_analysis = forced_ids.contains(track.id);
            if (!force_analysis && !missing_bpm && !missing_key && !missing_genre) {
                track.status = TrackStatus::Ready;
                publish_track(track);
                continue;
            }

            track.status = TrackStatus::Analyzing;
            publish_track(track);

            AudioMetadata analyzed =
                audioAnalyzer_.analyzeWithEssentia(track.id);
            if (analyzed.succeeded) {
                if (track.duration <= 0.0 && analyzed.duration > 0.0) {
                    track.duration = analyzed.duration;
                }
                AudioMetadata write_metadata;
                write_metadata.succeeded = true;
                if ((force_analysis || missing_bpm) && analyzed.bpm > 0.0) {
                    track.bpm = analyzed.bpm;
                    write_metadata.bpm = analyzed.bpm;
                }
                if ((force_analysis || missing_key) && !analyzed.key.empty()) {
                    track.key = analyzed.key;
                    write_metadata.key = analyzed.key;
                }
                if ((force_analysis || missing_genre) && !analyzed.genre.empty()) {
                    track.genre = analyzed.genre;
                    write_metadata.genre = analyzed.genre;
                }
                std::string write_error;
                metadataWriter_.write(track.id, write_metadata, write_error);
                track.status = TrackStatus::Ready;
            } else {
                track.status = TrackStatus::Error;
            }
            publish_track(track);
        }

        std::lock_guard<std::mutex> lock(metadataMutex_);
        if (generation == metadataGeneration_ && pendingMetadata_.empty()) {
            metadataBusy_ = false;
        }
    }
}
// ---------------- volume ----------------

int AppController::volume() const {
    std::lock_guard<std::mutex> lock(playbackMutex_);
    return volume_;
}

void AppController::volumeUp() {
    int next_volume = 0;
    {
        std::lock_guard<std::mutex> lock(playbackMutex_);
        volume_ = std::min(100, volume_ + 5);
        next_volume = volume_;
    }
    audioEngine_.setVolume(next_volume);
    previewAudioEngine_.setVolume(next_volume);
}

void AppController::volumeDown() {
    int next_volume = 0;
    {
        std::lock_guard<std::mutex> lock(playbackMutex_);
        volume_ = std::max(0, volume_ - 5);
        next_volume = volume_;
    }
    audioEngine_.setVolume(next_volume);
    previewAudioEngine_.setVolume(next_volume);
}

double AppController::playbackRate() const {
    return audioEngine_.playbackRate();
}

void AppController::setPlaybackRate(double rate) {
    std::lock_guard<std::mutex> lock(playbackMutex_);
    if (isOnlineMediaUrl(playingTrackId_)) {
        return;
    }
    audioEngine_.setPlaybackRate(rate);
}

bool AppController::preservePitch() const {
    return audioEngine_.preservePitch();
}

void AppController::setPreservePitch(bool preserve) {
    std::lock_guard<std::mutex> lock(playbackMutex_);
    if (isOnlineMediaUrl(playingTrackId_)) {
        return;
    }
    audioEngine_.setPreservePitch(preserve);
}

void AppController::setEqualizerGains(double lowDb, double midDb, double highDb) {
    audioEngine_.setEqualizerGains(lowDb, midDb, highDb);
}

bool AppController::resolveTelegramTrackForPlayback(const Track& track,
                                                    Track& localTrack,
                                                    std::string& error)
{
    localTrack = track;
    if (!isTelegramPath(track.id)) {
        return true;
    }
    if (!telegramInbox_) {
        error = "Telegram is not configured";
        return false;
    }

    auto parsed = parseTelegramItemPath(track.id);
    if (!parsed) {
        error = "Unsupported Telegram track";
        return false;
    }

    auto item = telegramInbox_->findAudioItem(parsed->first, parsed->second, error);
    if (!error.empty()) {
        return false;
    }
    if (!item) {
        error = "Telegram audio item was not found";
        return false;
    }

    if (!item->downloaded || item->localPath.empty() ||
        !fs::is_regular_file(item->localPath)) {
        if (!config_.telegram.downloadOnPlay) {
            error = "Telegram download_on_play is disabled";
            return false;
        }
        Track downloading = track;
        downloading.status = TrackStatus::Downloading;
        trackStore_.updateTrack(downloading);
        if (!telegramInbox_->downloadItem(*item, error)) {
            downloading.status = TrackStatus::Error;
            trackStore_.updateTrack(downloading);
            return false;
        }
    }

    localTrack.id = item->localPath.string();
    localTrack.title = item->fileName.empty() ? track.title : item->fileName;
    localTrack.type = EntryType::File;
    localTrack.status = TrackStatus::Ready;
    localTrack.duration = item->duration > 0 ? item->duration : track.duration;
    localTrack.sizeBytes = item->fileSize > 0 ? item->fileSize : track.sizeBytes;
    try {
        mergeMetadataIntoTrack(localTrack, audioAnalyzer_.readEmbeddedMetadata(localTrack.id));
    } catch (...) {
    }
    LibraryTrack imported_track = libraryTrackFromTrack(localTrack);
    item->importedTrackId = imported_track.id;
    std::string item_error;
    telegramRepository_->upsertAudioItem(*item, item_error);
    upsertLibraryTrack(localTrack);
    {
        std::string directory = currentPath();
        std::lock_guard<std::mutex> cache_lock(directoryCacheMutex_);
        auto cached = directoryCache_.find(directory);
        if (cached != directoryCache_.end()) {
            for (auto& existing : cached->second) {
                if (existing.id == track.id) {
                    existing = localTrack;
                    break;
                }
            }
        }
    }
    auto visible = trackStore_.getTracks();
    for (auto& existing : visible) {
        if (existing.id == track.id) {
            existing = localTrack;
            break;
        }
    }
    trackStore_.setTracks(visible);
    return true;
}

bool AppController::playTrack(const Track& track,
                              const std::vector<Track>& orderedTracks) {
    if (track.type != EntryType::File) {
        return false;
    }

    const std::string source_path = currentPath();
    if (isOnlineMediaUrl(track.id)) {
        auto stream = resolveStreamUrl(track.id, config_.ytdlp.cookiesFromBrowser,
                                       config_.ytdlp.cookiesPath);
        Track online = track;
        online.thumbnailUrl = mediumThumbnailUrl(online.thumbnailUrl);
        if (online.duration <= 0.0 && stream && stream->durationSeconds > 0.0) {
            online.duration = stream->durationSeconds;
        }
        if (!stream || !audioEngine_.play(stream->url, online.title, volume_, online.duration)) {
            return false;
        }
        online.format = stream->format;
        online.bitrateKbps = stream->bitrateKbps;
        online.sampleRateHz = stream->sampleRateHz;
        online.sizeBytes = stream->sizeBytes;
        {
            std::lock_guard<std::mutex> lock(playbackMutex_);
            resolvedStreamUrls_[track.id] = stream->url;
            if (resolvedStreamUrls_.size() > 64) {
                resolvedStreamUrls_.erase(resolvedStreamUrls_.begin());
            }
            playbackQueue_.clear();
            for (const auto& candidate : orderedTracks) {
                if (candidate.type != EntryType::File || !isOnlineMediaUrl(candidate.id)) {
                    continue;
                }
                playbackQueue_.push_back(candidate.id == online.id ? online : candidate);
            }
            if (playbackQueue_.empty()) {
                playbackQueue_.push_back(online);
            }
            playingTrackId_ = track.id;
            playingTrack_ = online;
            playingSourcePath_ = source_path;
        }
        {
            std::lock_guard<std::mutex> lock(directoryCacheMutex_);
            for (auto& [path, tracks] : directoryCache_) {
                (void)path;
                for (auto& entry : tracks) {
                    if (entry.id == track.id) entry = online;
                }
            }
            for (auto& [path, playlist] : virtualPlaylists_) {
                (void)path;
                for (auto& entry : playlist.tracks) {
                    if (entry.id == track.id) entry = online;
                }
            }
        }
        auto displayed = trackStore_.getTracks();
        for (auto& entry : displayed) {
            if (entry.id == track.id) entry = online;
        }
        trackStore_.setTracks(std::move(displayed));
        publishNowPlaying(online);
        return true;
    }

    Track playable = track;
    std::string resolve_error;
    if (!resolveTelegramTrackForPlayback(track, playable, resolve_error)) {
        return false;
    }

    std::vector<Track> playback_tracks = orderedTracks;
    for (auto& queued : playback_tracks) {
        if (queued.id == track.id) {
            queued = playable;
        }
    }

    std::lock_guard<std::mutex> lock(playbackMutex_);
    if (!audioEngine_.play(playable.id, playable.title, volume_)) {
        return false;
    }

    auto in_ordered_tracks = std::find_if(
        playback_tracks.begin(), playback_tracks.end(),
        [&](const Track& displayed) { return displayed.id == playable.id; });
    playbackQueue_ = in_ordered_tracks != playback_tracks.end()
        ? playback_tracks
        : std::vector<Track>{playable};
    playingTrackId_ = playable.id;
    playingTrack_ = playable;
    playingSourcePath_ = source_path;
    publishNowPlaying(playable);
    return true;
}

bool AppController::playPreviewTrack(const Track& track, double startSeconds) {
    if (track.type != EntryType::File) {
        return false;
    }

    std::lock_guard<std::mutex> lock(playbackMutex_);
    if (!previewAudioEngine_.play(track.id, track.title, volume_, 0.0,
                                  startSeconds)) {
        return false;
    }
    previewPlayingTrackId_ = track.id;
    return true;
}

void AppController::togglePreviewPause() {
    previewAudioEngine_.togglePause();
}

void AppController::stopPreviewPlayback() {
    std::lock_guard<std::mutex> lock(playbackMutex_);
    previewAudioEngine_.stop();
    previewPlayingTrackId_.clear();
}

void AppController::seekPreviewPlayback(double ratio) {
    previewAudioEngine_.seekToRatio(ratio);
}

void AppController::setPreviewLoopRange(double startSeconds, double endSeconds) {
    previewAudioEngine_.setLoopRange(startSeconds, endSeconds);
}

void AppController::clearPreviewLoopRange() {
    previewAudioEngine_.clearLoopRange();
}

PlaybackSnapshot AppController::previewPlaybackSnapshot() const {
    return previewAudioEngine_.snapshot();
}

std::string AppController::previewPlayingTrackId() const {
    std::lock_guard<std::mutex> lock(playbackMutex_);
    return previewPlayingTrackId_;
}

double AppController::previewPlaybackRate() const {
    return previewAudioEngine_.playbackRate();
}

void AppController::setPreviewPlaybackRate(double rate) {
    previewAudioEngine_.setPlaybackRate(rate);
}

bool AppController::previewPreservePitch() const {
    return previewAudioEngine_.preservePitch();
}

void AppController::setPreviewPreservePitch(bool preserve) {
    previewAudioEngine_.setPreservePitch(preserve);
}

void AppController::togglePause() {
    audioEngine_.togglePause();
    updateNowPlayingPlayback();
}

void AppController::stopPlayback() {
    std::lock_guard<std::mutex> lock(playbackMutex_);
    audioEngine_.stop();
    playingTrackId_.clear();
    playingTrack_ = {};
    playingSourcePath_.clear();
    nowPlaying_.clear();
}

bool AppController::playPreviousTrack() {
    std::lock_guard<std::mutex> lock(playbackMutex_);
    return playRelativeTrackLocked(-1, false);
}

bool AppController::playNextTrack() {
    std::lock_guard<std::mutex> lock(playbackMutex_);
    return playRelativeTrackLocked(1, false);
}

void AppController::cyclePlaybackMode() {
    std::lock_guard<std::mutex> lock(playbackMutex_);
    switch (playbackMode_) {
    case PlaybackMode::RepeatAll:
        playbackMode_ = PlaybackMode::Shuffle;
        break;
    case PlaybackMode::Shuffle:
        playbackMode_ = PlaybackMode::RepeatOne;
        break;
    case PlaybackMode::RepeatOne:
        playbackMode_ = PlaybackMode::RepeatAll;
        break;
    }
}

PlaybackMode AppController::playbackMode() const {
    std::lock_guard<std::mutex> lock(playbackMutex_);
    return playbackMode_;
}

void AppController::seekPlayback(double ratio) {
    audioEngine_.seekToRatio(ratio);
    updateNowPlayingPlayback();
}

PlaybackSnapshot AppController::playbackSnapshot() const {
    return audioEngine_.snapshot();
}

std::string AppController::playingTrackId() const {
    std::lock_guard<std::mutex> lock(playbackMutex_);
    return playingTrackId_;
}

Track AppController::playingTrack() const {
    std::lock_guard<std::mutex> lock(playbackMutex_);
    return playingTrack_;
}

bool AppController::openNowPlayingLocation(std::string& result)
{
    Track track;
    std::string source_path;
    {
        std::lock_guard<std::mutex> lock(playbackMutex_);
        track = playingTrack_;
        source_path = playingSourcePath_;
    }
    if (track.id.empty()) {
        result = "Nothing is playing";
        return false;
    }

    std::string destination;
    if (isOnlineMediaUrl(track.id)) {
        destination = source_path;
        if (destination.empty() || !isVirtualPlaylistPath(destination)) {
            result = "Online track source is no longer available";
            return false;
        }
    } else {
        destination = fs::path(track.id).parent_path().string();
        if (destination.empty()) {
            result = "Playing file has no parent folder";
            return false;
        }
    }

    scanDirectory(destination, true);
    result = "Now playing: " + track.title;
    return true;
}

bool AppController::isStreamingPlayback() const
{
    std::lock_guard<std::mutex> lock(playbackMutex_);
    return isOnlineMediaUrl(playingTrackId_);
}

void AppController::publishNowPlaying(const Track& track)
{
    nowPlaying_.publish(track.title, track.artist, track.thumbnailUrl,
                        isOnlineMediaUrl(track.id) ? std::string{} : track.id,
                        audioEngine_.snapshot());
}

void AppController::updateNowPlayingPlayback()
{
    const PlaybackSnapshot snapshot = audioEngine_.snapshot();
    if (snapshot.state == PlaybackState::Stopped ||
        snapshot.state == PlaybackState::Error) {
        nowPlaying_.clear();
        return;
    }
    nowPlaying_.updatePlayback(snapshot);
}

void AppController::playbackLoop(std::stop_token stop_token) {
    auto last_output_check = std::chrono::steady_clock::now();
    while (!stop_token.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        auto snapshot = audioEngine_.snapshot();
        if (snapshot.state == PlaybackState::Playing ||
            snapshot.state == PlaybackState::Paused) {
            nowPlaying_.updatePlayback(snapshot);
        }
        bool playback_active = snapshot.state == PlaybackState::Playing ||
                               snapshot.state == PlaybackState::Paused;
        auto now = std::chrono::steady_clock::now();
        if (playback_active &&
            now - last_output_check >= std::chrono::seconds(1)) {
            audioEngine_.followSystemAudioOutput();
            last_output_check = now;
        }
        prefetchNextOnlineStream(snapshot);
        if (!audioEngine_.consumeFinishedNaturally()) {
            continue;
        }

        std::lock_guard<std::mutex> lock(playbackMutex_);
        playRelativeTrackLocked(1, true);
    }
}

void AppController::prefetchNextOnlineStream(const PlaybackSnapshot& playback)
{
    if (playback.state != PlaybackState::Playing || playback.durationSeconds <= 0.0 ||
        playback.durationSeconds - playback.positionSeconds > 20.0) {
        return;
    }

    Track next;
    {
        std::lock_guard<std::mutex> lock(playbackMutex_);
        if (!isOnlineMediaUrl(playingTrackId_) || playbackQueue_.size() < 2 ||
            playbackMode_ == PlaybackMode::RepeatOne) {
            return;
        }
        auto current = std::find_if(playbackQueue_.begin(), playbackQueue_.end(),
            [&](const Track& track) { return track.id == playingTrackId_; });
        if (current == playbackQueue_.end()) {
            return;
        }
        const std::size_t index = (std::size_t)std::distance(
            playbackQueue_.begin(), current);
        next = playbackQueue_[(index + 1) % playbackQueue_.size()];
        if (!isOnlineMediaUrl(next.id) || resolvedStreamUrls_.contains(next.id) ||
            prefetchingTrackId_ == next.id || failedPrefetchTrackIds_.contains(next.id)) {
            return;
        }
        prefetchingTrackId_ = next.id;
    }

    // This runs on the playback worker, not the FTXUI thread.  Playback of
    // the current decoder continues while yt-dlp resolves the following URL.
    auto stream = resolveStreamUrl(next.id, config_.ytdlp.cookiesFromBrowser,
                                   config_.ytdlp.cookiesPath);
    if (!stream) {
        std::lock_guard<std::mutex> lock(playbackMutex_);
        if (prefetchingTrackId_ == next.id) {
            prefetchingTrackId_.clear();
        }
        failedPrefetchTrackIds_.insert(next.id);
        return;
    }

    // The prefetch result contains more than an expiring URL. Preserve the
    // selected stream's presentation metadata too, otherwise an automatic
    // transition plays correctly but leaves Size/Kbps/Rate/Frmt blank.
    next.format = stream->format;
    if (next.duration <= 0.0 && stream->durationSeconds > 0.0) {
        next.duration = stream->durationSeconds;
    }
    next.bitrateKbps = stream->bitrateKbps;
    next.sampleRateHz = stream->sampleRateHz;
    next.sizeBytes = stream->sizeBytes;
    {
        std::lock_guard<std::mutex> lock(playbackMutex_);
        if (prefetchingTrackId_ == next.id) {
            prefetchingTrackId_.clear();
        }
        resolvedStreamUrls_[next.id] = stream->url;
        failedPrefetchTrackIds_.erase(next.id);
        for (auto& queued : playbackQueue_) {
            if (queued.id == next.id) {
                queued = next;
            }
        }
        if (resolvedStreamUrls_.size() > 64) {
            resolvedStreamUrls_.erase(resolvedStreamUrls_.begin());
        }
    }

    // `playRelativeTrackLocked()` takes its Track from playbackQueue_, while
    // FTXUI renders TrackStore. Update both the current virtual folder and
    // its backing cache before the natural transition occurs.
    {
        std::lock_guard<std::mutex> lock(directoryCacheMutex_);
        for (auto& [path, tracks] : directoryCache_) {
            (void)path;
            for (auto& entry : tracks) {
                if (entry.id == next.id) entry = next;
            }
        }
        for (auto& [path, playlist] : virtualPlaylists_) {
            (void)path;
            for (auto& entry : playlist.tracks) {
                if (entry.id == next.id) entry = next;
            }
        }
    }
    trackStore_.updateTrack(next);
}

bool AppController::playRelativeTrackLocked(int direction, bool natural_end) {
    if (playbackQueue_.empty()) {
        return false;
    }

    auto current = std::find_if(
        playbackQueue_.begin(), playbackQueue_.end(),
        [&](const Track& track) { return track.id == playingTrackId_; });
    int index = current == playbackQueue_.end()
        ? 0
        : (int)std::distance(playbackQueue_.begin(), current);

    if (natural_end && playbackMode_ == PlaybackMode::RepeatOne) {
        // Keep the current index.
    } else if (natural_end && playbackMode_ == PlaybackMode::Shuffle &&
               playbackQueue_.size() > 1) {
        std::uniform_int_distribution<int> distribution(0, (int)playbackQueue_.size() - 2);
        int random_index = distribution(randomGenerator_);
        index = random_index >= index ? random_index + 1 : random_index;
    } else {
        int count = (int)playbackQueue_.size();
        index = (index + direction + count) % count;
    }

    Track next = playbackQueue_[(size_t)index];
    if (isOnlineMediaUrl(next.id)) {
        next.thumbnailUrl = mediumThumbnailUrl(next.thumbnailUrl);
        std::string directUrl;
        if (auto found = resolvedStreamUrls_.find(next.id);
            found != resolvedStreamUrls_.end()) {
            directUrl = found->second;
        } else {
            // Manual Next/Previous remains available even if the 20-second
            // prefetch did not finish or the server rejected it.
            auto stream = resolveStreamUrl(next.id, config_.ytdlp.cookiesFromBrowser,
                                           config_.ytdlp.cookiesPath);
            if (!stream) {
                return false;
            }
            directUrl = stream->url;
            if (next.duration <= 0.0 && stream->durationSeconds > 0.0) {
                next.duration = stream->durationSeconds;
            }
            resolvedStreamUrls_[next.id] = directUrl;
        }
        if (!audioEngine_.play(directUrl, next.title, volume_, next.duration)) {
            return false;
        }
        playingTrackId_ = next.id;
        playingTrack_ = next;
        publishNowPlaying(next);
        return true;
    }
    std::string resolve_error;
    if (!resolveTelegramTrackForPlayback(next, next, resolve_error)) {
        return false;
    }
    playbackQueue_[(size_t)index] = next;
    if (!audioEngine_.play(next.id, next.title, volume_)) {
        return false;
    }
    playingTrackId_ = next.id;
    playingTrack_ = next;
    publishNowPlaying(next);
    return true;
}

bool AppController::downloadToCurrentDirectory(const std::string& source,
                                               std::function<void()> on_finished) {
    std::string managed_destination;
    std::string managed_error;
    if (!config_.rootFolder.empty()) {
        if (!managedDownloadDirectory(managed_destination, managed_error)) {
            return false;
        }
        return downloadToDirectory(source, managed_destination, std::move(on_finished));
    }
    std::string destination = currentPath();
    if (destination.empty() || isVirtualPlaylistPath(destination) ||
        isTelegramPath(destination)) {
        destination = config_.musicDirectories.empty()
            ? "."
            : config_.musicDirectories.front();
    }

    return downloadToDirectory(source, destination, std::move(on_finished));
}

bool AppController::managedDownloadDirectory(std::string& destination,
                                             std::string& error)
{
    if (config_.rootFolder.empty()) {
        return false;
    }
    const fs::path root = expandUserPath(config_.rootFolder);
    std::error_code ec;
    const fs::path albums = root / "Download" / "Albums";
    const fs::path tracks = root / "Download" / "Downloads";
    fs::create_directories(albums, ec);
    if (!ec) {
        fs::create_directories(tracks, ec);
    }
    if (ec) {
        error = "Could not create download library: " + ec.message();
        return false;
    }

    std::string albumName;
    const std::string current = currentPath();
    if (current.starts_with("search://") &&
        current.find("/albums/") != std::string::npos) {
        std::lock_guard<std::mutex> lock(directoryCacheMutex_);
        auto album = virtualPlaylists_.find(current);
        if (album != virtualPlaylists_.end()) {
            albumName = album->second.name;
        }
    }
    fs::path target = albumName.empty()
        ? tracks
        : albums / safeFolderName(albumName);
    fs::create_directories(target, ec);
    if (ec) {
        error = "Could not create download folder: " + ec.message();
        return false;
    }
    destination = target.string();
    return true;
}

void AppController::invalidateManagedDownloadCache()
{
    std::lock_guard<std::mutex> lock(managedDownloadCacheMutex_);
    managedDownloadCacheReady_ = false;
    managedDownloadedTitles_.clear();
    managedDownloadedAlbumTracks_.clear();
    const fs::path download_root = fs::path(config_.rootFolder) / "Download";
    const std::string root = download_root.lexically_normal().string();
    std::lock_guard<std::mutex> directory_lock(directoryCacheMutex_);
    std::erase_if(directoryCache_, [&](const auto& item) {
        return startsWith(fs::path(item.first).lexically_normal().string(), root);
    });
}

void AppController::refreshManagedDownloadCache() const
{
    std::lock_guard<std::mutex> lock(managedDownloadCacheMutex_);
    if (managedDownloadCacheReady_) {
        return;
    }

    managedDownloadedTitles_.clear();
    managedDownloadedAlbumTracks_.clear();
    const fs::path search_root = fs::path(config_.rootFolder) / "Download";
    const fs::path albums_root = search_root / "Albums";
    std::error_code error;
    if (fs::is_directory(search_root, error)) {
        for (fs::recursive_directory_iterator it(search_root, error), end;
             !error && it != end; it.increment(error)) {
            if (!it->is_regular_file(error) || error ||
                !isAllowedFormat(lowerExtension(it->path()))) {
                continue;
            }
            const std::string title = downloadNameKey(it->path().stem().string());
            managedDownloadedTitles_.insert(title);
            const fs::path parent = it->path().parent_path();
            if (parent != albums_root &&
                startsWith(parent.lexically_normal().string(),
                           albums_root.lexically_normal().string())) {
                managedDownloadedAlbumTracks_.insert(
                    downloadNameKey(parent.filename().string()) + "\n" + title);
            }
        }
    }
    managedDownloadCacheReady_ = true;
}

bool AppController::isOnlineTrackDownloaded(const Track& track) const
{
    if (!isOnlineMediaUrl(track.id)) {
        return false;
    }
    refreshManagedDownloadCache();
    std::lock_guard<std::mutex> lock(managedDownloadCacheMutex_);
    return managedDownloadedTitles_.contains(downloadNameKey(track.title));
}

bool AppController::isOnlineAlbumDownloaded(const std::vector<Track>& tracks) const
{
    const auto first_media = std::find_if(tracks.begin(), tracks.end(),
        [](const Track& track) {
            return track.type == EntryType::File && isOnlineMediaUrl(track.id);
        });
    if (first_media == tracks.end()) {
        return false;
    }
    const std::string album = first_media->album;
    if (album.empty()) {
        return false;
    }
    refreshManagedDownloadCache();
    const std::string album_key = downloadNameKey(album);
    std::lock_guard<std::mutex> lock(managedDownloadCacheMutex_);
    for (const auto& track : tracks) {
        if (track.type != EntryType::File) {
            continue;
        }
        const std::string title_key = downloadNameKey(track.title);
        const bool found_in_album = std::any_of(
            managedDownloadedAlbumTracks_.begin(),
            managedDownloadedAlbumTracks_.end(),
            [&](const std::string& entry) {
                const size_t separator = entry.find('\n');
                return separator != std::string::npos &&
                    entry.substr(separator + 1) == title_key &&
                    entry.substr(0, separator).find(album_key) != std::string::npos;
            });
        if (!isOnlineMediaUrl(track.id) ||
            track.album != album || !found_in_album) {
            return false;
        }
    }
    return true;
}

bool AppController::downloadToDirectory(const std::string& source,
                                        const std::string& directory,
                                        std::function<void()> on_finished) {
    std::error_code ec;
    std::string destination = fs::weakly_canonical(directory, ec).string();
    if (ec) {
        destination = directory;
    }
    auto completion = [this, on_finished = std::move(on_finished)] {
        invalidateManagedDownloadCache();
        if (on_finished) {
            on_finished();
        }
    };
    return downloadManager_.start(source,
                                  destination,
                                  config_.downloadFormat,
                                  config_.ytdlp.cookiesFromBrowser,
                                  config_.ytdlp.cookiesPath,
                                  std::move(completion));
}

bool AppController::downloadTracksToCurrentDirectory(
    const std::vector<Track>& tracks,
    std::function<void()> on_finished)
{
    std::string destination;
    std::string destination_error;
    if (!config_.rootFolder.empty()) {
        if (!managedDownloadDirectory(destination, destination_error)) {
            return false;
        }
    } else {
        destination = currentPath();
        if (destination.empty() || isVirtualPlaylistPath(destination) ||
            isTelegramPath(destination)) {
            destination = config_.musicDirectories.empty()
                ? "."
                : config_.musicDirectories.front();
        }
    }
    // Albums are flat under Download/Albums so they are easy to browse:
    // <artist> — <album> (<year>). Single tracks stay in Downloads/<artist>.
    if (!config_.rootFolder.empty() && !tracks.empty()) {
        const std::string current = currentPath();
        const bool album_search_context = current.starts_with("search://") &&
            current.find("/albums/") != std::string::npos;
        const Track* first = nullptr;
        for (const auto& track : tracks) {
            if (track.type == EntryType::File && isOnlineMediaUrl(track.id)) {
                first = &track;
                break;
            }
        }
        // managedDownloadDirectory() already selected the exact virtual album
        // folder.  Rebuilding its name from track tags can differ from the
        // search album title (artist/year variants), leaving a second empty
        // folder behind.  Keep that canonical destination while browsing an
        // album.
        if (first && !album_search_context) {
            const std::string artist = safeFolderName(
                first->artist.empty() ? "Unknown Artist" : first->artist);
            const bool collection = tracks.size() > 1 && !first->album.empty() &&
                std::all_of(tracks.begin(), tracks.end(), [&](const Track& track) {
                    return track.type == EntryType::File && track.album == first->album;
                });
            // A track selected while browsing a virtual album belongs beside
            // the rest of that album. This also lets a later "Download all"
            // reuse the already downloaded file rather than create an empty
            // album directory plus a duplicate under Downloads.
            const bool track_from_open_album = album_search_context &&
                !first->album.empty();
            fs::path managed = fs::path(config_.rootFolder) / "Download" /
                ((collection || track_from_open_album) ? "Albums" : "Downloads");
            if (collection || track_from_open_album) {
                std::string album_folder = (first->artist.empty()
                    ? "Unknown Artist" : first->artist) + " — " + first->album;
                if (first->releaseDate.size() >= 4) {
                    album_folder += " (" + first->releaseDate.substr(0, 4) + ")";
                }
                managed /= safeFolderName(album_folder);
            } else {
                managed /= artist;
            }
            std::error_code create_error;
            fs::create_directories(managed, create_error);
            if (create_error) {
                return false;
            }
            destination = managed.string();
        }
    }
    // A configured library root keeps online downloads under Download.
    // Without one, retain the current download location but still give a full
    // album its own stable folder, just like music_cli.py does.
    if (config_.rootFolder.empty() && !tracks.empty()) {
        const Track* first = nullptr;
        for (const auto& track : tracks) {
            if (track.type == EntryType::File && isOnlineMediaUrl(track.id)) {
                first = &track;
                break;
            }
        }
        const bool collection = first != nullptr && tracks.size() > 1 &&
            !first->album.empty() && std::all_of(tracks.begin(), tracks.end(),
                [&](const Track& track) {
                    return track.type == EntryType::File &&
                        track.album == first->album;
                });
        if (collection) {
            const std::string artist = first->artist.empty()
                ? "Unknown Artist" : first->artist;
            std::string album_folder = artist + " — " + first->album;
            if (first->releaseDate.size() >= 4) {
                album_folder += " (" + first->releaseDate.substr(0, 4) + ")";
            }
            fs::path album_destination = fs::path(destination) / "Albums" /
                safeFolderName(album_folder);
            std::error_code create_error;
            fs::create_directories(album_destination, create_error);
            if (create_error) return false;
            destination = album_destination.string();
        }
    }
    std::vector<DownloadSnapshot::Item> items;
    items.reserve(tracks.size());
    const std::string current = currentPath();
    const bool open_virtual_album = current.starts_with("search://") &&
        current.find("/albums/") != std::string::npos;
    std::map<std::string, int> album_track_positions;
    if (open_virtual_album) {
        int position = 0;
        for (const auto& listed : trackStore_.getTracks()) {
            if (listed.type == EntryType::File && isOnlineMediaUrl(listed.id)) {
                album_track_positions.emplace(listed.id, ++position);
            }
        }
    }
    for (const auto& track : tracks) {
        if (track.type != EntryType::File || !isOnlineMediaUrl(track.id)) {
            continue;
        }
        DownloadSnapshot::Item item;
        item.title = track.title;
        item.artist = track.artist;
        item.album = track.album;
        item.genre = track.genre;
        item.thumbnailUrl = track.thumbnailUrl;
        item.releaseDate = track.releaseDate;
        item.webpageUrl = track.id;
        if (const auto found = album_track_positions.find(track.id);
            found != album_track_positions.end()) {
            item.index = found->second;
        }
        {
            std::lock_guard<std::mutex> lock(playbackMutex_);
            auto direct = resolvedStreamUrls_.find(track.id);
            item.source = direct == resolvedStreamUrls_.end()
                ? track.id
                : direct->second;
        }
        items.push_back(std::move(item));
    }
    auto completion = [this, on_finished = std::move(on_finished)] {
        invalidateManagedDownloadCache();
        if (on_finished) {
            on_finished();
        }
    };
    return downloadManager_.startBatch(std::move(items), destination,
                                       config_.downloadFormat,
                                       config_.ytdlp.cookiesFromBrowser,
                                       config_.ytdlp.cookiesPath,
                                       std::move(completion));
}

void AppController::cancelDownload() {
    downloadManager_.cancel();
}

DownloadSnapshot AppController::downloadSnapshot() const {
    return downloadManager_.snapshot();
}

bool AppController::canEditTrack(const Track& track, std::string& reason) const
{
    if (track.type != EntryType::File || track.id.empty()) {
        reason = "Select a track";
        return false;
    }
    if (isOnlineMediaUrl(track.id)) {
        reason = "Streaming track is caching; editor, analysis and speed controls unlock after download";
        return false;
    }
    std::error_code ec;
    if (!fs::is_regular_file(track.id, ec)) {
        reason = "Local audio file is unavailable";
        return false;
    }
    return true;
}


bool AppController::openPlaylistFile(const std::string& playlistPath,
                                     std::string& result)
{
    PlaylistImportResult imported;
    std::string error;
    if (!PlaylistImporter::importFile(playlistPath, imported, error)) {
        result = error;
        return false;
    }

    std::string key = std::string(kPlaylistPrefix) + imported.name;
    int suffix = 2;
    {
        std::lock_guard<std::mutex> lock(directoryCacheMutex_);
        while (virtualPlaylists_.contains(key)) {
            key = std::string(kPlaylistPrefix) + imported.name + " " +
                  std::to_string(suffix++);
        }
        virtualPlaylists_[key] = imported;
    }
    scanDirectory(key, true);
    result = "Playlist: " + imported.name + " | tracks " +
        std::to_string(imported.tracks.size());
    if (imported.missingFiles > 0) {
        result += " | missing " + std::to_string(imported.missingFiles);
    }
    return true;
}

bool AppController::isVirtualPlaylistPath(const std::string& path) const
{
    return startsWith(path, kPlaylistPrefix) || startsWith(path, "search://");
}

bool AppController::openOnlinePlaylist(const std::string& source,
                                       std::string& result)
{
    const std::string url = ProcessRunner::trim(source);
    if (!isOnlineMediaUrl(url)) {
        result = "Invalid online playlist URL";
        return false;
    }
    const auto yt_dlp = preferredYtDlp();
    if (!yt_dlp) {
        result = "yt-dlp not found";
        return false;
    }

    std::string output;
    const int exit_code = runYtDlpWithCookiesFileFallback({
        *yt_dlp, "--ignore-config", "--no-plugin-dirs",
        "--flat-playlist", "--no-warnings", "--playlist-end", "100",
        "--print", "%(playlist_title)s\t%(title)s\t%(webpage_url)s\t%(duration_string)s\t%(genre)s\t%(artist)s\t%(uploader)s\t%(thumbnail)s\t%(release_date)s\t%(upload_date)s",
        "--", url,
    }, config_.ytdlp.cookiesPath, &output);
    if (exit_code != 0) {
        result = compactProcessOutput(output);
        return false;
    }

    auto parseDuration = [](const std::string& value) {
        int seconds = 0;
        std::istringstream stream(value);
        for (std::string part; std::getline(stream, part, ':');) {
            try {
                seconds = seconds * 60 + std::stoi(part);
            } catch (...) {
                return 0.0;
            }
        }
        return (double)seconds;
    };
    auto knownValue = [](const std::string& value) {
        const std::string normalized = ProcessRunner::trim(value);
        return !normalized.empty() && normalized != "NA" && normalized != "None";
    };

    PlaylistImportResult playlist;
    playlist.name = "Online playlist";
    std::set<std::string> seenTracks;
    std::istringstream lines(output);
    for (std::string line; std::getline(lines, line);) {
        std::vector<std::string> fields;
        std::istringstream fieldStream(line);
        for (std::string field; std::getline(fieldStream, field, '\t');) {
            fields.push_back(std::move(field));
        }
        if (fields.size() < 3 || !isOnlineMediaUrl(fields[2]) ||
            !seenTracks.insert(fields[2]).second) {
            continue;
        }
        if (knownValue(fields[0])) playlist.name = fields[0];
        Track track;
        track.id = fields[2];
        track.title = fields[1].empty() ? track.id : fields[1];
        track.album = playlist.name;
        track.duration = fields.size() > 3 ? parseDuration(fields[3]) : 0.0;
        track.genre = fields.size() > 4 && knownValue(fields[4]) ? fields[4] : "";
        track.artist = fields.size() > 5 && knownValue(fields[5]) ? fields[5]
            : (fields.size() > 6 && knownValue(fields[6]) ? fields[6] : "");
        track.thumbnailUrl = fields.size() > 7 && knownValue(fields[7])
            ? mediumThumbnailUrl(fields[7]) : "";
        track.releaseDate = fields.size() > 8 && knownValue(fields[8]) ? fields[8]
            : (fields.size() > 9 && knownValue(fields[9]) ? fields[9] : "");
        track.type = EntryType::File;
        track.status = TrackStatus::Ready;
        playlist.tracks.push_back(std::move(track));
    }
    if (playlist.tracks.empty()) {
        result = "No playable tracks in playlist";
        return false;
    }
    for (auto& track : playlist.tracks) track.album = playlist.name;

    const std::string title = playlist.name;
    const size_t track_count = playlist.tracks.size();
    {
        std::lock_guard<std::mutex> lock(directoryCacheMutex_);
        virtualPlaylists_[kRecentSearch] = playlist;
    }
    setCurrentSearchLabel(title);
    saveSearchCache(playlist.tracks);
    scanDirectory(kRecentSearch, true);
    result = "Playlist: " + title + " | tracks " + std::to_string(track_count);
    return true;
}

bool AppController::searchMusic(const std::string& query,
                                bool groupAlbums,
                                std::string& result,
                                const OnlineSearchOptions& options)
{
    const std::string trimmed = ProcessRunner::trim(query);
    if (trimmed.empty()) {
        result = "Enter a music search query";
        return false;
    }
    auto yt_dlp = preferredYtDlp();

    auto parse_duration = [](const std::string& value) {
        int seconds = 0;
        std::istringstream stream(value);
        std::vector<int> parts;
        for (std::string part; std::getline(stream, part, ':');) {
            try {
                parts.push_back(std::stoi(part));
            } catch (...) {
                return 0.0;
            }
        }
        for (int part : parts) seconds = seconds * 60 + part;
        return (double)seconds;
    };

    auto fieldsFromLine = [](const std::string& line) {
        std::vector<std::string> fields;
        std::istringstream stream(line);
        for (std::string field; std::getline(stream, field, '\t');) {
            fields.push_back(std::move(field));
        }
        return fields;
    };

    if (groupAlbums) {
      {
        std::vector<OfficialAlbumSearchResult> official_albums;
        if (!searchOfficialAlbums(trimmed, official_albums, result)) {
            return false;
        }

        std::string key = "search://" + trimmed;
        int suffix = 2;
        PlaylistImportResult root;
        root.name = "Albums: " + trimmed;
        {
            std::lock_guard<std::mutex> lock(directoryCacheMutex_);
            while (virtualPlaylists_.contains(key)) {
                key = "search://" + trimmed + " " + std::to_string(suffix++);
            }
            for (auto& album : official_albums) {
                std::string display_name = album.artist.empty()
                    ? album.title
                    : album.artist + " — " + album.title;
                if (!album.year.empty()) display_name += " (" + album.year + ")";
                std::string child_path = key + "/albums/" + display_name;
                int child_suffix = 2;
                while (virtualPlaylists_.contains(child_path)) {
                    child_path = key + "/albums/" + display_name + " " +
                        std::to_string(child_suffix++);
                }
                PlaylistImportResult child;
                child.name = display_name;

                Track album_row;
                album_row.id = child_path;
                album_row.sourceId = album.id;
                album_row.title = album.title;
                album_row.artist = album.artist;
                album_row.album = album.title;
                album_row.releaseDate = album.year;
                album_row.thumbnailUrl = album.thumbnailUrl;
                album_row.type = EntryType::Album;
                album_row.status = TrackStatus::Ready;
                root.tracks.push_back(std::move(album_row));
                virtualPlaylists_[child_path] = std::move(child);
            }
            virtualPlaylists_[kRecentSearch] = root;
            virtualPlaylists_[key] = std::move(root);
        }
        rememberSearch(trimmed, true);
        setCurrentSearchLabel(trimmed);
        scanDirectory(key, true);
        result = "Official album search: " + trimmed + " | albums " +
            std::to_string(official_albums.size());
        return true;
      }

        constexpr size_t kOfficialAlbumLimit = 20;
        constexpr size_t kPlaylistFallbackLimit = 10;
        // Prefer the official YouTube Releases tab when the query can be an
        // artist handle. Unlike the mixed Music search page it contains the
        // complete discography (for example, Linkin Park returns far more
        // than the first three MPRE cards).
        struct AlbumSource {
            std::string title;
            std::string url;
            bool isPlaylist = false;
        };
        std::vector<AlbumSource> albumSources;
        std::set<std::string> seenAlbums;
        std::string artistHandle;
        for (unsigned char character : trimmed) {
            if (std::isalnum(character) && character < 0x80) {
                artistHandle.push_back((char)character);
            }
        }
        if (!artistHandle.empty()) {
            std::string releasesOutput;
            const std::string releasesUrl =
                "https://www.youtube.com/@" + artistHandle + "/releases";
            if (ProcessRunner::runWithCombinedOutput({
                    *yt_dlp, "--ignore-config", "--no-plugin-dirs",
                    "--flat-playlist", "--no-warnings",
                    "--playlist-end", "20",
                    "--print", "%(title)s\t%(webpage_url)s",
                    "--", releasesUrl,
                }, &releasesOutput) == 0) {
                std::istringstream releaseLines(releasesOutput);
                for (std::string line; std::getline(releaseLines, line);) {
                    const auto fields = fieldsFromLine(line);
                    if (fields.size() < 2 ||
                        fields[1].find("youtube.com/playlist?list=") == std::string::npos ||
                        !seenAlbums.insert(fields[1]).second) {
                        continue;
                    }
                    albumSources.push_back({fields[0], fields[1], false});
                    if (albumSources.size() == kOfficialAlbumLimit) break;
                }
            }
        }

        // YouTube Music search returns release browse URLs. It remains the
        // fallback for artists whose public YouTube handle differs from the
        // search text, and also fills a short Releases page up to 50 albums.
        std::string albumsOutput;
        const std::string musicSearch =
            "https://music.youtube.com/search?q=" + percentEncodeQuery(trimmed);
        int searchExit = ProcessRunner::runWithCombinedOutput({
            *yt_dlp, "--ignore-config", "--no-plugin-dirs",
            // Music search is a mixed page (songs, artists, albums). Read a
            // wider source page, then cap the actual album containers at 50.
            "--flat-playlist", "--no-warnings", "--playlist-end", "250",
            "--print", "%(extractor)s\t%(playlist_title)s\t%(webpage_url)s",
            "--", musicSearch,
        }, &albumsOutput);
        if (searchExit != 0) {
            result = compactProcessOutput(albumsOutput);
            return false;
        }

        std::istringstream albumLines(albumsOutput);
        for (std::string line; std::getline(albumLines, line);) {
            if (albumSources.size() >= kOfficialAlbumLimit) break;
            const auto fields = fieldsFromLine(line);
            if (fields.size() < 3 || fields[0] != "youtube:tab" ||
                // MPRE is a YouTube Music release. VL is a generic playlist
                // identifier and was the source of entries such as
                // "Reorder-topic" appearing as albums.
                fields[2].find("/browse/MPRE") == std::string::npos ||
                // On the YouTube Music search page playlist_title is the
                // query itself for every result (for example, all Linkin
                // Park releases report "linkin park").  Deduplicating by
                // that title kept only the first album. The MPRE browse URL
                // is the stable, per-release identity instead.
                !seenAlbums.insert(fields[2]).second) {
                continue;
            }
            albumSources.push_back({fields[1] == "NA" ? trimmed : fields[1], fields[2], false});
            if (albumSources.size() == kOfficialAlbumLimit) {
                break;
            }
        }

        // A strict album lookup can legitimately return only a few releases
        // for a new artist. In that case append up to ten closest playlists,
        // already ordered by YouTube relevance. They are visibly labelled so
        // a playlist is never mistaken for an official album.
        if (albumSources.size() < kOfficialAlbumLimit) {
            std::string playlistsOutput;
            const std::string playlistSearch =
                "https://www.youtube.com/results?search_query=" +
                percentEncodeQuery(trimmed) + "&sp=EgIQAw%3D%3D";
            if (ProcessRunner::runWithCombinedOutput({
                    *yt_dlp, "--ignore-config", "--no-plugin-dirs",
                    "--flat-playlist", "--no-warnings",
                    "--playlist-end", "50",
                    "--print", "%(title)s\t%(webpage_url)s",
                    "--", playlistSearch,
                }, &playlistsOutput) == 0) {
                size_t playlistCount = 0;
                std::istringstream playlistLines(playlistsOutput);
                for (std::string line; std::getline(playlistLines, line);) {
                    const auto fields = fieldsFromLine(line);
                    if (fields.size() < 2 ||
                        fields[1].find("youtube.com/playlist?list=") == std::string::npos ||
                        isTopicAutoGeneratedName(fields[0]) ||
                        !seenAlbums.insert(fields[1]).second) {
                        continue;
                    }
                    albumSources.push_back({fields[0], fields[1], true});
                    if (++playlistCount == kPlaylistFallbackLimit ||
                        albumSources.size() == kOfficialAlbumLimit + kPlaylistFallbackLimit) {
                        break;
                    }
                }
            }
        }

        std::string key = "search://" + trimmed;
        int suffix = 2;
        PlaylistImportResult root;
        root.name = "Albums: " + trimmed;
        std::vector<std::pair<std::string, PlaylistImportResult>> children;
        std::set<std::string> seenExpandedAlbums;
        std::vector<Track> catalogTracks;
        for (const auto& albumSource : albumSources) {
            std::string tracksOutput;
            const int tracksExit = ProcessRunner::runWithCombinedOutput({
                *yt_dlp, "--ignore-config", "--no-plugin-dirs",
                "--flat-playlist", "--no-warnings", "--playlist-end", "100",
                "--print", "%(playlist_title)s\t%(title)s\t%(webpage_url)s\t%(duration_string)s\t%(genre)s\t%(artist)s\t%(uploader)s\t%(thumbnail)s\t%(release_date)s\t%(upload_date)s",
                "--", albumSource.url,
            }, &tracksOutput);
            if (tracksExit != 0) continue;

            PlaylistImportResult child;
            child.name = albumSource.isPlaylist
                ? "Playlist - " + albumSource.title
                : albumSource.title;
            std::set<std::string> seenTracks;
            std::istringstream trackLines(tracksOutput);
            for (std::string line; std::getline(trackLines, line);) {
                const auto fields = fieldsFromLine(line);
                if (fields.size() < 3 || !isOnlineMediaUrl(fields[2]) ||
                    !seenTracks.insert(searchDuplicateKey(fields[1]).empty()
                        ? fields[2]
                        : searchDuplicateKey(fields[1]) + "\n" +
                          (fields.size() > 5 ? fields[5] : "")).second) {
                    continue;
                }
                if (!albumSource.isPlaylist && !fields[0].empty() && fields[0] != "NA") {
                    child.name = fields[0];
                }
                Track track;
                track.id = fields[2];
                track.title = fields[1].empty() ? track.id : fields[1];
                track.album = child.name;
                track.duration = fields.size() > 3 ? parse_duration(fields[3]) : 0.0;
                track.genre = fields.size() > 4 && fields[4] != "NA" ? fields[4] : "";
                track.artist = fields.size() > 5 && fields[5] != "NA" ? fields[5]
                    : (fields.size() > 6 && fields[6] != "NA" ? fields[6] : "");
                track.thumbnailUrl = fields.size() > 7 && fields[7] != "NA"
                    ? mediumThumbnailUrl(fields[7]) : "";
                track.releaseDate = fields.size() > 8 && fields[8] != "NA" ? fields[8]
                    : (fields.size() > 9 && fields[9] != "NA" ? fields[9] : "");
                track.type = EntryType::File;
                track.status = TrackStatus::Ready;
                child.tracks.push_back(std::move(track));
            }
            std::string albumKey = searchDuplicateKey(child.name);
            if (albumKey.starts_with("album")) albumKey.erase(0, 5);
            if (child.tracks.size() < 3 || isTopicAutoGeneratedName(child.name)) {
                continue;
            }
            std::string releaseYear;
            for (const auto& track : child.tracks) {
                if (track.releaseDate.size() < 4) continue;
                const std::string candidate = track.releaseDate.substr(0, 4);
                if (std::all_of(candidate.begin(), candidate.end(),
                                [](unsigned char character) { return std::isdigit(character); })) {
                    releaseYear = candidate;
                    break;
                }
            }
            if (!releaseYear.empty()) {
                child.name += " (" + releaseYear + ")";
            }
            for (auto& track : child.tracks) {
                track.album = child.name;
            }
            albumKey = searchDuplicateKey(child.name);
            if (albumKey.starts_with("album")) albumKey.erase(0, 5);
            if (seenExpandedAlbums.insert(albumKey.empty() ? albumSource.url : albumKey).second) {
                catalogTracks.insert(catalogTracks.end(), child.tracks.begin(), child.tracks.end());
                children.push_back({child.name, std::move(child)});
            }
        }

        if (children.empty()) {
            result = "No playable albums found";
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(directoryCacheMutex_);
            while (virtualPlaylists_.contains(key)) {
                key = "search://" + trimmed + " " + std::to_string(suffix++);
            }
            for (auto& [title, child] : children) {
                std::string childPath = key + "/albums/" + title;
                int childSuffix = 2;
                while (virtualPlaylists_.contains(childPath)) {
                    childPath = key + "/albums/" + title + " " +
                        std::to_string(childSuffix++);
                }
                Track folder;
                folder.id = childPath;
                folder.title = title;
                folder.type = EntryType::Directory;
                folder.status = TrackStatus::Ready;
                root.tracks.push_back(folder);
                virtualPlaylists_[childPath] = std::move(child);
            }
            virtualPlaylists_[kRecentSearch] = root;
            virtualPlaylists_[key] = std::move(root);
        }
        saveSearchCache(catalogTracks);
        rememberSearch(trimmed, true);
        setCurrentSearchLabel(trimmed);
        scanDirectory(key, true);
        result = "Album search: " + trimmed + " | albums " +
            std::to_string(children.size());
        return true;
    }

    // `search` is a YouTube Music song search, not a generic yt-dlp / YouTube
    // video query. The bridge requests fifty songs and puts Art Tracks first;
    // ordinary songs only fill a list when YTMusic omits the ATV label.
    {
        const int track_limit = std::clamp(options.maxTracks, 1, 50);
        std::vector<Track> found_tracks;
        const bool ytmusic_found = searchOfficialTracks(
            trimmed, track_limit, found_tracks, result);
        if (ytmusic_found) {
            found_tracks.erase(std::remove_if(found_tracks.begin(), found_tracks.end(),
                [&](const Track& track) {
                    return (options.minDurationSeconds > 0 &&
                            track.duration < options.minDurationSeconds) ||
                           (options.maxDurationSeconds > 0 &&
                            track.duration > options.maxDurationSeconds);
                }), found_tracks.end());
            if (!found_tracks.empty()) {
                std::string key = "search://" + trimmed;
                int suffix = 2;
                PlaylistImportResult root;
                root.name = trimmed;
                root.tracks = found_tracks;
                {
                    std::lock_guard<std::mutex> lock(directoryCacheMutex_);
                    while (virtualPlaylists_.contains(key)) {
                        key = "search://" + trimmed + " " + std::to_string(suffix++);
                    }
                    virtualPlaylists_[kRecentSearch] = root;
                    virtualPlaylists_[key] = std::move(root);
                }
                rememberSearch(trimmed, false);
                setCurrentSearchLabel(trimmed);
                saveSearchCache(found_tracks);
                scanDirectory(key, true);
                result = "Track: " + trimmed + " | tracks " +
                    std::to_string(found_tracks.size());
                return true;
            }
            result = "No YouTube Music songs match the duration filter";
        }
        return false;
    }

    std::vector<std::string> arguments = {
        *yt_dlp, "--ignore-config", "--no-plugin-dirs",
        "--flat-playlist", "--no-warnings",
        "--print", "%(title)s\t%(webpage_url)s\t%(duration_string)s\t%(album)s\t%(artist)s\t%(genre)s\t%(thumbnail)s\t%(release_date)s\t%(upload_date)s\t%(uploader)s\t%(channel)s",
    };
    const int track_limit = std::clamp(options.maxTracks, 1, 500);
    arguments.push_back("ytsearch" + std::to_string(track_limit) + ":" + trimmed + " music");
    std::string output;
    int exit_code = runYtDlpWithCookiesFileFallback(
        arguments, config_.ytdlp.cookiesPath, &output);
    if (exit_code != 0) {
        result = compactProcessOutput(output);
        return false;
    }

    auto knownValue = [](const std::string& value) {
        const std::string trimmed_value = ProcessRunner::trim(value);
        return !trimmed_value.empty() && trimmed_value != "NA" &&
            trimmed_value != "None" && trimmed_value != "null";
    };

    auto sourceArtist = [&](const std::vector<std::string>& fields) {
        if (fields.size() > 4 && knownValue(fields[4])) return fields[4];
        if (fields.size() > 9 && knownValue(fields[9])) return fields[9];
        if (fields.size() > 10 && knownValue(fields[10])) return fields[10];
        return std::string{};
    };
    const std::string queryKey = searchDuplicateKey(trimmed);
    auto officialPriority = [&](const std::string& title,
                                const std::vector<std::string>& fields) {
        std::string text = searchDuplicateKey(title);
        std::string source;
        if (fields.size() > 9 && knownValue(fields[9])) source += searchDuplicateKey(fields[9]);
        if (fields.size() > 10 && knownValue(fields[10])) source += searchDuplicateKey(fields[10]);
        int score = 0;
        if (text.find("official") != std::string::npos) score += 20;
        if (source.find("topic") != std::string::npos) score += 40;
        if (!queryKey.empty() && !source.empty() &&
            (queryKey.find(source) != std::string::npos ||
             source.find(queryKey) != std::string::npos)) {
            score += 30;
        }
        return score;
    };

    struct SearchResult {
        Track track;
        std::string album;
        std::string artist;
        int priority = 0;
    };
    std::vector<SearchResult> foundTracks;
    std::set<std::string> seen;
    std::istringstream lines(output);
    for (std::string line; std::getline(lines, line);) {
        std::vector<std::string> fields;
        std::istringstream field_stream(line);
        for (std::string field; std::getline(field_stream, field, '\t');) {
            fields.push_back(std::move(field));
        }
        if (fields.size() < 2 || !isOnlineMediaUrl(fields[1]) ||
            !seen.insert(searchDuplicateKey(fields[0]).empty()
                ? fields[1]
                : searchDuplicateKey(fields[0]) + "\n" +
                  (fields.size() > 4 ? fields[4] : "")).second) {
            continue;
        }
        Track track;
        track.id = std::move(fields[1]);
        track.title = fields[0].empty() ? track.id : fields[0];
        track.album = fields.size() > 3 && knownValue(fields[3]) ? fields[3] : "";
        track.artist = sourceArtist(fields);
        track.thumbnailUrl = fields.size() > 6 && knownValue(fields[6])
            ? mediumThumbnailUrl(fields[6]) : "";
        track.releaseDate = fields.size() > 7 && knownValue(fields[7]) ? fields[7]
            : (fields.size() > 8 && knownValue(fields[8]) ? fields[8] : "");
        track.duration = fields.size() > 2 ? parse_duration(fields[2]) : 0.0;
        track.genre = fields.size() > 5 && knownValue(fields[5]) ? fields[5] : "";
        track.type = EntryType::File;
        track.status = TrackStatus::Ready;
        SearchResult found;
        found.track = std::move(track);
        found.album = fields.size() > 3 && knownValue(fields[3]) ? fields[3] : "";
        found.artist = found.track.artist;
        found.priority = officialPriority(found.track.title, fields);
        foundTracks.push_back(std::move(found));
    }
    // Some yt-dlp extractors return an empty flat playlist when the extra
    // "music" topic is present. Retry a plain YouTube search before showing
    // an error; it still returns direct YouTube URLs for streaming.
    if (foundTracks.empty()) {
        arguments.back() = "ytsearch" + std::to_string(track_limit) + ":" + trimmed;
        output.clear();
        if (runYtDlpWithCookiesFileFallback(
                arguments, config_.ytdlp.cookiesPath, &output) == 0) {
            std::istringstream retry_lines(output);
            for (std::string line; std::getline(retry_lines, line);) {
                const auto fields = fieldsFromLine(line);
                if (fields.size() < 2 || !isOnlineMediaUrl(fields[1]) ||
                    !seen.insert(searchDuplicateKey(fields[0]).empty()
                        ? fields[1]
                        : searchDuplicateKey(fields[0]) + "\n" +
                          (fields.size() > 4 ? fields[4] : "")).second) {
                    continue;
                }
                Track track;
                track.id = fields[1];
                track.title = fields[0].empty() ? track.id : fields[0];
                track.album = fields.size() > 3 && knownValue(fields[3]) ? fields[3] : "";
                track.artist = sourceArtist(fields);
                track.thumbnailUrl = fields.size() > 6 && knownValue(fields[6])
                    ? mediumThumbnailUrl(fields[6]) : "";
                track.releaseDate = fields.size() > 7 && knownValue(fields[7]) ? fields[7]
                    : (fields.size() > 8 && knownValue(fields[8]) ? fields[8] : "");
                track.duration = fields.size() > 2 ? parse_duration(fields[2]) : 0.0;
                track.genre = fields.size() > 5 && knownValue(fields[5]) ? fields[5] : "";
                track.type = EntryType::File;
                track.status = TrackStatus::Ready;
                foundTracks.push_back({std::move(track),
                                       fields.size() > 3 && knownValue(fields[3]) ? fields[3] : "",
                                       sourceArtist(fields),
                                       officialPriority(fields[0], fields)});
            }
        }
    }
    foundTracks.erase(std::remove_if(foundTracks.begin(), foundTracks.end(),
        [&](const SearchResult& found) {
            return (options.minDurationSeconds > 0 &&
                    found.track.duration < options.minDurationSeconds) ||
                   (options.maxDurationSeconds > 0 &&
                    found.track.duration > options.maxDurationSeconds);
        }), foundTracks.end());
    std::stable_sort(foundTracks.begin(), foundTracks.end(),
        [](const SearchResult& left, const SearchResult& right) {
            return left.priority > right.priority;
        });
    if ((int)foundTracks.size() > track_limit) {
        foundTracks.resize((size_t)track_limit);
    }
    if (foundTracks.empty()) {
        const std::string detail = compactProcessOutput(output);
        result = "No music results found. Check Internet access or update yt-dlp.";
        if (!detail.empty()) {
            result += " yt-dlp: " + detail;
        }
        return false;
    }

    std::string key = "search://" + trimmed;
    int suffix = 2;
    std::size_t albumCount = 0;
    PlaylistImportResult root;
    root.name = groupAlbums ? "Albums: " + trimmed : trimmed;
    {
        std::lock_guard<std::mutex> lock(directoryCacheMutex_);
        while (virtualPlaylists_.contains(key)) {
            key = "search://" + trimmed + " " + std::to_string(suffix++);
        }
        if (!groupAlbums) {
            for (const auto& found : foundTracks) {
                root.tracks.push_back(found.track);
            }
        } else {
            std::unordered_map<std::string, std::string> albumPaths;
            for (const auto& foundTrack : foundTracks) {
                const Track& track = foundTrack.track;
                // Flat ytsearch often does not expose album tags. In that
                // case each album result is still represented as a folder,
                // ready to contain all tracks when the extractor does expose
                // the same album on subsequent results.
                std::string album = foundTrack.album.empty()
                    ? track.title
                    : foundTrack.album;
                if (!foundTrack.artist.empty()) {
                    album = foundTrack.artist + " — " + album;
                }
                if (track.title.empty()) album = "Album";
                const std::string albumKey = album;
                auto found = albumPaths.find(albumKey);
                std::string albumPath;
                if (found == albumPaths.end()) {
                    albumPath = key + "/albums/" + album;
                    int albumSuffix = 2;
                    while (virtualPlaylists_.contains(albumPath)) {
                        albumPath = key + "/albums/" + album + " " +
                            std::to_string(albumSuffix++);
                    }
                    albumPaths.emplace(albumKey, albumPath);
                    albumCount++;
                    Track folder;
                    folder.id = albumPath;
                    folder.title = album;
                    folder.type = EntryType::Directory;
                    folder.status = TrackStatus::Ready;
                    root.tracks.push_back(std::move(folder));
                    PlaylistImportResult child;
                    child.name = album;
                    child.tracks.push_back(track);
                    virtualPlaylists_[albumPath] = std::move(child);
                } else {
                    albumPath = found->second;
                    virtualPlaylists_[albumPath].tracks.push_back(track);
                }
            }
        }
        virtualPlaylists_[kRecentSearch] = root;
        virtualPlaylists_[key] = std::move(root);
    }
    rememberSearch(trimmed, groupAlbums);
    setCurrentSearchLabel(trimmed);
    std::vector<Track> cacheTracks;
    cacheTracks.reserve(foundTracks.size());
    for (const auto& found : foundTracks) {
        cacheTracks.push_back(found.track);
    }
    saveSearchCache(cacheTracks);
    scanDirectory(key, true);
    result = groupAlbums
        ? "Album search: " + trimmed + " | albums " +
            std::to_string(albumCount) + " | tracks " +
            std::to_string(foundTracks.size())
        : "Track: " + trimmed + " | tracks " +
            std::to_string(foundTracks.size());
    return true;
}

bool AppController::searchMusicAll(const std::string& query,
                                   std::string& result,
                                   const OnlineSearchOptions& options)
{
    std::string track_result;
    const bool tracks_found = searchMusic(query, false, track_result, options);
    std::vector<Track> tracks = tracks_found ? trackStore_.getTracks()
                                             : std::vector<Track>{};
    tracks.erase(std::remove_if(tracks.begin(), tracks.end(),
        [](const Track& track) { return track.type != EntryType::File; }),
        tracks.end());

    std::string album_result;
    const bool albums_found = searchMusic(query, true, album_result);
    std::vector<Track> albums = albums_found ? trackStore_.getTracks()
                                              : std::vector<Track>{};
    albums.erase(std::remove_if(albums.begin(), albums.end(),
        [](const Track& track) { return track.type != EntryType::Album; }),
        albums.end());

    if (!tracks_found && !albums_found) {
        result = !track_result.empty() ? track_result : album_result;
        if (!album_result.empty() && album_result != result) {
            result += " | albums: " + album_result;
        }
        return false;
    }

    const size_t track_count = tracks.size();
    const size_t album_count = albums.size();
    // `searchMusic(..., true)` stores its own cache as an implementation
    // detail. The visible recent-search cache should restore the ordinary
    // track result after a restart.
    saveSearchCache(tracks);
    PlaylistImportResult root;
    root.name = "Recent search: " + ProcessRunner::trim(query);
    // Album results are playlist rows, not folders in the browser. Keep them
    // first so Enter opens the release, while ordinary search tracks remain
    // playable directly below them.
    root.tracks = std::move(albums);
    root.tracks.insert(root.tracks.end(),
                       std::make_move_iterator(tracks.begin()),
                       std::make_move_iterator(tracks.end()));

    {
        std::lock_guard<std::mutex> lock(directoryCacheMutex_);
        virtualPlaylists_[kRecentSearch] = std::move(root);
    }
    setCurrentSearchLabel(query);
    scanDirectory(kRecentSearch, true);
    result = "Track: " + ProcessRunner::trim(query) + " | albums " +
        std::to_string(album_count) + " | tracks " + std::to_string(track_count);
    return true;
}

bool AppController::openOfficialAlbum(const Track& album, std::string& result)
{
    if (album.type != EntryType::Album || album.id.empty() || album.sourceId.empty()) {
        result = "Invalid album search result";
        return false;
    }

    size_t cached_track_count = 0;
    {
        std::lock_guard<std::mutex> lock(directoryCacheMutex_);
        const auto cached = virtualPlaylists_.find(album.id);
        if (cached != virtualPlaylists_.end()) {
            cached_track_count = (size_t)std::count_if(
                cached->second.tracks.begin(), cached->second.tracks.end(),
                [](const Track& track) { return track.type == EntryType::File; });
        }
    }
    if (cached_track_count > 0) {
        scanDirectory(album.id);
        result = "Album: " + album.title + " | " +
            std::to_string(cached_track_count) + " cached Art Tracks";
        return true;
    }

    OfficialAlbumSearchResult loaded;
    if (!loadOfficialAlbumTracks(album.sourceId, loaded, result)) {
        return false;
    }
    if (loaded.title.empty()) loaded.title = album.title;
    if (loaded.artist.empty()) loaded.artist = album.artist;
    if (loaded.year.empty()) loaded.year = album.releaseDate;
    if (loaded.thumbnailUrl.empty()) loaded.thumbnailUrl = album.thumbnailUrl;

    const std::size_t album_marker = album.id.rfind("/albums/");
    if (album_marker == std::string::npos) {
        result = "Invalid album search path";
        return false;
    }
    PlaylistImportResult child;
    child.name = loaded.artist.empty() ? loaded.title
        : loaded.artist + " — " + loaded.title;
    if (!loaded.year.empty()) child.name += " (" + loaded.year + ")";
    child.tracks = std::move(loaded.tracks);
    for (auto& track : child.tracks) {
        if (track.album.empty()) track.album = loaded.title;
        if (track.artist.empty()) track.artist = loaded.artist;
        if (track.releaseDate.empty()) track.releaseDate = loaded.year;
        if (track.thumbnailUrl.empty()) track.thumbnailUrl = loaded.thumbnailUrl;
    }
    Track back;
    back.id = album.id.substr(0, album_marker);
    back.title = "..";
    back.type = EntryType::Navigation;
    back.status = TrackStatus::Ready;
    child.tracks.insert(child.tracks.begin(), std::move(back));

    const size_t track_count = child.tracks.size() - 1;
    {
        std::lock_guard<std::mutex> lock(directoryCacheMutex_);
        if (!virtualPlaylists_.contains(album.id)) {
            result = "Album search result expired";
            return false;
        }
        virtualPlaylists_[album.id] = std::move(child);
    }
    scanDirectory(album.id, true);
    result = "Album: " + loaded.title + " | " +
        std::to_string(track_count) + " verified Art Tracks";
    return true;
}

bool AppController::openAlbumFromSearchTrack(const Track& track, std::string& result)
{
    if (track.type != EntryType::File || !isOnlineMediaUrl(track.id)) {
        result = "This is not an online search track";
        return false;
    }

    // Current YTMusic song rows contain the exact MPRE browse id. Search
    // cache files produced by older versions did not retain it, however. In
    // that case resolve the most relevant official release here and still
    // open its tracks directly; never replace the playlist with album cards.
    std::string browse_id = track.sourceId;
    if (browse_id.empty()) {
        if (track.album.empty()) {
            result = "This online track has no album metadata";
            return false;
        }
        const std::string query = track.artist.empty()
            ? track.album
            : track.artist + " " + track.album;
        std::vector<OfficialAlbumSearchResult> candidates;
        if (!searchOfficialAlbums(query, candidates, result)) {
            return false;
        }

        const std::string wanted_album = searchDuplicateKey(track.album);
        const std::string wanted_artist = searchDuplicateKey(track.artist);
        auto score = [&](const OfficialAlbumSearchResult& candidate) {
            const std::string album = searchDuplicateKey(candidate.title);
            const std::string artist = searchDuplicateKey(candidate.artist);
            int value = 0;
            if (!wanted_album.empty()) {
                if (album == wanted_album) value += 10000;
                else if (album.find(wanted_album) != std::string::npos ||
                         wanted_album.find(album) != std::string::npos) value += 1000;
            }
            if (!wanted_artist.empty()) {
                if (artist == wanted_artist) value += 5000;
                else if (artist.find(wanted_artist) != std::string::npos ||
                         wanted_artist.find(artist) != std::string::npos) value += 500;
            }
            return value;
        };
        const auto best = std::max_element(candidates.begin(), candidates.end(),
            [&](const OfficialAlbumSearchResult& left,
                const OfficialAlbumSearchResult& right) {
                return score(left) < score(right);
            });
        if (best == candidates.end() || best->id.empty()) {
            result = "No official album was found for this track";
            return false;
        }
        browse_id = best->id;
    }

    PlaylistImportResult release;
    bool cache_hit = false;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(directoryCacheMutex_);
        const auto cached = onlineAlbumCache_.find(browse_id);
        if (cached != onlineAlbumCache_.end()) {
            if (cached->second.expiresAt > now) {
                release = cached->second.release;
                cache_hit = true;
            } else {
                onlineAlbumCache_.erase(cached);
            }
        }
    }

    if (!cache_hit) {
        OfficialAlbumSearchResult loaded;
        if (!loadOfficialAlbumTracks(browse_id, loaded, result)) {
            return false;
        }
        if (loaded.title.empty()) loaded.title = track.album;
        if (loaded.artist.empty()) loaded.artist = track.artist;
        if (loaded.thumbnailUrl.empty()) loaded.thumbnailUrl = track.thumbnailUrl;
        if (loaded.year.empty()) loaded.year = track.releaseDate;
        if (loaded.title.empty()) {
            result = "Official album metadata is missing";
            return false;
        }

        release.name = loaded.artist.empty()
            ? loaded.title : loaded.artist + " — " + loaded.title;
        if (!loaded.year.empty()) release.name += " (" + loaded.year + ")";
        for (auto& item : loaded.tracks) {
            if (item.album.empty()) item.album = loaded.title;
            if (item.artist.empty()) item.artist = loaded.artist;
            if (item.releaseDate.empty()) item.releaseDate = loaded.year;
            if (item.thumbnailUrl.empty()) item.thumbnailUrl = loaded.thumbnailUrl;
            release.tracks.push_back(std::move(item));
        }
        if (release.tracks.empty()) {
            result = "The official album has no Art Tracks";
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(directoryCacheMutex_);
            onlineAlbumCache_[browse_id] = {
                release,
                now + std::chrono::hours(24),
            };
        }
    }

    if (release.name.empty() || release.tracks.empty()) {
        result = "Official album metadata is missing";
        return false;
    }

    const std::string display_name = release.name;
    const std::string parent_path = currentPath();
    std::string album_path = parent_path + "/albums/" + display_name;
    PlaylistImportResult album = std::move(release);
    Track back;
    back.id = parent_path;
    back.title = "..";
    back.type = EntryType::Navigation;
    back.status = TrackStatus::Ready;
    album.tracks.insert(album.tracks.begin(), std::move(back));
    const size_t track_count = album.tracks.size() - 1;

    {
        std::lock_guard<std::mutex> lock(directoryCacheMutex_);
        int suffix = 2;
        while (virtualPlaylists_.contains(album_path)) {
            album_path = parent_path + "/albums/" + display_name + " " +
                std::to_string(suffix++);
        }
        virtualPlaylists_[album_path] = std::move(album);
    }
    scanDirectory(album_path, true);
    result = "Album: " + display_name + " | Art Tracks " +
        std::to_string(track_count) + (cache_hit ? " (cached)" : "");
    return true;
}

bool AppController::searchLocalMusic(const std::string& query, std::string& result)
{
    const std::string needle = searchDuplicateKey(ProcessRunner::trim(query));
    if (needle.empty()) {
        result = "Enter an artist, title, or file name";
        return false;
    }
    std::vector<Track> matches;
    std::set<std::string> seen;
    for (const auto& directory : config_.musicDirectories) {
        std::error_code error;
        const fs::path root = expandUserPath(directory);
        if (!fs::is_directory(root, error)) continue;
        for (fs::recursive_directory_iterator it(root, error), end;
             !error && it != end; it.increment(error)) {
            if (!it->is_regular_file(error) ||
                !isAllowedFormat(lowerExtension(it->path()))) {
                continue;
            }
            const std::string path = it->path().string();
            const std::string name = it->path().stem().string();
            if (searchDuplicateKey(name).find(needle) == std::string::npos &&
                searchDuplicateKey(path).find(needle) == std::string::npos) {
                continue;
            }
            if (!seen.insert(path).second) continue;
            Track track;
            track.id = path;
            track.title = name;
            track.type = EntryType::File;
            track.status = TrackStatus::Ready;
            track.format = lowerExtension(it->path());
            if (!track.format.empty() && track.format.front() == '.') {
                track.format.erase(track.format.begin());
            }
            track.sizeBytes = it->file_size(error);
            matches.push_back(std::move(track));
        }
    }
    if (matches.empty()) {
        result = "No local matches: " + query;
        return false;
    }
    std::sort(matches.begin(), matches.end(), [](const Track& left, const Track& right) {
        return left.title < right.title;
    });
    PlaylistImportResult local;
    local.name = "Local: " + ProcessRunner::trim(query);
    local.tracks = std::move(matches);
    const size_t count = local.tracks.size();
    {
        std::lock_guard<std::mutex> lock(directoryCacheMutex_);
        virtualPlaylists_[kRecentSearch] = std::move(local);
        virtualPlaylists_[kRecentSearch].name = "Local: " +
            ProcessRunner::trim(query);
    }
    scanDirectory(kRecentSearch, true);
    result = "Local: " + ProcessRunner::trim(query) + " | tracks " +
        std::to_string(count);
    return true;
}

bool AppController::isOnlineMediaUrl(const std::string& value)
{
    return startsWith(value, "https://") || startsWith(value, "http://");
}

bool AppController::startDesktopRecording(std::string& result)
{
    const fs::path destination = fs::path(config_.rootFolder) / "Records";
    temporaryDesktopRecording_ = false;
    return screenRecorder_.start(destination.string(), result);
}

bool AppController::startAudDRecording(std::string& result)
{
    std::error_code error;
    // FIND is not a system-temporary recording: keep the short-lived file in
    // the visible TPlay/Search folder so it stays with the application's
    // library and avoids /var/folders. It is removed immediately after AudD
    // receives it and is never added to Records.
    const fs::path destination = expandUserPath(config_.rootFolder) / "Search";
    const fs::path recording = destination / "temp.wav";
    temporaryDesktopRecording_ = true;
    if (!screenRecorder_.start(destination.string(), result, "temp.wav")) {
        temporaryDesktopRecording_ = false;
        fs::remove(recording, error);
        return false;
    }
    return true;
}

void AppController::stopDesktopRecording()
{
    screenRecorder_.stop();
    const auto snapshot = screenRecorder_.snapshot();
    const bool temporary = temporaryDesktopRecording_.exchange(false);
    if (temporary) {
        return;
    }
    if (!snapshot.filePath.empty()) {
        scanDirectory(fs::path(snapshot.filePath).parent_path().string(), true);
    }
}

RecordingSnapshot AppController::recordingSnapshot() const
{
    return screenRecorder_.snapshot();
}

bool AppController::auddRecordingHasAudio() const
{
    const RecordingSnapshot snapshot = screenRecorder_.snapshot();
    if (snapshot.state != RecordingState::Recording || snapshot.filePath.empty()) {
        return false;
    }

    // TPlayRecorder writes a 44-byte PCM WAV header followed by signed 16-bit
    // stereo samples. Its header is finalized only after recording stops, so
    // inspect the most recent raw samples while it is still being written.
    std::ifstream input(snapshot.filePath, std::ios::binary | std::ios::ate);
    if (!input) return false;
    const std::streamoff end = input.tellg();
    constexpr std::streamoff kWavHeaderBytes = 44;
    constexpr std::streamoff kProbeBytes = 48'000 * 2 * 2; // last 0.5 sec
    if (end <= kWavHeaderBytes) return false;

    std::streamoff bytes = std::min(kProbeBytes, end - kWavHeaderBytes);
    bytes -= bytes % (std::streamoff)sizeof(std::int16_t);
    if (bytes <= 0) return false;
    input.seekg(end - bytes);
    std::vector<std::int16_t> samples((std::size_t)(bytes / sizeof(std::int16_t)));
    input.read(reinterpret_cast<char*>(samples.data()), bytes);
    const std::streamsize count = input.gcount() /
        (std::streamsize)sizeof(std::int16_t);
    if (count <= 0) return false;

    long double sum_squares = 0.0;
    for (std::streamsize index = 0; index < count; ++index) {
        const long double value = samples[(std::size_t)index];
        sum_squares += value * value;
    }
    const double rms = std::sqrt((double)(sum_squares / count));
    // Digital silence from ScreenCaptureKit is zero. This low threshold is
    // deliberate: quiet music should begin FIND, while file/header noise does
    // not trigger it.
    return rms >= 100.0;
}

void AppController::discardAudDRecording(const std::string& filePath) const
{
    if (filePath.empty()) return;
    std::error_code error;
    const fs::path root = expandUserPath(config_.rootFolder) / "Search";
    const fs::path canonical_root = fs::weakly_canonical(root, error);
    if (error) return;
    const fs::path file = fs::weakly_canonical(filePath, error);
    if (error || file.filename() != "temp.wav" || file.parent_path() != canonical_root) {
        return;
    }
    // Search also holds persistent caches and history, so only the temporary
    // AudD audio file may be removed here.
    fs::remove(file, error);
}

bool AppController::auddFindEnabled() const
{
    return auddRecognizer_.configured();
}

int AppController::auddListenSeconds() const
{
    return config_.audd.listenSeconds;
}

bool AppController::recognizeAudDRecording(const std::string& filePath,
                                            AudDMatch& match,
                                            std::string& error) const
{
    return auddRecognizer_.recognize(filePath, match, error);
}

bool AppController::separateTrack(const Track& track) {
    std::string reason;
    if (!canEditTrack(track, reason)) {
        return false;
    }
    DemucsConfig config = config_.demucs;
    if (config.outputDirectory.empty()) {
        config.outputDirectory = (fs::path(config_.rootFolder) / "Separated").string();
    }
    return stemSeparator_.start(track, config);
}

bool AppController::separateTrack(const Track& track, const DemucsConfig& config) {
    std::string reason;
    if (!canEditTrack(track, reason)) {
        return false;
    }
    DemucsConfig effective = config;
    if (effective.outputDirectory.empty()) {
        effective.outputDirectory = (fs::path(config_.rootFolder) / "Separated").string();
    }
    std::error_code ec;
    fs::create_directories(effective.outputDirectory, ec);
    return !ec && stemSeparator_.start(track, effective);
}

StemSeparationSnapshot AppController::stemSeparationSnapshot() const {
    return stemSeparator_.snapshot();
}

bool AppController::normalizeTracks(const std::vector<Track>& tracks,
                                    const NormalizationOptions& options) {
    return audioProcessor_.normalize(tracks, currentPath(), options, [this] {
        scanDirectory(currentPath(), true);
    });
}

bool AppController::convertTracks(const std::vector<Track>& tracks,
                                  const ConvertOptions& options) {
    return audioProcessor_.convert(tracks, currentPath(), options, [this] {
        scanDirectory(currentPath(), true);
    });
}

AudioProcessSnapshot AppController::audioProcessSnapshot() const {
    return audioProcessor_.snapshot();
}

bool AppController::startAutoCueFolder() {
    if (!config_.autoCue.enabled) {
        std::lock_guard<std::mutex> lock(autoCueMutex_);
        autoCueProgress_.status = "Auto Cue disabled";
        autoCueProgress_.done = true;
        return false;
    }

    bool expected = false;
    if (!autoCueBusy_.compare_exchange_strong(expected, true)) {
        return false;
    }

    if (autoCueWorker_.joinable()) {
        autoCueWorker_.request_stop();
        autoCueWorker_.join();
    }

    autoCueCancel_ = false;
    std::string folder = currentPath();
    {
        std::lock_guard<std::mutex> lock(autoCueMutex_);
        autoCueProgress_ = {};
        autoCueProgress_.running = true;
        autoCueProgress_.status = "Starting";
    }

    autoCueWorker_ = std::jthread([this, folder](std::stop_token token) {
        auto progress = [this](const AutoCueProgress& snapshot) {
            std::lock_guard<std::mutex> lock(autoCueMutex_);
            autoCueProgress_ = snapshot;
        };
        auto result = [this](const fs::path& file,
                             const AutoCueResult& cues,
                             const std::vector<SeratoCue>& serato_cues,
                             std::string& error) {
            return saveAutoCueResult(file, cues, serato_cues, error, false);
        };
        autoCueProcessor_.processFolder(
            folder,
            config_.autoCue.writeJson,
            false,
            config_.autoCue.backupBeforeWrite,
            config_.autoCue.overwriteExistingCues,
            config_.autoCue.cleanupAfterWrite,
            config_.autoCue.cues,
            progress,
            autoCueCancel_,
            result);
        if (!token.stop_requested()) {
            std::string export_error;
            exportLibraryCollection(export_error);
        }
        autoCueBusy_ = false;
        if (!token.stop_requested()) {
            scanDirectory(folder, true);
        }
    });
    return true;
}

bool AppController::startAutoCueTrack(const Track& track) {
    if (!config_.autoCue.enabled) {
        std::lock_guard<std::mutex> lock(autoCueMutex_);
        autoCueProgress_.status = "Auto Cue disabled";
        autoCueProgress_.done = true;
        return false;
    }
    std::string local_reason;
    if (!canEditTrack(track, local_reason)) {
        std::lock_guard<std::mutex> lock(autoCueMutex_);
        autoCueProgress_.status = local_reason;
        autoCueProgress_.done = true;
        return false;
    }

    bool expected = false;
    if (!autoCueBusy_.compare_exchange_strong(expected, true)) {
        return false;
    }

    if (autoCueWorker_.joinable()) {
        autoCueWorker_.request_stop();
        autoCueWorker_.join();
    }

    autoCueCancel_ = false;
    std::string folder = currentPath();
    fs::path file = track.id;
    {
        std::lock_guard<std::mutex> lock(autoCueMutex_);
        autoCueProgress_ = {};
        autoCueProgress_.running = true;
        autoCueProgress_.status = "Starting";
    }

    autoCueWorker_ = std::jthread([this, folder, file](std::stop_token token) {
        auto progress = [this](const AutoCueProgress& snapshot) {
            std::lock_guard<std::mutex> lock(autoCueMutex_);
            autoCueProgress_ = snapshot;
        };
        auto result = [this](const fs::path& file,
                             const AutoCueResult& cues,
                             const std::vector<SeratoCue>& serato_cues,
                             std::string& error) {
            return saveAutoCueResult(file, cues, serato_cues, error);
        };
        autoCueProcessor_.processFiles(
            std::vector<fs::path>{file},
            config_.autoCue.writeJson,
            false,
            config_.autoCue.backupBeforeWrite,
            config_.autoCue.overwriteExistingCues,
            config_.autoCue.cleanupAfterWrite,
            config_.autoCue.cues,
            progress,
            autoCueCancel_,
            result);
        autoCueBusy_ = false;
        if (!token.stop_requested()) {
            scanDirectory(folder, true);
        }
    });
    return true;
}

AutoCueProgress AppController::autoCueSnapshot() const {
    std::lock_guard<std::mutex> lock(autoCueMutex_);
    return autoCueProgress_;
}

AutoCueFeatures AppController::waveformForTrack(const Track& track,
                                                std::string& error) const {
    AutoCueFeatures features;
    if (!canEditTrack(track, error)) {
        return features;
    }
    try {
        features = audioAnalyzer_.extractWaveformFeatures(track.id);
    } catch (const std::exception& e) {
        error = std::string("Waveform failed: ") + e.what();
    } catch (...) {
        error = "Waveform failed";
    }
    return features;
}

bool AppController::trimTrack(const Track& track,
                              double startSeconds,
                              double endSeconds,
                              std::string& error,
                              Track* outputTrack) {
    if (!canEditTrack(track, error)) {
        return false;
    }
    if (endSeconds <= startSeconds + 0.05) {
        error = "Trim range is too short";
        return false;
    }
    auto ffmpeg = ProcessRunner::findExecutable("ffmpeg");
    if (!ffmpeg) {
        error = "ffmpeg not found";
        return false;
    }

    fs::path input(track.id);
    fs::path output = input.parent_path() /
        (input.stem().string() + "_trim" + input.extension().string());
    int index = 2;
    std::error_code ec;
    while (fs::exists(output, ec)) {
        output = input.parent_path() /
            (input.stem().string() + "_trim_" + std::to_string(index) +
             input.extension().string());
        index++;
    }

    auto seconds = [](double value) {
        std::ostringstream stream;
        stream << std::fixed << std::setprecision(3) << std::max(0.0, value);
        return stream.str();
    };

    std::vector<std::string> args = {
        *ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
        "-ss", seconds(startSeconds),
        "-to", seconds(endSeconds),
        "-i", input.string(),
        "-map", "0", "-map_metadata", "0",
        "-c", "copy",
        output.string(),
    };

    std::string ffmpeg_output;
    int exit_code = ProcessRunner::runWithCombinedOutput(args, &ffmpeg_output);
    if (exit_code != 0 || !fs::exists(output, ec)) {
        fs::remove(output, ec);
        std::vector<std::string> fallback = {
            *ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
            "-ss", seconds(startSeconds),
            "-to", seconds(endSeconds),
            "-i", input.string(),
            "-map", "0:a:0", "-map_metadata", "0",
            output.string(),
        };
        ffmpeg_output.clear();
        exit_code = ProcessRunner::runWithCombinedOutput(fallback, &ffmpeg_output);
    }
    if (exit_code != 0 || !fs::exists(output, ec)) {
        error = ProcessRunner::trim(ffmpeg_output);
        if (error.empty()) {
            error = "ffmpeg trim failed";
        }
        return false;
    }

    if (outputTrack != nullptr) {
        *outputTrack = track;
        outputTrack->id = output.string();
        outputTrack->title = output.stem().string();
        outputTrack->duration = std::max(0.0, endSeconds - startSeconds);
        try {
            mergeMetadataIntoTrack(*outputTrack,
                                   audioAnalyzer_.readEmbeddedMetadata(outputTrack->id));
        } catch (...) {
        }
    }

    scanDirectory(currentPath(), true);
    return true;
}

bool AppController::writeManualCues(const Track& track,
                                    const std::vector<SeratoCue>& cues,
                                    std::string& error) {
    if (!canEditTrack(track, error)) {
        return false;
    }

    Track cue_track;
    if (!cueSafeTrackForWrite(track, cue_track, error)) {
        return false;
    }

    LibraryTrack library_track = libraryTrackForCues(cue_track, cues);
    if (trackRepository_ && cueRepository_) {
        if (!trackRepository_->upsertTrack(library_track, error)) {
            return false;
        }
        if (!cueRepository_->replaceCues(library_track.id, library_track.cues, error)) {
            return false;
        }
    }
    if (!exportLibraryCues(library_track, error)) {
        return false;
    }
    if (cue_track.id != track.id) {
        scanDirectory(currentPath(), true);
    }
    return true;
}

bool AppController::syncCueMetadata(const Track& track, std::string& result)
{
    return syncCueMetadata(track, CueSyncDirection::Auto, result);
}

bool AppController::syncCueMetadata(const Track& track,
                                    CueSyncDirection direction,
                                    std::string& result)
{
    if (!canEditTrack(track, result)) {
        return false;
    }

    Track read_track = cueSafeTrackForRead(track);

    std::string serato_error;
    auto serato_cues = seratoCueWriter_.readCues(read_track.id, serato_error);
    std::string traktor_error;
    auto traktor_cues = traktorMetadataWriter_.readCues(read_track.id, traktor_error);

    sqlite3* db = libraryDatabase_.isOpen() ? libraryDatabase_.handle() : nullptr;
    std::int64_t now = nowUnixSeconds();

    struct SourceSnapshot {
        std::string name;
        std::vector<SeratoCue> cues;
        std::string hash;
        std::int64_t timestamp = 0;
    };

    std::vector<SourceSnapshot> sources = {
        {"serato", sortedCues(serato_cues), cueHash(serato_cues), 0},
        {"traktor", sortedCues(traktor_cues), cueHash(traktor_cues), 0},
    };

    for (auto& source : sources) {
        std::string old_hash =
            syncStateGet(db, syncStateKey(read_track.id, source.name, "hash"));
        std::int64_t old_time = syncStateTime(db, read_track.id, source.name);
        if (old_hash.empty()) {
            source.timestamp = old_time > 0 ? old_time : (source.cues.empty() ? 0 : now);
        } else if (old_hash != source.hash) {
            source.timestamp = now;
        } else {
            source.timestamp = old_time;
        }
    }

    auto rank = [&](const std::string& source) {
        if (source == config_.autoCue.syncPrefer) {
            return 2;
        }
        if (source == "serato") {
            return 1;
        }
        return 0;
    };

    auto sourceByName = [&](const std::string& name) {
        return std::find_if(sources.begin(), sources.end(),
                            [&](const SourceSnapshot& source) {
                                return source.name == name;
                            });
    };

    auto chosen = sources.end();
    if (direction == CueSyncDirection::SeratoToTraktor) {
        chosen = sourceByName("serato");
    } else if (direction == CueSyncDirection::TraktorToSerato) {
        chosen = sourceByName("traktor");
    } else if (sources.size() == 2 &&
               sources[0].cues.empty() != sources[1].cues.empty()) {
        chosen = sources[0].cues.empty() ? sources.begin() + 1 : sources.begin();
    } else {
        chosen = std::max_element(
            sources.begin(),
            sources.end(),
            [&](const SourceSnapshot& a, const SourceSnapshot& b) {
                if (a.timestamp != b.timestamp) {
                    return a.timestamp < b.timestamp;
                }
                return rank(a.name) < rank(b.name);
            });
    }
    const bool explicit_direction = direction != CueSyncDirection::Auto;
    if (chosen == sources.end() ||
        (!explicit_direction && chosen->cues.empty())) {
        result = "No cue source found";
        return false;
    }

    std::vector<SeratoCue> chosen_cues = sortedCues(chosen->cues);
    if (chosen->name == "traktor") {
        chosen_cues = colorizeImportedTraktorCues(chosen_cues, config_.autoCue);
    }

    Track cue_track;
    if (!cueSafeTrackForWrite(track, cue_track, result)) {
        result = "Cue-safe conversion error: " + result;
        return false;
    }

    std::string write_error;
    seratoCueWriter_.writeCues(cue_track.id, {}, write_error, false, true);
    LibraryTrack clear_track = libraryTrackForCues(cue_track, {});
    traktorMetadataWriter_.writeCues(clear_track, write_error);

    LibraryTrack sync_track = libraryTrackForCues(cue_track, chosen_cues);
    auto writeSerato = [&]() {
        if (!seratoCueWriter_.writeCues(cue_track.id,
                                        chosen_cues,
                                        write_error,
                                        false,
                                        true)) {
            result = "Serato sync error: " + write_error;
            return false;
        }
        return true;
    };
    auto writeTraktor = [&]() {
        if (!traktorMetadataWriter_.writeCues(sync_track, write_error)) {
            result = "Traktor sync error: " + write_error;
            return false;
        }
        return true;
    };
    if (!writeSerato() || !writeTraktor()) {
        return false;
    }

    if (trackRepository_ && cueRepository_) {
        if (!trackRepository_->upsertTrack(sync_track, write_error) ||
            !cueRepository_->replaceCues(sync_track.id, sync_track.cues, write_error)) {
            result = "Library cue sync error: " + write_error;
            return false;
        }
    }

    std::int64_t timestamp = nowUnixSeconds();
    std::string hash = cueHash(chosen_cues);
    syncStateTouch(db, cue_track.id, "serato", hash, timestamp);
    syncStateTouch(db, cue_track.id, "traktor", hash, timestamp);

    result = "Cue sync: " + chosen->name + " -> Serato + Traktor | cues " +
        std::to_string(chosen_cues.size());
    if (cue_track.id != track.id) {
        result += " | " + fs::path(cue_track.id).filename().string();
    }
    scanDirectory(currentPath(), true);
    return true;
}

bool AppController::exportLibrary(std::string& error)
{
    if (!trackRepository_ || !cueRepository_) {
        error = "Library database is disabled";
        return false;
    }
    LibraryExporter exporter(*trackRepository_, *cueRepository_, seratoCueWriter_);
    LibraryExportOptions options;
    options.exportSerato = config_.library.exportSerato;
    options.exportRekordbox = config_.library.exportRekordbox;
    options.exportTraktor = config_.library.exportTraktor;
    options.exportJson = config_.library.exportJson;
    options.backupBeforeSeratoWrite = config_.autoCue.backupBeforeWrite;
    options.overwriteExistingSeratoCues = config_.autoCue.overwriteExistingCues;
    options.syncFolder = expandUserPath(config_.library.syncFolder);
    options.outputFolder = options.syncFolder / "exports";
    return exporter.exportAll(options, error);
}

bool AppController::exportLibrary(std::string& result, bool validateAfterExport)
{
    std::string error;
    if (!exportLibrary(error)) {
        result = error;
        return false;
    }
    if (validateAfterExport &&
        (config_.library.exportRekordbox ||
         config_.library.exportTraktor ||
         config_.library.exportJson)) {
        if (!validateLibraryExport(result)) {
            return false;
        }
        return true;
    }
    result = "Library exported: " + enabledLibraryExportsLabel();
    return true;
}

bool AppController::importSeratoCues(std::string& result)
{
    if (!trackRepository_ || !cueRepository_) {
        result = "Library database is disabled";
        return false;
    }

    std::string error;
    auto tracks = trackRepository_->listTracks(error);
    if (!error.empty()) {
        result = error;
        return false;
    }

    int scanned = 0;
    int imported_tracks = 0;
    int imported_cues = 0;
    for (const auto& track : tracks) {
        scanned++;
        auto existing = cueRepository_->cuesForTrack(track.id, error);
        if (!error.empty()) {
            result = error;
            return false;
        }
        if (!existing.empty() || track.path.empty()) {
            continue;
        }

        std::string serato_error;
        auto serato_cues = seratoCueWriter_.readCues(track.path, serato_error);
        if (serato_cues.empty()) {
            continue;
        }

        Track app_track;
        app_track.id = track.path.string();
        app_track.title = track.title.empty()
            ? track.path.stem().string()
            : track.title;
        app_track.duration = track.duration;
        app_track.bpm = track.bpm;
        app_track.key = track.key;
        app_track.genre = track.genre;
        app_track.sizeBytes = track.fileSize;
        app_track.type = EntryType::File;

        LibraryTrack imported = libraryTrackForCues(app_track, serato_cues);
        if (imported.id != track.id) {
            imported.id = track.id;
        }
        if (!cueRepository_->replaceCues(track.id, imported.cues, error)) {
            result = error;
            return false;
        }
        imported_tracks++;
        imported_cues += (int)imported.cues.size();
    }

    result = "Serato cue import: scanned " + std::to_string(scanned) +
        " tracks | imported " + std::to_string(imported_tracks) +
        " tracks | cues " + std::to_string(imported_cues);
    return true;
}

std::string AppController::enabledLibraryExportsLabel() const
{
    std::vector<std::string> exports;
    if (config_.library.exportSerato) {
        exports.push_back("Serato");
    }
    if (config_.library.exportRekordbox) {
        exports.push_back("Rekordbox");
    }
    if (config_.library.exportTraktor) {
        exports.push_back("Traktor");
    }
    if (config_.library.exportJson) {
        exports.push_back("JSON");
    }
    if (exports.empty()) {
        return "none";
    }
    std::ostringstream stream;
    for (std::size_t i = 0; i < exports.size(); ++i) {
        if (i > 0) {
            stream << " | ";
        }
        stream << exports[i];
    }
    return stream.str();
}

bool AppController::importChangedJson(std::string& error)
{
    if (!trackRepository_ || !cueRepository_) {
        error = "Library database is disabled";
        return false;
    }

    fs::path folder = expandUserPath(config_.library.syncFolder);
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) {
        error = "Sync folder not found: " + folder.string();
        return false;
    }

    JsonSync sync;
    ConflictResolver resolver;
    int imported = 0;
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        if (ec) {
            error = ec.message();
            return false;
        }
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".json") {
            continue;
        }

        auto incoming = sync.importTrack(entry.path(), error);
        if (!incoming) {
            return false;
        }

        auto local = trackRepository_->findById(incoming->id, error);
        if (local) {
            local->cues = cueRepository_->cuesForTrack(local->id, error);
            if (!error.empty()) {
                return false;
            }
        }
        LibraryTrack merged = local
            ? resolver.resolve(*local, *incoming)
            : *incoming;
        if (local && merged.updatedAt == local->updatedAt &&
            merged.contentHash == local->contentHash &&
            merged.cues.size() == local->cues.size()) {
            continue;
        }

        if (!trackRepository_->upsertTrack(merged, error)) {
            return false;
        }
        if (!cueRepository_->replaceCues(merged.id, merged.cues, error)) {
            return false;
        }
        if (!exportLibraryCues(merged, error)) {
            return false;
        }
        imported++;
    }

    error = "Imported JSON tracks: " + std::to_string(imported);
    return true;
}

bool AppController::validateLibraryExport(std::string& result)
{
    ExportValidator validator;
    ExportValidationSummary summary;
    fs::path sync_folder = expandUserPath(config_.library.syncFolder);
    fs::path export_folder = sync_folder / "exports";
    ExportValidationOptions options;
    options.validateRekordbox = config_.library.exportRekordbox;
    options.validateTraktor = config_.library.exportTraktor;
    options.validateJson = config_.library.exportJson;
    std::string error;
    if (!validator.validateExportFolder(export_folder,
                                        sync_folder,
                                        options,
                                        summary,
                                        error)) {
        result = error;
        return false;
    }

    result = "Export valid: Rekordbox tracks " +
        std::to_string(summary.rekordboxTracks) + ", cues " +
        std::to_string(summary.rekordboxCues) + " | Traktor tracks " +
        std::to_string(summary.traktorTracks) + ", cues " +
        std::to_string(summary.traktorCues) + " | JSON tracks " +
        std::to_string(summary.jsonTracks) + ", cues " +
        std::to_string(summary.jsonCues);
    return true;
}

bool AppController::validateSeratoCues(std::string& result) const
{
    if (!trackRepository_ || !cueRepository_) {
        result = "Library database is disabled";
        return false;
    }

    std::string error;
    auto tracks = trackRepository_->listTracks(error);
    if (!error.empty()) {
        result = error;
        return false;
    }

    int checked_tracks = 0;
    int matched_tracks = 0;
    int mismatched_tracks = 0;
    int library_cues_total = 0;
    int serato_cues_total = 0;
    std::string mismatch_example;
    for (const auto& track : tracks) {
        auto library_cues = cueRepository_->cuesForTrack(track.id, error);
        if (!error.empty()) {
            result = error;
            return false;
        }
        if (library_cues.empty()) {
            continue;
        }

        checked_tracks++;
        library_cues_total += (int)library_cues.size();

        std::string serato_error;
        auto serato_cues = seratoCueWriter_.readCues(track.path, serato_error);
        serato_cues_total += (int)serato_cues.size();
        bool matches = serato_cues.size() == library_cues.size();
        for (const auto& library_cue : library_cues) {
            auto serato_match = std::find_if(
                serato_cues.begin(),
                serato_cues.end(),
                [&](const SeratoCue& cue) {
                    return cue.index == library_cue.index;
                });
            if (serato_match == serato_cues.end()) {
                matches = false;
                if (mismatch_example.empty()) {
                    mismatch_example = track.title + " missing cue index " +
                        std::to_string(library_cue.index);
                }
                break;
            }
            bool name_matches = serato_match->name == library_cue.name;
            bool position_matches =
                std::abs(serato_match->seconds - library_cue.positionSeconds) <= 0.02;
            if (!name_matches || !position_matches) {
                matches = false;
                if (mismatch_example.empty()) {
                    std::ostringstream stream;
                    stream << track.title << " cue " << library_cue.index
                           << " differs";
                    if (!name_matches) {
                        stream << " name";
                    }
                    if (!position_matches) {
                        stream << " position";
                    }
                    mismatch_example = stream.str();
                }
                break;
            }
        }
        if (matches) {
            matched_tracks++;
        } else {
            mismatched_tracks++;
        }
    }

    result = "Serato validation: checked " + std::to_string(checked_tracks) +
        " tracks | matched " + std::to_string(matched_tracks) +
        " | mismatched " + std::to_string(mismatched_tracks) +
        " | library cues " + std::to_string(library_cues_total) +
        " | serato cues " + std::to_string(serato_cues_total);
    if (!mismatch_example.empty()) {
        result += " | first mismatch: " + mismatch_example;
    }
    return mismatched_tracks == 0;
}

bool AppController::validateTraktorEmbeddedCues(std::string& result) const
{
    if (!trackRepository_ || !cueRepository_) {
        result = "Library database is disabled";
        return false;
    }

    std::string error;
    auto tracks = trackRepository_->listTracks(error);
    if (!error.empty()) {
        result = error;
        return false;
    }

    int checked_tracks = 0;
    int supported_tracks = 0;
    int tagged_tracks = 0;
    int missing_tags = 0;
    int matched_tracks = 0;
    int mismatched_tracks = 0;
    int unsupported_tracks = 0;
    int library_cues_total = 0;
    int embedded_cues_total = 0;
    std::string first_missing;
    for (const auto& track : tracks) {
        auto library_cues = cueRepository_->cuesForTrack(track.id, error);
        if (!error.empty()) {
            result = error;
            return false;
        }
        if (library_cues.empty()) {
            continue;
        }

        checked_tracks++;
        library_cues_total += (int)library_cues.size();
        std::string inspect_error;
        auto status = traktorMetadataWriter_.inspect(track.path, inspect_error);
        if (!inspect_error.empty()) {
            if (first_missing.empty()) {
                first_missing = track.title + ": " + inspect_error;
            }
            missing_tags++;
            continue;
        }
        if (!status.supportedContainer) {
            unsupported_tracks++;
            continue;
        }
        supported_tracks++;
        if (status.hasTraktor4Tag) {
            tagged_tracks++;
            std::string read_error;
            auto traktor_cues = traktorMetadataWriter_.readCues(track.path, read_error);
            embedded_cues_total += (int)traktor_cues.size();
            bool matches = read_error.empty() &&
                traktor_cues.size() == library_cues.size();
            for (const auto& library_cue : library_cues) {
                auto embedded = std::find_if(
                    traktor_cues.begin(),
                    traktor_cues.end(),
                    [&](const SeratoCue& cue) {
                        return cue.index == library_cue.index;
                    });
                if (embedded == traktor_cues.end()) {
                    matches = false;
                    break;
                }
                bool name_matches = embedded->name == library_cue.name ||
                    embedded->name == "n.n.";
                bool position_matches =
                    std::abs(embedded->seconds - library_cue.positionSeconds) <= 0.02;
                if (!name_matches || !position_matches) {
                    matches = false;
                    break;
                }
            }
            if (matches) {
                matched_tracks++;
            } else {
                mismatched_tracks++;
                if (first_missing.empty()) {
                    first_missing = track.title + ": TRAKTOR4 cues differ";
                }
            }
        } else {
            missing_tags++;
            if (first_missing.empty()) {
                first_missing = track.title + ": " + status.detail;
            }
        }
    }

    result = "Traktor embedded validation: checked " +
        std::to_string(checked_tracks) +
        " tracks | supported " + std::to_string(supported_tracks) +
        " | tagged " + std::to_string(tagged_tracks) +
        " | matched " + std::to_string(matched_tracks) +
        " | mismatched " + std::to_string(mismatched_tracks) +
        " | missing " + std::to_string(missing_tags) +
        " | unsupported " + std::to_string(unsupported_tracks) +
        " | library cues " + std::to_string(library_cues_total) +
        " | embedded cues " + std::to_string(embedded_cues_total);
    if (!first_missing.empty()) {
        result += " | first missing: " + first_missing;
    }
    return missing_tags == 0 && unsupported_tracks == 0 && mismatched_tracks == 0;
}

bool AppController::libraryStatus(std::string& result) const
{
    if (!trackRepository_) {
        result = "Library database is disabled";
        return false;
    }
    std::string error;
    LibraryStats stats = trackRepository_->stats(error);
    if (!error.empty()) {
        result = error;
        return false;
    }
    std::ostringstream stream;
    stream << "Library: " << libraryDatabase_.path().string()
           << " | tracks " << stats.tracks
           << " | cues " << stats.cues
           << " | loops " << stats.loops
           << " | playlists " << stats.playlists
           << " | exports " << enabledLibraryExportsLabel();
    result = stream.str();
    return true;
}

std::vector<SeratoCue> AppController::readManualCues(const Track& track,
                                                     std::string& error) {
    if (track.type != EntryType::File || track.id.empty()) {
        error = "Select a track";
        return {};
    }

    Track cue_track = cueSafeTrackForRead(track);

    std::string serato_error;
    auto serato_cues = seratoCueWriter_.readCues(cue_track.id, serato_error);

    std::string traktor_error;
    auto traktor_cues = traktorMetadataWriter_.readCues(cue_track.id, traktor_error);

    bool has_traktor_tag = false;
    if (traktor_error.empty()) {
        std::string inspect_error;
        auto traktor_status = traktorMetadataWriter_.inspect(cue_track.id, inspect_error);
        has_traktor_tag = inspect_error.empty() && traktor_status.hasTraktor4Tag;
    }

    std::vector<SeratoCue> chosen = chooseCueSyncSource(
        serato_cues,
        traktor_cues,
        config_.autoCue.syncPrefer);
    bool chosen_from_traktor = serato_cues.empty() && !traktor_cues.empty();
    if (chosen_from_traktor) {
        chosen = colorizeImportedTraktorCues(chosen, config_.autoCue);
    }

    if (chosen.empty()) {
        if (trackRepository_ && cueRepository_) {
            LibraryTrack saved_track = libraryTrackForCues(cue_track, {});
            std::string saved_error;
            saved_track.cues = cueRepository_->cuesForTrack(saved_track.id, saved_error);
            if (saved_error.empty() && !saved_track.cues.empty()) {
                error.clear();
                return sortedCues(seratoCuesFromLibraryTrack(saved_track));
            }
        }
        if (has_traktor_tag || serato_error.empty() || traktor_error.empty()) {
            error.clear();
            return {};
        }
        error = serato_error.empty() ? traktor_error : serato_error;
        return {};
    }

    auto syncSerato = [&]() {
        if (config_.autoCue.writeSerato &&
            !cuePositionsMatch(chosen, serato_cues)) {
            std::string write_error;
            seratoCueWriter_.writeCues(cue_track.id,
                                       chosen,
                                       write_error,
                                       false,
                                       true);
        }
    };
    auto syncTraktor = [&]() {
        if (config_.autoCue.writeTraktor &&
            !cuePositionsMatch(chosen, traktor_cues)) {
            LibraryTrack sync_track = libraryTrackForCues(cue_track, chosen);
            std::string write_error;
            traktorMetadataWriter_.writeCues(sync_track, write_error);
        }
    };
    syncSerato();
    syncTraktor();

    if (trackRepository_ && cueRepository_) {
        LibraryTrack sync_track = libraryTrackForCues(cue_track, chosen);
        std::string write_error;
        if (trackRepository_->upsertTrack(sync_track, write_error)) {
            cueRepository_->replaceCues(sync_track.id, sync_track.cues, write_error);
        }
    }

    if (libraryDatabase_.isOpen()) {
        std::int64_t timestamp = nowUnixSeconds();
        std::string hash = cueHash(chosen);
        sqlite3* db = libraryDatabase_.handle();
        syncStateTouch(db, cue_track.id, "serato", hash, timestamp);
        syncStateTouch(db, cue_track.id, "traktor", hash, timestamp);
    }

    error.clear();
    return sortedCues(chosen);
}

std::string AppController::cuePreviewForTrack(const Track& track, int width) const {
    if (track.type != EntryType::File || width < 12) {
        return {};
    }
    Track cue_track = cueSafeTrackForRead(track);
    auto renderSeratoCues = [&](const std::vector<SeratoCue>& cues) {
        double duration = track.duration;
        if (duration <= 0.0) {
            AudioMetadata metadata = audioAnalyzer_.readEmbeddedMetadata(cue_track.id);
            duration = metadata.duration;
        }
        std::vector<CuePoint> preview_cues;
        preview_cues.reserve(cues.size());
        for (const auto& cue : cues) {
            preview_cues.push_back({cue.name, cue.seconds});
        }
        return renderCuePreview(preview_cues, duration, width);
    };

    std::string serato_error;
    auto serato_cues = seratoCueWriter_.readCues(cue_track.id, serato_error);

    std::string traktor_error;
    auto traktor_cues = traktorMetadataWriter_.readCues(cue_track.id, traktor_error);

    auto chosen_cues = chooseCueSyncSource(
        serato_cues,
        traktor_cues,
        config_.autoCue.syncPrefer);
    if (!chosen_cues.empty()) {
        return renderSeratoCues(chosen_cues);
    }
    if (trackRepository_ && cueRepository_) {
        LibraryTrack saved_track = libraryTrackForCues(cue_track, {});
        std::string saved_error;
        saved_track.cues = cueRepository_->cuesForTrack(saved_track.id, saved_error);
        if (saved_error.empty() && !saved_track.cues.empty()) {
            return renderSeratoCues(seratoCuesFromLibraryTrack(saved_track));
        }
    }
    if (traktor_error.empty()) {
        std::string inspect_error;
        auto traktor_status = traktorMetadataWriter_.inspect(cue_track.id, inspect_error);
        if (inspect_error.empty() && traktor_status.hasTraktor4Tag) {
            return {};
        }
    }

    std::filesystem::path json = cue_track.id;
    json += ".cues.json";
    std::ifstream stream(json);
    AutoCueResult cues;
    bool has_cues = false;
    if (stream) {
        std::string content((std::istreambuf_iterator<char>(stream)),
                            std::istreambuf_iterator<char>());
        auto numberAfter = [&](const std::string& key) {
            size_t pos = content.find("\"" + key + "\"");
            if (pos == std::string::npos) {
                return 0.0;
            }
            pos = content.find(':', pos);
            if (pos == std::string::npos) {
                return 0.0;
            }
            try {
                return std::stod(content.substr(pos + 1));
            } catch (...) {
                return 0.0;
            }
        };
        cues.start.positionSeconds = numberAfter("start");
        cues.drop1.positionSeconds = numberAfter("drop1");
        cues.breakdown.positionSeconds = numberAfter("break");
        cues.drop2.positionSeconds = numberAfter("drop2");
        has_cues = cues.start.positionSeconds > 0.0 ||
            cues.drop1.positionSeconds > 0.0 ||
            cues.breakdown.positionSeconds > 0.0 ||
            cues.drop2.positionSeconds > 0.0;
    }

    if (!has_cues) {
        return {};
    }

    double duration = track.duration;
    if (duration <= 0.0) {
        AudioMetadata metadata = audioAnalyzer_.readEmbeddedMetadata(track.id);
        duration = metadata.duration;
    }
    return renderCuePreview(cues, duration, width);
}

bool AppController::metadataBusy() const {
    return metadataBusy_.load();
}

bool AppController::directoryScanBusy() const {
    return directoryScansInFlight_.load() > 0;
}

namespace {

bool validFolderName(const std::string& name) {
    return !name.empty() && name != "." && name != ".." &&
           name.find('/') == std::string::npos;
}

std::string decodeFlatValue(std::string value) {
    value = ProcessRunner::trim(std::move(value));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }

    std::string decoded;
    decoded.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '\\' || i + 1 >= value.size()) {
            decoded += value[i];
            continue;
        }

        char next = value[++i];
        if (next == 'n') {
            decoded += '\n';
        } else if (next == 'r') {
            decoded += '\r';
        } else if (next == 't') {
            decoded += '\t';
        } else {
            decoded += next;
        }
    }
    return decoded;
}

std::string cleanMetadataName(std::string name) {
    constexpr std::string_view format_tags = "format.tags.";
    constexpr std::string_view format = "format.";
    constexpr std::string_view streams = "streams.stream.";

    if (name.starts_with(format_tags)) {
        return "TAG:" + name.substr(format_tags.size());
    }
    if (name.starts_with(format)) {
        return name.substr(format.size());
    }
    if (name.starts_with(streams)) {
        std::string rest = name.substr(streams.size());
        size_t dot = rest.find('.');
        if (dot != std::string::npos) {
            std::string stream = rest.substr(0, dot);
            std::string field = rest.substr(dot + 1);
            constexpr std::string_view tags = "tags.";
            if (field.starts_with(tags)) {
                return "STREAM " + stream + " TAG:" + field.substr(tags.size());
            }
            return "stream " + stream + " " + field;
        }
    }
    return name;
}

}  // namespace

bool AppController::createFolder(const std::string& name, std::string& error) {
    if (!validFolderName(name)) {
        error = "Invalid folder name";
        return false;
    }

    fs::path path = fs::path(currentPath()) / name;
    std::error_code ec;
    if (!fs::create_directory(path, ec)) {
        error = ec ? ec.message() : "Folder already exists";
        return false;
    }
    scanDirectory(currentPath(), true);
    return true;
}

bool AppController::renameFolder(const std::string& path,
                                 const std::string& new_name,
                                 std::string& error) {
    if (!validFolderName(new_name) || !fs::is_directory(path)) {
        error = "Invalid folder name or selection";
        return false;
    }
    for (const auto& root : config_.musicDirectories) {
        if (fs::path(path) == fs::path(root)) {
            error = "A library root cannot be renamed here";
            return false;
        }
    }

    fs::path source(path);
    fs::path target = source.parent_path() / new_name;
    std::error_code ec;
    fs::rename(source, target, ec);
    if (ec) {
        error = ec.message();
        return false;
    }

    if (fs::path(currentPath()) == source) {
        scanDirectory(target.string(), true);
    } else {
        scanDirectory(currentPath(), true);
    }
    return true;
}

bool AppController::moveTrack(const Track& track,
                              const std::string& destination,
                              std::string& error) {
    if (track.type != EntryType::File || destination.empty()) {
        error = "Select a track and destination folder";
        return false;
    }

    fs::path target_dir(destination);
    if (!target_dir.is_absolute()) {
        target_dir = fs::path(currentPath()) / target_dir;
    }
    if (!fs::is_directory(target_dir)) {
        error = "Destination folder does not exist";
        return false;
    }

    fs::path target = target_dir / fs::path(track.id).filename();
    if (target == fs::path(track.id)) {
        error = "Track is already in this folder";
        return false;
    }
    if (fs::exists(target)) {
        error = "A track with this name already exists in destination";
        return false;
    }

    std::error_code ec;
    fs::rename(track.id, target, ec);
    if (ec) {
        error = ec.message();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(playbackMutex_);
        for (auto& queued : playbackQueue_) {
            if (queued.id == track.id) {
                queued.id = target.string();
            }
        }
        if (playingTrackId_ == track.id) {
            playingTrackId_ = target.string();
            playingTrack_.id = target.string();
        }
    }

    invalidateDirectoryCache(target_dir.string());
    removeCachedEntry(currentPath(), track.id);
    trackStore_.removeTrack(track.id);
    {
        std::lock_guard<std::mutex> lock(playbackMutex_);
        displayedTracks_.erase(
            std::remove_if(displayedTracks_.begin(), displayedTracks_.end(),
                           [&](const Track& displayed) {
                               return displayed.id == track.id;
                           }),
            displayedTracks_.end());
    }
    return true;
}

bool AppController::deleteEntry(const Track& entry, std::string& error) {
    if (entry.id.empty()) {
        error = "Select a track or folder";
        return false;
    }

    fs::path path(entry.id);
    if (!fs::exists(path)) {
        error = "Selected item does not exist";
        return false;
    }

    for (const auto& root : config_.musicDirectories) {
        if (fs::equivalent(path, fs::path(root))) {
            error = "A library root cannot be deleted here";
            return false;
        }
    }

    std::error_code ec;
    bool is_directory = fs::is_directory(path, ec);
    if (ec) {
        error = ec.message();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(playbackMutex_);
        auto is_inside_deleted_directory = [&](const std::string& queued_path) {
            std::error_code relative_error;
            fs::path relative = fs::relative(queued_path, path, relative_error);
            return !relative_error &&
                   !relative.empty() &&
                   !relative.string().starts_with("..");
        };

        if ((!is_directory && playingTrackId_ == entry.id) ||
            (is_directory && is_inside_deleted_directory(playingTrackId_))) {
            audioEngine_.stop();
            playingTrackId_.clear();
        }
        playbackQueue_.erase(
            std::remove_if(playbackQueue_.begin(), playbackQueue_.end(),
                           [&](const Track& queued) {
                               return queued.id == entry.id ||
                                      (is_directory && is_inside_deleted_directory(queued.id));
                           }),
            playbackQueue_.end());
    }

    if (is_directory) {
        fs::remove_all(path, ec);
    } else {
        fs::remove(path, ec);
    }
    if (ec) {
        error = ec.message();
        return false;
    }

    if (is_directory) {
        scanDirectory(currentPath(), true);
    } else {
        removeCachedEntry(currentPath(), entry.id);
        trackStore_.removeTrack(entry.id);
        {
            std::lock_guard<std::mutex> lock(playbackMutex_);
            displayedTracks_.erase(
                std::remove_if(displayedTracks_.begin(), displayedTracks_.end(),
                               [&](const Track& track) {
                                   return track.id == entry.id;
                               }),
                displayedTracks_.end());
        }
    }
    return true;
}

bool AppController::startExternalDrag(const Track& track, std::string& error) const {
    if (track.type != EntryType::File || !fs::is_regular_file(track.id)) {
        error = "Select an audio track";
        return false;
    }

#if defined(__APPLE__)
    auto drag = ProcessRunner::findExecutable("drag");
    if (!drag) {
        error = "Install dragterm: github.com/Wevah/dragterm";
        return false;
    }
    if (!ProcessRunner::launchDetached({*drag, track.id})) {
        error = "Unable to start external drag";
        return false;
    }
    return true;
#else
    error = "External drag is available on macOS";
    return false;
#endif
}

bool AppController::openFolderExternally(const std::string& path, std::string& error) const {
    if (path.empty() || !fs::is_directory(path)) {
        error = "Select a folder";
        return false;
    }

#if defined(__APPLE__)
    if (!ProcessRunner::launchDetached({"open", path})) {
        error = "Unable to open Finder";
        return false;
    }
    return true;
#else
    if (!ProcessRunner::launchDetached({"xdg-open", path})) {
        error = "Unable to open folder";
        return false;
    }
    return true;
#endif
}

bool AppController::openExternalUrl(const std::string& url, std::string& error) const {
    if (!url.starts_with("http://") && !url.starts_with("https://")) {
        error = "Unsupported URL";
        return false;
    }

#if defined(__APPLE__)
    if (!ProcessRunner::launchDetached({"open", url})) {
        error = "Unable to open URL";
        return false;
    }
    return true;
#else
    if (!ProcessRunner::launchDetached({"xdg-open", url})) {
        error = "Unable to open URL";
        return false;
    }
    return true;
#endif
}

std::vector<std::pair<std::string, std::string>>
AppController::metadataDetails(const Track& track) const {
    std::vector<std::pair<std::string, std::string>> details;
    std::set<std::string> seen;

    auto add = [&](std::string name, std::string value) {
        if (name.empty() || value == "N/A") {
            return;
        }
        std::string key = name + "\n" + value;
        if (seen.insert(key).second) {
            details.emplace_back(std::move(name), std::move(value));
        }
    };

    add("title", track.title);
    add("path", track.id);
    if (track.duration > 0.0) {
        add("duration", std::to_string((int)track.duration) + " sec");
    }
    if (track.bpm > 0.0) {
        add("bpm", std::to_string((int)track.bpm));
    }
    add("key", track.key);
    add("genre", track.genre);
    if (track.bitrateKbps > 0.0) {
        add("bitrate", std::to_string((int)(track.bitrateKbps + 0.5)) + " kbps");
    }
    if (track.sampleRateHz > 0.0) {
        add("sample rate", std::to_string((int)(track.sampleRateHz + 0.5)) + " Hz");
    }
    if (track.sizeBytes > 0) {
        add("size", std::to_string(track.sizeBytes) + " bytes");
    }

    if (track.id.empty() || track.type != EntryType::File) {
        return details;
    }

    auto ffprobe = ProcessRunner::findExecutable("ffprobe");
    if (!ffprobe) {
        add("error", "ffprobe not found");
        return details;
    }

    std::string output;
    int exit_code = ProcessRunner::run({
        *ffprobe,
        "-v", "error",
        "-show_entries",
        "format=format_name,format_long_name,duration,size,bit_rate:format_tags:"
        "stream=index,codec_type,codec_name,codec_long_name,sample_rate,channels,"
        "channel_layout,bits_per_sample,duration,bit_rate:stream_tags",
        "-of", "flat",
        track.id,
    }, &output);
    if (exit_code != 0) {
        add("error", "Unable to read metadata");
        return details;
    }

    std::stringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        size_t separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        add(cleanMetadataName(ProcessRunner::trim(line.substr(0, separator))),
            decodeFlatValue(line.substr(separator + 1)));
    }

    return details;
}
