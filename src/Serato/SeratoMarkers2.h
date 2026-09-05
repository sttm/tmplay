#pragma once

#include "SeratoCueWriter.h"

#include <cstdint>
#include <vector>

std::vector<std::uint8_t> encodeSeratoMarkers2(const std::vector<SeratoCue>& cues);
std::vector<std::uint8_t> encodeSeratoMarkers2Mp4(const std::vector<SeratoCue>& cues);
std::vector<std::uint8_t> encodeSeratoMarkers2Ogg(const std::vector<SeratoCue>& cues);
std::vector<std::uint8_t> encodeSeratoMarkersLegacy(const std::vector<SeratoCue>& cues);
std::vector<std::uint8_t> encodeSeratoMarkersLegacyMp4(const std::vector<SeratoCue>& cues);
std::vector<SeratoCue> decodeSeratoMarkers2(const std::vector<std::uint8_t>& markers);
std::vector<SeratoCue> decodeSeratoMarkers2Mp4(const std::vector<std::uint8_t>& markers);
std::vector<SeratoCue> decodeSeratoMarkers2Ogg(const std::vector<std::uint8_t>& markers);
