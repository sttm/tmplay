#include "DownloadManager.hpp"

#include "ProcessRunner.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fcntl.h>
#include <optional>
#include <regex>
#include <sstream>
#include <sys/wait.h>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

class CompletionGuard {
public:
    CompletionGuard(std::atomic_bool& running,
                    DownloadManager::FinishedCallback& onFinished)
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

    CompletionGuard(const CompletionGuard&) = delete;
    CompletionGuard& operator=(const CompletionGuard&) = delete;

private:
    std::atomic_bool& running_;
    DownloadManager::FinishedCallback& onFinished_;
};

struct CommandResult {
    int exitCode = -1;
    bool cancelled = false;
    std::string output;
};

struct PlaylistEntry {
    int index = 0;
    std::string title;
    std::string source;
    std::string artist;
    std::string album;
    std::string genre;
    std::string thumbnailUrl;
    std::string releaseDate;
    std::string webpageUrl;
};

bool validSource(const std::string& value)
{
    return !value.empty() && value.find('\0') == std::string::npos;
}

std::string normalizedFormat(std::string format)
{
    std::transform(format.begin(), format.end(), format.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    static constexpr std::string_view supported[] = {
        "aac", "alac", "flac", "m4a", "mp4", "mp3", "opus", "vorbis", "wav",
    };
    for (const auto candidate : supported) {
        if (format == candidate) {
            return format;
        }
    }
    return "mp4";
}

std::string downloadedFilePath(const std::string& output)
{
    std::vector<std::string> lines;
    std::stringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        std::string candidate = ProcessRunner::trim(line);
        if (!candidate.empty()) {
            lines.push_back(std::move(candidate));
        }
    }

    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        if (fs::exists(*it)) {
            return *it;
        }
    }
    if (!lines.empty()) {
        return lines.back();
    }
    return {};
}

std::string lowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return value;
}

bool youtubeNeedsCookies(const std::string& output)
{
    std::string lower = lowerCopy(output);
    return lower.find("sign in to confirm") != std::string::npos ||
           lower.find("not a bot") != std::string::npos ||
           lower.find("use --cookies-from-browser") != std::string::npos ||
           lower.find("--cookies for the authentication") != std::string::npos;
}

std::optional<std::string> preferredYtDlp()
{
    if (auto system = ProcessRunner::findSystemExecutable("yt-dlp")) {
        return system;
    }
    return ProcessRunner::findExecutable("yt-dlp");
}

std::string compactError(std::string output)
{
    output = ProcessRunner::trim(std::move(output));
    if (output.empty()) {
        return "Download failed";
    }
    constexpr size_t max_length = 320;
    if (output.size() > max_length) {
        output = output.substr(output.size() - max_length);
        output = "..." + output;
    }
    return output;
}

std::string safeOutputStem(std::string value)
{
    value = ProcessRunner::trim(std::move(value));
    for (char& character : value) {
        if (character == '/' || character == '\\' || character == ':' ||
            character == '\0') {
            character = '-';
        }
    }
    while (!value.empty() && (value.back() == '.' || value.back() == ' ')) {
        value.pop_back();
    }
    return value.empty() ? "audio" : value;
}

std::vector<std::string> splitTabs(const std::string& line)
{
    std::vector<std::string> parts;
    std::stringstream stream(line);
    std::string part;
    while (std::getline(stream, part, '\t')) {
        parts.push_back(std::move(part));
    }
    return parts;
}

bool parsePercent(const std::string& line, float& progress)
{
    static const std::regex percent_regex(R"((\d+(?:\.\d+)?)%)");
    std::smatch match;
    if (!std::regex_search(line, match, percent_regex)) {
        return false;
    }
    try {
        progress = std::clamp(std::stof(match[1].str()) / 100.0f, 0.0f, 1.0f);
        return true;
    } catch (...) {
        return false;
    }
}

