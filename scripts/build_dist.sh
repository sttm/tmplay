#!/usr/bin/env bash
set -euo pipefail

# Kept as the historical universal-distribution entry point.
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/build_macos_universal.sh" "$@"
