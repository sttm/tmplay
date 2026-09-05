# tmplay

Terminal music player and local music-library tool for macOS, written in
C++20. It plays local and online tracks, searches YouTube Music, downloads
audio via yt-dlp, analyzes BPM/key/genre locally, records desktop audio and
separates stems with Demucs.

The ready-to-run macOS Universal 2 archive is released separately. This
repository contains only the source code and build resources; personal
configuration, test fixtures, models, local builds and release archives are
intentionally excluded.

Keyboard shortcuts and commands are available from the in-app help panel.

A cross-platform terminal-based music player and library browser written in C++20. Architecture inspired by mpd + ncmpcpp with focus on modularity and extensibility.

## 🚀 Features

### ✅ Implemented
- **Two-pane file browser** – Navigate music libraries with ease
  - Left: Directory tree with smart folder/file detection
  - Right: Track table with time, BPM, key, bitrate, file size and visible item count
- **Directory navigation** – Browse nested directories with back/forward
- **Audio playback** – Play selected tracks from the terminal UI
- **Pause/Resume/Stop** – Playback controls with live state display
- **Progress bar** – Current time and total duration
- **Transport control** – Compact status icons with hotkeys for previous, next and repeat/shuffle mode
- **macOS audio routing** – Uses the system AVFoundation route so multi-output audio interface speaker assignments are respected
- **Volume control** – Adjust volume with clickable `-` / `+` controls or keyboard shortcuts
- **Catalog pipeline** – Download audio and cover art from yt-dlp-supported services and clean titles; BPM/key/Discogs EffNet genre analysis runs locally only when an unanalysed folder is opened
- **Playlist import** – Open Serato `.crate`, M3U, Traktor `.nml`, or Rekordbox `.xml` as a temporary local playlist
- **Local music search** – Search and audition/download music with the locally installed `yt-dlp`, without a web backend
- **Desktop audio recording** – Record the macOS desktop output to WAV with ScreenCaptureKit
- **Stem separation** – Run local `demucs-rs` HTDemucs (Metal on macOS) for two- or four-stem exports
- **Library actions** – Create/rename folders and move tracks with the `D` destination workflow
- **DAW export on macOS** – Hand a selected track to another application with `G` and the `drag` utility
- **Config system** – TOML-based configuration
- **Clean state machine** – Responsive UI with FTXUI

Note: Metadata is read and analyzed in the background. Unknown values briefly show as `--:--` or `...` so folder navigation stays fast.

## 📦 Requirements

### macOS
```bash
./scripts/bootstrap_macos.sh
```

It installs the dependencies used to compile a local developer build.

For a manual setup:

```bash
brew install cmake pkg-config ftxui tomlplusplus ffmpeg yt-dlp taglib eigen libsamplerate chromaprint fftw libyaml onnxruntime rust
python3 -m pip install --user --upgrade ytmusicapi
git clone https://github.com/Wevah/dragterm.git /tmp/dragterm
xcodebuild -project /tmp/dragterm/dragterm.xcodeproj -scheme dragterm -configuration Release -derivedDataPath /tmp/dragterm/build CODE_SIGNING_ALLOWED=NO build
install -m 0755 /tmp/dragterm/build/Build/Products/Release/drag /opt/homebrew/bin/drag
```

Native analysis needs the local C++ assets under `external/essentia-install`.
The `Analyze` action on a local track runs the bundled Discogs EffNet model in
`models/genre_discogs400`, so genre inference is local and
does not require a backend or Python/TensorFlow runtime.  The model is
distributed under Essentia's CC BY-NC-SA 4.0 terms.
Stem separation uses `external/demucs-rs` and the single local model
`models/htdemucs.safetensors`. The CMake build places the `demucs` companion
next to `tmplay`; no model download is needed at runtime.

### Linux (Ubuntu/Debian)
```bash
sudo apt-get install libftxui-dev cmake
```

### Windows
Use vcpkg or build FTXUI from source.

## 🔨 Build

```bash
cd /path/to/tmplay
./scripts/prepare_demucs_rs.sh
cmake -B build
cmake --build build
cd build
./tmplay
```

## Standalone macOS app bundles