std::string normalizedStem(std::string value)
{
    // Album files are named "01. Track" so Finder and the local browser keep
    // release order. Ignore that prefix when checking whether a track exists.
    size_t prefix_end = 0;
    while (prefix_end < value.size() &&
           std::isdigit((unsigned char)value[prefix_end])) {
        ++prefix_end;
    }
    if (prefix_end > 0 && prefix_end < value.size() &&
        (value[prefix_end] == '.' || value[prefix_end] == '-' ||
         std::isspace((unsigned char)value[prefix_end]))) {
        while (prefix_end < value.size() &&
               (value[prefix_end] == '.' || value[prefix_end] == '-' ||
                std::isspace((unsigned char)value[prefix_end]))) {
            ++prefix_end;
        }
        value.erase(0, prefix_end);
    }
    std::string result;
    result.reserve(value.size());
    for (unsigned char c : value) {
        if (std::isalnum(c)) {
            result.push_back((char)std::tolower(c));
        }
    }
    return result;
}

std::optional<std::string> existingAudioForTitle(const std::string& outputDirectory,
                                                 const std::string& title)
{
    std::error_code ec;
    if (!fs::is_directory(outputDirectory, ec)) {
        return std::nullopt;
    }
    const std::string target = normalizedStem(title);
    if (target.empty()) {
        return std::nullopt;
    }
    static constexpr std::string_view audio_exts[] = {
        ".mp3", ".m4a", ".mp4", ".aac", ".alac", ".flac", ".wav", ".aiff", ".aif", ".ogg", ".opus",
    };
    for (const auto& entry : fs::directory_iterator(outputDirectory, ec)) {
        if (ec || !entry.is_regular_file(ec)) {
            continue;
        }
        std::string ext = lowerCopy(entry.path().extension().string());
        bool audio = false;
        for (auto candidate : audio_exts) {
            if (ext == candidate) {
                audio = true;
                break;
            }
        }
        if (!audio) {
            continue;
        }
        if (normalizedStem(entry.path().stem().string()) == target) {
            return entry.path().string();
        }
    }
    return std::nullopt;
}

