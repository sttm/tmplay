#include "ExportValidator.h"

#include "../Library/LibraryJson.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <string_view>

namespace fs = std::filesystem;

namespace {

std::string readFile(const fs::path& file, std::string& error)
{
    std::ifstream stream(file);
    if (!stream) {
        error = "Could not read export file: " + file.string();
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(stream)),
                       std::istreambuf_iterator<char>());
}

int countOccurrences(const std::string& text, const std::string& needle)
{
    int count = 0;
    std::size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        count++;
        pos += needle.size();
    }
    return count;
}

bool requireContains(const std::string& text,
                     const std::string& needle,
                     const std::string& label,
                     std::string& error)
{
    if (text.find(needle) != std::string::npos) {
        return true;
    }
    error = "Invalid " + label + ": missing " + needle;
    return false;
}

std::string xmlAttribute(std::string_view tag, std::string_view name)
{
    std::string needle = std::string(name) + "=\"";
    std::size_t pos = tag.find(needle);
    if (pos == std::string_view::npos) {
        return {};
    }
    pos += needle.size();
    std::size_t end = tag.find('"', pos);
    if (end == std::string_view::npos) {
        return {};
    }
    return std::string(tag.substr(pos, end - pos));
}

double doubleAttribute(std::string_view tag, std::string_view name)
{
    std::string value = xmlAttribute(tag, name);
    if (value.empty()) {
        return 0.0;
    }
    try {
        return std::stod(value);
    } catch (...) {
        return 0.0;
    }
}

int intAttribute(std::string_view tag, std::string_view name)
{
    std::string value = xmlAttribute(tag, name);
    if (value.empty()) {
        return 0;
    }
    try {
        return std::stoi(value);
    } catch (...) {
        return 0;
    }
}

std::string htmlDecode(std::string value)
{
    struct Replacement {
        const char* from;
        const char* to;
    };
    static constexpr Replacement replacements[] = {
        {"&quot;", "\""},
        {"&apos;", "'"},
        {"&lt;", "<"},
        {"&gt;", ">"},
        {"&amp;", "&"},
    };
    for (const auto& replacement : replacements) {
        std::size_t pos = 0;
        while ((pos = value.find(replacement.from, pos)) != std::string::npos) {
            value.replace(pos, std::strlen(replacement.from), replacement.to);
            pos += std::strlen(replacement.to);
        }
    }
    return value;
}

int hexValue(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

std::string percentDecode(std::string value)
{
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            int high = hexValue(value[i + 1]);
            int low = hexValue(value[i + 2]);
            if (high >= 0 && low >= 0) {
                decoded += (char)((high << 4) | low);
                i += 2;
                continue;
            }
        }
        decoded += value[i];
    }
    return decoded;
}

std::string normalizeTrackKey(std::string value)
{
    value = percentDecode(htmlDecode(std::move(value)));
    constexpr std::string_view file_prefix = "file://localhost";
    if (value.starts_with(file_prefix)) {
        value.erase(0, file_prefix.size());
    }
    return value;
}

std::string cueSignature(const std::string& track,
                         int index,
                         const std::string& name,
                         double seconds)
{
    std::ostringstream stream;
    stream << normalizeTrackKey(track) << "|" << index << "|" << name << "|"
           << (long long)std::llround(seconds * 1000.0);
    return stream.str();
}

std::vector<std::string_view> tagsNamed(std::string_view xml,
                                        std::string_view tagName)
{
    std::vector<std::string_view> tags;
    std::string open = "<" + std::string(tagName);
    std::size_t pos = 0;
    while ((pos = xml.find(open, pos)) != std::string_view::npos) {
        std::size_t end = xml.find('>', pos);
        if (end == std::string_view::npos) {
            break;
        }
        tags.push_back(xml.substr(pos, end - pos + 1));
        pos = end + 1;
    }
    return tags;
}

