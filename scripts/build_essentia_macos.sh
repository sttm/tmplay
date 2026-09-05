#!/usr/bin/env bash
set -euo pipefail

# Build the C++ Essentia library into the project instead of /usr/local.  Each
# architecture gets its own prefix so arm64 and x86_64 builds can coexist and
# later be joined in the Universal 2 application.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARCH="${1:-$(uname -m)}"
SOURCE_DIR="$ROOT_DIR/external/essentia-src"

case "$ARCH" in
  arm64)
    BREW_PREFIX="${TPLAY_BREW_PREFIX:-/opt/homebrew}"
    INSTALL_DIR="${TPLAY_ESSENTIA_PREFIX:-$ROOT_DIR/external/essentia-install}"
    ;;
  x86_64)
    BREW_PREFIX="${TPLAY_BREW_PREFIX:-/usr/local}"
    INSTALL_DIR="${TPLAY_ESSENTIA_PREFIX:-$ROOT_DIR/external/essentia-install-x86_64}"
    ;;
  *)
    echo "Usage: $0 <arm64|x86_64>" >&2
    exit 1
    ;;
esac

PYTHON_BIN="${TPLAY_ESSENTIA_PYTHON:-$BREW_PREFIX/bin/python3}"
if [[ ! -x "$PYTHON_BIN" ]]; then
  echo "Missing Python for the $ARCH Essentia build: $PYTHON_BIN" >&2
  exit 1
fi
if [[ ! -d "$SOURCE_DIR/.git" ]]; then
  git clone --depth 1 --branch master \
    https://github.com/MTG/essentia.git "$SOURCE_DIR"
fi

run_target() {
  if [[ "$ARCH" == "x86_64" && "$(uname -m)" == "arm64" ]]; then
    arch -x86_64 "$@"
  else
    "$@"
  fi
}

export PATH="$BREW_PREFIX/bin:$PATH"
export PKG_CONFIG_PATH="$BREW_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export CFLAGS="${CFLAGS:-} -arch $ARCH"
export CXXFLAGS="${CXXFLAGS:-} -arch $ARCH"
export LDFLAGS="${LDFLAGS:-} -arch $ARCH"

# Do not build Python bindings, extractors or TensorFlow support: TPlay needs
# only Essentia's C++ analysis algorithms.  The resulting static archive still
# uses the native FFmpeg/FFTW/TagLib libraries from the matching Homebrew.
(
  cd "$SOURCE_DIR"
  run_target "$PYTHON_BIN" ./waf clean || true
  run_target "$PYTHON_BIN" ./waf configure \
    --mode=release --build-static --prefix="$INSTALL_DIR"
  run_target "$PYTHON_BIN" ./waf
  run_target "$PYTHON_BIN" ./waf install
)

LIBRARY="$INSTALL_DIR/lib/libessentia.a"
if [[ ! -f "$LIBRARY" ]]; then
  echo "Essentia install did not create $LIBRARY" >&2
  exit 1
fi
lipo "$LIBRARY" -verify_arch "$ARCH"
echo "Built Essentia ($ARCH): $INSTALL_DIR"
