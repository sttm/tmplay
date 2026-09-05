cmake_minimum_required(VERSION 3.16)
project(tmplay LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(TPLAY_BUILD_TESTS "Build tmplay smoke tests" OFF)
if(TPLAY_BUILD_TESTS)
    enable_testing()
endif()

# Homebrew hint (important on macOS)
if(APPLE)
    enable_language(OBJCXX)
    if(CMAKE_OSX_ARCHITECTURES MATCHES "x86_64" AND EXISTS "/usr/local")
        list(APPEND CMAKE_PREFIX_PATH "/usr/local")
    elseif(EXISTS "/opt/homebrew")
        list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew")
    elseif(EXISTS "/usr/local")
        list(APPEND CMAKE_PREFIX_PATH "/usr/local")
    endif()
endif()

find_package(ftxui REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(BLAS REQUIRED)
find_package(TagLib QUIET)
find_package(SQLite3 REQUIRED)

# demucs-rs is a separate Rust/Burn/Metal executable.  ONNX Runtime is used
# only by AudioAnalyzer's Discogs-EffNet genre classifier.
option(TPLAY_ENABLE_ONNXRUNTIME "Enable ONNX Runtime genre classifier" ON)

if(APPLE AND TPLAY_ENABLE_ONNXRUNTIME)
    find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h
        PATHS ${CMAKE_PREFIX_PATH}/include/onnxruntime /opt/homebrew/include/onnxruntime /usr/local/include/onnxruntime)
    find_library(ONNXRUNTIME_LIBRARY onnxruntime
        PATHS ${CMAKE_PREFIX_PATH}/lib /opt/homebrew/lib /usr/local/lib)
    if(ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIBRARY)
        message(STATUS "ONNX Runtime found for Discogs genre classifier: ${ONNXRUNTIME_LIBRARY}")
        set(TPLAY_WITH_ONNXRUNTIME ON)
    else()
        message(STATUS "ONNX Runtime not found; Discogs genre classifier disabled")
        set(TPLAY_WITH_ONNXRUNTIME OFF)
    endif()
else()
    set(TPLAY_WITH_ONNXRUNTIME OFF)
endif()

set(TPLAY_ESSENTIA_PREFIX
    "${CMAKE_CURRENT_SOURCE_DIR}/external/essentia-install"
    CACHE PATH "Prefix of the architecture-matched local Essentia build")

if(APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES "x86_64" AND
   NOT CMAKE_OSX_ARCHITECTURES MATCHES ";")
    set(TPLAY_HOMEBREW_PREFIX "/usr/local")
else()
    set(TPLAY_HOMEBREW_PREFIX "/opt/homebrew")
endif()
set(ESSENTIA_PKG_CONFIG_PATH
    "${TPLAY_ESSENTIA_PREFIX}/lib/pkgconfig"
    "${TPLAY_HOMEBREW_PREFIX}/opt/ffmpeg/lib/pkgconfig"
    "${TPLAY_HOMEBREW_PREFIX}/opt/libsamplerate/lib/pkgconfig"
    "${TPLAY_HOMEBREW_PREFIX}/opt/taglib/lib/pkgconfig"
    "${TPLAY_HOMEBREW_PREFIX}/opt/chromaprint/lib/pkgconfig"
    "${TPLAY_HOMEBREW_PREFIX}/opt/fftw/lib/pkgconfig"
    "${TPLAY_HOMEBREW_PREFIX}/opt/libyaml/lib/pkgconfig"
    "${TPLAY_HOMEBREW_PREFIX}/opt/eigen/share/pkgconfig"
    "${TPLAY_HOMEBREW_PREFIX}/lib/pkgconfig"
)
string(JOIN ":" ESSENTIA_PKG_CONFIG_PATH_JOINED ${ESSENTIA_PKG_CONFIG_PATH})
if(DEFINED ENV{PKG_CONFIG_PATH} AND NOT "$ENV{PKG_CONFIG_PATH}" STREQUAL "")
    set(ENV{PKG_CONFIG_PATH} "${ESSENTIA_PKG_CONFIG_PATH_JOINED}:$ENV{PKG_CONFIG_PATH}")
else()
    set(ENV{PKG_CONFIG_PATH} "${ESSENTIA_PKG_CONFIG_PATH_JOINED}")
endif()
pkg_check_modules(ESSENTIA REQUIRED essentia)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat libavcodec libavutil libswresample)

