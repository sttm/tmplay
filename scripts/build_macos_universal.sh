#!/usr/bin/env bash
set -euo pipefail

# Build one Universal 2 application from two fully native bundles.  This is
# intentionally different from compiling CMake with a single universal flag:
# Homebrew packages are normally single-architecture, whereas this method
# builds every dependency with its native Homebrew installation first, then
# joins matching Mach-O files with lipo.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_NAME="${TPLAY_APP_NAME:-tmplay}"
ARM_BUILD_DIR="${ARM_BUILD_DIR:-$ROOT_DIR/build-arm64}"
INTEL_BUILD_DIR="${INTEL_BUILD_DIR:-$ROOT_DIR/build-x86_64}"
DIST_ROOT="$ROOT_DIR/dist"
ARM_APP="$DIST_ROOT/$APP_NAME-arm64/$APP_NAME.app"
INTEL_APP="$DIST_ROOT/$APP_NAME-x86_64/$APP_NAME.app"
UNIVERSAL_DIR="$DIST_ROOT/$APP_NAME-universal2"
UNIVERSAL_APP="$UNIVERSAL_DIR/$APP_NAME.app"
PY_RUNTIME_REL="Contents/Resources/ytmusic-runtime"
FRAMEWORKS_REL="Contents/Frameworks"

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "Universal macOS packaging requires Darwin" >&2
  exit 1
fi

if [[ "${SKIP_ARCH_BUILDS:-OFF}" != "ON" ]]; then
  BUILD_DIR="$ARM_BUILD_DIR" "$ROOT_DIR/scripts/build_macos_silicon.sh"
  BUILD_DIR="$INTEL_BUILD_DIR" "$ROOT_DIR/scripts/build_macos_intel.sh"
fi

for app in "$ARM_APP" "$INTEL_APP"; do
  if [[ ! -d "$app" ]]; then
    echo "Missing architecture bundle: $app" >&2
    echo "Build both arm64 and x86_64 variants first, or omit SKIP_ARCH_BUILDS." >&2
    exit 1
  fi
done

TEMP_DIR="$(mktemp -d "$ROOT_DIR/.universal-merge.XXXXXX")"
trap 'rm -rf "$TEMP_DIR"' EXIT
ARM_MERGE_APP="$TEMP_DIR/${APP_NAME}-arm64.app"
INTEL_MERGE_APP="$TEMP_DIR/${APP_NAME}-x86_64.app"
ditto "$ARM_APP" "$ARM_MERGE_APP"
ditto "$INTEL_APP" "$INTEL_MERGE_APP"

is_macho() {
  file -b "$1" | grep -q "Mach-O"
}

