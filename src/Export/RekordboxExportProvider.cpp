#include "RekordboxExportProvider.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <cctype>
#include <charconv>

namespace fs = std::filesystem;

namespace {

struct CueRgb {
    int red = 255;
    int green = 255;
    int blue = 255;
};

std::string escapeXml(const std::string& value)
{
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&apos;";
            break;
        default:
            out += c;
            break;
        }
    }
    return out;
}

std::string cueColor(const LibraryCue& cue)
{
    if (cue.color.empty()) {
        return "#ffffff";
    }
    if (cue.color.starts_with("#")) {
        return cue.color;
    }
    if (cue.color.starts_with("0x") || cue.color.starts_with("0X")) {
        return "#" + cue.color.substr(2);
    }
    return "#" + cue.color;
}

CueRgb cueRgb(const LibraryCue& cue)
{
    std::string value = cueColor(cue);
    if (value.starts_with("#")) {
        value.erase(value.begin());
    }
    unsigned int rgb = 0xffffffu;
    if (value.size() == 6) {
        std::from_chars(value.data(), value.data() + value.size(), rgb, 16);
    }
    return {
        (int)((rgb >> 16) & 0xffu),
        (int)((rgb >> 8) & 0xffu),
        (int)(rgb & 0xffu),
    };
}

std::string percentEncodePath(const std::string& path)
{
    std::ostringstream stream;
    stream << std::hex << std::uppercase << std::setfill('0');
    for (unsigned char c : path) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            stream << (char)c;
        } else {
            stream << '%' << std::setw(2) << (int)c;
        }
    }
    return stream.str();
}

std::string fileUri(const fs::path& path)
{
    return "file://localhost" + percentEncodePath(fs::absolute(path).string());
}

fs::path rekordboxTrackExportPath(const LibraryTrack& track)
{
    fs::path output = track.path;
    output += ".rekordbox.xml";
    return output;
}

}  // namespace

bool RekordboxExportProvider::exportTrack(const LibraryTrack& track, std::string& error)
{
    if (track.path.empty()) {
        error = "Rekordbox export skipped: empty track path";
        return false;
    }

    fs::path output = rekordboxTrackExportPath(track);
    std::ofstream stream(output);
    if (!stream) {
        error = "Could not write Rekordbox XML: " + output.string();
        return false;
    }

    stream << R"(<?xml version="1.0" encoding="UTF-8"?>)" << "\n";
    stream << R"(<DJ_PLAYLISTS Version="1.0.0">)" << "\n";
    stream << R"(  <PRODUCT Name="tmplay" Version="1.0" Company="tmplay"/>)" << "\n";
    stream << "  <COLLECTION Entries=\"1\">\n";
    stream << "    <TRACK TrackID=\"1\" Name=\"" << escapeXml(track.title)
           << "\" Artist=\"" << escapeXml(track.artist)
           << "\" Album=\"" << escapeXml(track.album)
           << "\" Location=\"" << escapeXml(fileUri(track.path))
           << "\" TotalTime=\"" << (int)track.duration
           << "\" AverageBpm=\"" << std::fixed << std::setprecision(2) << track.bpm
           << "\" Tonality=\"" << escapeXml(track.key)
           << "\" Genre=\"" << escapeXml(track.genre)
           << "\" Comments=\"" << escapeXml(track.comments)
           << "\" Rating=\"" << track.rating << "\">\n";
    for (const auto& cue : track.cues) {
        CueRgb rgb = cueRgb(cue);
        stream << "      <POSITION_MARK Name=\"" << escapeXml(cue.name)
               << "\" Type=\"0\" Start=\""
               << std::fixed << std::setprecision(3) << cue.positionSeconds
               << "\" Num=\"" << cue.index
               << "\" Red=\"" << rgb.red
               << "\" Green=\"" << rgb.green
               << "\" Blue=\"" << rgb.blue
               << "\" Color=\""
               << escapeXml(cueColor(cue)) << "\"/>\n";
    }
    stream << "    </TRACK>\n";
    stream << "  </COLLECTION>\n";
    stream << "  <PLAYLISTS>\n";
    stream << "    <NODE Type=\"0\" Name=\"ROOT\" Count=\"1\">\n";
    stream << "      <NODE Name=\"tmplay Export\" Type=\"1\" KeyType=\"0\" Entries=\"1\">\n";
    stream << "        <TRACK Key=\"1\"/>\n";
    stream << "      </NODE>\n";
    stream << "    </NODE>\n";
    stream << "  </PLAYLISTS>\n";
    stream << "</DJ_PLAYLISTS>\n";
    return true;
}