The release bundle includes tmplay, all non-system libraries, `ffmpeg`,
`ffprobe`, the self-updating official `yt-dlp` binary, the desktop recorder,
Demucs, local models, and the compiled YouTube Music bridge. It runs without
Homebrew, Python, FFmpeg, or yt-dlp installed on the destination Mac.

Build on Apple Silicon:

```bash
./scripts/build_macos_silicon.sh
```

Build for Intel Macs (run it on an Intel Mac, or on Apple Silicon with an
x86_64 Homebrew installation under `/usr/local` and Rosetta):

```bash
./scripts/build_macos_intel.sh
```

Each script creates `dist/tmplay-<arch>-macos.zip`. Unzip it and run
`tmplay.app/Contents/MacOS/tmplay` from Terminal (or double-click the bundled
`run.command`). In the input bar, enter `update` to update the bundled yt-dlp
executable without replacing tmplay itself.

For one macOS archive that runs on Apple Silicon and Intel, use:

```bash
./scripts/build_dist.sh
```

This emits `dist/tmplay-universal2-macos.zip` and verifies both executable
architectures before packaging. All non-system C++ dependencies must also be
installed as universal binaries.

## ⚙️ Configuration

Each build creates `config.toml` from the public `config.example.toml`.
Keep API keys and local paths only in your private `config.toml`, which is
ignored by Git. Edit the generated build configuration:

```toml
[music]
# Optional; when omitted, tmplay uses ~/Music/tmplay for the current user.
root_folder = "~/Music/tmplay"
directories = [
    "/path/to/music/library",
    "/another/music/path"
]
formats = ["mp3", "m4a", "wav", "flac"]
download_format = "m4a"

[player]
volume = 80

[ui]
# bright (default), full, or underline
selection_style = "bright"


[columns]
time = true
bpm = true
key = true
kbps = true
size = true
```

Set any `[columns]` value to `false` to hide it in the track table after
restarting `tmplay`. The `Title` column is always visible.

When `music.root_folder` is set, tmplay creates a visible `Download/` folder
next to `Search/`. Single tracks go to `Download/Downloads/<artist>/`; albums
go to `Download/Albums/<artist> — <album> (<year>)/`. Existing matching audio is skipped
and shown as already downloaded.

## ⌨️ Keyboard Shortcuts

| Key | Action |
|-----|--------|
| `←` | Return from tracks to directories; in the tree go to the parent directory |
| `→` | Open a directory; when it is already open, move into its track list |
| `↑` / `↓` | Navigate (up/down) |
| `Enter` | Open directory / Select track |
| `Space` | Pause / resume |
| `[` / `Х` and `]` / `Ъ` | Previous / next track |
| `F` / `А` | Open selected/current folder in Finder |
| `I` / `Ш` | Focus the input bar |
| `H` / `Р` | Toggle input help |
| `Y` / `Н` | Toggle download and Demucs activity details |
| `R` / `К` | Refresh current directory |
| `D` / `В` | Move selected track: select a destination folder, then press `Enter`; `Escape` cancels |
| `G` / `П` | On macOS, drag the selected track into a DAW or DJ application |
| `S` / `Ы` | Separate selected track with Demucs |
| `X` / `Ч` | Open Auto Cue popup: selected track, all folder, or cancel |
| `M` / `Ь` | Switch repeat/shuffle mode |
| `O` / `Щ` | Stop playback |
| `Q` / `Й` | Ask for quit confirmation |
| `-` / `+` | Decrease / increase volume |
| `Backspace` / `Delete` | Ask to delete the selected track or folder |

The input bar accepts pasted URLs from any service supported by yt-dlp, including YouTube and SoundCloud. Use `download <source>` for an explicit yt-dlp source, plus `mkdir <name>`, `rename <name>`, `refresh`, and `update`. `update` updates the yt-dlp copy bundled with a standalone app. Links containing `list=` or `/sets/` open a `Yes` / `No` confirmation popup before all playlist tracks are downloaded; use mouse, `Left` / `Right`, `Enter`, or `Escape`.

