#include "TrackStore.hpp"

#include <algorithm>

void TrackStore::addTrack(const Track& track) {
    std::lock_guard<std::mutex> lock(mutex_);
    tracks_.push_back(track);
}

void TrackStore::setTracks(const std::vector<Track>& tracks) {
    std::lock_guard<std::mutex> lock(mutex_);
    tracks_ = tracks;
}

bool TrackStore::updateTrack(const Track& track) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& existing : tracks_) {
        if (existing.id == track.id) {
            existing = track;
            return true;
        }
    }
    return false;
}

bool TrackStore::removeTrack(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto old_size = tracks_.size();
    tracks_.erase(
        std::remove_if(tracks_.begin(), tracks_.end(),
                       [&](const Track& track) { return track.id == id; }),
        tracks_.end());
    return tracks_.size() != old_size;
}

std::vector<Track> TrackStore::getTracks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tracks_;
}
