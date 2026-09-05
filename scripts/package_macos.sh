#!/usr/bin/env bash
set -euo pipefail

# Create a relocatable macOS .app. The result contains every non-system
# dylib, ffmpeg/ffprobe, yt-dlp, the recorder, Demucs, models and the compiled
# YouTube Music bridge. Nothing from Homebrew is needed at runtime.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-"$ROOT_DIR/build"}"
ARCH="${2:-$(uname -m)}"
BREW_PREFIX="${3:-${TPLAY_BREW_PREFIX:-}}"
APP_NAME="tmplay"
DIST_ROOT="$ROOT_DIR/dist"
PACKAGE_NAME="$APP_NAME-$ARCH"
PACKAGE_DIR="$DIST_ROOT/$PACKAGE_NAME"
APP_DIR="$PACKAGE_DIR/$APP_NAME.app"
CONTENTS_DIR="$APP_DIR/Contents"
MACOS_DIR="$CONTENTS_DIR/MacOS"
TOOLS_DIR="$MACOS_DIR/tools"
FRAMEWORKS_DIR="$CONTENTS_DIR/Frameworks"
RESOURCES_DIR="$CONTENTS_DIR/Resources"

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "macOS packaging requires Darwin" >&2
  exit 1
fi

case "$ARCH" in
  arm64) default_prefix="/opt/homebrew" ;;
  x86_64) default_prefix="/usr/local" ;;
  universal2) default_prefix="/opt/homebrew" ;;
  *) echo "Unsupported macOS architecture: $ARCH" >&2; exit 1 ;;
esac
BREW_PREFIX="${BREW_PREFIX:-$default_prefix}"

require_executable() {
  local path="$1"
  local label="$2"
  if [[ ! -x "$path" ]]; then
    echo "Missing $label: $path" >&2
    exit 1
  fi
}

require_executable "$BUILD_DIR/tmplay" "tmplay executable"
require_executable "$BUILD_DIR/demucs" "Demucs companion"
require_executable "$BUILD_DIR/tmplay-recorder" "desktop recorder"
require_executable "$BUILD_DIR/ytmusic-dist/tmplay-ytmusic/tmplay-ytmusic" "self-contained YouTube Music bridge"
require_executable "$BREW_PREFIX/bin/ffmpeg" "ffmpeg for $ARCH"
require_executable "$BREW_PREFIX/bin/ffprobe" "ffprobe for $ARCH"

# The official macOS yt-dlp binary is self-updating with `yt-dlp -U`, unlike
# Homebrew's Python launcher. Allow a pinned/offline copy to be supplied.
YT_DLP_SOURCE="${YT_DLP_BINARY:-}"
if [[ -n "$YT_DLP_SOURCE" ]]; then
  require_executable "$YT_DLP_SOURCE" "YT_DLP_BINARY"
fi

rm -rf "$PACKAGE_DIR"
mkdir -p "$TOOLS_DIR" "$FRAMEWORKS_DIR" "$RESOURCES_DIR"

cp "$ROOT_DIR/resources/Info.plist" "$CONTENTS_DIR/Info.plist"
cp "$ROOT_DIR/resources/TPlayLauncher" "$MACOS_DIR/tmplay-launcher"
cp "$ROOT_DIR/resources/run.command" "$RESOURCES_DIR/run.command"
chmod 0755 "$MACOS_DIR/tmplay-launcher" "$RESOURCES_DIR/run.command"

cp "$BUILD_DIR/tmplay" "$MACOS_DIR/tmplay"
cp "$BUILD_DIR/config.toml" "$MACOS_DIR/config.toml"
mkdir -p "$MACOS_DIR/scripts" "$MACOS_DIR/models"
cp "$ROOT_DIR/scripts/ytmusic_bridge.py" "$MACOS_DIR/scripts/ytmusic_bridge.py"

for tool in demucs tmplay-recorder; do
  cp "$BUILD_DIR/$tool" "$TOOLS_DIR/$tool"
