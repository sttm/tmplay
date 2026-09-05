#!/usr/bin/env bash
set -euo pipefail

# Build one self-contained, architecture-specific TPlay.app. The target
# Homebrew installation must be native for the requested CPU architecture.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARCH="${1:-}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 8)}"

case "$ARCH" in
  arm64)
    BREW_PREFIX="${TPLAY_BREW_PREFIX:-/opt/homebrew}"
    ;;
  x86_64)
    BREW_PREFIX="${TPLAY_BREW_PREFIX:-/usr/local}"
    ;;
  *)
    echo "Usage: $0 <arm64|x86_64>" >&2
    exit 1
    ;;
esac

HOST_ARCH="$(uname -m)"
RUN_UNDER_ROSETTA=0
if [[ "$HOST_ARCH" == "arm64" && "$ARCH" == "x86_64" ]]; then
  RUN_UNDER_ROSETTA=1
elif [[ "$HOST_ARCH" != "$ARCH" ]]; then
  echo "Cannot build $ARCH on $HOST_ARCH. Build it on a matching Mac." >&2
  exit 1
fi

run_target() {
  if [[ "$RUN_UNDER_ROSETTA" == 1 ]]; then
    arch -x86_64 "$@"
  else
    "$@"
  fi
}

if [[ ! -x "$BREW_PREFIX/bin/brew" ]]; then
  echo "Missing $ARCH Homebrew: $BREW_PREFIX/bin/brew" >&2
  echo "Install the dependencies under that architecture, then run again." >&2
  exit 1
fi
if [[ ! -x "$BREW_PREFIX/bin/ffmpeg" || ! -x "$BREW_PREFIX/bin/ffprobe" ]]; then
  echo "Install ffmpeg in $BREW_PREFIX first." >&2
  exit 1
fi

PYTHON_BIN="${TPLAY_PYTHON:-$BREW_PREFIX/bin/python3}"
if [[ ! -x "$PYTHON_BIN" ]]; then
  echo "Missing Python for $ARCH: $PYTHON_BIN" >&2
  echo "Install it with: $BREW_PREFIX/bin/brew install python" >&2
  exit 1
fi

BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-$ARCH}"
case "$ARCH" in
  arm64) ESSENTIA_PREFIX="${TPLAY_ESSENTIA_PREFIX:-$ROOT_DIR/external/essentia-install}" ;;
  x86_64) ESSENTIA_PREFIX="${TPLAY_ESSENTIA_PREFIX:-$ROOT_DIR/external/essentia-install-x86_64}" ;;
esac
export PATH="$BREW_PREFIX/bin:$PATH"
export PKG_CONFIG_PATH="$BREW_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

run_target "$ROOT_DIR/scripts/prepare_demucs_rs.sh"
run_target cmake -S "$ROOT_DIR" -B "$BUILD_DIR" --fresh \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
  -DTPLAY_ESSENTIA_PREFIX="$ESSENTIA_PREFIX" \
  -DCMAKE_PREFIX_PATH="$BREW_PREFIX"
run_target cmake --build "$BUILD_DIR" --target tmplay -j"$JOBS"

PYTHON_ENV="$BUILD_DIR/tmplay-ytmusic-venv"
run_target "$PYTHON_BIN" -m venv "$PYTHON_ENV"
run_target "$PYTHON_ENV/bin/python" -m pip install --upgrade \
  --disable-pip-version-check ytmusicapi pyinstaller
run_target "$PYTHON_ENV/bin/python" -m PyInstaller \
  --noconfirm --clean --onedir --name tmplay-ytmusic \
  --collect-data ytmusicapi \
  --distpath "$BUILD_DIR/ytmusic-dist" \
  --workpath "$BUILD_DIR/pyinstaller-work" \
  --specpath "$BUILD_DIR/pyinstaller-spec" \
  "$ROOT_DIR/scripts/ytmusic_bridge.py"

lipo "$BUILD_DIR/ytmusic-dist/tmplay-ytmusic/tmplay-ytmusic" -verify_arch "$ARCH"
run_target "$ROOT_DIR/scripts/package_macos.sh" "$BUILD_DIR" "$ARCH" "$BREW_PREFIX"

echo "Completed standalone $ARCH bundle."