CommandResult runCancellable(const std::vector<std::string>& args,
                             const std::atomic_bool& cancelRequested,
                             const std::function<void(const std::string&)>& onLine = {})
{
    CommandResult result;
    if (args.empty()) {
        return result;
    }

    int pipe_fd[2] = {-1, -1};
    if (pipe(pipe_fd) != 0) {
        return result;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return result;
    }

    if (pid == 0) {
        close(pipe_fd[0]);
        int dev_null = open("/dev/null", O_RDONLY);
        if (dev_null >= 0) {
            dup2(dev_null, STDIN_FILENO);
            close(dev_null);
        }
        dup2(pipe_fd[1], STDOUT_FILENO);
        dup2(pipe_fd[1], STDERR_FILENO);
        close(pipe_fd[1]);

        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    close(pipe_fd[1]);
    int flags = fcntl(pipe_fd[0], F_GETFL, 0);
    if (flags >= 0) {
        fcntl(pipe_fd[0], F_SETFL, flags | O_NONBLOCK);
    }

    std::array<char, 4096> buffer{};
    std::string pending_line;
    bool killed = false;
    auto kill_started = std::chrono::steady_clock::time_point{};

    while (true) {
        ssize_t bytes_read = read(pipe_fd[0], buffer.data(), buffer.size());
        while (bytes_read > 0) {
            result.output.append(buffer.data(), (size_t)bytes_read);
            pending_line.append(buffer.data(), (size_t)bytes_read);
            size_t newline = std::string::npos;
            while ((newline = pending_line.find('\n')) != std::string::npos) {
                std::string line = pending_line.substr(0, newline);
                pending_line.erase(0, newline + 1);
                if (onLine) {
                    onLine(ProcessRunner::trim(std::move(line)));
                }
            }
            bytes_read = read(pipe_fd[0], buffer.data(), buffer.size());
        }

        int status = 0;
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            if (!pending_line.empty() && onLine) {
                onLine(ProcessRunner::trim(std::move(pending_line)));
            }
            result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            break;
        }

        if (cancelRequested.load()) {
            result.cancelled = true;
            if (!killed) {
                kill(pid, SIGTERM);
                killed = true;
                kill_started = std::chrono::steady_clock::now();
            } else if (std::chrono::steady_clock::now() - kill_started >
                       std::chrono::seconds(2)) {
                kill(pid, SIGKILL);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }

    close(pipe_fd[0]);
    return result;
}

std::vector<std::string> ytDlpPlaylistArguments(const std::string& executable,
                                                const std::string& source,
                                                const std::optional<std::string>& cookiesBrowser,
                                                const std::optional<std::string>& cookiesPath)
{
    std::vector<std::string> args = {
        executable,
        "--ignore-config",
        "--no-plugin-dirs",
        "--flat-playlist",
        "--no-warnings",
        "--print",
        "%(playlist_index)s\t%(title)s\t%(webpage_url)s",
    };
    if (cookiesBrowser && !cookiesBrowser->empty()) {
        args.push_back("--cookies-from-browser");
        args.push_back(*cookiesBrowser);
    }
    if (cookiesPath && !cookiesPath->empty()) {
        args.push_back("--cookies");
        args.push_back(*cookiesPath);
    }
    args.push_back("--");
    args.push_back(source);
    return args;
}

std::vector<std::string> ytDlpArguments(const std::string& executable,
                                        const std::string& source,
                                        const std::string& outputDirectory,
                                        const std::string& format,
                                        const std::string& knownTitle,
                                        const std::optional<std::string>& cookiesBrowser,
                                        const std::optional<std::string>& cookiesPath)
{
    // A direct googlevideo URL no longer carries the webpage title. Keep the
    // title received from Search so an explicit download has a useful stable
    // filename instead of e.g. "videoplayback.m4a".
    fs::path output_template = fs::path(outputDirectory) /
        (safeOutputStem(knownTitle.empty() ? "%(title)s" : knownTitle) + ".%(ext)s");
    std::vector<std::string> args = {
        executable,
        "--ignore-config",
        "--no-plugin-dirs",
        "-f", "bestaudio",
        "--no-playlist",
        "--no-overwrites",
        "--extract-audio",
        "--audio-format", format,
        // Normalize downloaded artwork before post-processing so the same
        // cover is embedded for a pasted URL, a Search track, and an album.
        "--convert-thumbnails", "jpg",
        // The thumbnail converter is also used by the regular yt-dlp path.
        // Preserve its aspect ratio, crop the centre, and emit one square
        // JPEG before EmbedThumbnail attaches it to the audio container.
        "--postprocessor-args", "ThumbnailsConvertor+ffmpeg_o:-vf scale=600:600:force_original_aspect_ratio=increase,crop=600:600",
        "--embed-thumbnail",
        "--embed-metadata",
        "--replace-in-metadata", "title", "(?i)\\([^)]*official[^)]*\\)", "",
        "--replace-in-metadata", "title", "\\[.*?\\]", "",
        "--replace-in-metadata", "title", "\\s+", " ",
        "--replace-in-metadata", "title", "^\\s+|\\s+$", "",
        "--print", "after_move:filepath",
        "-o", output_template.string(),
    };
    if (cookiesBrowser && !cookiesBrowser->empty()) {
        args.push_back("--cookies-from-browser");
        args.push_back(*cookiesBrowser);
    }
    if (cookiesPath && !cookiesPath->empty()) {
        args.push_back("--cookies");
        args.push_back(*cookiesPath);
    }
    args.push_back("--");
    args.push_back(source);
    return args;
}

bool isDirectMediaUrl(const std::string& source)
{
    const std::string lower = lowerCopy(source);
    return lower.find("googlevideo.com") != std::string::npos ||
           lower.find("googleusercontent.com") != std::string::npos;
}

std::vector<std::string> ffmpegDirectDownloadArguments(
    const std::string& executable,
    const PlaylistEntry& entry,
    const std::string& outputDirectory,
    const std::string& format,
    const std::string& outputStem,
    std::string& outputPath)
{
    // A WebM/Opus stream cannot be copied into an M4A container.  Select the
    // requested local format explicitly, while still avoiding a second yt-dlp
    // page lookup when a direct URL has already been resolved for playback.
    const std::string extension = format == "mp4" ? "m4a" : format;
    outputPath = (fs::path(outputDirectory) /
                  (safeOutputStem(outputStem) + "." + extension)).string();
    std::vector<std::string> args = {
        executable,
        "-nostdin", "-hide_banner", "-loglevel", "warning", "-n",
        "-progress", "pipe:1",
        "-i", entry.source,
    };
    // Keep the download consistent with online playback: use only the
    // thumbnail returned by the original YT Music track request.  Do not
    // derive or look up a replacement image when that field is absent.
    const std::string& artwork_url = entry.thumbnailUrl;
    const bool attachArtwork = !artwork_url.empty() && format != "wav";
    if (attachArtwork) {
        // yt-dlp's thumbnail field is the selected medium artwork URL.  It
        // avoids a second metadata lookup and is embedded as an attached JPEG
        // picture by FFmpeg.
        args.insert(args.end(), {"-i", artwork_url,
            "-filter_complex",
            "[1:v]scale=600:600:force_original_aspect_ratio=increase,crop=600:600[cover]"});
    }
    args.insert(args.end(), {"-map", "0:a:0", "-map_metadata", "-1",
                             "-metadata", "title=" + entry.title});
    if (attachArtwork) {
        // Crop rather than stretch, so every embedded cover is square.
        args.insert(args.end(), {"-map", "[cover]", "-c:v", "mjpeg",
                                 "-disposition:v:0", "attached_pic"});
    } else {
        args.insert(args.end(), {"-vn"});
    }
    if (!entry.artist.empty()) {
        args.insert(args.end(), {"-metadata", "artist=" + entry.artist,
                                 "-metadata", "album_artist=" + entry.artist});
    }
    if (!entry.album.empty()) {
        args.insert(args.end(), {"-metadata", "album=" + entry.album,
                                 "-metadata", "track=" + std::to_string(entry.index),
                                 "-metadata", "disc=1"});
    }
    if (!entry.genre.empty()) {
        args.insert(args.end(), {"-metadata", "genre=" + entry.genre});
    }
    if (!entry.releaseDate.empty()) {
        args.insert(args.end(), {"-metadata", "date=" + entry.releaseDate});
    }
    if (!entry.webpageUrl.empty()) {
        args.insert(args.end(), {"-metadata", "purl=" + entry.webpageUrl});
    }
    args.insert(args.end(), {"-metadata", "comment=Downloaded with tmplay"});
    if (format == "mp3") {
        args.insert(args.end(), {"-c:a", "libmp3lame"});
    } else if (format == "flac") {
        args.insert(args.end(), {"-c:a", "flac"});
    } else if (format == "opus") {
        args.insert(args.end(), {"-c:a", "libopus"});
    } else if (format == "vorbis") {
        args.insert(args.end(), {"-c:a", "libvorbis"});
    } else if (format == "wav") {
        args.insert(args.end(), {"-c:a", "pcm_s16le"});
    } else if (format == "alac") {
        args.insert(args.end(), {"-c:a", "alac"});
    } else {
        // m4a / mp4 / aac
        args.insert(args.end(), {"-c:a", "aac"});
    }
    if (format == "m4a" || format == "mp4") {
        args.insert(args.end(), {"-movflags", "+faststart"});
    }
    args.push_back(outputPath);
    return args;
}

std::vector<PlaylistEntry> parsePlaylistOutput(const std::string& output,
                                               const std::string& fallbackSource)
{
    std::vector<PlaylistEntry> entries;
    std::stringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        line = ProcessRunner::trim(std::move(line));
        if (line.empty() || line.starts_with("WARNING") || line.starts_with("ERROR")) {
            continue;
        }
        auto parts = splitTabs(line);
        if (parts.size() < 3) {
            continue;
        }
        PlaylistEntry entry;
        try {
            entry.index = std::max(1, std::stoi(parts[0]));
        } catch (...) {
            entry.index = (int)entries.size() + 1;
        }
        entry.title = ProcessRunner::trim(parts[1]);
        entry.source = ProcessRunner::trim(parts[2]);
        if (entry.title.empty() || entry.title == "NA") {
            entry.title = "Item " + std::to_string(entry.index);
        }
        if (entry.source.empty() || entry.source == "NA") {
            entry.source = fallbackSource;
        }
        entries.push_back(std::move(entry));
    }
    if (entries.empty()) {
        entries.push_back(PlaylistEntry{1, "Download", fallbackSource});
    }
    for (size_t i = 0; i < entries.size(); ++i) {
        entries[i].index = (int)i + 1;
    }
    return entries;
}

}  // namespace

