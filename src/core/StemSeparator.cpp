#include "StemSeparator.hpp"

#include "MacActivity.hpp"
#include "ProcessRunner.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <pthread/qos.h>
#endif

namespace fs = std::filesystem;

namespace {

class ScopedStemActivity {
public:
    ScopedStemActivity()
    {
#if defined(__APPLE__)
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
        if (IOPMAssertionCreateWithName(
                kIOPMAssertionTypePreventUserIdleSystemSleep,
                kIOPMAssertionLevelOn,
                CFSTR("tmplay stem separation"),
                &assertion_) != kIOReturnSuccess) {
            assertion_ = kIOPMNullAssertionID;
        }
#endif
    }

    ~ScopedStemActivity()
    {
#if defined(__APPLE__)
        if (assertion_ != kIOPMNullAssertionID) {
            IOPMAssertionRelease(assertion_);
        }
#endif
    }

private:
#if defined(__APPLE__)
    IOPMAssertionID assertion_ = kIOPMNullAssertionID;
    MacActivity macActivity_{"tmplay stem separation"};
#endif
};

bool validTrack(const Track& track)
{
    return track.type == EntryType::File && !track.id.empty() &&
           track.id.find('\0') == std::string::npos && fs::is_regular_file(track.id);
}

std::string stemName(int stems)
{
    return stems == 2 ? "2 stems" : "4 stems";
}

std::string compactOutput(std::string value)
{
    value = ProcessRunner::trim(std::move(value));
    std::replace(value.begin(), value.end(), '\n', ' ');
    std::replace(value.begin(), value.end(), '\r', ' ');
    while (value.find("  ") != std::string::npos) {
        value.erase(value.find("  "), 1);
    }
    return value.size() > 240 ? "..." + value.substr(value.size() - 237) : value;
}

std::string outputExtension(const DemucsConfig& config)
{
    if (config.outputFormat == "mp3") return ".mp3";
    if (config.outputFormat == "flac") return ".flac";
    return ".wav";
}

std::string displayStemName(const std::string& stem)
{
    static const std::map<std::string, std::string> names = {
        {"vocals", "acapella"},
        {"no_vocals", "instrumental"},
        {"drums", "drums"},
        {"bass", "bass"},
        {"other", "other"},
    };
    const auto found = names.find(stem);
    return found == names.end() ? stem : found->second;
}

fs::path uniqueStemPath(const fs::path& directory,
                        const std::string& baseName,
                        const std::string& stem,
                        const std::string& extension)
{
    fs::path candidate = directory /
        (baseName + " (" + displayStemName(stem) + ")" + extension);
    std::error_code ec;
    for (int index = 2; fs::exists(candidate, ec); ++index) {
        candidate = directory /
            (baseName + " (" + displayStemName(stem) + " " +
             std::to_string(index) + ")" + extension);
    }
    return candidate;
}

fs::path separatedTrackDirectory(const Track& track, const fs::path& output)
{
    return output / "htdemucs" / fs::path(track.id).stem();
}

bool stemsAlreadySeparated(const fs::path& directory,
                           const Track& track,
                           const DemucsConfig& config)
{
    const std::vector<std::string> stems = config.stems == 2
        ? std::vector<std::string>{"vocals", "no_vocals"}
        : std::vector<std::string>{"drums", "bass", "other", "vocals"};
    const std::string base = fs::path(track.id).stem().string();
    const std::string extension = outputExtension(config);
    std::error_code ec;
    return std::all_of(stems.begin(), stems.end(), [&](const std::string& stem) {
        return fs::is_regular_file(directory /
                                   (base + " (" + displayStemName(stem) + ")" +
                                    extension), ec) && !ec;
    });
}

std::optional<fs::path> findHtdemucsModel()
{
    const fs::path executableDir = ProcessRunner::executableDirectory();
    const std::array<fs::path, 5> directories = {
        executableDir / "models",
        executableDir.parent_path() / "models",
        executableDir.parent_path().parent_path() / "models",
        fs::current_path() / "models",
        fs::current_path().parent_path() / "models",
    };
    for (const auto& directory : directories) {
        const fs::path candidate = directory / "htdemucs.safetensors";
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) return candidate;
    }
    return std::nullopt;
}

