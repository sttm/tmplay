#!/usr/bin/env bash
set -euo pipefail

# Verify that a tmplay macOS bundle is self-contained without copying macOS
# system libraries. Run after package_macos.sh or build_macos_universal.sh.

APP_PATH="${1:-}"
if [[ -z "$APP_PATH" ]]; then
  echo "Usage: $0 /path/to/tmplay.app" >&2
  exit 64
fi

APP_PATH="$(cd "$APP_PATH" && pwd)"
if [[ ! -d "$APP_PATH/Contents" ]]; then
  echo "Not a macOS application bundle: $APP_PATH" >&2
  exit 64
fi

is_macho() {
  file -b "$1" | grep -q 'Mach-O'
}

failed=0
homebrew_references=0
while IFS= read -r -d '' item; do
  is_macho "$item" || continue
  while IFS= read -r dependency; do
    case "$dependency" in
      /opt/homebrew/*|/usr/local/*)
        printf 'External Homebrew dependency: %s -> %s\n' "$item" "$dependency" >&2
        homebrew_references=$((homebrew_references + 1))
        failed=1
        ;;
    esac
  done < <(otool -L "$item" | awk '/^[[:space:]]/ { print $1 }')
done < <(find "$APP_PATH" -type f -print0)

system_copies=0
while IFS= read -r -d '' library; do
  printf 'Bundled macOS system library: %s\n' "$library" >&2
  system_copies=$((system_copies + 1))
  failed=1
done < <(find "$APP_PATH/Contents" \( -path '*/Frameworks/*' -o -path '*/Frameworks-arm64/*' -o -path '*/Frameworks-x86_64/*' \) \
  -type f \( -name 'libSystem.B.dylib' -o -name 'libobjc.A.dylib' -o -name 'libc++.1.dylib' \
             -o -name 'libz.1.dylib' -o -name 'libsqlite3.dylib' \) -print0)

if (( failed )); then
  echo "Bundle audit failed: ${homebrew_references} Homebrew reference(s), ${system_copies} copied system library/libraries." >&2
  exit 1
fi

echo "Bundle audit passed: no Homebrew references and no copied macOS system libraries."
echo "macOS system frameworks and /usr/lib libraries remain dynamically linked, as intended."
du -sh "$APP_PATH"
