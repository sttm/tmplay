#include "FolderProcessor.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <vector>

namespace fs = std::filesystem;

namespace {

bool isAudioFile(const fs::path& file)
{
    std::string ext = file.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return ext == ".mp3" || ext == ".m4a" || ext == ".wav" ||
           ext == ".aiff" || ext == ".aif" || ext == ".flac" ||
           ext == ".ogg" || ext == ".aac" || ext == ".alac";
}

bool isSeratoWritable(const fs::path& file)
{
    std::string ext = file.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return ext == ".mp3" || ext == ".wav" || ext == ".flac" ||
           ext == ".ogg" || ext == ".m4a" || ext == ".mp4" ||
           ext == ".aac" || ext == ".alac" || ext == ".aif" ||
           ext == ".aiff";
}

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return value;
}

double positionForType(const AutoCueResult& cues,
                       const std::string& rawType,
                       int index)
{
    std::string type = lower(rawType);
    auto clamp = [&](double seconds) {
        if (cues.duration <= 0.0) {
            return std::max(0.0, seconds);
        }
        return std::clamp(seconds, 0.0, std::max(0.0, cues.duration - 0.25));
    };
    if (type == "start" || type == "first_sound" || type == "first_bar") {
        return clamp(cues.start.positionSeconds);
    }
    if (type == "intro_end" || type == "verse_start" || type == "mix_in") {
        return clamp(cues.drop1.positionSeconds);
    }
    if (type == "chorus_start" || type == "first_drop" ||
        type == "drop" || type == "energy_peak" || type == "buildup") {
        return clamp(cues.breakdown.positionSeconds);
    }
    if (type == "bridge_start" || type == "breakdown" ||
        type == "second_drop") {
        return clamp(cues.drop2.positionSeconds);
    }
    if (type == "outro_start" || type == "mix_out") {
        double fallback = cues.duration > 0.0
            ? cues.duration * 0.85
            : cues.drop2.positionSeconds + 16.0;
        return clamp(std::max(cues.drop2.positionSeconds, fallback));
    }

    switch (index) {
    case 0:
        return clamp(cues.start.positionSeconds);
    case 1:
        return clamp(cues.drop1.positionSeconds);
    case 2:
        return clamp(cues.breakdown.positionSeconds);
    default:
        return clamp(cues.drop2.positionSeconds);
    }
}

std::vector<SeratoCue> toSeratoCues(
    const AutoCueResult& cues,
    const std::vector<AutoCueConfig::Slot>& slots)
{
    std::vector<SeratoCue> result;
    result.reserve(slots.size());
    std::array<bool, 8> used{};
    for (const auto& slot : slots) {
        int index = std::clamp(slot.index, 0, 7);
        if (used[(size_t)index]) {
            continue;
        }
        used[(size_t)index] = true;
        result.push_back({
            index,
            slot.name.empty() ? "CUE" : slot.name,
            positionForType(cues, slot.type, slot.index),
            slot.colorRgb,
        });
    }
    return result;
}

void removeGeneratedFiles(const fs::path& file)
{
    std::error_code ec;
    fs::path json = file;
    json += ".cues.json";
    fs::remove(json, ec);

    fs::path backup = file;
    backup += ".autocue.bak";
    fs::remove(backup, ec);
}

}  // namespace

FolderProcessor::FolderProcessor(AutoCueMarker& marker, SeratoCueWriter& writer)
    : marker_(marker), writer_(writer)
{
}

void FolderProcessor::processFolder(const fs::path& folder,
                                    bool writeJson,
                                    bool writeSerato,
                                    bool backupBeforeWrite,
                                    bool overwriteExistingCues,
                                    bool cleanupAfterWrite,
                                    const std::vector<AutoCueConfig::Slot>& cueSlots,
                                    ProgressCallback onProgress,
                                    std::atomic_bool& cancel,
                                    ResultCallback onResult)
{
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        if (ec) {
            break;
        }
        if (entry.is_regular_file(ec) && isAudioFile(entry.path())) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    processFiles(std::move(files),
                 writeJson,
                 writeSerato,
                 backupBeforeWrite,
                 overwriteExistingCues,
                 cleanupAfterWrite,
                 cueSlots,
                 std::move(onProgress),
                 cancel,
                 std::move(onResult));
}

void FolderProcessor::processFiles(std::vector<fs::path> files,
                                   bool writeJson,
                                   bool writeSerato,
                                   bool backupBeforeWrite,
                                   bool overwriteExistingCues,
                                   bool cleanupAfterWrite,
                                   const std::vector<AutoCueConfig::Slot>& cueSlots,
                                   ProgressCallback onProgress,
                                   std::atomic_bool& cancel,
                                   ResultCallback onResult)
{
    AutoCueProgress progress;
    progress.total = (int)files.size();
    progress.running = true;
    progress.status = "Starting";
    onProgress(progress);

    for (const auto& file : files) {
        if (cancel.load()) {
            progress.status = "Cancelled";
            break;
        }
        progress.current++;
        progress.currentFile = file.filename().string();
        progress.status = "Analyzing";
        onProgress(progress);

        std::string error;
        AutoCueResult cues = marker_.analyze(file, error);
        if (!error.empty()) {
            progress.errors++;
            progress.status = error;
            onProgress(progress);
            continue;
        }

        if (writeJson) {
            progress.status = "Writing JSON";
            onProgress(progress);
            std::string json_error;
            if (!marker_.writeDebugJson(file, cues, json_error)) {
                progress.errors++;
                progress.status = json_error;
                onProgress(progress);
                continue;
            }
        }

        std::vector<SeratoCue> serato_cues = toSeratoCues(cues, cueSlots);

        if (onResult) {
            progress.status = "Saving library";
            onProgress(progress);
            std::string export_error;
            if (!onResult(file, cues, serato_cues, export_error)) {
                progress.errors++;
                progress.status = export_error.empty()
                    ? "Cue export failed"
                    : export_error;
                onProgress(progress);
                continue;
            }
        }

        if (writeSerato) {
            progress.status = "Writing Serato";
            onProgress(progress);
            std::string serato_error;
            bool wrote = writer_.writeCues(file,
                                           serato_cues,
                                           serato_error,
                                           backupBeforeWrite,
                                           overwriteExistingCues);
            if (!wrote && isSeratoWritable(file)) {
                progress.errors++;
                progress.status = serato_error;
                onProgress(progress);
                continue;
            }
            if (!wrote) {
                progress.status = serato_error;
            }
        }

        if (cleanupAfterWrite) {
            progress.status = "Cleaning up";
            onProgress(progress);
            removeGeneratedFiles(file);
        }

        progress.success++;
        onProgress(progress);
    }

    progress.running = false;
    progress.done = true;
    if (progress.status != "Cancelled") {
        progress.status = "Auto Cue complete";
    }
    onProgress(progress);
}