# demucs-rs is shipped as a small companion executable.  It keeps the Metal
# inference stack out of the C++ process and gives both CPU architectures the
# same source-separation implementation.  A universal2 build is assembled by
# scripts/build_dist.sh because Cargo emits one architecture per invocation.
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES MATCHES ";")
    # Cargo is a native executable too.  Do not let an Intel Cargo from
    # /usr/local run under Rosetta during an Apple-Silicon build (or vice
    # versa), because it would produce the wrong Demucs architecture.
    find_program(CARGO_EXECUTABLE cargo
        PATHS "${TPLAY_HOMEBREW_PREFIX}/bin"
        NO_DEFAULT_PATH
        REQUIRED)
    find_program(SWIFTC_EXECUTABLE swiftc REQUIRED)
    set(DEMUCS_RS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/demucs-rs")
    # Cargo's default `target` folder is shared by native and Rosetta builds.
    # It does not reliably distinguish aarch64 and x86_64 artifacts there,
    # which can put an Intel Demucs binary into an arm64 package. Keep Cargo
    # output inside the architecture-specific CMake build directory instead.
    set(DEMUCS_RS_TARGET_DIR "${CMAKE_BINARY_DIR}/demucs-rs-target")
    set(DEMUCS_RS_BINARY "${CMAKE_BINARY_DIR}/demucs")
    add_custom_command(
        OUTPUT "${DEMUCS_RS_BINARY}"
        COMMAND "${CMAKE_COMMAND}" -E env
                "CARGO_TARGET_DIR=${DEMUCS_RS_TARGET_DIR}"
                "${CARGO_EXECUTABLE}" build --release -p demucs-cli
        COMMAND "${CMAKE_COMMAND}" -E copy
                "${DEMUCS_RS_TARGET_DIR}/release/demucs" "${DEMUCS_RS_BINARY}"
        WORKING_DIRECTORY "${DEMUCS_RS_DIR}"
        DEPENDS "${DEMUCS_RS_DIR}/Cargo.toml"
                "${DEMUCS_RS_DIR}/demucs-cli/src/main.rs"
        COMMENT "Building demucs-rs companion"
        VERBATIM)
    add_custom_target(tplay_demucs_rs DEPENDS "${DEMUCS_RS_BINARY}")

    set(TPLAY_RECORDER_BINARY "${CMAKE_BINARY_DIR}/tmplay-recorder")
    add_custom_command(
        OUTPUT "${TPLAY_RECORDER_BINARY}"
        COMMAND "${SWIFTC_EXECUTABLE}" -O
                "${CMAKE_CURRENT_SOURCE_DIR}/src/Record/TPlayRecorder.swift"
                -framework ScreenCaptureKit -framework AVFoundation -framework CoreMedia
                -o "${TPLAY_RECORDER_BINARY}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/Record/TPlayRecorder.swift"
        COMMENT "Building ScreenCaptureKit recorder companion"
        VERBATIM)
    add_custom_target(tplay_recorder DEPENDS "${TPLAY_RECORDER_BINARY}")
endif()

add_executable(tmplay
    src/main.cpp
    src/core/TrackStore.cpp
    src/core/AppController.cpp
    src/core/Config.cpp
    src/core/AudioEngine.mm
    src/core/OnlineStreamDecoder.cpp
    src/core/MacActivity.mm
    src/core/MacNowPlaying.mm
    src/core/DownloadManager.cpp
    src/core/PlaylistImporter.cpp
    src/core/ScreenRecorder.cpp
    src/core/AudDRecognizer.cpp
    src/core/StemSeparator.cpp
    src/core/AudioProcessor.cpp
    src/core/ProcessRunner.cpp
    src/core/AudioAnalyzer.cpp
    src/core/MetadataWriter.cpp
    src/Library/LibraryDatabase.cpp
    src/Library/TrackId.cpp
    src/Library/TrackRepository.cpp
    src/Library/CueRepository.cpp
    src/Library/LoopRepository.cpp
    src/Library/PlaylistRepository.cpp
    src/Library/LibraryJson.cpp
    src/ui/ManualCueEditor.cpp
    src/ui/TrimEditor.cpp
    src/AutoCue/AutoCueMarker.cpp
    src/AutoCue/CueDetector.cpp
    src/AutoCue/CuePreview.cpp
    src/AutoCue/FolderProcessor.cpp
    src/Serato/SeratoCueWriter.cpp
    src/Serato/SeratoMarkers2.cpp
    src/Export/SeratoExportProvider.cpp
    src/Export/RekordboxExportProvider.cpp
    src/Export/TraktorExportProvider.cpp
    src/Export/LibraryExporter.cpp
    src/Export/ExportValidator.cpp
    src/Sync/JsonSync.cpp
    src/Telegram/TelegramBotClient.cpp
    src/Telegram/TelegramInboxService.cpp
    src/Telegram/TelegramRepository.cpp
    src/Traktor/TraktorMetadataWriter.cpp
)

if(NOT TagLib_FOUND)
    find_path(TAGLIB_INCLUDE_DIR taglib/tag.h PATHS /opt/homebrew/include /usr/local/include)
    find_library(TAGLIB_LIBRARY tag PATHS /opt/homebrew/lib /usr/local/lib)
    if(NOT TAGLIB_INCLUDE_DIR OR NOT TAGLIB_LIBRARY)
        message(FATAL_ERROR "TagLib not found")
    endif()
endif()

if(APPLE)
    # Never mix Apple-Silicon Homebrew headers into an Intel/Rosetta build.
    # The Universal bundle is made from two native builds, so this directory
    # must match the active CMake architecture slice.
    if(CMAKE_OSX_ARCHITECTURES MATCHES "x86_64" AND NOT CMAKE_OSX_ARCHITECTURES MATCHES ";")
        target_include_directories(tmplay PRIVATE /usr/local/include)
    else()
        target_include_directories(tmplay PRIVATE /opt/homebrew/include)
    endif()
endif()

if(NOT TagLib_FOUND)
    target_include_directories(tmplay PRIVATE ${TAGLIB_INCLUDE_DIR})
endif()

target_include_directories(tmplay
    SYSTEM PRIVATE ${ESSENTIA_INCLUDE_DIRS}
)

# Essentia still declares a comparator through std::binary_function.  Newer
# Apple libc++ hides that removed C++17 compatibility type unless this switch
# is present.  It is required for the Intel FFmpeg-9 Essentia build as well as
# current Xcode SDKs.
if(APPLE)
    target_compile_definitions(tmplay PRIVATE
        _LIBCPP_ENABLE_CXX17_REMOVED_UNARY_BINARY_FUNCTION)
endif()

# Essentia's locally installed static archive can retain a Homebrew Cellar
# path after `brew reinstall chromaprint`. Prefer Homebrew's stable opt link
# so an analysis build remains linkable across Chromaprint patch upgrades.
set(TPLAY_ESSENTIA_LIBRARY_DIRS ${ESSENTIA_LIBRARY_DIRS})
if(APPLE)
    list(FILTER TPLAY_ESSENTIA_LIBRARY_DIRS EXCLUDE REGEX "/Cellar/chromaprint/")
endif()
target_link_directories(tmplay
    PRIVATE ${TPLAY_ESSENTIA_LIBRARY_DIRS}
)
if(APPLE)
    if(CMAKE_OSX_ARCHITECTURES MATCHES "x86_64" AND NOT CMAKE_OSX_ARCHITECTURES MATCHES ";")
        set(TPLAY_CHROMAPRINT_PREFIX "/usr/local")
    else()
        set(TPLAY_CHROMAPRINT_PREFIX "/opt/homebrew")
    endif()
    if(EXISTS "${TPLAY_CHROMAPRINT_PREFIX}/opt/chromaprint/lib")
        target_link_directories(tmplay PRIVATE "${TPLAY_CHROMAPRINT_PREFIX}/opt/chromaprint/lib")
    endif()
endif()

target_link_libraries(tmplay
    PRIVATE ftxui::screen
    PRIVATE ftxui::dom
    PRIVATE ftxui::component
    PRIVATE Threads::Threads
    PRIVATE SQLite3::SQLite3
    PRIVATE ${ESSENTIA_LIBRARIES}
    PRIVATE PkgConfig::FFMPEG
)

if(TagLib_FOUND)
    target_link_libraries(tmplay PRIVATE TagLib::TagLib)
else()
    target_link_libraries(tmplay PRIVATE ${TAGLIB_LIBRARY})
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(tmplay PRIVATE m dl)
endif()

if(APPLE)
    set_source_files_properties(
        src/core/AudioEngine.mm
        src/core/MacActivity.mm
        src/core/MacNowPlaying.mm
        PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    target_link_libraries(tmplay PRIVATE "-framework AVFoundation" "-framework Foundation" "-framework AppKit" "-framework MediaPlayer" "-framework IOKit" "-framework CoreFoundation" "-framework CoreAudio" "-framework CoreMedia" "-framework Accelerate")
    # A terminal launch has no app bundle. Embed the same stable identity in
    # the Mach-O so MediaPlayer can register tmplay with Control Center.
    target_link_options(tmplay PRIVATE
        "-Wl,-sectcreate,__TEXT,__info_plist,${CMAKE_CURRENT_SOURCE_DIR}/resources/TPlayCLI-Info.plist")
    if(TPLAY_WITH_ONNXRUNTIME)
        target_compile_definitions(tmplay PRIVATE TPLAY_WITH_ONNXRUNTIME=1)
        target_include_directories(tmplay PRIVATE ${ONNXRUNTIME_INCLUDE_DIR})
        target_link_libraries(tmplay PRIVATE ${ONNXRUNTIME_LIBRARY})
    endif()
endif()

if(TARGET tplay_demucs_rs)
    add_dependencies(tmplay tplay_demucs_rs)
endif()
if(TARGET tplay_recorder)
    add_dependencies(tmplay tplay_recorder)
endif()

# A developer may keep a private config.toml outside Git.  New public clones
# start from the safe template instead.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/config.toml")
    configure_file(config.toml config.toml COPYONLY)
else()
    configure_file(config.example.toml config.toml COPYONLY)
endif()
configure_file(scripts/ytmusic_bridge.py scripts/ytmusic_bridge.py COPYONLY)

if(APPLE)
    add_custom_target(package_macos
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/package_macos.sh" "${CMAKE_BINARY_DIR}"
        DEPENDS tmplay
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Create portable macOS zip package"
    )
endif()

if(TPLAY_BUILD_TESTS)
add_executable(tplay_library_export_smoke
    tests/library_export_smoke.cpp
    src/Export/LibraryExporter.cpp
    src/Export/RekordboxExportProvider.cpp
    src/Export/TraktorExportProvider.cpp
    src/Export/SeratoExportProvider.cpp
    src/Export/ExportValidator.cpp
    src/Sync/JsonSync.cpp
    src/Library/LibraryDatabase.cpp
    src/Library/TrackRepository.cpp
    src/Library/CueRepository.cpp
    src/Library/TrackId.cpp
    src/Library/LibraryJson.cpp
    src/Telegram/TelegramRepository.cpp
    src/Traktor/TraktorMetadataWriter.cpp
    src/Serato/SeratoCueWriter.cpp
    src/Serato/SeratoMarkers2.cpp
)
target_link_libraries(tplay_library_export_smoke
    PRIVATE SQLite3::SQLite3
)
if(TagLib_FOUND)
    target_link_libraries(tplay_library_export_smoke PRIVATE TagLib::TagLib)
else()
    target_include_directories(tplay_library_export_smoke PRIVATE ${TAGLIB_INCLUDE_DIR})
    target_link_libraries(tplay_library_export_smoke PRIVATE ${TAGLIB_LIBRARY})
endif()
add_test(NAME library_export_smoke COMMAND tplay_library_export_smoke)

add_executable(tplay_playlist_importer_smoke
    tests/playlist_importer_smoke.cpp
    src/core/PlaylistImporter.cpp
)
add_test(NAME playlist_importer_smoke COMMAND tplay_playlist_importer_smoke)
endif()
