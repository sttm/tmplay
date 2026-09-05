#include "AudioProcessor.hpp"

#include "ProcessRunner.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

class CompletionGuard {
public:
    CompletionGuard(std::atomic_bool& running,
                    AudioProcessor::FinishedCallback& onFinished)
        : running_(running), onFinished_(onFinished)
    {
    }

    ~CompletionGuard()
    {
        running_ = false;
        if (onFinished_) {
            onFinished_();
        }
    }

private:
    std::atomic_bool& running_;
    AudioProcessor::FinishedCallback& onFinished_;
};

bool validTrack(const Track& track)
{
    return track.type == EntryType::File &&
           !track.id.empty() &&
           track.id.find('\0') == std::string::npos &&
           fs::is_regular_file(track.id);
}

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return value;
}

std::string normalizedFormat(std::string format)
{
    format = lower(std::move(format));
    static constexpr std::string_view supported[] = {
        "wav", "mp3", "m4a", "flac",
    };
    for (const auto candidate : supported) {
        if (format == candidate) {
            return format;
        }
    }
    return "mp3";
}

std::string outputFormatFor(const Track& track)
{
    std::string extension = lower(fs::path(track.id).extension().string());
    if (!extension.empty() && extension.front() == '.') {
        extension.erase(extension.begin());
    }
    if (extension == "wav" || extension == "mp3" ||
        extension == "m4a" || extension == "flac") {
        return extension;
    }
    return "m4a";
}

std::vector<std::string> codecArgs(const std::string& format)
{
    if (format == "wav") {
        return {"-c:a", "pcm_s16le"};
    }
    if (format == "mp3") {
        return {"-c:a", "libmp3lame", "-b:a", "320k"};
    }
    if (format == "flac") {
        return {"-c:a", "flac"};
    }
    return {"-c:a", "aac", "-b:a", "256k"};
}

std::string lufsFilter(const NormalizationOptions& options)
{
    int target = std::clamp(options.targetLufs, -30, -5);
    double true_peak = target >= -9 ? -1.0 : -1.5;
    int lra = target >= -9 ? 7 : 11;
    return "loudnorm=I=" + std::to_string(target) +
        ":TP=" + std::to_string(true_peak) +
        ":LRA=" + std::to_string(lra);
}

std::string compactOutput(std::string value)
{
    value = ProcessRunner::trim(std::move(value));
    std::replace(value.begin(), value.end(), '\n', ' ');
    std::replace(value.begin(), value.end(), '\r', ' ');
    while (value.find("  ") != std::string::npos) {
        value.erase(value.find("  "), 1);
    }
    if (value.size() > 220) {
        value = "..." + value.substr(value.size() - 217);
    }
    return value;
}

fs::path uniqueOutputPath(const fs::path& directory,
                          const fs::path& source,
                          const std::string& suffix,
                          const std::string& format)
{
    fs::path base = directory / (source.stem().string() + suffix);
    fs::path candidate = base;
    candidate.replace_extension("." + format);
    int index = 2;
    std::error_code ec;
    while (fs::exists(candidate, ec)) {
        candidate = directory /
            (source.stem().string() + suffix + " " + std::to_string(index));
        candidate.replace_extension("." + format);
        index++;
    }
    return candidate;
}

std::vector<Track> validTracksOnly(const std::vector<Track>& tracks)
{
    std::vector<Track> result;
    for (const auto& track : tracks) {
        if (validTrack(track)) {
            result.push_back(track);
        }
    }
    return result;
}

}  // namespace

AudioProcessor::~AudioProcessor()
{
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool AudioProcessor::normalize(const std::vector<Track>& tracks,
                               const std::string& sourceDirectory,
                               const NormalizationOptions& options,
                               FinishedCallback on_finished)
{
    std::vector<Track> selected = validTracksOnly(tracks);
    if (selected.empty()) {
        setState(AudioProcessState::Error, AudioProcessKind::Normalize,
                 "Error", "Select at least one audio track");
        return false;
    }

    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        setState(AudioProcessState::Error, AudioProcessKind::Normalize,
                 "Error", "Audio processing already running");
        return false;
    }

    if (worker_.joinable()) {
        worker_.join();
    }

    setState(AudioProcessState::Running,
             AudioProcessKind::Normalize,
             "Normalizing",
             std::to_string(selected.size()) + " track(s)",
             {},
             0.02f);
    worker_ = std::thread(&AudioProcessor::run, this,
                          Request{AudioProcessKind::Normalize,
                                  std::move(selected),
                                  sourceDirectory,
                                  options,
                                  {},
                                  std::move(on_finished)});
    return true;
}

