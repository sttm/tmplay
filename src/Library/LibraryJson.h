#pragma once

#include "LibraryModels.h"

#include <optional>
#include <string>

namespace LibraryJson {

std::string trackToJson(const LibraryTrack& track);
std::optional<LibraryTrack> trackFromJson(const std::string& json);

}  // namespace LibraryJson