DownloadManager::~DownloadManager()
{
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool DownloadManager::start(const std::string& source,
                            const std::string& outputDirectory,
                            const std::string& format,
                            std::vector<std::string> cookiesFromBrowser,
                            std::string cookiesPath,
                            FinishedCallback on_finished)
{
    if (!validSource(source)) {
        setState(DownloadState::Error, "Error", "Download source is empty");
        return false;
    }

    if (!fs::is_directory(outputDirectory)) {
        setState(DownloadState::Error, "Error", "Current directory is not writable");
        return false;
    }

    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        setState(DownloadState::Error, "Error", "Download already running");
        return false;
    }

    if (worker_.joinable()) {
        worker_.join();
    }

    cancelRequested_ = false;
    std::string selected_format = normalizedFormat(format);
    setState(DownloadState::Running,
             "Downloading",
             "To: " + outputDirectory + " | " + selected_format,
             {},
             0.2f);
    setItems({});
    DownloadRequest request{
        source,
        outputDirectory,
        selected_format,
        std::move(cookiesFromBrowser),
        std::move(cookiesPath),
        {},
        std::move(on_finished),
    };
    worker_ = std::thread(&DownloadManager::run, this, std::move(request));
    return true;
}

bool DownloadManager::startBatch(std::vector<DownloadSnapshot::Item> items,
                                 const std::string& outputDirectory,
                                 const std::string& format,
                                 std::vector<std::string> cookiesFromBrowser,
                                 std::string cookiesPath,
                                 FinishedCallback on_finished)
{
    items.erase(std::remove_if(items.begin(), items.end(), [](const auto& item) {
        return !validSource(item.source);
    }), items.end());
    if (items.empty()) {
        setState(DownloadState::Error, "Error", "Album has no downloadable tracks");
        return false;
    }
    if (!fs::is_directory(outputDirectory)) {
        setState(DownloadState::Error, "Error", "Current directory is not writable");
        return false;
    }

    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        setState(DownloadState::Error, "Error", "Download already running");
        return false;
    }
    if (worker_.joinable()) {
        worker_.join();
    }

    const bool has_explicit_track_positions = std::any_of(
        items.begin(), items.end(), [](const auto& item) { return item.index > 0; });
    for (size_t index = 0; index < items.size(); ++index) {
        if (!has_explicit_track_positions) {
            items[index].index = (int)index + 1;
        }
        items[index].status = "queued";
    }
    cancelRequested_ = false;
    std::string selected_format = normalizedFormat(format);
    setState(DownloadState::Running, "Preparing album", "To: " + outputDirectory,
             {}, 0.0f);
    setItems(items);
    DownloadRequest request{
        {},
        outputDirectory,
        selected_format,
        std::move(cookiesFromBrowser),
        std::move(cookiesPath),
        std::move(items),
        std::move(on_finished),
    };
    worker_ = std::thread(&DownloadManager::run, this, std::move(request));
    return true;
}

