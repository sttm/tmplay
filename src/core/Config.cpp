#include "Config.hpp"
#include "ProcessRunner.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <filesystem>

#include <toml++/toml.h>

namespace fs = std::filesystem;

namespace {

std::string lowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return value;
}

std::vector<std::string> detectedCookieBrowsers()
{
    struct Browser {
        const char* appName;
        const char* ytDlpName;
    };

    static constexpr Browser browsers[] = {
        {"Google Chrome.app", "chrome"},
        {"Safari.app", "safari"},
        {"Firefox.app", "firefox"},
        {"Microsoft Edge.app", "edge"},
        {"Opera.app", "opera"},
        {"Brave Browser.app", "brave"},
        {"Vivaldi.app", "vivaldi"},
        {"Yandex.app", "yandex"},
        {"Yandex Browser.app", "yandex"},
    };

    std::vector<std::string> result;
#if defined(__APPLE__)
    for (const auto& browser : browsers) {
        if (fs::exists(fs::path("/Applications") / browser.appName)) {
            result.push_back(browser.ytDlpName);
        }
    }
#endif
    return result;
}

void loadKeyBindingSection(const toml::table& tbl,
                           const char* section,
                           KeyBindingMap& output)
{
    auto* keybinds = tbl[section].as_table();
    if (!keybinds) {
        return;
    }

    for (auto&& [key, value] : *keybinds) {
        std::vector<std::string> bindings;
        if (auto scalar = value.as_string()) {
            std::string binding = scalar->value_or("");
            if (!binding.empty()) {
                bindings.push_back(binding);
            }
        } else if (auto arr = value.as_array()) {
            for (auto&& item : *arr) {
                std::string binding = item.value_or(std::string(""));
                if (!binding.empty()) {
                    bindings.push_back(std::move(binding));
                }
            }
        }

        if (!bindings.empty()) {
            output[std::string(key.str())] = std::move(bindings);
        }
    }
}

}  // namespace