done
cp -R "$BUILD_DIR/ytmusic-dist/tmplay-ytmusic" "$RESOURCES_DIR/ytmusic-runtime"
cp "$ROOT_DIR/resources/tplay-ytmusic-wrapper" "$TOOLS_DIR/tmplay-ytmusic"
# PyInstaller copies package installation metadata too.  Its nested
# Info.plist makes macOS treat the directory as a second bundle and prevents
# signing TPlay.app; ytmusicapi itself does not use this metadata at runtime.
find "$RESOURCES_DIR/ytmusic-runtime/_internal" -maxdepth 1 -type d \
  \( -name '*.dist-info' -o -name '*.egg-info' \) -exec rm -rf {} +
cp "$BREW_PREFIX/bin/ffmpeg" "$TOOLS_DIR/ffmpeg"
cp "$BREW_PREFIX/bin/ffprobe" "$TOOLS_DIR/ffprobe"

if [[ -n "$YT_DLP_SOURCE" ]]; then
  cp "$YT_DLP_SOURCE" "$TOOLS_DIR/yt-dlp-bin"
else
  curl --fail --location --retry 3 --silent --show-error \
    "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_macos" \
    --output "$TOOLS_DIR/yt-dlp-bin"
fi
cp "$ROOT_DIR/resources/yt-dlp-wrapper" "$TOOLS_DIR/yt-dlp"
chmod 0755 "$TOOLS_DIR/yt-dlp-bin" "$TOOLS_DIR/yt-dlp" "$TOOLS_DIR/tmplay-ytmusic"

copy_model_if_present() {
  local name="$1"
  local source="$ROOT_DIR/models/$name"
  if [[ -f "$source" ]]; then
    cp "$source" "$MACOS_DIR/models/$name"
  fi
}

copy_model_if_present "htdemucs.safetensors"
for genre_model in genre_discogs400; do
  if [[ -d "$ROOT_DIR/models/$genre_model" ]]; then
    cp -R "$ROOT_DIR/models/$genre_model" "$MACOS_DIR/models/$genre_model"
  fi
done
cp "$ROOT_DIR/README.md" "$RESOURCES_DIR/README.txt"

