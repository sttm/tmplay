#include "AutoCueMarker.h"

#include "CueDetector.h"
#include "../core/AudioAnalyzer.hpp"

#include <fstream>

AutoCueMarker::AutoCueMarker(AudioAnalyzer& analyzer)
    : analyzer_(analyzer)
{
}

AutoCueResult AutoCueMarker::analyze(const std::filesystem::path& file,
                                     std::string& error) const
{
    try {
        AutoCueFeatures features = analyzer_.extractAutoCueFeatures(file);
        return CueDetector{}.detect(features);
    } catch (const std::exception& ex) {
        error = ex.what();
    } catch (...) {
        error = "Auto cue analysis failed";
    }
    return {};
}

bool AutoCueMarker::writeDebugJson(const std::filesystem::path& file,
                                   const AutoCueResult& cues,
                                   std::string& error) const
{
    std::filesystem::path output = file;
    output += ".cues.json";
    std::ofstream stream(output);
    if (!stream) {
        error = "Could not write " + output.string();
        return false;
    }
    stream << "{\n"
           << "  \"cue1\": { \"name\": \"" << cues.start.name
           << "\", \"position\": " << cues.start.positionSeconds << " },\n"
           << "  \"cue2\": { \"name\": \"" << cues.drop1.name
           << "\", \"position\": " << cues.drop1.positionSeconds << " },\n"
           << "  \"cue3\": { \"name\": \"" << cues.breakdown.name
           << "\", \"position\": " << cues.breakdown.positionSeconds << " },\n"
           << "  \"cue4\": { \"name\": \"" << cues.drop2.name
           << "\", \"position\": " << cues.drop2.positionSeconds << " },\n"
           << "  \"start\": " << cues.start.positionSeconds << ",\n"
           << "  \"drop1\": " << cues.drop1.positionSeconds << ",\n"
           << "  \"break\": " << cues.breakdown.positionSeconds << ",\n"
           << "  \"drop2\": " << cues.drop2.positionSeconds << "\n"
           << "}\n";
    return true;
}
