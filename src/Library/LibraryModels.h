#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct LibraryCue {
    int index = 0;
    std::string name;
    std::string type;
    double positionSeconds = 0.0;
    std::string color;
};

struct LibraryLoop {
    std::string name;
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    std::string color;
};

struct LibraryPlaylist {
    std::string id;
    std::string name;
    std::string description;
    std::vector<std::string> trackIds;
    std::int64_t createdAt = 0;
    std::int64_t updatedAt = 0;
};

struct LibraryTrack {
    std::string id;
    std::filesystem::path path;
    std::string title;
    std::string artist;
    std::string album;
    double duration = 0.0;
    double bpm = 0.0;
    std::string key;
    std::string camelotKey;
    std::string genre;
    int rating = 0;
    std::string comments;
    std::uintmax_t fileSize = 0;
    std::int64_t fileMtime = 0;
    std::string contentHash;
    std::string waveformPath;
    std::vector<LibraryCue> cues;
    std::vector<LibraryLoop> loops;
    std::int64_t createdAt = 0;
    std::int64_t updatedAt = 0;
};
