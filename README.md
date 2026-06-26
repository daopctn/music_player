# JARVIS Audio

A terminal music player in C++. Audio-only YouTube playback — no video, no ads.

Three strictly-separated layers so any one can be swapped without touching the
others:

| Layer | File | Wraps |
|-------|------|-------|
| **Resolver** | `src/Resolver.{hpp,cpp}` | the `yt-dlp` subprocess — query in, `vector<Track>` out |
| **Engine** | `src/Engine.{hpp,cpp}` | `libmpv` — owns the playlist, playback, volume, seek |
| **UI** | `src/main.cpp` | FTXUI render loop — talks only to Engine + Resolver |

## Dependencies

```bash
sudo apt install libmpv-dev yt-dlp cmake g++
```

FTXUI is fetched automatically by CMake (`FetchContent`) — no system install.
`yt-dlp` must be on `PATH` at runtime.

## Build

```bash
cmake -B build
cmake --build build -j
```

Produces `build/jarvis`.

## Run

```bash
./build/jarvis
```

### Keys

| Key | Action |
|-----|--------|
| `/` | focus the search box |
| `Enter` (in search) | run the search |
| `Esc` | leave the search box |
| `↑/↓` or `j/k` | move through results |
| `g` / `G` | jump to first / last result |
| `Enter` (on a result) | add it to the queue |
| `Tab` | switch focus between results and queue |
| `Enter` (in queue) | jump to & play that entry |
| `d` / `Delete` (in queue) | remove the selected entry |
| `J` / `K` (in queue) | move the selected entry down / up |
| `space` | play / pause |
| `n` / `b` | next / previous track |
| `←` / `→` | seek ∓5 s |
| `+` / `-` | volume up / down |
| `s` | shuffle queue |
| `r` | toggle repeat (loop playlist) |
| `x` | stop & clear queue |
| `q` | quit |

The control bar icons (⏮ ⏯ ⏭ ⏹ 🔉 🔊 🔀 🔁) are clickable with the mouse.

## Connect your YouTube account

There's no OAuth login — yt-dlp (and mpv) authenticate with your browser
cookies. Set the browser once and relaunch:

```bash
export JARVIS_COOKIES_BROWSER=firefox   # or chrome, chromium, brave, edge
./build/jarvis
```

You can target a profile: `firefox:Default`, `chrome:Profile 1`, etc.

This unlocks personal/Premium streams and library sources:

| Key | Source |
|-----|--------|
| `L` | your liked songs (`:ytfav`) |
| `W` | watch later (`:ytwatchlater`) |

The header shows `yt: <browser>` when configured, `yt: anon` otherwise.

Notes: close the browser first — `--cookies-from-browser` locks the cookie DB.
Cookies grant full account access from disk; YouTube may occasionally bot-check
authenticated requests.

## Album art

Art is fetched from `i.ytimg.com` and rendered as a **real image via the Kitty
graphics protocol** (unicode-placeholder mode) when running under ghostty or
kitty — detected from `$TERM_PROGRAM` / `$KITTY_WINDOW_ID` / `$TERM`. On any
other terminal it falls back to half-block (`▀`) color art. Force the fallback
with `JARVIS_NO_KITTY=1`.

## Design notes

- **No JSON library.** `yt-dlp --print` is asked for tab-separated fields and we
  split on the tab. (Note: `--print` does *not* interpret `\t` escapes, so a
  literal TAB byte is embedded in the template.)
- **Stream-URL expiry is a non-issue.** Only the `youtube.com/watch?v=ID` URL is
  stored; mpv's `ytdl=yes` re-resolves the real stream at load time.
- **Threading is contained.** Exactly two background threads: a ~4 Hz ticker that
  posts a refresh event so the progress bar animates, and a detached worker per
  search so the UI never blocks on `yt-dlp`. All UI mutation happens on the FTXUI
  loop via `ScreenInteractive::PostEvent`.