void DownloadManager::cancel()
{
    cancelRequested_ = true;
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot_.state == DownloadState::Running) {
        snapshot_.message = "Stopping";
        snapshot_.detail = "Cancelling download";
    }
}

DownloadSnapshot DownloadManager::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void DownloadManager::run(DownloadRequest request)
{
    CompletionGuard completion(running_, request.onFinished);
    auto fail = [&](std::string detail,
                    std::string file_path = {},
                    float progress = 0.0f) {
        setState(DownloadState::Error, "Error", std::move(detail),
                 std::move(file_path), progress);
    };

    auto yt_dlp = preferredYtDlp();
    if (!yt_dlp) {
        fail("yt-dlp not found");
        return;
    }

    std::vector<PlaylistEntry> entries;
    if (!request.items.empty()) {
        entries.reserve(request.items.size());
        for (const auto& item : request.items) {
            entries.push_back({item.index, item.title, item.source,
                               item.artist, item.album, item.genre,
                               item.thumbnailUrl, item.releaseDate,
                               item.webpageUrl});
        }
    } else {
        std::string playlist_output;
        bool retried_for_cookies = false;

        auto fetch_playlist = [&](const std::optional<std::string>& browser,
                                  const std::optional<std::string>& cookies_path) {
        if (browser && !browser->empty()) {
            setState(DownloadState::Running,
                     "Reading playlist",
                     "Trying cookies: " + *browser,
                     {},
                     0.05f);
        } else if (cookies_path) {
            setState(DownloadState::Running,
                     "Reading playlist",
                     "Trying cookies file",
                     {},
                     0.05f);
        }
        auto result = runCancellable(
            ytDlpPlaylistArguments(*yt_dlp, request.source, browser, cookies_path),
            cancelRequested_);
        playlist_output = std::move(result.output);
        return !result.cancelled && result.exitCode == 0;
        };

        if (!fetch_playlist(std::nullopt, std::nullopt)) {
        if (cancelRequested_.load()) {
            fail("Download cancelled");
            return;
        }
        if (youtubeNeedsCookies(playlist_output)) {
            if (!request.cookiesPath.empty() && fs::is_regular_file(request.cookiesPath) &&
                fetch_playlist(std::nullopt, request.cookiesPath)) {
                retried_for_cookies = true;
            }
            for (const auto& browser : request.cookiesFromBrowser) {
                if (retried_for_cookies) break;
                retried_for_cookies = true;
                if (fetch_playlist(browser, std::nullopt)) {
                    break;
                }
            }
        }
        }

        if (cancelRequested_.load()) {
        fail("Download cancelled");
        return;
        }

        if (youtubeNeedsCookies(playlist_output) && !retried_for_cookies) {
            fail("YouTube requires cookies. Add [ytdlp] cookies_from_browser = [\"chrome\", \"safari\"]");
            return;
        }

        entries = parsePlaylistOutput(playlist_output, request.source);
    }
    std::vector<DownloadSnapshot::Item> items;
    items.reserve(entries.size());
    for (const auto& entry : entries) {
        DownloadSnapshot::Item item;
        item.index = entry.index;
        item.title = entry.title;
        item.artist = entry.artist;
        item.album = entry.album;
        item.genre = entry.genre;
        item.thumbnailUrl = entry.thumbnailUrl;
        item.releaseDate = entry.releaseDate;
        item.webpageUrl = entry.webpageUrl;
        item.source = entry.source;
        item.status = "queued";
        items.push_back(std::move(item));
    }
    setItems(std::move(items));

    int completed = 0;
    int skipped = 0;
    int failed_count = 0;
    std::string last_file_path;
    std::string last_error;
    const int total = std::max(1, (int)entries.size());
    const bool album_collection = entries.size() > 1 && !entries.front().album.empty() &&
        std::all_of(entries.begin(), entries.end(), [&](const PlaylistEntry& entry) {
            return entry.album == entries.front().album;
        });

    auto aggregate = [&](int done, float item_progress) {
        return std::clamp(((float)done + item_progress) / (float)total, 0.0f, 1.0f);
    };

    for (size_t index = 0; index < entries.size(); ++index) {
        if (cancelRequested_.load()) {
            fail("Download cancelled", last_file_path,
                 aggregate(completed + skipped + failed_count, 0.0f));
            return;
        }

        const auto& entry = entries[index];
        const bool numbered_album_track = album_collection ||
            (entry.index > 0 && !entry.album.empty());
        const int track_number = entry.index > 0 ? entry.index : (int)index + 1;
        const std::string output_title = numbered_album_track
            ? (track_number < 10 ? "0" : "") + std::to_string(track_number) +
                ". " + entry.title
            : entry.title;
        if (auto existing = existingAudioForTitle(request.outputDirectory, entry.title)) {
            // A selected album track may have been downloaded before the rest
            // of the album.  Rename it into its release position when the
            // complete album is requested, rather than treating it as an
            // unnumbered file which Finder puts at the end.
            if (numbered_album_track) {
                const fs::path current_file = *existing;
                const fs::path expected = fs::path(request.outputDirectory) /
                    (safeOutputStem(output_title) + current_file.extension().string());
                std::error_code rename_error;
                if (current_file != expected && !fs::exists(expected, rename_error)) {
                    fs::rename(current_file, expected, rename_error);
                    if (!rename_error) {
                        *existing = expected.string();
                    }
                }
            }
            ++skipped;
            last_file_path = *existing;
            updateItem(index, "skipped", "Already exists", *existing, 1.0f);
            setState(DownloadState::Running,
                     "Downloading",
                     "Skipped existing: " + fs::path(*existing).filename().string(),
                     *existing,
                     aggregate(completed + skipped + failed_count, 0.0f));
            continue;
        }

        updateItem(index, "running", "Downloading", {}, 0.0f);
        setState(DownloadState::Running,
                 "Downloading",
                 entry.title + " | To: " + request.outputDirectory,
                 {},
                 aggregate(completed + skipped + failed_count, 0.0f));

        std::string output;
        std::string file_path;
        int exit_code = -1;
        bool item_retried_for_cookies = false;
        auto attempt = [&](const std::optional<std::string>& browser,
                           const std::optional<std::string>& cookies_path) {
            output.clear();
            if (browser && !browser->empty()) {
                updateItem(index, "running", "Trying cookies: " + *browser, {}, 0.05f);
            } else if (cookies_path) {
                updateItem(index, "running", "Trying cookies file", {}, 0.05f);
            }
            std::string direct_output_path;
            std::vector<std::string> arguments;
            const bool direct_media = isDirectMediaUrl(entry.source);
            if (direct_media) {
                auto ffmpeg = ProcessRunner::findExecutable("ffmpeg");
                if (!ffmpeg) {
                    output = "ffmpeg not found for direct stream download";
                    exit_code = -1;
                    return false;
                }
                arguments = ffmpegDirectDownloadArguments(
                    *ffmpeg, entry, request.outputDirectory, request.format,
                    output_title,
                    direct_output_path);
            } else {
                arguments = ytDlpArguments(*yt_dlp, entry.source,
                                            request.outputDirectory,
                                            request.format, output_title, browser,
                                            cookies_path);
            }
            auto result = runCancellable(
                arguments,
                cancelRequested_,
                [&](const std::string& line) {
                    if (line.empty()) {
                        return;
                    }
                    float item_progress = 0.0f;
                    if (parsePercent(line, item_progress)) {
                        updateItem(index, "running", line, {}, item_progress);
                        setState(DownloadState::Running,
                                 "Downloading",
                                 entry.title + " | To: " + request.outputDirectory,
                                 {},
                                 aggregate(completed + skipped + failed_count, item_progress));
                    } else if (line.find("Destination:") != std::string::npos) {
                        updateItem(index, "running", line, {}, 0.05f);
                    }
                });
            output = std::move(result.output);
            exit_code = result.cancelled ? -2 : result.exitCode;
            file_path = direct_media ? direct_output_path : downloadedFilePath(output);
            if (file_path.empty()) {
                if (auto existing = existingAudioForTitle(request.outputDirectory, entry.title)) {
                    file_path = *existing;
                }
            }
            return exit_code == 0 && !file_path.empty() && fs::exists(file_path);
        };

        if (!attempt(std::nullopt, std::nullopt)) {
            if (cancelRequested_.load()) {
                fail("Download cancelled", last_file_path,
                     aggregate(completed + skipped + failed_count, 0.0f));
                return;
            }
            if (youtubeNeedsCookies(output)) {
                if (!request.cookiesPath.empty() && fs::is_regular_file(request.cookiesPath) &&
                    attempt(std::nullopt, request.cookiesPath)) {
                    item_retried_for_cookies = true;
                }
                for (const auto& browser : request.cookiesFromBrowser) {
                    if (item_retried_for_cookies) break;
                    item_retried_for_cookies = true;
                    if (attempt(browser, std::nullopt)) {
                        break;
                    }
                }
            }
        }

        if (exit_code != 0 || file_path.empty() || !fs::exists(file_path)) {
            ++failed_count;
            if (youtubeNeedsCookies(output) && !item_retried_for_cookies) {
                last_error = "YouTube requires cookies. Add [ytdlp] cookies_from_browser = [\"chrome\", \"safari\"]";
            } else {
                last_error = compactError(output);
            }
            updateItem(index, "error", last_error, {}, 1.0f);
            setState(DownloadState::Running,
                     "Downloading",
                     "Error: " + last_error,
                     {},
                     aggregate(completed + skipped + failed_count, 0.0f));
            continue;
        }

        const std::string filename = fs::path(file_path).filename().string();
        last_file_path = file_path;
        ++completed;
        updateItem(index, "done", filename, file_path, 1.0f);
        setState(DownloadState::Running,
                 "Downloading",
                 filename,
                 file_path,
                 aggregate(completed + skipped + failed_count, 0.0f));
    }

    if (completed == 0 && skipped == 0 && failed_count > 0) {
        fail(last_error.empty() ? "Download failed" : last_error, last_file_path, 1.0f);
        return;
    }

    std::string detail = "downloaded " + std::to_string(completed) +
        " | skipped " + std::to_string(skipped) +
        " | errors " + std::to_string(failed_count);
    if (!last_file_path.empty()) {
        detail += " | " + fs::path(last_file_path).filename().string();
    }
    if (failed_count > 0) {
        setState(DownloadState::Error, "Finished with errors", detail, last_file_path, 1.0f);
    } else {
        const bool batchAlreadyPresent = !request.items.empty() &&
            completed == 0 && skipped == total;
        setState(DownloadState::Done,
                 batchAlreadyPresent
                    ? (request.items.size() == 1
                        ? "Track already downloaded"
                        : "Album already downloaded")
                    : "Success",
                 detail, last_file_path, 1.0f);
    }
}

void DownloadManager::setItems(std::vector<DownloadSnapshot::Item> items)
{
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.items = std::move(items);
}

void DownloadManager::updateItem(size_t itemIndex,
                                 std::string status,
                                 std::string detail,
                                 std::string filePath,
                                 float progress)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (itemIndex >= snapshot_.items.size()) {
        return;
    }
    auto& item = snapshot_.items[itemIndex];
    item.status = std::move(status);
    item.detail = std::move(detail);
    if (!filePath.empty()) {
        item.filePath = std::move(filePath);
    }
    item.progress = std::clamp(progress, 0.0f, 1.0f);
}

void DownloadManager::setState(DownloadState state,
                               std::string message,
                               std::string detail,
                               std::string filePath,
                               float progress)
{
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.state = state;
    snapshot_.message = std::move(message);
    snapshot_.detail = std::move(detail);
    snapshot_.filePath = std::move(filePath);
    snapshot_.progress = std::clamp(progress, 0.0f, 1.0f);
}
