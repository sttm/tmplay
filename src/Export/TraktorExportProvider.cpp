#include "TraktorExportProvider.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;

namespace {

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

fs::path traktorTrackExportPath(const LibraryTrack& track)
{
    fs::path output = track.path;
    output += ".traktor.nml";
    return output;
}

std::string traktorDirectory(const fs::path& path)
{
    std::string directory = fs::absolute(path.parent_path()).string();
    if (!directory.empty() && directory.back() != '/') {
        directory += "/";
    }
    return directory;
}

std::string traktorPrimaryKey(const LibraryTrack& track)
{
    return fs::absolute(track.path).string();
}

void writeTrackEntry(std::ofstream& stream, const LibraryTrack& track)
{
    stream << "    <ENTRY MODIFIED_DATE=\"\" MODIFIED_TIME=\"\" AUDIO_ID=\""
           << escapeXml(track.id) << "\">\n";
    stream << "      <LOCATION DIR=\"" << escapeXml(traktorDirectory(track.path))
           << "\" FILE=\"" << escapeXml(track.path.filename().string())
           << "\" VOLUME=\"\" VOLUMEID=\"\"/>\n";
    stream << "      <INFO TITLE=\"" << escapeXml(track.title)
           << "\" ARTIST=\"" << escapeXml(track.artist)
           << "\" ALBUM=\"" << escapeXml(track.album)
           << "\" GENRE=\"" << escapeXml(track.genre)
           << "\" COMMENT=\"" << escapeXml(track.comments)
           << "\" RATING=\"" << track.rating
           << "\" KEY=\"" << escapeXml(track.key)
           << "\" PLAYTIME=\"" << (int)track.duration << "\"/>\n";
    if (track.bpm > 0.0) {
        stream << "      <TEMPO BPM=\"" << std::fixed << std::setprecision(3)
               << track.bpm << "\" BPM_QUALITY=\"100.000\"/>\n";
    }
    for (const auto& cue : track.cues) {
        stream << "      <CUE_V2 NAME=\"" << escapeXml(cue.name)
               << "\" DISPL_ORDER=\"" << cue.index
               << "\" TYPE=\"0\" START=\""
               << std::fixed << std::setprecision(3) << cue.positionSeconds
               << "\" LEN=\"0.000\" REPEATS=\"-1\" HOTCUE=\"" << cue.index + 1
               << "\"/>\n";
    }
    stream << "    </ENTRY>\n";
}

void writeTraktorHeader(std::ofstream& stream)
{
    stream << R"(<?xml version="1.0" encoding="UTF-8" standalone="no" ?>)" << "\n";
    stream << "<NML VERSION=\"19\">\n";
    stream << "  <HEAD COMPANY=\"www.native-instruments.com\" PROGRAM=\"tmplay\"/>\n";
}

void writeTraktorPlaylist(std::ofstream& stream,
                          const std::string& name,
                          const std::vector<LibraryTrack>& tracks)
{
    stream << "  <PLAYLISTS>\n";
    stream << "    <NODE TYPE=\"FOLDER\" NAME=\"$ROOT\">\n";
    stream << "      <SUBNODES COUNT=\"1\">\n";
    stream << "        <NODE TYPE=\"PLAYLIST\" NAME=\"" << escapeXml(name) << "\">\n";
    stream << "          <PLAYLIST ENTRIES=\"" << tracks.size()
           << "\" TYPE=\"LIST\" UUID=\"\">\n";
    for (const auto& track : tracks) {
        stream << "            <ENTRY>\n";
        stream << "              <PRIMARYKEY TYPE=\"TRACK\" KEY=\""
               << escapeXml(traktorPrimaryKey(track)) << "\"/>\n";
        stream << "            </ENTRY>\n";
    }
    stream << "          </PLAYLIST>\n";
    stream << "        </NODE>\n";
    stream << "      </SUBNODES>\n";
    stream << "    </NODE>\n";
    stream << "  </PLAYLISTS>\n";
}

}  // namespace

bool TraktorExportProvider::exportTrack(const LibraryTrack& track, std::string& error)
{
    if (track.path.empty()) {
        error = "Traktor export skipped: empty track path";
        return false;
    }
    fs::path output = traktorTrackExportPath(track);
    std::ofstream stream(output);
    if (!stream) {
        error = "Could not write Traktor NML: " + output.string();
        return false;
    }

    writeTraktorHeader(stream);
    stream << "  <COLLECTION ENTRIES=\"1\">\n";
    writeTrackEntry(stream, track);
    stream << "  </COLLECTION>\n";
    writeTraktorPlaylist(stream, "tmplay Export", std::vector<LibraryTrack>{track});
    stream << "</NML>\n";
    return true;
}

bool TraktorExportProvider::exportPlaylist(const LibraryPlaylist& playlist,
                                           const std::vector<LibraryTrack>& tracks,
                                           std::string& error)
{
    fs::path output = playlist.id.empty()
        ? fs::path("tmplay_traktor.nml")
        : fs::path(playlist.id + ".traktor.nml");
    if (!output.parent_path().empty()) {
        std::error_code ec;
        fs::create_directories(output.parent_path(), ec);
    }
    std::ofstream stream(output);
    if (!stream) {
        error = "Could not write Traktor playlist NML: " + output.string();
        return false;
    }
    writeTraktorHeader(stream);
    stream << "  <COLLECTION ENTRIES=\"" << tracks.size() << "\">\n";
    for (const auto& track : tracks) {
        writeTrackEntry(stream, track);
    }
    stream << "  </COLLECTION>\n";
    writeTraktorPlaylist(stream,
                         playlist.name.empty()
                             ? std::string("tmplay Library")
                             : playlist.name,
                         tracks);
    stream << "</NML>\n";
    return true;
}