is_system_library() {
  local path="$1"
  [[ "$path" == /usr/lib/* || "$path" == /System/Library/* ||
     "$path" == /Library/Apple/* ]]
}

resolve_dependency() {
  local dependency="$1"
  local owner="$2"
  local candidate=""
  if [[ -f "$dependency" ]]; then
    printf '%s\n' "$dependency"
    return 0
  fi
  case "$dependency" in
    @loader_path/*)
      candidate="$(dirname "$owner")/${dependency#@loader_path/}"
      [[ -f "$candidate" ]] && { printf '%s\n' "$candidate"; return 0; }
      ;;
    @executable_path/*)
      candidate="$MACOS_DIR/${dependency#@executable_path/}"
      [[ -f "$candidate" ]] && { printf '%s\n' "$candidate"; return 0; }
      ;;
    @rpath/*)
      while IFS= read -r rpath; do
        [[ -z "$rpath" ]] && continue
        rpath="${rpath/@loader_path/$(dirname "$owner")}"
        rpath="${rpath/@executable_path/$MACOS_DIR}"
        candidate="$rpath/${dependency#@rpath/}"
        [[ -f "$candidate" ]] && { printf '%s\n' "$candidate"; return 0; }
      done < <(otool -l "$owner" | awk '
        $1 == "cmd" && $2 == "LC_RPATH" { want = 1; next }
        want && $1 == "path" { print $2; want = 0 }
      ')
      ;;
  esac
  return 1
}

dylib_dependencies() {
  local owner="$1"
  local dependency
  while IFS= read -r dependency; do
    [[ -z "$dependency" ]] && continue
    is_system_library "$dependency" && continue
    resolve_dependency "$dependency" "$owner" || true
  # Dependency rows start with whitespace.  Filtering by `NR > 1` is wrong
  # for a Universal binary: otool prints another non-indented architecture
  # header which can make the binary appear to depend on itself.
  done < <(otool -L "$owner" | awk '/^[[:space:]]/ { print $1 }')
}

declare -a queue=()
declare -a copied=()

already_copied() {
  local name="$1"
  local item
  for item in "${copied[@]-}"; do
    [[ "$item" == "$name" ]] && return 0
  done
  return 1
}

declare -a machos=(
  "$MACOS_DIR/tmplay"
  "$TOOLS_DIR/demucs"
  "$TOOLS_DIR/tmplay-recorder"
  "$RESOURCES_DIR/ytmusic-runtime/tmplay-ytmusic"
  "$TOOLS_DIR/ffmpeg"
  "$TOOLS_DIR/ffprobe"
  "$TOOLS_DIR/yt-dlp-bin"
)

for binary in "${machos[@]}"; do
  file "$binary" | grep -q "Mach-O" || continue
  while IFS= read -r dependency; do
    queue+=("$dependency")
  done < <(dylib_dependencies "$binary")
done

while ((${#queue[@]})); do
  dependency="${queue[0]}"
  queue=("${queue[@]:1}")
  base="$(basename "$dependency")"
  target="$FRAMEWORKS_DIR/$base"
  already_copied "$base" && continue
  copied+=("$base")
  cp "$dependency" "$target"
  chmod u+w "$target"
  while IFS= read -r child; do
    queue+=("$child")
  done < <(dylib_dependencies "$target")
done

rewrite_dependencies() {
  local owner="$1"
  local prefix="$2"
  local dependency base
  while IFS= read -r dependency; do
    base="$(basename "$dependency")"
    [[ -f "$FRAMEWORKS_DIR/$base" ]] || continue
    install_name_tool -change "$dependency" "$prefix/$base" "$owner"
  done < <(otool -L "$owner" | awk '/^[[:space:]]/ { print $1 }')
}

rewrite_dependencies "$MACOS_DIR/tmplay" "@executable_path/../Frameworks"
for binary in "${machos[@]:1}"; do
  file "$binary" | grep -q "Mach-O" || continue
  if [[ "$binary" == "$RESOURCES_DIR/ytmusic-runtime/"* ]]; then
    rewrite_dependencies "$binary" "@executable_path/../../Frameworks"
  else
    rewrite_dependencies "$binary" "@executable_path/../../Frameworks"
  fi
done
for dylib in "$FRAMEWORKS_DIR"/*.dylib; do
  [[ -f "$dylib" ]] || continue
  install_name_tool -id "@rpath/$(basename "$dylib")" "$dylib"
  rewrite_dependencies "$dylib" "@loader_path"
done

verify_architecture() {
  local artifact="$1"
  if [[ "$ARCH" == "universal2" ]]; then
    lipo "$artifact" -verify_arch arm64 x86_64
  else
    lipo "$artifact" -verify_arch "$ARCH"
  fi
}

for artifact in "$MACOS_DIR/tmplay" "$TOOLS_DIR"/* \
                "$RESOURCES_DIR/ytmusic-runtime/tmplay-ytmusic" \
                "$FRAMEWORKS_DIR"/*.dylib; do
  [[ -f "$artifact" ]] || continue
  file "$artifact" | grep -q "Mach-O" || continue
  verify_architecture "$artifact"
done

codesign --force --deep --sign - "$APP_DIR" >/dev/null
codesign --verify --deep --strict "$APP_DIR"

(
  cd "$DIST_ROOT"
  rm -f "$PACKAGE_NAME-macos.zip"
  zip -qry "$PACKAGE_NAME-macos.zip" "$PACKAGE_NAME"
)

echo "Created: $DIST_ROOT/$PACKAGE_NAME-macos.zip"
echo "Application: $APP_DIR"
echo "Run from Terminal: $APP_DIR/Contents/MacOS/tmplay"
