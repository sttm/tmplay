#pragma once

#include "Track.hpp"

#include <vector>
#include <mutex>

class TrackStore {
public:
    void addTrack(const Track& track);
    void setTracks(const std::vector<Track>& tracks);
    bool updateTrack(const Track& track);
    bool removeTrack(const std::string& id);
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        tracks_.clear();
    }
    std::vector<Track> getTracks() const;

private:
    std::vector<Track> tracks_;
    mutable std::mutex mutex_;
};
