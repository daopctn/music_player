# JARVIS Audio — Build Plan

A terminal music player in C++. Audio-only YouTube playback, no video, no ads.
Built as an ownable, extensible module — not a throwaway script.

---

## Architecture

Three layers, kept strictly separate so any one can be swapped without touching
the others. **This boundary is the whole game.** Get it right in Phase 1 and the
rest is additive.

| Layer | Responsibility | Depends on |
|-------|---------------|------------|
| **Resolver** | Wraps the `yt-dlp` subprocess. In: search string / URL. Out: `vector<Track>`. The *only* Python dependency, quarantined behind one class. | yt-dlp |
| **Engine** | Wraps `libmpv`. Owns the playlist, playback state, volume, seek. Knows nothing about the terminal or about searching. | libmpv |
| **UI** | Reads commands, renders state. Starts as a getline REPL, ends as FTXUI. Talks only to Engine + Resolver — never to mpv or yt-dlp directly. | Engine, Resolver |

> **Key constraint:** there is no good native C++ library that resolves YouTube
> streams. yt-dlp (Python) is the only serious option. C++ does not eliminate
> that dependency — it moves it behind a `popen`. Accept this up front.

---

## Phase 0 — Environment & skeleton  *(½ day)*

**Goal:** an empty binary that compiles, links `libmpv`, and exits clean.

- Use **CMake** (standard for non-Qt C++; clean `find_package` for mpv).
- Verify toolchain: `libmpv-dev`, `yt-dlp` on PATH.
- Stub the three classes with empty methods. Compile.

**Milestone:** `./jarvis` runs and quits. Proves the link works before any logic.

---

## Phase 1 — Resolver, in isolation  *(1 day)*

**Goal:** type a query, get a clean list of tracks printed. **No playback yet.**

- `popen("yt-dlp ...")`, capture stdout, parse.
- **Decision — drop JSON:** instead of nlohmann, ask yt-dlp for line-delimited
  fields:
  `yt-dlp --flat-playlist --print "%(title)s\t%(id)s\t%(duration)s" ytsearch8:<q>`
  Split on tabs. Zero JSON lib, cleaner binary.
- Handle failure cases now while cheap: yt-dlp missing, no network, zero results,
  unicode titles.

**Milestone:** search works and is robust. This is the riskiest external
dependency — de-risk it first.

---

## Phase 2 — Engine + minimal REPL  *(1 day)*

**Goal:** queue a result and hear it. The MVP — usable end to end.

- libmpv init: `vid=no`, `ytdl=yes`, `ytdl-format=bestaudio/best`.
- `loadfile <url> append-play` — let mpv own the playlist.
- Commands: search → number → play, plus `p n b ls np v +/- x`.
- **Single-threaded.** libmpv plays autonomously; query state on demand. No event
  pump yet.

**Milestone:** *stopping point if you just want a working tool.* Everything past
here is polish.

---

## Phase 3 — Robustness pass  *(1 day)*

**Goal:** doesn't embarrass itself when things go wrong.

- Stream URL expiry — resolve at play time, not search time (mpv's `ytdl=yes`
  re-resolves on load, so this is mostly free).
- Dead / age-restricted / region-locked videos: skip gracefully, don't crash the
  queue.
- Ctrl-C → clean shutdown (`mpv_terminate_destroy`).
- Empty queue, seeking past end, volume bounds.

**Milestone:** hand it to a friend without a README of caveats.

---

## Phase 4 — FTXUI cockpit  *(2–3 days)*

**Goal:** the part that makes it feel like JARVIS instead of a shell.

- FTXUI owns the render loop — feed it Engine state, it redraws. The
  input-while-playing problem dissolves: no more printing over the prompt.
- Layout: search-results pane (top), queue pane (bottom), now-playing bar with a
  live progress meter.
- **The one real concurrency decision:** progress bar updates ~1×/sec → a
  background thread posts a refresh event to the UI loop. This is the *single*
  threading touchpoint in the whole app. FTXUI gives the safe primitive
  (`ScreenInteractive::PostEvent`) — no hand-rolled mutexes. Keep threads
  contained here and nowhere else.

**Milestone:** two-pane TUI with live progress bar. The "done" version.

---

## Phase 5 — JARVIS extras  *(open-ended, pick what you want)*

Now it's a platform, not a tool.

- **SponsorBlock** — skip in-song sponsor segments via yt-dlp chapter data.
- **Persistent queue / playlists** — save to file, reload on launch.
- **MQTT hook** — tie into the ESP32 + WS2812 rig so the LED strip pulses to
  play/pause state. The homelab angle.
- **Local cache** — optionally download bestaudio so replays are instant /
  offline.

---

## Critical path

Everything is gated by **Phase 1**. If yt-dlp resolution is solid, the rest is
well-trodden libmpv + FTXUI work with no real unknowns.

```
Phase 1 (resolver)  ──►  Phase 2 (engine + REPL = MVP)  ──►  Phase 4 (TUI = done)
     ▲ de-risk here          ▲ ~2.5 days in                    ▲ ~1 week of evenings
```

**The one rule:** don't build the TUI first. It's the fun part and the temptation
is real — but if the resolver/engine isn't clean underneath, the TUI just makes
the cracks prettier.

---

## Dependencies

```bash
sudo apt install libmpv-dev yt-dlp cmake g++
# FTXUI added at Phase 4 (FetchContent in CMake, no system install needed)
```

## Suggested layout

```
jarvis-audio/
├── CMakeLists.txt
├── README.md
└── src/
    ├── Track.hpp      # shared data type
    ├── Resolver.hpp   # yt-dlp boundary
    ├── Resolver.cpp
    ├── Engine.hpp     # libmpv boundary
    ├── Engine.cpp
    └── main.cpp       # UI layer
```
