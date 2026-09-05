#include "PlaylistImporter.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>

namespace fs = std::filesystem;

namespace {

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return value;
}

std::string trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string percentDecode(std::string value)
{
    std::string decoded;
    decoded.reserve(value.size());
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            int high = hex(value[i + 1]);
            int low = hex(value[i + 2]);
            if (high >= 0 && low >= 0) {
                decoded.push_back((char)(high * 16 + low));
                i += 2;
                continue;
            }
        }
        decoded.push_back(value[i]);
    }
    return decoded;
}

std::string normalizePath(std::string path)
{
    path = trim(percentDecode(std::move(path)));
    constexpr std::string_view localhost = "file://localhost";
    constexpr std::string_view file = "file://";
    if (path.rfind(localhost, 0) == 0) {
        path.erase(0, localhost.size());
    } else if (path.rfind(file, 0) == 0) {
        path.erase(0, file.size());
    }
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

std::string readTextFile(const fs::path& file, std::string& error)
{
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        error = "Could not open playlist file";
        return {};
    }
    return {std::istreambuf_iterator<char>(input), {}};
}

std::string attribute(const std::string& tag, const std::string& name)
{
    const std::regex pattern(name + R"(\s*=\s*([\"'])(.*?)\1)",
                             std::regex::icase);
    std::smatch match;
    return std::regex_search(tag, match, pattern) ? match[2].str() : std::string{};
}

std::vector<std::string> parseM3u(const std::string& text)
{
    std::vector<std::string> paths;
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        line = trim(line);
        if (!line.empty() && line.front() != '#') {
            paths.push_back(normalizePath(std::move(line)));
        }
    }
    return paths;
}

std::vector<std::string> parseCrate(const std::string& data)
{
    std::vector<std::string> paths;
    const auto* bytes = reinterpret_cast<const unsigned char*>(data.data());
    const std::size_t size = data.size();
    std::function<void(std::size_t, std::size_t)> scan =
        [&](std::size_t begin, std::size_t end) {
            std::size_t offset = begin;
            while (offset + 8 <= end) {
                std::string tag(reinterpret_cast<const char*>(bytes + offset), 4);
                const std::size_t length = ((std::size_t)bytes[offset + 4] << 24) |
                                           ((std::size_t)bytes[offset + 5] << 16) |
                                           ((std::size_t)bytes[offset + 6] << 8) |
                                           (std::size_t)bytes[offset + 7];
                const std::size_t payload = offset + 8;
                if (length > end - payload) {
                    return;
                }
                if (tag == "ptrk") {
                    std::string path;
                    for (std::size_t i = 0; i + 1 < length; i += 2) {
                        const unsigned char high = bytes[payload + i];
                        const unsigned char low = bytes[payload + i + 1];
                        if (high == 0 && low == 0) break;
                        if (high == 0) path.push_back((char)low);
                    }
                    if (!path.empty()) paths.push_back(normalizePath(std::move(path)));
                } else if (!tag.empty() && tag.front() == 'o') {
                    scan(payload, payload + length);
                }
                offset = payload + length;
            }
        };
    scan(0, size);
    return paths;
}

std::vector<std::string> parseTraktor(const std::string& xml)
{
    std::vector<std::string> paths;
    const std::regex location(R"(<LOCATION\b[^>]*>)", std::regex::icase);
    for (auto it = std::sregex_iterator(xml.begin(), xml.end(), location);
         it != std::sregex_iterator(); ++it) {
        const std::string tag = it->str();
        const std::string file = attribute(tag, "FILE");
        if (file.empty()) continue;
        paths.push_back(normalizePath(attribute(tag, "VOLUME") +
                                      attribute(tag, "DIR") + file));
    }
    return paths;
}

std::vector<std::string> parseRekordbox(const std::string& xml)
{
    std::vector<std::string> paths;
    const std::regex track(R"(<TRACK\b[^>]*>)", std::regex::icase);
    for (auto it = std::sregex_iterator(xml.begin(), xml.end(), track);
         it != std::sregex_iterator(); ++it) {
        const std::string path = attribute(it->str(), "Location");
        if (!path.empty()) paths.push_back(normalizePath(path));
    }
    return paths;
}

}  // namespace

bool PlaylistImporter::importFile(const std::string& playlistPath,
                                  PlaylistImportResult& result,
                                  std::string& error)
{
    const fs::path file(playlistPath);
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        error = "Playlist file does not exist";
        return false;
    }
    const std::string extension = lower(file.extension().string());
    const std::string content = readTextFile(file, error);
    if (!error.empty()) return false;

    std::vector<std::string> paths;
    if (extension == ".crate") paths = parseCrate(content);
    else if (extension == ".m3u" || extension == ".m3u8") paths = parseM3u(content);
    else if (extension == ".nml") paths = parseTraktor(content);
    else if (extension == ".xml") paths = parseRekordbox(content);
    else {
        error = "Supported playlist files: .crate, .m3u, .m3u8, .nml, .xml";
        return false;
    }

    result = {};
    result.name = file.stem().string();
    std::set<std::string> seen;
    for (auto path : paths) {
        if (path.empty() || !seen.insert(lower(path)).second) continue;
        fs::path trackPath(path);
        if (trackPath.is_relative()) trackPath = file.parent_path() / trackPath;
        if (!fs::is_regular_file(trackPath, ec)) {
            result.missingFiles++;
            continue;
        }
        Track track;
        track.id = trackPath.lexically_normal().string();
        track.title = trackPath.stem().string();
        track.type = EntryType::File;
        track.status = TrackStatus::Ready;
        track.sizeBytes = fs::file_size(trackPath, ec);
        if (ec) track.sizeBytes = 0;
        result.tracks.push_back(std::move(track));
    }
    if (result.tracks.empty()) {
        error = paths.empty() ? "Playlist contains no track references" :
                                "No referenced audio files were found locally";
        return false;
    }
    return true;
}
