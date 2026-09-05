#!/usr/bin/env bash
# Installs TPlay's macOS prerequisites and always relinks the local build.
# Re-run this script after `brew upgrade ffmpeg`, because FFmpeg changes its
# ABI version and a previously linked development binary can no longer start.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 8)}"

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "This bootstrap script is for macOS." >&2
  exit 1
fi
if ! command -v brew >/dev/null 2>&1; then
  echo "Homebrew is required: https://brew.sh" >&2
  exit 1
fi

# ffmpeg/ffprobe and yt-dlp deliberately remain external command-line tools.
# The package target bundles C++ dylibs, but these tools stay updateable by brew.
brew install cmake pkg-config ftxui tomlplusplus ffmpeg yt-dlp taglib \
  eigen libsamplerate chromaprint fftw libyaml onnxruntime rust

# Official album search resolves verified YouTube Music Art Tracks through a
# small bundled bridge. Keep this as a Python package so it can be updated
# independently of TPlay's native dependencies.
python3 -m pip install --user --upgrade ytmusicapi

if ! xcrun --find swiftc >/dev/null 2>&1; then
  echo "Xcode Command Line Tools are required: xcode-select --install" >&2
  exit 1
fi

"$ROOT_DIR/scripts/prepare_demucs_rs.sh"

# A clean link is intentional. It prevents a stale tplay from referencing a
# removed Homebrew dylib such as libavformat.62 after an FFmpeg upgrade.
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --target clean
cmake --build "$BUILD_DIR" --target tmplay -j"$JOBS"

echo
echo "tmplay is ready: $BUILD_DIR/tmplay"
echo "Run: $BUILD_DIR/tmplay"
