#!/usr/bin/env bash
set -euo pipefail

# Creates the compact source archive intended for a GitHub Release. The
# .gitattributes export-ignore rules omit tests, local builds, models and
# transient files while retaining CMake, source, resources and build scripts.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="${1:-source}"
OUTPUT_DIR="${2:-$ROOT_DIR/dist}"
ARCHIVE="$OUTPUT_DIR/tmplay-${VERSION}-source.zip"

cd "$ROOT_DIR"
git diff --quiet || {
  echo "Refusing to archive uncommitted changes. Commit the release first." >&2
  exit 1
}
git diff --cached --quiet || {
  echo "Refusing to archive staged but uncommitted changes. Commit the release first." >&2
  exit 1
}

mkdir -p "$OUTPUT_DIR"
git archive --format=zip --prefix="tmplay-${VERSION}/" --output="$ARCHIVE" HEAD
echo "Created GitHub source archive: $ARCHIVE"