Config Config::load(const std::string& path) {
    Config config;
    config.formats = {
        "mp3", "wav", "flac", "ogg", "oga", "m4a", "mp4", "aac",
        "alac", "aif", "aiff"
    };

    auto tbl = toml::parse_file(path);

    // MUSIC DIRS
    if (auto arr = tbl["music"]["directories"].as_array()) {
        for (auto&& dir : *arr) {
            config.musicDirectories.push_back(
                dir.value_or("")
            );
        }
    }
    config.rootFolder = tbl["music"]["root_folder"].value_or(std::string(""));

    // PLAYER
    config.volume =
        tbl["player"]["volume"].value_or(80);
    config.fps = std::clamp(
        tbl["player"]["fps"].value_or(
            tbl["autocue"]["manual_editor_fps"].value_or(20)),
        1,
        30);
    config.selectionStyle = lowerCopy(
        tbl["ui"]["selection_style"].value_or(std::string("bright")));
    if (config.selectionStyle != "bright" &&
        config.selectionStyle != "full" &&
        config.selectionStyle != "underline") {
        config.selectionStyle = "bright";
    }
    config.showTrackNumbers = tbl["ui"]["show_track_numbers"].value_or(true);

    loadKeyBindingSection(tbl, "keybinds", config.keybinds);
    loadKeyBindingSection(tbl, "keybinds_marker", config.keybindsMarker);
    loadKeyBindingSection(tbl, "keybinds_trim", config.keybindsTrim);
    if (auto arr = tbl["music"]["formats"].as_array()) {
        config.formats.clear();
        for (auto&& f : *arr) {
            config.formats.push_back(f.value_or(""));
        }
    }
    config.downloadFormat =
        tbl["music"]["download_format"].value_or(
            tbl["download_format"].value_or(std::string("m4a")));
    config.downloadFormat = lowerCopy(config.downloadFormat);

    config.library.enabled = tbl["library"]["enabled"].value_or(true);
    config.library.databasePath =
        tbl["library"]["database_path"].value_or(
            std::string("~/Music/tmplay/tmplay_library.db"));
    config.library.syncFolder =
        tbl["library"]["sync_folder"].value_or(
            std::string("~/Library/Mobile Documents/com~apple~CloudDocs/tmplay"));
    config.library.exportSerato =
        tbl["library"]["export_serato"].value_or(
            tbl["autocue"]["write_serato"].value_or(true));
    config.library.exportRekordbox =
        tbl["library"]["export_rekordbox"].value_or(true);
    config.library.exportTraktor =
        tbl["library"]["export_traktor"].value_or(true);
    config.library.exportJson =
        tbl["library"]["export_json"].value_or(true);

    config.telegram.enabled = tbl["telegram"]["enabled"].value_or(false);
    config.telegram.mode =
        lowerCopy(tbl["telegram"]["mode"].value_or(std::string("bot")));
    config.telegram.botToken =
        ProcessRunner::trim(tbl["telegram"]["bot_token"].value_or(std::string("")));
    // The Telegram browser is meaningful only after a bot is configured.
    // Treat an empty token as disabled even when the sample config still has
    // `enabled = true`.
    if (config.telegram.botToken.empty()) {
        config.telegram.enabled = false;
    }
    config.telegram.downloadRoot =
        tbl["telegram"]["download_root"].value_or(
            std::string("~/Music/tmplay/Telegram"));
    config.telegram.discoverChats =
        tbl["telegram"]["discover_chats"].value_or(true);
    config.telegram.syncOnStart =
        tbl["telegram"]["sync_on_start"].value_or(false);
    config.telegram.syncOnOpenFolder =
        tbl["telegram"]["sync_on_open_folder"].value_or(true);
    config.telegram.downloadOnPlay =
        tbl["telegram"]["download_on_play"].value_or(true);
    config.telegram.autoAnalyzeAfterDownload =
        tbl["telegram"]["auto_analyze_after_download"].value_or(true);
    config.telegram.createPlaylistPerChat =
        tbl["telegram"]["create_playlist_per_chat"].value_or(true);
    config.telegram.maxFileSizeMb =
        std::clamp(tbl["telegram"]["max_file_size_mb"].value_or(300), 1, 2000);
    if (auto extensions = tbl["telegram"]["allowed_extensions"].as_array()) {
        config.telegram.allowedExtensions.clear();
        for (auto&& extension : *extensions) {
            std::string value = lowerCopy(extension.value_or(std::string("")));
            if (value.starts_with(".")) {
                value.erase(value.begin());
            }
            if (!value.empty()) {
                config.telegram.allowedExtensions.push_back(std::move(value));
            }
        }
    }
    if (config.telegram.allowedExtensions.empty()) {
        config.telegram.allowedExtensions = {
            "mp3", "m4a", "wav", "flac", "aiff", "aac", "ogg", "opus"
        };
    }

    if (auto browsers = tbl["ytdlp"]["cookies_from_browser"].as_array()) {
        for (auto&& browser : *browsers) {
            std::string name = lowerCopy(browser.value_or(std::string("")));
            if (!name.empty() &&
                std::find(config.ytdlp.cookiesFromBrowser.begin(),
                          config.ytdlp.cookiesFromBrowser.end(),
                          name) == config.ytdlp.cookiesFromBrowser.end()) {
                config.ytdlp.cookiesFromBrowser.push_back(std::move(name));
            }
        }
    }
    if (config.ytdlp.cookiesFromBrowser.empty()) {
        config.ytdlp.cookiesFromBrowser = detectedCookieBrowsers();
    }
    config.ytdlp.cookiesPath = ProcessRunner::trim(
        tbl["ytdlp"]["cookies_path"].value_or(std::string("")));
    if (!config.ytdlp.cookiesPath.empty()) {
        fs::path cookies(config.ytdlp.cookiesPath);
        if (cookies.is_relative()) {
            cookies = fs::path(path).parent_path() / cookies;
        }
        config.ytdlp.cookiesPath = cookies.lexically_normal().string();
    }

    config.audd.apiKey = ProcessRunner::trim(
        tbl["audd"]["api_key"].value_or(std::string("")));
    config.audd.listenSeconds = std::clamp(
        tbl["audd"]["listen_sec"].value_or(12), 12, 20);

    config.input.cursorType = lowerCopy(ProcessRunner::trim(
        tbl["input"]["cursor_type"].value_or(std::string("bar"))));
    if (config.input.cursorType == "|" || config.input.cursorType == "line" ||
        config.input.cursorType == "vertical") {
        config.input.cursorType = "bar";
    } else if (config.input.cursorType == "_" ||
               config.input.cursorType == "underscore") {
        config.input.cursorType = "underline";
    }
    if (config.input.cursorType != "bar" &&
        config.input.cursorType != "underline") {
        config.input.cursorType = "bar";
    }

    config.columns.time = tbl["columns"]["time"].value_or(true);
    config.columns.bpm = tbl["columns"]["bpm"].value_or(true);
    config.columns.key = tbl["columns"]["key"].value_or(true);
    config.columns.kbps = tbl["columns"]["kbps"].value_or(true);
    config.columns.size = tbl["columns"]["size"].value_or(true);
    config.columns.rate = tbl["columns"]["rate"].value_or(true);
    config.columns.frmt = tbl["columns"]["frmt"].value_or(true);
    config.columns.genre = tbl["columns"]["genre"].value_or(true);
    auto knownColumn = [](const std::string& name) {
        return name == "time" || name == "bpm" || name == "key" ||
               name == "kbps" || name == "size" || name == "rate" ||
               name == "frmt" || name == "genre";
    };
    auto addColumn = [&](std::vector<std::string>& order, std::string name) {
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        if (!knownColumn(name) ||
            std::find(order.begin(), order.end(), name) != order.end()) {
            return;
        }
        order.push_back(std::move(name));
    };
    std::vector<std::string> column_order;
    if (auto order = tbl["columns"]["order"].as_array()) {
        for (auto&& item : *order) {
            addColumn(column_order, item.value_or(std::string("")));
        }
    }
    for (const auto& name : config.columns.order) {
        addColumn(column_order, name);
    }
    config.columns.order = std::move(column_order);

    config.demucs.stems = tbl["demucs"]["stems"].value_or(2);
    if (config.demucs.stems != 2 &&
        config.demucs.stems != 4) {
        config.demucs.stems = 2;
    }
    config.demucs.backend =
        lowerCopy(tbl["demucs"]["backend"].value_or(std::string("rust")));
    if (config.demucs.backend != "rust") {
        config.demucs.backend = "rust";
    }
    config.demucs.model =
        tbl["demucs"]["model"].value_or(std::string("htdemucs"));
    config.demucs.twoStemSource =
        tbl["demucs"]["two_stem_source"].value_or(std::string("vocals"));
    config.demucs.outputDirectory =
        tbl["demucs"]["output_directory"].value_or(std::string(""));
    config.demucs.outputFormat =
        tbl["demucs"]["output_format"].value_or(std::string("wav"));
    std::transform(config.demucs.outputFormat.begin(),
                   config.demucs.outputFormat.end(),
                   config.demucs.outputFormat.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (config.demucs.outputFormat != "wav" &&
        config.demucs.outputFormat != "mp3" &&
        config.demucs.outputFormat != "flac") {
        config.demucs.outputFormat = "wav";
    }
    config.demucs.jobs = std::max(0, tbl["demucs"]["jobs"].value_or(0));
    config.demucs.overlap =
        std::clamp(tbl["demucs"]["overlap"].value_or(0.05f), 0.0f, 0.25f);
    config.demucs.shiftSeconds =
        std::clamp(tbl["demucs"]["shift_seconds"].value_or(0.0f), 0.0f, 0.5f);

    config.autoCue.enabled = tbl["autocue"]["enabled"].value_or(true);
    config.autoCue.writeSerato = tbl["autocue"]["write_serato"].value_or(true);
    config.autoCue.writeTraktor = tbl["autocue"]["write_traktor"].value_or(true);
    config.autoCue.writeJson = tbl["autocue"]["write_json"].value_or(false);
    config.autoCue.backupBeforeWrite =
        tbl["autocue"]["backup_before_write"].value_or(false);
    config.autoCue.overwriteExistingCues =
        tbl["autocue"]["overwrite_existing_cues"].value_or(true);
    config.autoCue.cleanupAfterWrite =
        tbl["autocue"]["cleanup_after_write"].value_or(true);
    config.autoCue.syncPrefer =
        tbl["autocue"]["sync_prefer"].value_or(std::string("serato"));
    std::transform(config.autoCue.syncPrefer.begin(),
                   config.autoCue.syncPrefer.end(),
                   config.autoCue.syncPrefer.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (config.autoCue.syncPrefer != "serato" &&
        config.autoCue.syncPrefer != "traktor") {
        config.autoCue.syncPrefer = "serato";
    }
    config.autoCue.mode =
        tbl["autocue"]["mode"].value_or(std::string("structure"));
    std::transform(config.autoCue.mode.begin(),
                   config.autoCue.mode.end(),
                   config.autoCue.mode.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    config.autoCue.beatsPerBar =
        std::clamp(tbl["autocue"]["beats_per_bar"].value_or(4), 1, 16);
    config.autoCue.snapToBar =
        tbl["autocue"]["snap_to_bar"].value_or(true);

    auto parseColor = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        if (value == "green") return 0x00ff00u;
        if (value == "blue") return 0x0080ffu;
        if (value == "red") return 0xff0000u;
        if (value == "orange") return 0xff8000u;
        if (value == "yellow") return 0xffff00u;
        if (value == "purple") return 0x8000ffu;
        if (value == "white") return 0xffffffu;
        if (value == "cyan") return 0x00ffffu;
        if (value == "grey" || value == "gray") return 0x808080u;
        if (value.starts_with("#")) {
            value.erase(value.begin());
        }
        if (value.starts_with("0x")) {
            value.erase(0, 2);
        }
        std::uint32_t color = 0xffffff;
        if (value.size() == 6) {
            std::from_chars(value.data(), value.data() + value.size(), color, 16);
        }
        return color;
    };

    config.autoCue.manualWaveformColorRgb = parseColor(
        tbl["autocue"]["waveform_color"].value_or(std::string("cyan")));
    config.autoCue.manualPlayheadColorRgb = parseColor(
        tbl["autocue"]["playhead_color"].value_or(std::string("yellow")));
    config.autoCue.waveformDisplay =
        tbl["autocue"]["waveform_display"].value_or(std::string("filled"));
    std::transform(config.autoCue.waveformDisplay.begin(),
                   config.autoCue.waveformDisplay.end(),
                   config.autoCue.waveformDisplay.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (config.autoCue.waveformDisplay != "filled" &&
        config.autoCue.waveformDisplay != "line" &&
        config.autoCue.waveformDisplay != "bars") {
        config.autoCue.waveformDisplay = "filled";
    }
    if (auto cues = tbl["autocue"]["cues"].as_array()) {
        for (auto&& cueNode : *cues) {
            auto* cue = cueNode.as_table();
            if (cue == nullptr) {
                continue;
            }
            AutoCueConfig::Slot slot;
            slot.index = std::clamp(
                (*cue)["index"].value_or((int)config.autoCue.cues.size()),
                0,
                7);
            slot.name = (*cue)["name"].value_or(std::string("CUE"));
            slot.type = (*cue)["type"].value_or(std::string("energy_peak"));
            std::transform(slot.type.begin(), slot.type.end(), slot.type.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            slot.colorRgb = parseColor((*cue)["color"].value_or(std::string("white")));
            config.autoCue.cues.push_back(slot);
        }
    }
    if (config.autoCue.cues.empty()) {
        config.autoCue.cues = {
            {0, "START", "start", 0x00ff00},
            {1, "DROP 1", "first_drop", 0xff0000},
            {2, "BREAK", "breakdown", 0x0080ff},
            {3, "DROP 2", "second_drop", 0xff8000},
            {4, "OUTRO", "outro_start", 0xffff00},
        };
    }

    return config;
}