bool RekordboxExportProvider::exportPlaylist(const LibraryPlaylist& playlist,
                                             const std::vector<LibraryTrack>& tracks,
                                             std::string& error)
{
    fs::path output = playlist.id.empty()
        ? fs::path("tmplay_rekordbox.xml")
        : fs::path(playlist.id + ".rekordbox.xml");
    if (!output.parent_path().empty()) {
        std::error_code ec;
        fs::create_directories(output.parent_path(), ec);
    }
    std::ofstream stream(output);
    if (!stream) {
        error = "Could not write Rekordbox playlist XML: " + output.string();
        return false;
    }
    stream << R"(<?xml version="1.0" encoding="UTF-8"?>)" << "\n";
    stream << R"(<DJ_PLAYLISTS Version="1.0.0">)" << "\n";
    stream << R"(  <PRODUCT Name="tmplay" Version="1.0" Company="tmplay"/>)" << "\n";
    stream << "  <COLLECTION Entries=\"" << tracks.size() << "\">\n";
    int track_id = 1;
    for (const auto& track : tracks) {
        int current_id = track_id++;
        stream << "    <TRACK TrackID=\"" << current_id << "\" Name=\""
               << escapeXml(track.title) << "\" Artist=\""
               << escapeXml(track.artist) << "\" Album=\""
               << escapeXml(track.album) << "\" Location=\""
               << escapeXml(fileUri(track.path)) << "\" TotalTime=\""
               << (int)track.duration << "\" AverageBpm=\""
               << std::fixed << std::setprecision(2) << track.bpm
               << "\" Tonality=\"" << escapeXml(track.key)
               << "\" Genre=\"" << escapeXml(track.genre)
               << "\" Comments=\"" << escapeXml(track.comments)
               << "\" Rating=\"" << track.rating << "\">\n";
        for (const auto& cue : track.cues) {
            CueRgb rgb = cueRgb(cue);
            stream << "      <POSITION_MARK Name=\"" << escapeXml(cue.name)
                   << "\" Type=\"0\" Start=\""
                   << std::fixed << std::setprecision(3) << cue.positionSeconds
                   << "\" Num=\"" << cue.index
                   << "\" Red=\"" << rgb.red
                   << "\" Green=\"" << rgb.green
                   << "\" Blue=\"" << rgb.blue
                   << "\" Color=\""
                   << escapeXml(cueColor(cue)) << "\"/>\n";
        }
        stream << "    </TRACK>\n";
    }
    stream << "  </COLLECTION>\n";
    stream << "  <PLAYLISTS>\n";
    stream << "    <NODE Type=\"0\" Name=\"ROOT\" Count=\"1\">\n";
    stream << "      <NODE Name=\"" << escapeXml(playlist.name.empty()
               ? std::string("tmplay Library")
               : playlist.name)
           << "\" Type=\"1\" KeyType=\"0\" Entries=\"" << tracks.size() << "\">\n";
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        stream << "        <TRACK Key=\"" << (i + 1) << "\"/>\n";
    }
    stream << "      </NODE>\n";
    stream << "    </NODE>\n";
    stream << "  </PLAYLISTS>\n";
    stream << "</DJ_PLAYLISTS>\n";
    return true;
}