std::set<std::string> rekordboxCueSignatures(const std::string& xml)
{
    std::set<std::string> signatures;
    std::size_t pos = 0;
    while ((pos = xml.find("<TRACK ", pos)) != std::string::npos) {
        std::size_t track_tag_end = xml.find('>', pos);
        if (track_tag_end == std::string::npos) {
            break;
        }
        std::size_t track_end = xml.find("</TRACK>", track_tag_end);
        if (track_end == std::string::npos) {
            break;
        }
        std::string_view track_tag(xml.data() + pos, track_tag_end - pos + 1);
        std::string location = htmlDecode(xmlAttribute(track_tag, "Location"));
        std::string_view body(xml.data() + track_tag_end + 1,
                              track_end - track_tag_end - 1);
        for (auto cue_tag : tagsNamed(body, "POSITION_MARK")) {
            signatures.insert(cueSignature(
                location,
                intAttribute(cue_tag, "Num"),
                htmlDecode(xmlAttribute(cue_tag, "Name")),
                doubleAttribute(cue_tag, "Start")));
        }
        pos = track_end + 8;
    }
    return signatures;
}

std::set<std::string> traktorCueSignatures(const std::string& nml)
{
    std::set<std::string> signatures;
    std::size_t pos = 0;
    while ((pos = nml.find("<ENTRY ", pos)) != std::string::npos) {
        std::size_t entry_tag_end = nml.find('>', pos);
        if (entry_tag_end == std::string::npos) {
            break;
        }
        std::size_t entry_end = nml.find("</ENTRY>", entry_tag_end);
        if (entry_end == std::string::npos) {
            break;
        }
        std::string_view body(nml.data() + entry_tag_end + 1,
                              entry_end - entry_tag_end - 1);
        std::string track_key;
        auto locations = tagsNamed(body, "LOCATION");
        if (!locations.empty()) {
            track_key = htmlDecode(xmlAttribute(locations.front(), "DIR")) +
                htmlDecode(xmlAttribute(locations.front(), "FILE"));
        }
        for (auto cue_tag : tagsNamed(body, "CUE_V2")) {
            signatures.insert(cueSignature(
                track_key,
                intAttribute(cue_tag, "HOTCUE") - 1,
                htmlDecode(xmlAttribute(cue_tag, "NAME")),
                doubleAttribute(cue_tag, "START")));
        }
        pos = entry_end + 8;
    }
    return signatures;
}

std::set<std::string> jsonCueSignatures(const fs::path& folder,
                                        std::string& error)
{
    std::set<std::string> signatures;
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) {
        error = "JSON sync folder not found: " + folder.string();
        return signatures;
    }
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        if (ec) {
            error = ec.message();
            return {};
        }
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".json") {
            continue;
        }
        std::string json = readFile(entry.path(), error);
        if (!error.empty()) {
            return {};
        }
        auto track = LibraryJson::trackFromJson(json);
        if (!track) {
            error = "Invalid tmplay track JSON: " + entry.path().string();
            return {};
        }
        for (const auto& cue : track->cues) {
            signatures.insert(cueSignature(
                "file://localhost" + track->path.string(),
                cue.index,
                cue.name,
                cue.positionSeconds));
        }
    }
    return signatures;
}

bool compareCueSignatures(const std::set<std::string>& expected,
                          const std::set<std::string>& actual,
                          const char* label,
                          std::string& error)
{
    if (expected == actual) {
        return true;
    }
    std::string example;
    for (const auto& signature : expected) {
        if (!actual.contains(signature)) {
            example = signature;
            break;
        }
    }
    if (example.empty()) {
        for (const auto& signature : actual) {
            if (!expected.contains(signature)) {
                example = signature;
                break;
            }
        }
    }
    error = std::string("Export cue signature mismatch: ") + label;
    if (!example.empty()) {
        error += " differs at " + example;
    }
    return false;
}

}  // namespace

bool ExportValidator::validateRekordboxXml(
    const fs::path& file,
    ExportValidationSummary& summary,
    std::string& error) const
{
    std::string xml = readFile(file, error);
    if (!error.empty()) {
        return false;
    }
    if (!requireContains(xml, "<DJ_PLAYLISTS", "Rekordbox XML", error) ||
        !requireContains(xml, "<COLLECTION", "Rekordbox XML", error) ||
        !requireContains(xml, "<PLAYLISTS>", "Rekordbox XML", error)) {
        return false;
    }
    summary.rekordboxTracks += countOccurrences(xml, "<TRACK TrackID=");
    summary.rekordboxCues += countOccurrences(xml, "<POSITION_MARK ");
    if (summary.rekordboxTracks <= 0) {
        error = "Invalid Rekordbox XML: no tracks";
        return false;
    }
    return true;
}

