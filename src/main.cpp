// JARVIS Audio — UI layer.
// Talks only to Engine + Resolver, never to mpv or yt-dlp directly.
//
// FTXUI owns the render loop. A single background thread posts a refresh
// event ~4x/sec so the progress bar animates — that is the ONE threading
// touchpoint in the whole app (plus the detached search worker).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include "Engine.hpp"
#include "Resolver.hpp"
#include "Thumbnail.hpp"
#include "Track.hpp"

using namespace ftxui;

// fmtTime() comes from util.hpp (shared with Track/Resolver).

namespace {
constexpr int kArtCols = 38;            // thumbnail width in terminal cells
constexpr int kArtColWidth = 44;        // art column width (frame + padding)
constexpr int kResultArt = 16;          // per-row thumbnail width in result list
constexpr int kResultPage = 5;          // rows per PageUp/Down / wheel jump
constexpr int kRadioRefillAt = 3;       // refill radio when fewer queued ahead
constexpr int kSuggestDebounceMs = 150; // typing pause before fetching suggests
constexpr int kTickMs = 250;            // progress-bar refresh interval
} // namespace

int main() {
    Resolver resolver;
    Engine engine;

    auto screen = ScreenInteractive::Fullscreen();

    // ---- shared UI state ----
    std::string query;
    std::string status = "Press '/' to search.  Enter on a result to queue it.";

    std::mutex resultsMu;
    std::vector<Track> results;          // guarded by resultsMu
    std::vector<std::string> resultLabels;
    int resultSelected = 0;
    std::atomic<bool> searching{false};

    Box resultsBox;    // screen rect of the results list (for wheel hit-test)
    Box queueBox;      // screen rect of the queue list
    int queueHover = -1; // queue row under the mouse, -1 = none

    // account auth probe: 0 = checking, 1 = authorized, 2 = not signed in
    std::atomic<int> authState{0};

    // autoplay (radio): refill the queue with related tracks when it runs low
    bool autoplay = true;
    std::atomic<bool> radioFetching{false};
    std::mutex radioMu;
    std::vector<Track> radioPending;   // guarded by radioMu
    std::string lastSeedId;            // last track id seen playing (persists)
    std::string radioAttempt;          // seed we've already fetched radio for

    std::vector<std::string> queueLabels; // rebuilt each frame, no lock needed
    int queueSelected = 0;

    // results pane is hidden until a full search runs (Enter without a pick)
    bool showResults = false;
    std::atomic<bool> wantPlayTop{false}; // play top hit after the next search

    // autocomplete suggestions while typing
    using clock = std::chrono::steady_clock;
    clock::time_point lastEdit;
    bool suggestDirty = false;
    std::atomic<bool> suggesting{false};
    std::mutex sugMu;
    std::vector<std::string> sugPending;       // guarded by sugMu
    std::vector<std::string> suggestions;       // shown in the dropdown
    std::string suggestFor;                     // query the list matches
    int sugSelected = -1;                       // -1 = nothing chosen yet
    bool suggestNav = false; // vim "normal" mode: navigating the dropdown
    bool searchActive = false; // INSERT mode: keys routed to the search field

    // Publish a finished fetch into the results pane: swap in the tracks,
    // rebuild the menu labels, reset selection, set status, then redraw.
    // Shared tail of every search/library worker (runs on the worker thread).
    auto publishResults = [&](std::vector<Track>&& found, std::string msg) {
        {
            std::lock_guard<std::mutex> lk(resultsMu);
            results = std::move(found);
            resultLabels.clear();
            for (const auto& t : results)
                resultLabels.push_back("[" + t.durationStr() + "] " + t.title);
            resultSelected = 0;
        }
        status = std::move(msg);
        searching = false;
        screen.PostEvent(Event::Custom);
    };

    // playTop=true: queue the top hit when the search returns (suggestion pick)
    auto launchSearch = [&](const std::string& q, bool playTop) {
        if (q.empty()) return;
        if (searching.exchange(true)) return; // one search at a time
        wantPlayTop = playTop;
        status = "Searching: " + q + " …";
        std::thread([&, q] {
            std::vector<Track> found;
            std::string msg;
            try {
                found = resolver.search(q);
                msg = found.empty() ? "No results for: " + q
                                    : std::to_string(found.size()) +
                                          " results for: " + q;
            } catch (const std::exception& e) {
                msg = std::string("Search failed: ") + e.what();
            }
            publishResults(std::move(found), std::move(msg));
        }).detach();
    };

    // Load a personal library source into the results view. The caller passes
    // a fetcher (resolver.likedSongs / homeFeed / …) so yt-dlp alias tokens
    // stay inside Resolver.
    auto launchLibrary = [&](std::function<std::vector<Track>()> fetch,
                             const std::string& label) {
        if (searching.exchange(true)) return;
        showResults = true;
        status = "Loading " + label + " …";
        std::thread([&, fetch, label] {
            std::vector<Track> found;
            std::string msg;
            try {
                found = fetch();
                msg = found.empty()
                          ? label + " empty (need cookies? set "
                                    "JARVIS_COOKIES_BROWSER)"
                          : std::to_string(found.size()) + " in " + label;
            } catch (const std::exception& e) {
                msg = std::string("Library failed: ") + e.what();
            }
            publishResults(std::move(found), std::move(msg));
        }).detach();
    };

    // ---- search input (single line, autocomplete dropdown) ----
    InputOption inputOpt;
    inputOpt.placeholder = "search query…";
    inputOpt.multiline = false;
    inputOpt.on_change = [&] {
        lastEdit = clock::now();
        suggestDirty = true;
        sugSelected = -1;           // typing resets any highlighted suggestion
    };
    // on_enter handled in the global handler (needs suggestion context)
    auto input = Input(&query, inputOpt);

    // ---- results menu ----
    MenuOption resultsOpt;
    resultsOpt.on_enter = [&] {
        Track picked;
        bool have = false;
        {
            std::lock_guard<std::mutex> lk(resultsMu);
            if (resultSelected >= 0 &&
                resultSelected < static_cast<int>(results.size())) {
                picked = results[resultSelected];
                have = true;
            }
        }
        if (!have) return;
        // A Mix / playlist isn't a single video — open it: expand into the
        // results list so its songs can be browsed and queued.
        if (picked.playlist) {
            std::string url = picked.url();
            launchLibrary([&, url] { return resolver.library(url); },
                          "mix");
            return; // stay in the results view
        }
        engine.enqueue(picked);
        status = "Queued: " + picked.title;
        showResults = false; // collapse results back to the now-playing view
    };
    auto resultsMenu = Menu(&resultLabels, &resultSelected, resultsOpt);

    // ---- queue menu (editable: Enter plays, d removes, J/K reorder) ----
    MenuOption queueOpt;
    queueOpt.on_enter = [&] { engine.playIndex(queueSelected); };
    auto queueMenu = Menu(&queueLabels, &queueSelected, queueOpt);

    // ---- thumbnail cache (async, redraws when art lands) ----
    ThumbnailCache thumbs;
    thumbs.setRefresh([&] { screen.PostEvent(Event::Custom); });

    // probe whether the cookies actually authorize us (liked list needs login)
    if (!resolver.cookieBrowser().empty()) {
        std::thread([&] {
            bool ok = false;
            try { ok = !resolver.library(":ytfav", 1).empty(); } catch (...) {}
            authState = ok ? 1 : 2;
            screen.PostEvent(Event::Custom);
        }).detach();
        // open on the personalized YouTube home feed
        launchLibrary([&] { return resolver.homeFeed(); }, "home");
    }

    // ---- clickable icon control bar (each also has a keyboard shortcut) ----
    ButtonOption bopt = ButtonOption::Ascii();
    auto btnPrev = Button("‹ prev", [&] { engine.prev(); }, bopt);
    auto btnPlay = Button("play/pause", [&] { engine.togglePause(); }, bopt);
    auto btnNext = Button("next ›", [&] { engine.next(); }, bopt);
    auto btnStop = Button("stop", [&] { engine.stop(); }, bopt);
    auto btnVolDn = Button("vol −", [&] { engine.volumeDown(); }, bopt);
    auto btnVolUp = Button("vol +", [&] { engine.volumeUp(); }, bopt);
    auto btnShuf = Button("shuffle", [&] { engine.shuffle(); }, bopt);
    auto btnRep = Button("repeat", [&] { engine.toggleRepeat(); }, bopt);
    auto controls = Container::Horizontal({btnPrev, btnPlay, btnNext, btnStop,
                                           btnVolDn, btnVolUp, btnShuf, btnRep});

    // NOTE: `input` is intentionally NOT in this container — keeping it out of
    // the focus/navigation chain means arrowing to the top of a list can't
    // accidentally land in the search box. Typing is routed to it manually
    // while searchActive (see the handler below).
    auto layout = Container::Vertical({resultsMenu, queueMenu, controls});
    resultsMenu->TakeFocus();

    // Clamped vim j/k (and arrows) over a list of `n` rows. Returns true when
    // the event was a movement key (and updates `sel`), false otherwise.
    auto vimMove = [](const Event& e, int& sel, int n) -> bool {
        if (e == Event::Character('j') || e == Event::ArrowDown) {
            if (n) sel = std::min(sel + 1, n - 1);
            return true;
        }
        if (e == Event::Character('k') || e == Event::ArrowUp) {
            if (n) sel = std::max(sel - 1, 0);
            return true;
        }
        return false;
    };
    // Map a mouse position to a row index inside `box`, or -1 if outside / past
    // the last of `count` rows.
    auto hitRow = [](const Mouse& m, const Box& box, int count) -> int {
        if (m.x < box.x_min || m.x > box.x_max || m.y < box.y_min ||
            m.y > box.y_max)
            return -1;
        int idx = m.y - box.y_min;
        return (idx >= 0 && idx < count) ? idx : -1;
    };

    // ---- global hotkeys (active when the search box is NOT focused) ----
    auto root = CatchEvent(layout, [&](Event e) {
        if (e.is_mouse()) {
            const auto& m = e.mouse();
            // wheel over the results list moves the keyboard selection (cards
            // are multi-line, so a Y→row hit-test isn't reliable — drive the
            // highlight instead, which auto-scrolls via focus).
            bool overResults = m.x >= resultsBox.x_min &&
                               m.x <= resultsBox.x_max &&
                               m.y >= resultsBox.y_min && m.y <= resultsBox.y_max;
            if (showResults && overResults &&
                (m.button == Mouse::WheelUp || m.button == Mouse::WheelDown)) {
                std::lock_guard<std::mutex> lk(resultsMu);
                int n = (int)results.size();
                if (m.button == Mouse::WheelDown)
                    resultSelected = std::min(resultSelected + 1,
                                              std::max(0, n - 1));
                else
                    resultSelected = std::max(resultSelected - 1, 0);
                return true;
            }
            queueHover = hitRow(m, queueBox, (int)engine.queue().size());
            return false; // let the menu handle the click/scroll too
        }
        // run a full search for the current query and reveal the results
        auto runSearch = [&] {
            showResults = true;
            launchSearch(query, /*playTop=*/false);
            suggestions.clear(); sugSelected = -1;
            suggestNav = false; searchActive = false;
            resultsMenu->TakeFocus();
        };

        if (e == Event::Character('/') && !searchActive) {
            suggestNav = false; searchActive = true; return true;
        }

        // INSERT mode — keys routed straight to the search field
        if (searchActive) {
            if (e == Event::Escape) { // → NORMAL mode, keep the dropdown
                searchActive = false;
                if (!suggestions.empty()) {
                    suggestNav = true;
                    if (sugSelected < 0) sugSelected = 0;
                }
                return true;
            }
            if (e == Event::Return) { runSearch(); return true; }
            return input->OnEvent(e); // typing / editing
        }

        // NORMAL mode — vim navigation over the suggestion dropdown
        if (suggestNav) {
            if (e == Event::Character('i') || e == Event::Character('a')) {
                suggestNav = false; searchActive = true; return true;
            }
            if (e == Event::Character('j') || e == Event::ArrowDown) {
                sugSelected =
                    std::min(sugSelected + 1, (int)suggestions.size() - 1);
                return true;
            }
            if (e == Event::Character('k') || e == Event::ArrowUp) {
                sugSelected = std::max(sugSelected - 1, 0);
                return true;
            }
            if (e == Event::Return) {
                if (sugSelected >= 0 && sugSelected < (int)suggestions.size())
                    query = suggestions[sugSelected];
                runSearch();
                return true;
            }
            if (e == Event::Escape) {
                suggestions.clear(); sugSelected = -1; suggestNav = false;
                return true;
            }
            // other keys (transport) fall through
        }
        if (e == Event::Character('q')) { screen.Exit(); return true; }
        // Tab toggles focus between the results and queue panes
        if (e == Event::Tab || e == Event::TabReverse) {
            if (queueMenu->Focused()) resultsMenu->TakeFocus();
            else queueMenu->TakeFocus();
            return true;
        }
        if (e == Event::Escape) return true; // swallow: Esc must not quit

        // transport — works from any pane
        if (e == Event::Character(' ')) { engine.togglePause(); return true; }
        if (e == Event::Character('n')) { engine.next();        return true; }
        if (e == Event::Character('b')) { engine.prev();        return true; }
        if (e == Event::Character('x')) { engine.stop();        return true; }
        if (e == Event::Character('s')) { engine.shuffle();     return true; }
        if (e == Event::Character('r')) { engine.toggleRepeat(); return true; }
        if (e == Event::Character('a')) { autoplay = !autoplay; return true; }
        if (e == Event::Character('H')) { // YouTube home recommendations
            launchLibrary([&] { return resolver.homeFeed(); }, "home");
            resultsMenu->TakeFocus(); return true;
        }
        if (e == Event::Character('L')) { // your liked songs
            launchLibrary([&] { return resolver.likedSongs(); }, "liked songs");
            resultsMenu->TakeFocus(); return true;
        }
        if (e == Event::Character('W')) { // watch later
            launchLibrary([&] { return resolver.watchLater(); }, "watch later");
            resultsMenu->TakeFocus(); return true;
        }
        if (e == Event::Character('+') || e == Event::Character('=')) {
            engine.volumeUp(); return true;
        }
        if (e == Event::Character('-') || e == Event::Character('_')) {
            engine.volumeDown(); return true;
        }
        if (e == Event::ArrowLeft)  { engine.seekRelative(-5);  return true; }
        if (e == Event::ArrowRight) { engine.seekRelative(+5);  return true; }

        // queue editing (when the queue pane has focus). j/k are clamped to the
        // list so they can NEVER spill over and switch panes — only Tab does.
        if (queueMenu->Focused()) {
            int n = (int)engine.queue().size();
            if (e == Event::Character('d') || e == Event::Delete) {
                engine.removeAt(queueSelected);
                queueSelected = std::min(queueSelected, std::max(0, n - 2));
                return true;
            }
            if (e == Event::Character('J')) { // move entry down
                engine.moveDown(queueSelected);
                queueSelected = std::min(queueSelected + 1, std::max(0, n - 1));
                return true;
            }
            if (e == Event::Character('K')) { // move entry up
                engine.moveUp(queueSelected);
                queueSelected = std::max(queueSelected - 1, 0);
                return true;
            }
            if (vimMove(e, queueSelected, n)) return true;
            if (e == Event::Return) return queueMenu->OnEvent(e); // play entry
            return false;
        }

        // results pane: vim movement, clamped to the list (no pane switching)
        if (resultsMenu->Focused()) {
            std::lock_guard<std::mutex> lk(resultsMu);
            int n = (int)results.size();
            if (vimMove(e, resultSelected, n)) return true;
            if (e == Event::Character('g') || e == Event::Home) {
                resultSelected = 0; return true;
            }
            if (e == Event::Character('G') || e == Event::End) {
                resultSelected = std::max(0, n - 1); return true;
            }
            if (e == Event::PageDown) {
                resultSelected = std::min(resultSelected + kResultPage,
                                          std::max(0, n - 1));
                return true;
            }
            if (e == Event::PageUp) {
                resultSelected = std::max(resultSelected - kResultPage, 0);
                return true;
            }
        }
        return false;
    });

    // ---- renderer ----
    auto ui = Renderer(root, [&] {
        engine.pump();

        // ---- autocomplete: drain fetched suggestions, fetch on typing pause
        {
            std::lock_guard<std::mutex> lk(sugMu);
            if (suggestFor == query) suggestions = sugPending;
        }
        if (!searchActive && !suggestNav) {
            suggestions.clear();
            sugSelected = -1;
        } else if (searchActive && suggestDirty && !suggesting &&
                   !query.empty()) {
            auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(
                            clock::now() - lastEdit)
                            .count();
            if (idle >= kSuggestDebounceMs) {
                suggestDirty = false;
                suggesting = true;
                std::string q = query;
                std::thread([&, q] {
                    auto s = resolver.suggest(q);
                    {
                        std::lock_guard<std::mutex> lk(sugMu);
                        sugPending = std::move(s);
                        suggestFor = q;
                    }
                    suggesting = false;
                    screen.PostEvent(Event::Custom);
                }).detach();
            }
        }

        // play the top hit once a suggestion-pick search returns
        if (wantPlayTop && !searching) {
            std::lock_guard<std::mutex> lk(resultsMu);
            if (!results.empty()) {
                engine.enqueue(results[0]);
                status = "Playing: " + results[0].title;
            }
            wantPlayTop = false;
        }

        // ---- autoplay: drain fetched radio tracks, then refill if low ----
        {
            std::vector<Track> batch;
            {
                std::lock_guard<std::mutex> lk(radioMu);
                batch.swap(radioPending);
            }
            for (const auto& t : batch) {
                bool dup = t.id.empty();
                for (const auto& q : engine.queue())
                    if (q.id == t.id) { dup = true; break; }
                if (!dup) engine.appendAuto(t);
            }
        }
        // Snapshot mpv state once per frame — every engine getter is a blocking
        // IPC round-trip, so read each value once and reuse it below.
        const Track* cur = engine.current();
        int curPos = engine.playlistPos();
        bool isIdle = engine.idle();
        bool isPaused = engine.paused();
        int upcoming = (curPos < 0)
                           ? (int)engine.queue().size()
                           : (int)engine.queue().size() - 1 - curPos;

        // remember a seed even across the idle gap at end-of-queue
        if (cur && !cur->id.empty()) lastSeedId = cur->id;

        if (autoplay && !radioFetching && upcoming < kRadioRefillAt) {
            std::string seed = (cur && !cur->id.empty()) ? cur->id : lastSeedId;
            // one radio fetch per distinct seed → no tight refetch loop
            if (!seed.empty() && seed != radioAttempt) {
                radioAttempt = seed;
                radioFetching = true;
                std::thread([&, seed] {
                    std::vector<Track> r;
                    try { r = resolver.radio(seed); } catch (...) {}
                    {
                        std::lock_guard<std::mutex> lk(radioMu);
                        radioPending = std::move(r);
                    }
                    radioFetching = false;
                    screen.PostEvent(Event::Custom);
                }).detach();
            }
        }

        // now-playing bar
        double pos = engine.position();
        double dur = engine.duration();
        double ratio = (dur > 0 && pos >= 0) ? pos / dur : 0.0;
        std::string title = cur ? cur->title : "(nothing playing)";
        std::string state = isIdle ? "STOP" : (isPaused ? "PAUSE" : "PLAY");

        // queue labels (mark the currently-playing entry)
        queueLabels.clear();
        const auto& q = engine.queue();
        for (int i = 0; i < static_cast<int>(q.size()); ++i) {
            std::string marker = (i == curPos)        ? "♪ "
                                 : engine.isManual(i) ? "  "
                                                      : "~ "; // ~ = radio
            queueLabels.push_back(marker + q[i].title);
        }
        if (queueLabels.empty()) queueLabels.push_back("  (queue empty)");

        Color accent = Color::Cyan;
        Color stateColor = isIdle
                               ? Color::RedLight
                               : (isPaused ? Color::YellowLight
                                           : Color::GreenLight);

        // ---- header ----
        auto header = hbox({
            text(" JARVIS AUDIO ") | bold | color(Color::Black) |
                bgcolor(accent),
            text(" audio-only youtube ") | dim,
            filler(),
            (autoplay ? text(" radio on ") | color(Color::Black) |
                            bgcolor(Color::GreenLight)
                      : text(" radio off ") | dim),
            [&]() -> Element {
                if (resolver.cookieBrowser().empty())
                    return text(" yt: anon ") | dim;
                int a = authState.load();
                std::string b = resolver.cookieBrowser();
                if (a == 1)
                    return text(" yt: " + b + " ✓ ") | color(Color::Black) |
                           bgcolor(Color::GreenLight);
                if (a == 2)
                    return text(" yt: " + b + " ✗ not signed in ") |
                           color(Color::White) | bgcolor(Color::Red);
                return text(" yt: " + b + " checking… ") | dim;
            }(),
            text(thumbs.kitty() ? " kitty-gfx " : " ascii-art ") | dim |
                color(accent),
        });

        // ---- search ----
        std::string tag = searchActive ? " INSERT " : suggestNav ? " NORMAL "
                                                                  : "  search ";
        auto searchRow = hbox({
                             text(tag) | bold |
                                 (searchActive
                                      ? bgcolor(accent) | color(Color::Black)
                                      : color(accent)),
                             text(" "),
                             input->Render() | flex,
                         }) |
                         size(HEIGHT, EQUAL, 1) |
                         bgcolor(Color::RGB(28, 28, 34));

        // ---- browser: art column (left), results (right) ----
        std::string nowId = cur ? cur->id : "";
        auto nowArt = thumbs.get(nowId, kArtCols);

        // iPod-style: art, then track info + progress, then transport
        auto npInfo = vbox({
            text(title) | bold | center,
            hbox({
                text(" " + state + " ") | bold | color(Color::Black) |
                    bgcolor(stateColor),
                filler(),
                (engine.repeat() ? text("repeat ") | color(accent) : text("")),
                text("vol " + std::to_string(engine.volume()) + " "),
            }),
            hbox({
                text(fmtTime(pos) + " "),
                gauge(static_cast<float>(ratio)) | color(accent) | flex,
                text(" " + fmtTime(dur)),
            }),
        });
        auto transport = vbox({
            hbox({btnPrev->Render(), btnPlay->Render(), btnNext->Render()}) |
                center,
            hbox({btnVolDn->Render(), btnStop->Render(), btnVolUp->Render()}) |
                center,
            hbox({btnShuf->Render(), btnRep->Render()}) | center,
        });
        auto nowArtWin =
            window(text(" now playing ") | bold | color(stateColor),
                   vbox({
                       nowArt | center | flex,
                       separator(),
                       npInfo,
                       separator(),
                       transport,
                   })) |
            flex;
        // ---- results: one bordered card per item (thumbnail + title + time) ----
        Element resultsList;
        {
            std::lock_guard<std::mutex> lk(resultsMu);
            if (results.empty()) {
                resultsList = text(searching ? "searching…" : "no results") |
                              dim | center | flex;
            } else {
                Elements cards;
                for (int i = 0; i < (int)results.size(); ++i) {
                    const Track& t = results[i];
                    auto info = vbox({
                        hbox({
                            paragraph(t.title) | bold | flex,
                            text(" " + t.durationStr()) | color(accent),
                        }),
                        text(t.channel.empty() ? "unknown channel"
                                               : t.channel) |
                            dim,
                    });
                    Element card = hbox({
                                       thumbs.get(t.id, kResultArt),
                                       text(" "),
                                       info | flex,
                                   }) |
                                   border;
                    if (i == resultSelected)
                        card = card | bgcolor(Color::RGB(40, 44, 60)) |
                               color(accent) | focus;
                    cards.push_back(card);
                }
                resultsList = vbox(cards) | yframe | flex;
            }
        }
        auto resultsWin =
            window(text(" results ") | bold | color(accent),
                   resultsList | reflect(resultsBox)) |
            flex;

        auto queueTitle =
            queueMenu->Focused()
                ? text(" queue · ↵ play  d remove  J/K move ") | bold |
                      color(Color::Black) | bgcolor(accent)
                : text(" queue ") | bold | color(accent);
        auto queueMenuEl =
            queueMenu->Render() | vscroll_indicator | yframe | reflect(queueBox);

        // suggestion dropdown — visible while typing or navigating it
        bool showSuggest =
            (searchActive || suggestNav) && !suggestions.empty();
        Element suggestBox = filler();
        if (showSuggest) {
            Elements rows;
            for (int i = 0; i < (int)suggestions.size(); ++i) {
                auto r = text("  " + suggestions[i] + " ");
                if (i == sugSelected)
                    r = r | bold | color(Color::Black) | bgcolor(accent);
                rows.push_back(r);
            }
            const char* hint = suggestNav
                                   ? " suggestions [NORMAL] · j/k move · ↵ "
                                     "search · i edit · esc close "
                                   : " suggestions [INSERT] · esc to navigate · "
                                     "↵ search ";
            suggestBox = window(text(hint) | dim, vbox(rows)) |
                         size(HEIGHT, LESS_THAN, 11);
        }

        // art column (now playing + preview/coming-next) shown in both views
        auto artCol = nowArtWin | size(WIDTH, EQUAL, kArtColWidth);

        // body: results view (raw Enter) vs now-playing view
        Element body;
        if (showResults) {
            body = vbox({
                       hbox({artCol, resultsWin}) | flex,
                       window(queueTitle, queueMenuEl) |
                           size(HEIGHT, LESS_THAN, 9),
                   }) |
                   flex;
        } else {
            body = hbox({
                       artCol,
                       window(queueTitle, queueMenuEl) | flex,
                   }) |
                   flex;
        }

        std::string err = engine.lastError();
        auto footer = hbox({
            err.empty() ? text(" " + status) | dim | flex
                        : text(" playback error: " + err) |
                              color(Color::RedLight) | flex,
            text("/ search · tab pane · ↵ play/results · d remove · "
                 "J/K reorder · ␣ pause · n/b skip · ←/→ seek · "
                 "s shuffle · r repeat · a radio · H home · L liked · "
                 "W later · x stop · q quit ") |
                dim,
        });

        Elements children = {header, searchRow};
        if (showSuggest) children.push_back(suggestBox);
        children.push_back(body);
        children.push_back(footer);
        return vbox(children) | flex;
    });

    // ---- refresh ticker: the single progress-bar thread ----
    std::atomic<bool> running{true};
    std::thread ticker([&] {
        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kTickMs));
            screen.PostEvent(Event::Custom);
        }
    });

    screen.Loop(ui);

    running = false;
    ticker.join();
    return 0;
}