fs::path makeTempDirectory()
{
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path directory = fs::temp_directory_path() /
        ("tmplay-demucs-" + std::to_string(tick));
    std::error_code ec;
    fs::create_directories(directory, ec);
    if (ec) throw std::runtime_error("Could not create temp directory: " + ec.message());
    return directory;
}

bool convertStemOutput(const std::string& ffmpeg,
                       const fs::path& wavPath,
                       const fs::path& targetPath,
                       const DemucsConfig& config,
                       std::string* error)
{
    if (config.outputFormat == "wav") return true;
    std::vector<std::string> args = {ffmpeg, "-y", "-i", wavPath.string()};
    if (config.outputFormat == "mp3") {
        args.insert(args.end(), {"-codec:a", "libmp3lame", "-q:a", "2"});
    } else if (config.outputFormat == "flac") {
        args.insert(args.end(), {"-compression_level", "5"});
    }
    args.push_back(targetPath.string());
    std::string output;
    if (ProcessRunner::runWithCombinedOutput(args, &output) == 0) return true;
    if (error) *error = compactOutput(output);
    return false;
}

}  // namespace

StemSeparator::~StemSeparator()
{
    if (worker_.joinable()) worker_.join();
}

bool StemSeparator::start(const Track& track, const DemucsConfig& config)
{
    if (!validTrack(track)) {
        setState(StemSeparationState::Error, "Error", "Select an audio track");
        return false;
    }
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        setState(StemSeparationState::Error, "Error", "Stem separation already running");
        return false;
    }
    if (worker_.joinable()) worker_.join();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        startedAt_ = std::chrono::steady_clock::now();
    }
    setState(StemSeparationState::Running, "Separating",
             track.title + " | " + stemName(config.stems), {}, 0.08f);
    worker_ = std::thread(&StemSeparator::run, this, Request{track, config});
    return true;
}

StemSeparationSnapshot StemSeparator::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    StemSeparationSnapshot result = snapshot_;
    if (result.state == StemSeparationState::Running) {
        result.elapsedSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - startedAt_).count();
    }
    return result;
}