bool ExportValidator::validateTraktorNml(
    const fs::path& file,
    ExportValidationSummary& summary,
    std::string& error) const
{
    std::string nml = readFile(file, error);
    if (!error.empty()) {
        return false;
    }
    if (!requireContains(nml, "<NML", "Traktor NML", error) ||
        !requireContains(nml, "<COLLECTION", "Traktor NML", error) ||
        !requireContains(nml, "<PLAYLISTS>", "Traktor NML", error)) {
        return false;
    }
    summary.traktorTracks += countOccurrences(nml, "<ENTRY MODIFIED_DATE=");
    summary.traktorCues += countOccurrences(nml, "<CUE_V2 ");
    if (summary.traktorTracks <= 0) {
        error = "Invalid Traktor NML: no tracks";
        return false;
    }
    return true;
}

bool ExportValidator::validateJsonFolder(
    const fs::path& folder,
    ExportValidationSummary& summary,
    std::string& error) const
{
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) {
        error = "JSON sync folder not found: " + folder.string();
        return false;
    }
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        if (ec) {
            error = ec.message();
            return false;
        }
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".json") {
            continue;
        }
        std::string json = readFile(entry.path(), error);
        if (!error.empty()) {
            return false;
        }
        if (!requireContains(json, "\"id\"", "JSON sync", error) ||
            !requireContains(json, "\"path\"", "JSON sync", error) ||
            !requireContains(json, "\"cues\"", "JSON sync", error)) {
            return false;
        }
        summary.jsonTracks++;
        summary.jsonCues += countOccurrences(json, "\"index\":");
    }
    if (summary.jsonTracks <= 0) {
        error = "JSON sync folder has no track files";
        return false;
    }
    return true;
}

bool ExportValidator::validateExportFolder(
    const fs::path& exportFolder,
    const fs::path& syncFolder,
    ExportValidationSummary& summary,
    std::string& error) const
{
    return validateExportFolder(exportFolder,
                                syncFolder,
                                ExportValidationOptions{},
                                summary,
                                error);
}

bool ExportValidator::validateExportFolder(
    const fs::path& exportFolder,
    const fs::path& syncFolder,
    const ExportValidationOptions& options,
    ExportValidationSummary& summary,
    std::string& error) const
{
    summary = {};
    fs::path rekordbox = exportFolder / "tmplay_library.rekordbox.xml";
    fs::path traktor = exportFolder / "tmplay_library.traktor.nml";
    if (options.validateRekordbox &&
        !validateRekordboxXml(rekordbox, summary, error)) {
        return false;
    }
    if (options.validateTraktor &&
        !validateTraktorNml(traktor, summary, error)) {
        return false;
    }
    if (options.validateJson &&
        !validateJsonFolder(syncFolder, summary, error)) {
        return false;
    }
    int expected_cues = -1;
    auto checkCueCount = [&](bool enabled, int count, const char* label) {
        if (!enabled) {
            return true;
        }
        if (expected_cues < 0) {
            expected_cues = count;
            return true;
        }
        if (count == expected_cues) {
            return true;
        }
        error = std::string("Export cue count mismatch: ") + label +
            " has " + std::to_string(count) +
            ", expected " + std::to_string(expected_cues);
        return false;
    };
    if (!checkCueCount(options.validateRekordbox,
                       summary.rekordboxCues,
                       "Rekordbox") ||
        !checkCueCount(options.validateTraktor,
                       summary.traktorCues,
                       "Traktor") ||
        !checkCueCount(options.validateJson,
                       summary.jsonCues,
                       "JSON")) {
        return false;
    }
    if (options.validateJson &&
        (options.validateRekordbox || options.validateTraktor)) {
        auto json_signatures = jsonCueSignatures(syncFolder, error);
        if (!error.empty()) {
            return false;
        }
        if (options.validateRekordbox) {
            std::string xml = readFile(rekordbox, error);
            if (!error.empty()) {
                return false;
            }
            if (!compareCueSignatures(json_signatures,
                                      rekordboxCueSignatures(xml),
                                      "Rekordbox vs JSON",
                                      error)) {
                return false;
            }
        }
        if (options.validateTraktor) {
            std::string nml = readFile(traktor, error);
            if (!error.empty()) {
                return false;
            }
            if (!compareCueSignatures(json_signatures,
                                      traktorCueSignatures(nml),
                                      "Traktor vs JSON",
                                      error)) {
                return false;
            }
        }
    }
    if (!options.validateRekordbox &&
        !options.validateTraktor &&
        !options.validateJson) {
        error = "No export validators enabled";
        return false;
    }
    return true;
}
