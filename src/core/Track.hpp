#pragma once

#include <cstdint>
#include <string>

enum class TrackStatus {
    Ready,
    Downloading,
    Analyzing,
    Error
};
enum class EntryType {
    File,
    Directory,
    // Virtual album-search rows and the ".." return row live in the
    // playlist pane, but are never playable media files.
    Album,
    Navigation
};

struct Track {
    std::string id;
    std::string title;
    std::string artist;
    std::string album;
    // Search-only metadata, used when a direct stream is saved locally.
    std::string thumbnailUrl;
    std::string releaseDate;
    // Provider-specific, opaque ID used to fetch a virtual album only when
    // the user opens it. It is never used as a media URL.
    std::string sourceId;
    // Set for YouTube Music results; it distinguishes an explicit album cut
    // from an otherwise identical clean release in the search UI.
    bool isExplicit = false;

    double duration = 0.0;
    double bpm = 0.0;
    double bitrateKbps = 0.0;
    double sampleRateHz = 0.0;
    std::uintmax_t sizeBytes = 0;
    // For online entries this is the selected source container (e.g. webm).
    // Local files derive the value from their path when it is empty.
    std::string format;
    EntryType type = EntryType::File;
    std::string key;
    std::string genre;
    TrackStatus status = TrackStatus::Ready;
};
