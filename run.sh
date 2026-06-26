#!/usr/bin/env bash
# One-command launcher: configure (first run) -> build -> run.
#   ./run.sh            play
#   JARVIS_COOKIES_BROWSER=chrome ./run.sh   personalize via your YouTube login
set -euo pipefail

cd "$(dirname "$0")"
BUILD_DIR=build

# Friendly check for the runtime tools the app shells out to.
missing=()
for bin in cmake mpv yt-dlp curl; do
  command -v "$bin" >/dev/null 2>&1 || missing+=("$bin")
done
if ((${#missing[@]})); then
  echo "Missing required tool(s): ${missing[*]}" >&2
  echo "Install them, then re-run ./run.sh" >&2
  exit 1
fi

# Configure once; CMake's FetchContent pulls FTXUI on the first configure.
if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
fi

# Incremental build (no-op when nothing changed).
cmake --build "$BUILD_DIR" --parallel

exec "$BUILD_DIR/jarvis" "$@"