void StemSeparator::run(Request request)
{
    ScopedStemActivity activity;
    fs::path tempDir;
    const auto finish = [&] {
        if (!tempDir.empty()) {
            std::error_code ec;
            fs::remove_all(tempDir, ec);
        }
        running_ = false;
        if (onFinished_) onFinished_();
    };
    const auto fail = [&](std::string detail, std::string output = {}) {
        setState(StemSeparationState::Error, "Error", std::move(detail),
                 std::move(output), 1.0f);
        finish();
    };

    const auto ffmpeg = ProcessRunner::findExecutable("ffmpeg");
    const auto demucs = ProcessRunner::findExecutable("demucs");
    const auto model = findHtdemucsModel();
    if (!ffmpeg) {
        fail("ffmpeg is missing from the tmplay bundle.");
        return;
    }
    if (!demucs || !model) {
        fail(!demucs ? "Bundled demucs-rs executable is missing."
                     : "Missing models/htdemucs.safetensors for demucs-rs.");
        return;
    }

    const fs::path output = request.config.outputDirectory.empty()
        ? fs::path(request.track.id).parent_path() / "separated"
        : fs::path(request.config.outputDirectory);
    const fs::path trackOutput = separatedTrackDirectory(request.track, output);
    if (stemsAlreadySeparated(trackOutput, request.track, request.config)) {
        setState(StemSeparationState::Done, "Already separated",
                 request.track.title + " | " + stemName(request.config.stems),
                 trackOutput.string(), 1.0f);
        finish();
        return;
    }
    std::error_code ec;
    fs::create_directories(trackOutput, ec);
    if (ec) {
        fail(ec.message(), trackOutput.string());
        return;
    }

    try {
        tempDir = makeTempDirectory();
        const fs::path rawOutput = tempDir / "raw-stems";
        setState(StemSeparationState::Running, "Separating",
                 "demucs-rs Metal | " + request.track.title,
                 trackOutput.string(), 0.25f);
        std::string demucsOutput;
        if (ProcessRunner::runWithCombinedOutput({
                *demucs, request.track.id, "--model", "htdemucs",
                "--model-path", model->string(), "--output", rawOutput.string(),
            }, &demucsOutput) != 0) {
            fail("demucs-rs failed: " + compactOutput(demucsOutput), trackOutput.string());
            return;
        }
        const auto rawStem = [&](const std::string& name) { return rawOutput / (name + ".wav"); };
        for (const char* name : {"drums", "bass", "other", "vocals"}) {
            if (!fs::is_regular_file(rawStem(name))) {
                fail("demucs-rs did not produce " + std::string(name) + ".wav",
                     trackOutput.string());
                return;
            }
        }

        const std::string baseName = fs::path(request.track.id).stem().string();
        std::string writeError;
        const auto exportStem = [&](const fs::path& source, const std::string& name) {
            const fs::path target = uniqueStemPath(trackOutput, baseName, name,
                                                   outputExtension(request.config));
            if (request.config.outputFormat == "wav") {
                std::error_code copyError;
                fs::copy_file(source, target, fs::copy_options::none, copyError);
                if (copyError) writeError = copyError.message();
                return !copyError;
            }
            return convertStemOutput(*ffmpeg, source, target, request.config, &writeError);
        };

        setState(StemSeparationState::Running, "Separating", "Writing stems",
                 trackOutput.string(), 0.92f);
        if (request.config.stems == 2) {
            const fs::path instrumental = tempDir / "no_vocals.wav";
            std::string mixOutput;
            if (ProcessRunner::runWithCombinedOutput({
                    *ffmpeg, "-y", "-i", rawStem("drums").string(),
                    "-i", rawStem("bass").string(), "-i", rawStem("other").string(),
                    "-filter_complex", "[0:a][1:a][2:a]amix=inputs=3:normalize=0",
                    "-c:a", "pcm_f32le", instrumental.string(),
                }, &mixOutput) != 0 || !fs::is_regular_file(instrumental)) {
                fail("Could not create instrumental stem: " + compactOutput(mixOutput),
                     trackOutput.string());
                return;
            }
            if (!exportStem(rawStem("vocals"), "vocals") ||
                !exportStem(instrumental, "no_vocals")) {
                fail(writeError.empty() ? "Could not write stems" : writeError,
                     trackOutput.string());
                return;
            }
        } else {
            for (const char* name : {"drums", "bass", "other", "vocals"}) {
                if (!exportStem(rawStem(name), name)) {
                    fail(writeError.empty() ? "Could not write stems" : writeError,
                         trackOutput.string());
                    return;
                }
            }
        }
    } catch (const std::exception& ex) {
        fail(ex.what(), trackOutput.string());
        return;
    }

    setState(StemSeparationState::Done, "Stems ready",
             request.track.title + " | " + stemName(request.config.stems),
             trackOutput.string(), 1.0f);
    finish();
}

void StemSeparator::setOnFinished(std::function<void()> callback)
{
    onFinished_ = std::move(callback);
}

void StemSeparator::setState(StemSeparationState state,
                             std::string message,
                             std::string detail,
                             std::string outputDirectory,
                             float progress)
{
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.state = state;
    snapshot_.message = std::move(message);
    snapshot_.detail = std::move(detail);
    snapshot_.outputDirectory = std::move(outputDirectory);
    snapshot_.progress = std::clamp(progress, 0.0f, 1.0f);
    snapshot_.elapsedSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - startedAt_).count();
}