rewrite_framework_directory() {
  local app="$1"
  local arch="$2"
  local executable dependency replacement
  while IFS= read -r -d '' executable; do
    is_macho "$executable" || continue
    while IFS= read -r dependency; do
      [[ "$dependency" == @executable_path/*/Frameworks/* ]] || continue
      replacement="${dependency%/Frameworks/*}/Frameworks-${arch}/${dependency##*/}"
      install_name_tool -change "$dependency" "$replacement" "$executable"
    # A fat executable has a second, non-indented architecture header.
    # Only indented lines are actual dynamic-library dependencies.
    done < <(otool -L "$executable" | awk '/^[[:space:]]/ { print $1 }')
  done < <(find "$app" -type f -print0)
}

# The two Homebrew installations may legitimately have different dependency
# versions.  Keep those dylibs in architecture-specific Frameworks folders;
# each thin executable is rewritten before it becomes a Universal 2 binary.
rewrite_framework_directory "$ARM_MERGE_APP" arm64
rewrite_framework_directory "$INTEL_MERGE_APP" x86_64

rm -rf "$UNIVERSAL_DIR"
mkdir -p "$UNIVERSAL_DIR"
ditto "$ARM_MERGE_APP" "$UNIVERSAL_APP"
mv "$UNIVERSAL_APP/$FRAMEWORKS_REL" "$UNIVERSAL_APP/${FRAMEWORKS_REL}-arm64"
ditto "$INTEL_MERGE_APP/$FRAMEWORKS_REL" "$UNIVERSAL_APP/${FRAMEWORKS_REL}-x86_64"

# PyInstaller embeds the minor Python version in both paths and extension
# names.  Homebrew can provide different minor versions for arm64 and x86_64,
# so keep the two self-contained runtimes separate and select one at launch.
mv "$UNIVERSAL_APP/$PY_RUNTIME_REL" "$UNIVERSAL_APP/${PY_RUNTIME_REL}-arm64"
ditto "$INTEL_MERGE_APP/$PY_RUNTIME_REL" "$UNIVERSAL_APP/${PY_RUNTIME_REL}-x86_64"
cp "$ROOT_DIR/resources/tplay-ytmusic-universal-wrapper" \
   "$UNIVERSAL_APP/Contents/MacOS/tools/tmplay-ytmusic"
chmod 0755 "$UNIVERSAL_APP/Contents/MacOS/tools/tmplay-ytmusic"

merge_macho() {
  local arm_file="$1"
  local intel_file="$2"
  local output_file="$3"
  local arm_slice="$TEMP_DIR/arm64"
  local intel_slice="$TEMP_DIR/x86_64"

  rm -f "$arm_slice" "$intel_slice"

  if ! lipo "$arm_file" -verify_arch arm64 >/dev/null 2>&1; then
    echo "Missing arm64 slice: $arm_file" >&2
    exit 1
  fi
  if ! lipo "$intel_file" -verify_arch x86_64 >/dev/null 2>&1; then
    echo "Missing x86_64 slice: $intel_file" >&2
    exit 1
  fi

  # Most artifacts are already thin.  `lipo -extract` rejects a thin file,
  # while `lipo -thin` is needed only for a pre-existing universal helper
  # (such as an official yt-dlp binary).  Normalize each input before merge.
  if lipo -detailed_info "$arm_file" 2>/dev/null | grep -q "Non-fat"; then
    cp "$arm_file" "$arm_slice"
  else
    lipo "$arm_file" -thin arm64 -output "$arm_slice"
  fi
  if lipo -detailed_info "$intel_file" 2>/dev/null | grep -q "Non-fat"; then
    cp "$intel_file" "$intel_slice"
  else
    lipo "$intel_file" -thin x86_64 -output "$intel_slice"
  fi
  lipo -create "$arm_slice" "$intel_slice" -output "$output_file"
  lipo "$output_file" -verify_arch arm64 x86_64
}

while IFS= read -r -d '' arm_file; do
  relative_path="${arm_file#"$ARM_MERGE_APP"/}"
  intel_file="$INTEL_MERGE_APP/$relative_path"
  output_file="$UNIVERSAL_APP/$relative_path"

  # Handled as separate, architecture-specific PyInstaller runtimes above.
  [[ "$relative_path" == "$PY_RUNTIME_REL/"* ]] && continue
  # Homebrew dylib versions may differ; their architecture-specific folders
  # were copied above and thin executables point at the matching folder.
  [[ "$relative_path" == "$FRAMEWORKS_REL/"* ]] && continue

  if [[ ! -f "$intel_file" ]]; then
    echo "Intel bundle is missing file: $relative_path" >&2
    exit 1
  fi

  if is_macho "$arm_file"; then
    if ! is_macho "$intel_file"; then
      echo "Architecture mismatch for: $relative_path" >&2
      exit 1
    fi
    merge_macho "$arm_file" "$intel_file" "$output_file"
  fi
done < <(find "$ARM_MERGE_APP" -type f -print0)

while IFS= read -r -d '' artifact; do
  is_macho "$artifact" || continue
  case "$artifact" in
    *"/${PY_RUNTIME_REL}-arm64/"*) lipo "$artifact" -verify_arch arm64 ;;
    *"/${PY_RUNTIME_REL}-x86_64/"*) lipo "$artifact" -verify_arch x86_64 ;;
    *"/${FRAMEWORKS_REL}-arm64/"*) lipo "$artifact" -verify_arch arm64 ;;
    *"/${FRAMEWORKS_REL}-x86_64/"*) lipo "$artifact" -verify_arch x86_64 ;;
    *) lipo "$artifact" -verify_arch arm64 x86_64 ;;
  esac
done < <(find "$UNIVERSAL_APP" -type f -print0)

codesign --force --deep --sign - "$UNIVERSAL_APP" >/dev/null
codesign --verify --deep --strict "$UNIVERSAL_APP"

(
  cd "$DIST_ROOT"
  rm -f "$APP_NAME-universal2-macos.zip"
  zip -qry "$APP_NAME-universal2-macos.zip" "$APP_NAME-universal2"
)

echo "Created universal application: $UNIVERSAL_APP"
echo "Archive: $DIST_ROOT/$APP_NAME-universal2-macos.zip"
