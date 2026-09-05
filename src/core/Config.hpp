#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

struct ColumnVisibility {
    bool time = true;
    bool bpm = true;
    bool key = true;
    bool kbps = true;
    bool size = true;
    bool rate = true;
    bool frmt = true;
    bool genre = true;
    std::vector<std::string> order = {
        "time", "bpm", "key", "kbps", "rate", "size", "frmt", "genre"
    };
};

struct DemucsConfig {
    int stems = 2;
    std::string backend = "rust";
    std::string model = "htdemucs";
    std::string twoStemSource = "vocals";
    std::string outputDirectory;
    std::string outputFormat = "wav";
    int jobs = 0;
    float overlap = 0.05f;
    float shiftSeconds = 0.0f;
};

struct AutoCueConfig {
    bool enabled = true;
    bool writeSerato = true;
    bool writeTraktor = true;
    bool writeJson = false;
    bool backupBeforeWrite = false;
    bool overwriteExistingCues = true;
    bool cleanupAfterWrite = true;
    std::string syncPrefer = "serato";
    std::string mode = "structure";
    int beatsPerBar = 4;
    bool snapToBar = true;
    std::uint32_t manualWaveformColorRgb = 0x20d0d8;
    std::uint32_t manualPlayheadColorRgb = 0xf3df3f;
    std::string waveformDisplay = "filled";
    struct Slot {
        int index = 0;
        std::string name;
        std::string type;
        std::uint32_t colorRgb = 0xffffff;
    };
    std::vector<Slot> cues;
};

struct YtDlpConfig {
    std::vector<std::string> cookiesFromBrowser;
    // Optional Netscape-format cookies.txt. It is used only after YouTube
    // explicitly asks for sign-in / bot verification.
    std::string cookiesPath;
};

struct AudDConfig {
    // Personal key from https://dashboard.audd.io/. FIND is hidden when it
    // is empty so the regular recorder remains available without AudD.
    std::string apiKey;
    int listenSeconds = 12;
};

struct InputStyleConfig {
    std::string cursorType = "bar";
};

struct TelegramConfig {
    bool enabled = false;
    std::string mode = "bot";
    std::string botToken;
    std::string downloadRoot = "~/Music/tmplay/Telegram";
    bool discoverChats = true;
    bool syncOnStart = false;
    bool syncOnOpenFolder = true;
    bool downloadOnPlay = true;
    bool autoAnalyzeAfterDownload = true;
    bool createPlaylistPerChat = true;
    int maxFileSizeMb = 300;
    std::vector<std::string> allowedExtensions = {
        "mp3", "m4a", "wav", "flac", "aiff", "aac", "ogg", "opus"
    };
};

struct LibraryConfig {
    std::string databasePath = "~/Music/tmplay/tmplay_library.db";
    std::string syncFolder = "~/Library/Mobile Documents/com~apple~CloudDocs/tmplay";
    bool enabled = true;
    bool exportSerato = true;
    bool exportRekordbox = true;
    bool exportTraktor = true;
    bool exportJson = true;
};

using KeyBindingMap = std::unordered_map<std::string, std::vector<std::string>>;

struct Config {
    std::vector<std::string> musicDirectories;
    std::string rootFolder;

    int volume = 80;
    int fps = 20;
    // bright = current behaviour, full = inverse row, underline = underline row.
    std::string selectionStyle = "bright";
    // Number tracks in preserved online album / playlist order.
    bool showTrackNumbers = true;
    
    std::vector<std::string> formats;
    std::string downloadFormat = "m4a";
    ColumnVisibility columns;
    LibraryConfig library;
    DemucsConfig demucs;
    AutoCueConfig autoCue;
    YtDlpConfig ytdlp;
    AudDConfig audd;
    InputStyleConfig input;
    TelegramConfig telegram;
    KeyBindingMap keybinds;
    KeyBindingMap keybindsMarker;
    KeyBindingMap keybindsTrim;

    static Config load(const std::string& path);
};