Use `playlist <file>` to open a `.crate`, `.m3u`, `.m3u8`, `.nml`, or `.xml` file as an in-memory playlist: source audio remains untouched and missing references are reported. `track <text>` searches YouTube Music and creates a dynamic online playlist. `search <text>` searches configured local music folders. `album <text>` uses YouTube Music via the bundled `ytmusicapi` bridge and returns album cards quickly. Album results are rows in the playlist (`Artist — Album (Year)`), not folders in the left pane: press Enter to fetch that album's tracks and verify them as Art Tracks (ATV), then select `..` to return to the album results. The online track context menu provides `Play`, `Open album`, and `Download`; `Open album` searches the official release by artist and album. Press Enter starts a direct in-memory audio stream and does not save anything. Clicking the cyan `↓` afterwards reuses the already resolved direct media URL when it is still valid; otherwise it uses the original source URL. Local editor functions are available only after an explicit download completes. Inside an album playlist use `↓ album` to queue every displayed verified track. Pasting a result URL or using `download <url>` saves it into `Download/Downloads/`.


The `REC` button in the input bar toggles desktop audio recording; `record`, `record start`, and `record stop` do the same from the input. The recorded WAV is placed in the current real folder (or the first configured music directory when browsing a virtual playlist). macOS 13+ asks once for Screen Recording permission.

Press `S` / `Ы` on a selected track to open a Demucs popup with `2stems` / `4stems`, `mp3` / `wav`, and `Yes` / `No`. Use arrows and `Enter`, or click with the mouse. Empty `output_directory` writes into `separated` next to the source file. This build uses the four-source `htdemucs` model; `6stems` is intentionally not enabled.

Press `X` / `Ч` to open the native Essentia Auto Cue popup. Choose `selected track`, `all folder`, or `cancel`. tmplay can write `track.ext.cues.json` for inspection and, for MP3/M4A/MP4 files, creates `track.ext.autocue.bak` before replacing the Serato cue metadata block. With `cleanup_after_write = true`, generated JSON and backup files are removed after each successful write.

Track playback automatically continues through the track list captured when playback starts, even while the display is off or another directory is being browsed.
Mouse hover selects only visible rows without scrolling; double click opens a directory or plays a track. The left pane shows `Folder <current>` and switches to `Move to <destination>` during relocation. To move a track, press `D` / `В`, choose a directory, and press `Enter`.
On macOS, select or hover a track and press `G` / `П`; the external [`dragterm`](https://github.com/Wevah/dragterm) `drag` window lets you drop that file into a DAW, DJ software, Finder, or another application.
On macOS the display may turn off during playback, while tmplay keeps the system awake until playback is paused or stopped. Audio follows the system default output device and reloads when that output changes.

## 🏗️ Project Structure

```
src/
├── main.cpp              # UI and input handling (FTXUI)
├── core/
│   ├── AppController.hpp/cpp    # Application logic & state
│   ├── AudioAnalyzer.hpp/cpp     # Essentia BPM/key analysis
│   ├── MetadataWriter.hpp/cpp    # TagLib/ffmpeg metadata persistence
│   ├── DownloadManager.hpp/cpp   # yt-dlp pipeline
│   ├── StemSeparator.hpp/cpp     # Demucs stem separation
│   ├── TrackStore.hpp/cpp       # Thread-safe track storage
│   ├── Config.hpp/cpp           # TOML config parsing
│   └── Track.hpp                # Data models
└── ui/
    └── BrowserState.hpp         # UI state machine
```

## 🔍 Technical Details

- **Language**: C++20
- **UI Framework**: FTXUI
- **Config Format**: TOML (via toml++/toml.h)
- **Thread Safety**: Mutex-protected TrackStore
- **Build System**: CMake 3.16+

## 📝 Design Principles

1. **Separation of Concerns**: Core logic separate from UI
2. **Type Safety**: Strict media, directory, and virtual-navigation entry distinction
3. **Modularity**: Each component has clear responsibilities
4. **Error Handling**: Graceful degradation on permission errors

## 🚧 Known Limitations

- Metadata analysis is queued one track at a time to keep browser navigation responsive.
- Playback continuation uses the track list from the directory where playback was started.

## 📄 License

MIT

## 👨‍💻 Development Status

**Current Stage**: Final MVP playback browser ✅

**Next Stage**: Async processing pipeline
