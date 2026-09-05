#!/usr/bin/env bash
set -euo pipefail

bundle_root="$(cd "$(dirname "$0")/../.." && pwd)"
exec "$bundle_root/Contents/MacOS/tmplay" "$@"
