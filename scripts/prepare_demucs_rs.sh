#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEMUCS_DIR="$ROOT_DIR/external/demucs-rs"
REVISION="5d9f61a"

if [[ ! -d "$DEMUCS_DIR/.git" ]]; then
  git clone https://github.com/nikhilunni/demucs-rs.git "$DEMUCS_DIR"
fi

if git -C "$DEMUCS_DIR" apply --reverse --check \
  "$ROOT_DIR/patches/demucs-rs-local-model.patch"; then
  echo "demucs-rs local-model patch is already applied."
  exit 0
fi

if ! git -C "$DEMUCS_DIR" diff --quiet -- demucs-cli/src/main.rs; then
  echo "demucs-rs has local changes; refusing to overwrite them" >&2
  exit 1
fi

git -C "$DEMUCS_DIR" fetch --depth 1 origin "$REVISION"
git -C "$DEMUCS_DIR" checkout --detach "$REVISION"
git -C "$DEMUCS_DIR" apply "$ROOT_DIR/patches/demucs-rs-local-model.patch"
echo "Prepared demucs-rs $REVISION with TPlay's local-model patch."
