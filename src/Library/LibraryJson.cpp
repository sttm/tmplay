#include "LibraryJson.h"

#include <charconv>
#include <cctype>
#include <sstream>
#include <string_view>
#include <vector>

namespace {

std::string escapeJson(const std::string& value)
{
    std::ostringstream stream;
    for (char c : value) {
        switch (c) {
        case '\\':
            stream << "\\\\";
            break;
        case '"':
            stream << "\\\"";
            break;
        case '\n':
            stream << "\\n";
            break;
        case '\r':
            stream << "\\r";
            break;
        case '\t':
            stream << "\\t";
            break;
        default:
            stream << c;
            break;
        }
    }
    return stream.str();
}

std::string jsonStringField(std::string_view json, std::string_view key)
{
    std::string needle = "\"" + std::string(key) + "\":\"";
    std::size_t pos = json.find(needle);
    if (pos == std::string_view::npos) {
        return {};
    }
    pos += needle.size();
    std::string value;
    bool escaped = false;
    for (; pos < json.size(); ++pos) {
        char c = json[pos];
        if (escaped) {
            switch (c) {
            case 'n':
                value += '\n';
                break;
            case 'r':
                value += '\r';
                break;
            case 't':
                value += '\t';
                break;
            default:
                value += c;
                break;
            }
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') {
            break;
        }
        value += c;
    }
    return value;
}

double jsonNumberField(std::string_view json, std::string_view key)
{
    std::string needle = "\"" + std::string(key) + "\":";
    std::size_t pos = json.find(needle);
    if (pos == std::string_view::npos) {
        return 0.0;
    }
    pos += needle.size();
    std::size_t end = pos;
    while (end < json.size() &&
           (std::isdigit((unsigned char)json[end]) || json[end] == '.' ||
            json[end] == '-' || json[end] == '+')) {
        ++end;
    }
    double value = 0.0;
    std::from_chars(json.data() + pos, json.data() + end, value);
    return value;
}

std::string objectField(std::string_view object, std::string_view key)
{
    return jsonStringField(object, key);
}

std::vector<std::string_view> arrayObjects(std::string_view json, std::string_view key)
{
    std::vector<std::string_view> objects;
    std::string needle = "\"" + std::string(key) + "\":[";
    std::size_t pos = json.find(needle);
    if (pos == std::string_view::npos) {
        return objects;
    }
    pos += needle.size();
    int depth = 0;
    std::size_t object_start = std::string_view::npos;
    bool in_string = false;
    bool escaped = false;
    for (; pos < json.size(); ++pos) {
        char c = json[pos];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            if (depth == 0) {
                object_start = pos;
            }
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0 && object_start != std::string_view::npos) {
                objects.push_back(json.substr(object_start, pos - object_start + 1));
                object_start = std::string_view::npos;
            }
        } else if (c == ']' && depth == 0) {
            break;
        }
    }
    return objects;
}

}  // namespace

namespace LibraryJson {

std::string trackToJson(const LibraryTrack& track)
{
    std::ostringstream stream;
    stream << "{";
    stream << "\"id\":\"" << escapeJson(track.id) << "\",";
    stream << "\"path\":\"" << escapeJson(track.path.string()) << "\",";
    stream << "\"title\":\"" << escapeJson(track.title) << "\",";
    stream << "\"artist\":\"" << escapeJson(track.artist) << "\",";
    stream << "\"album\":\"" << escapeJson(track.album) << "\",";
    stream << "\"duration\":" << track.duration << ",";
    stream << "\"bpm\":" << track.bpm << ",";
    stream << "\"key\":\"" << escapeJson(track.key) << "\",";
    stream << "\"camelot_key\":\"" << escapeJson(track.camelotKey) << "\",";
    stream << "\"genre\":\"" << escapeJson(track.genre) << "\",";
    stream << "\"rating\":" << track.rating << ",";
    stream << "\"comments\":\"" << escapeJson(track.comments) << "\",";
    stream << "\"file_size\":" << track.fileSize << ",";
    stream << "\"file_mtime\":" << track.fileMtime << ",";
    stream << "\"content_hash\":\"" << escapeJson(track.contentHash) << "\",";
    stream << "\"waveform_path\":\"" << escapeJson(track.waveformPath) << "\",";
    stream << "\"updated_at\":" << track.updatedAt << ",";
    stream << "\"cues\":[";
    for (std::size_t i = 0; i < track.cues.size(); ++i) {
        const auto& cue = track.cues[i];
        if (i > 0) {
            stream << ",";
        }
        stream << "{";
        stream << "\"index\":" << cue.index << ",";
        stream << "\"name\":\"" << escapeJson(cue.name) << "\",";
        stream << "\"type\":\"" << escapeJson(cue.type) << "\",";
        stream << "\"position\":" << cue.positionSeconds << ",";
        stream << "\"color\":\"" << escapeJson(cue.color) << "\"";
        stream << "}";
    }
    stream << "],";
    stream << "\"loops\":[";
    for (std::size_t i = 0; i < track.loops.size(); ++i) {
        const auto& loop = track.loops[i];
        if (i > 0) {
            stream << ",";
        }
        stream << "{";
        stream << "\"name\":\"" << escapeJson(loop.name) << "\",";
        stream << "\"start\":" << loop.startSeconds << ",";
        stream << "\"end\":" << loop.endSeconds << ",";
        stream << "\"color\":\"" << escapeJson(loop.color) << "\"";
        stream << "}";
    }
    stream << "]";
    stream << "}";
    return stream.str();
}

std::optional<LibraryTrack> trackFromJson(const std::string& json)
{
    LibraryTrack track;
    track.id = jsonStringField(json, "id");
    track.path = jsonStringField(json, "path");
    if (track.id.empty() || track.path.empty()) {
        return std::nullopt;
    }
    track.title = jsonStringField(json, "title");
    track.artist = jsonStringField(json, "artist");
    track.album = jsonStringField(json, "album");
    track.duration = jsonNumberField(json, "duration");
    track.bpm = jsonNumberField(json, "bpm");
    track.key = jsonStringField(json, "key");
    track.camelotKey = jsonStringField(json, "camelot_key");
    track.genre = jsonStringField(json, "genre");
    track.rating = (int)jsonNumberField(json, "rating");
    track.comments = jsonStringField(json, "comments");
    track.fileSize = (std::uintmax_t)jsonNumberField(json, "file_size");
    track.fileMtime = (std::int64_t)jsonNumberField(json, "file_mtime");
    track.contentHash = jsonStringField(json, "content_hash");
    track.waveformPath = jsonStringField(json, "waveform_path");
    track.updatedAt = (std::int64_t)jsonNumberField(json, "updated_at");

    for (auto object : arrayObjects(json, "cues")) {
        LibraryCue cue;
        cue.index = (int)jsonNumberField(object, "index");
        cue.name = objectField(object, "name");
        cue.type = objectField(object, "type");
        cue.positionSeconds = jsonNumberField(object, "position");
        cue.color = objectField(object, "color");
        track.cues.push_back(cue);
    }

    for (auto object : arrayObjects(json, "loops")) {
        LibraryLoop loop;
        loop.name = objectField(object, "name");
        loop.startSeconds = jsonNumberField(object, "start");
        loop.endSeconds = jsonNumberField(object, "end");
        loop.color = objectField(object, "color");
        track.loops.push_back(loop);
    }
    return track;
}

}  // namespace LibraryJson
