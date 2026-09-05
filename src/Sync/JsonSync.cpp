#include "JsonSync.h"

#include "../Library/LibraryJson.h"

#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

namespace {

std::string safeFileName(std::string value)
{
    for (char& c : value) {
        if (c == '/' || c == '\\' || c == ':' || c == '\0') {
            c = '_';
        }
    }
    if (value.empty()) {
        value = "track";
    }
    return value;
}

}  // namespace

bool JsonSync::exportTrack(const LibraryTrack& track,
                           const fs::path& folder,
                           std::string& error)
{
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (ec) {
        error = ec.message();
        return false;
    }

    fs::path output = folder / (safeFileName(track.id) + ".json");
    std::ofstream stream(output);
    if (!stream) {
        error = "Could not write JSON sync file: " + output.string();
        return false;
    }
    stream << LibraryJson::trackToJson(track) << "\n";
    return true;
}

std::optional<LibraryTrack> JsonSync::importTrack(const fs::path& file,
                                                  std::string& error)
{
    std::ifstream stream(file);
    if (!stream) {
        error = "Could not read JSON sync file: " + file.string();
        return std::nullopt;
    }
    std::string json((std::istreambuf_iterator<char>(stream)),
                     std::istreambuf_iterator<char>());
    auto track = LibraryJson::trackFromJson(json);
    if (!track) {
        error = "Invalid tmplay track JSON: " + file.string();
    }
    return track;
}

bool JsonSync::exportLibrary(const std::filesystem::path&, std::string& error)
{
    error = "Full JSON library export needs a repository cursor";
    return false;
}

bool JsonSync::importLibrary(const std::filesystem::path&, std::string& error)
{
    error = "Full JSON library import needs repository merge wiring";
    return false;
}