bool AudioProcessor::convert(const std::vector<Track>& tracks,
                             const std::string& sourceDirectory,
                             const ConvertOptions& options,
                             FinishedCallback on_finished)
{
    std::vector<Track> selected = validTracksOnly(tracks);
    if (selected.empty()) {
        setState(AudioProcessState::Error, AudioProcessKind::Convert,
                 "Error", "Select at least one audio track");
        return false;
    }

    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        setState(AudioProcessState::Error, AudioProcessKind::Convert,
                 "Error", "Audio processing already running");
        return false;
    }

    if (worker_.joinable()) {
        worker_.join();
    }

    ConvertOptions normalized = options;
    normalized.format = normalizedFormat(normalized.format);
    setState(AudioProcessState::Running,
             AudioProcessKind::Convert,
             "Converting",
             std::to_string(selected.size()) + " track(s) -> " + normalized.format,
             {},
             0.02f);
    worker_ = std::thread(&AudioProcessor::run, this,
                          Request{AudioProcessKind::Convert,
                                  std::move(selected),
                                  sourceDirectory,
                                  {},
                                  std::move(normalized),
                                  std::move(on_finished)});
    return true;
}

AudioProcessSnapshot AudioProcessor::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void AudioProcessor::run(Request request)
{
    CompletionGuard completion(running_, request.onFinished);

    auto ffmpeg = ProcessRunner::findExecutable("ffmpeg");
    if (!ffmpeg) {
        setState(AudioProcessState::Error, request.kind,
                 "Error", "ffmpeg not found", {}, 1.0f);
        return;
    }

    fs::path source_directory = request.sourceDirectory.empty()
        ? fs::path(request.tracks.front().id).parent_path()
        : fs::path(request.sourceDirectory);
    fs::path output_directory = source_directory /
        (request.kind == AudioProcessKind::Normalize ? "normalized" : "converted");
    std::error_code ec;
    fs::create_directories(output_directory, ec);
    if (ec) {
        setState(AudioProcessState::Error, request.kind,
                 "Error", ec.message(), output_directory.string(), 1.0f);
        return;
    }

    size_t done = 0;
    for (const auto& track : request.tracks) {
        float base_progress = (float)done / (float)request.tracks.size();
        setState(AudioProcessState::Running,
                 request.kind,
                 request.kind == AudioProcessKind::Normalize
                     ? "Normalizing"
                     : "Converting",
                 track.title,
                 output_directory.string(),
                 base_progress);

        std::string error;
        if (!processOne(*ffmpeg, request, track, output_directory, error)) {
            setState(AudioProcessState::Error, request.kind,
                     "Error", std::move(error), output_directory.string(), 1.0f);
            return;
        }
        done++;
        setState(AudioProcessState::Running,
                 request.kind,
                 request.kind == AudioProcessKind::Normalize
                     ? "Normalizing"
                     : "Converting",
                 track.title,
                 output_directory.string(),
                 (float)done / (float)request.tracks.size());
    }

    setState(AudioProcessState::Done,
             request.kind,
             request.kind == AudioProcessKind::Normalize
                 ? "Normalized"
                 : "Converted",
             std::to_string(request.tracks.size()) + " track(s)",
             output_directory.string(),
             1.0f);
}

bool AudioProcessor::processOne(const std::string& ffmpeg,
                                const Request& request,
                                const Track& track,
                                const fs::path& outputDirectory,
                                std::string& error) const
{
    fs::path input(track.id);
    std::string format = request.kind == AudioProcessKind::Normalize
        ? outputFormatFor(track)
        : normalizedFormat(request.convert.format);
    std::string suffix = request.kind == AudioProcessKind::Normalize
        ? "_" + std::to_string(request.normalization.targetLufs) + "LUFS"
        : "";
    fs::path output = uniqueOutputPath(outputDirectory, input, suffix, format);

    std::vector<std::string> args = {
        ffmpeg,
        "-nostdin",
        "-hide_banner",
        "-loglevel",
        "error",
        "-y",
        "-i",
        input.string(),
        "-vn",
    };

    if (request.kind == AudioProcessKind::Normalize) {
        args.push_back("-af");
        args.push_back(lufsFilter(request.normalization));
    }

    std::vector<std::string> codec = codecArgs(format);
    args.insert(args.end(), codec.begin(), codec.end());
    args.push_back(output.string());

    std::string output_text;
    int exit_code = ProcessRunner::runWithCombinedOutput(args, &output_text);
    if (exit_code != 0 || !fs::exists(output)) {
        error = compactOutput(output_text);
        if (error.empty()) {
            error = "ffmpeg failed: " + track.title;
        }
        return false;
    }
    return true;
}

void AudioProcessor::setState(AudioProcessState state,
                              AudioProcessKind kind,
                              std::string message,
                              std::string detail,
                              std::string outputDirectory,
                              float progress)
{
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.state = state;
    snapshot_.kind = kind;
    snapshot_.message = std::move(message);
    snapshot_.detail = std::move(detail);
    snapshot_.outputDirectory = std::move(outputDirectory);
    snapshot_.progress = std::clamp(progress, 0.0f, 1.0f);
}
