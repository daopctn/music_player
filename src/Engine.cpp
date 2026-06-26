#include "Engine.hpp"

#include <mpv/client.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <stdexcept>
#include <string>

namespace {
constexpr int kVolMax = 130; // mpv allows >100 (soft amplification)

// Human-readable name for an mpv event id (for the debug log).
const char* eventName(int id) {
    switch (id) {
        case MPV_EVENT_START_FILE:       return "START_FILE";
        case MPV_EVENT_END_FILE:         return "END_FILE";
        case MPV_EVENT_FILE_LOADED:      return "FILE_LOADED";
        case MPV_EVENT_IDLE:             return "IDLE";
        case MPV_EVENT_PLAYBACK_RESTART: return "PLAYBACK_RESTART";
        case MPV_EVENT_SEEK:             return "SEEK";
        case MPV_EVENT_LOG_MESSAGE:      return "LOG";
        case MPV_EVENT_SHUTDOWN:         return "SHUTDOWN";
        case MPV_EVENT_COMMAND_REPLY:    return "COMMAND_REPLY";
        default:                         return "?";
    }
}
const char* endReason(int r) {
    switch (r) {
        case MPV_END_FILE_REASON_EOF:      return "EOF";
        case MPV_END_FILE_REASON_STOP:     return "STOP";
        case MPV_END_FILE_REASON_QUIT:     return "QUIT";
        case MPV_END_FILE_REASON_ERROR:    return "ERROR";
        case MPV_END_FILE_REASON_REDIRECT: return "REDIRECT";
        default:                           return "?";
    }
}
} // namespace

void Engine::log(const std::string& msg) {
    if (!log_.is_open()) return;
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
    log_ << buf << "  " << msg << '\n';
    log_.flush();
}

Engine::Engine() {
    // Debug log: enabled when JARVIS_LOG is set to a file path.
    if (const char* p = std::getenv("JARVIS_LOG"))
        if (*p) log_.open(p, std::ios::app);
    log("=== Engine start ===");

    mpv_ = mpv_create();
    if (!mpv_) throw std::runtime_error("mpv_create failed");

    // Audio only, let yt-dlp resolve streams, prefer best audio.
    mpv_set_option_string(mpv_, "vid", "no");
    mpv_set_option_string(mpv_, "ytdl", "yes");
    mpv_set_option_string(mpv_, "ytdl-format", "bestaudio/best");
    // ytdl-raw-options passed straight to yt-dlp (comma-separated key=value).
    //
    // Force the ANDROID_VR client. It serves audio WITHOUT YouTube's JS
    // "nsig"/PO-token challenges, which the default web client now requires and
    // which fail on machines with no JS runtime — leaving "Requested format is
    // not available" so EVERY track instant-skips through the whole queue.
    //
    // The catch: android_vr does NOT support cookies ("Skipping client
    // android_vr since it does not support cookies"). Passing
    // cookies-from-browser drops android_vr, falls back to the web client, and
    // re-breaks playback. So we deliberately do NOT send cookies to the player.
    // Search/library keep their personalization via Resolver's own cookie arg;
    // only playback goes anonymous (public videos all play fine).
    //
    // Premium/private playback needs the web client, i.e. a JS runtime (e.g.
    // deno) + the EJS challenge solver. If you've installed those, set
    // JARVIS_PLAYBACK_COOKIES=1 to send cookies and let yt-dlp pick the web
    // client instead.
    std::string rawOpts = "extractor-args=youtube:player_client=android_vr";
    const char* b = std::getenv("JARVIS_COOKIES_BROWSER");
    const char* useCookies = std::getenv("JARVIS_PLAYBACK_COOKIES");
    if (b && *b && useCookies && *useCookies)
        rawOpts = std::string("cookies-from-browser=") + b;
    mpv_set_option_string(mpv_, "ytdl-raw-options", rawOpts.c_str());
    // Don't pause at end of a file; advance the playlist instead.
    mpv_set_option_string(mpv_, "keep-open", "no");
    // Quiet: we render our own UI; mpv's logs would corrupt the TUI.
    mpv_set_option_string(mpv_, "terminal", "no");
    mpv_set_option_string(mpv_, "msg-level", "all=no");

    if (mpv_initialize(mpv_) < 0) {
        mpv_terminate_destroy(mpv_);
        mpv_ = nullptr;
        throw std::runtime_error("mpv_initialize failed");
    }

    // When debugging, route mpv's own log (ytdl_hook, format selection, errors)
    // into our file so we can see WHY a stream failed to play.
    if (log_.is_open())
        mpv_request_log_messages(mpv_, "warn");
}

Engine::~Engine() {
    if (mpv_) mpv_terminate_destroy(mpv_); // clean shutdown
}

void Engine::command(const char** args) const {
    mpv_command(mpv_, args); // best-effort; playback errors surface via events
}

void Engine::loadfile(const Track& t, const char* mode) {
    std::string url = t.url();
    log(std::string("loadfile ") + mode + "  " + t.id + "  \"" + t.title + "\"");
    const char* cmd[] = {"loadfile", url.c_str(), mode, nullptr};
    command(cmd);
}

void Engine::movePlaylist(int from, int to) {
    std::string f = std::to_string(from), t = std::to_string(to);
    const char* cmd[] = {"playlist-move", f.c_str(), t.c_str(), nullptr};
    command(cmd);
}

double Engine::getDouble(const char* prop) const {
    double v = 0;
    if (mpv_get_property(mpv_, prop, MPV_FORMAT_DOUBLE, &v) < 0) return -1;
    return v;
}

int Engine::getFlag(const char* prop) const {
    int v = 0;
    if (mpv_get_property(mpv_, prop, MPV_FORMAT_FLAG, &v) < 0) return -1;
    return v;
}

void Engine::appendAuto(const Track& t) {
    loadfile(t, "append-play");
    queue_.push_back(t);
    manual_.push_back(0);
}

void Engine::enqueue(const Track& t) {
    // Append, then hoist it above the autoplay block so manual picks win.
    loadfile(t, "append-play");
    queue_.push_back(t);
    manual_.push_back(1);

    // Target = first auto entry after the current track.
    int cur = playlistPos();
    int target = (int)queue_.size() - 1;
    for (int i = std::max(cur + 1, 0); i < (int)queue_.size() - 1; ++i) {
        if (!manual_[i]) { target = i; break; }
    }
    int from = (int)queue_.size() - 1;
    log("enqueue move from=" + std::to_string(from) + " target=" +
        std::to_string(target) + " cur=" + std::to_string(cur));
    if (target < from) {
        movePlaylist(from, target);
        // mirror the move in our vectors
        Track moved = queue_.back();
        char mf = manual_.back();
        queue_.pop_back();
        manual_.pop_back();
        queue_.insert(queue_.begin() + target, moved);
        manual_.insert(manual_.begin() + target, mf);
    }
}

void Engine::playNow(const Track& t) {
    stop();
    loadfile(t, "replace");
    queue_.push_back(t);
    manual_.push_back(1);
}

bool Engine::isManual(int i) const {
    return i >= 0 && i < (int)manual_.size() && manual_[i];
}

int Engine::upcoming() const {
    int cur = playlistPos();
    if (cur < 0) return (int)queue_.size();
    return (int)queue_.size() - 1 - cur;
}

void Engine::togglePause() {
    const char* cmd[] = {"cycle", "pause", nullptr};
    command(cmd);
}

void Engine::next() {
    log("CMD next()  pos=" + std::to_string(playlistPos()));
    const char* cmd[] = {"playlist-next", "force", nullptr};
    command(cmd);
}

void Engine::prev() {
    log("CMD prev()  pos=" + std::to_string(playlistPos()));
    const char* cmd[] = {"playlist-prev", "force", nullptr};
    command(cmd);
}

void Engine::stop() {
    const char* clear[] = {"playlist-clear", nullptr};
    command(clear);
    const char* st[] = {"stop", nullptr};
    command(st);
    queue_.clear();
    manual_.clear();
}

void Engine::setVolume(int vol) {
    vol = std::clamp(vol, 0, kVolMax);
    double v = vol;
    mpv_set_property(mpv_, "volume", MPV_FORMAT_DOUBLE, &v);
}

void Engine::volumeUp(int step)   { setVolume(volume() + step); }
void Engine::volumeDown(int step) { setVolume(volume() - step); }

void Engine::seekRelative(double seconds) {
    if (idle()) return;
    std::string s = std::to_string(seconds);
    const char* cmd[] = {"seek", s.c_str(), "relative", nullptr};
    command(cmd);
}

void Engine::toggleRepeat() {
    const char* on = repeat() ? "no" : "inf";
    mpv_set_property_string(mpv_, "loop-playlist", on);
}

void Engine::shuffle() {
    if (queue_.empty()) return;
    const char* cmd[] = {"playlist-shuffle", nullptr};
    command(cmd);

    // mpv reordered its internal playlist; mirror that into queue_ so the
    // UI's highlight and titles stay aligned. Match rows by their URL.
    int64_t count = 0;
    if (mpv_get_property(mpv_, "playlist/count", MPV_FORMAT_INT64, &count) < 0)
        return;
    std::vector<Track> reordered;
    std::vector<char> reorderedManual;
    reordered.reserve(queue_.size());
    for (int64_t i = 0; i < count; ++i) {
        std::string key = "playlist/" + std::to_string(i) + "/filename";
        char* fn = nullptr;
        if (mpv_get_property(mpv_, key.c_str(), MPV_FORMAT_STRING, &fn) < 0 ||
            !fn)
            continue;
        std::string url = fn;
        mpv_free(fn);
        for (size_t j = 0; j < queue_.size(); ++j)
            if (queue_[j].url() == url) {
                reordered.push_back(queue_[j]);
                reorderedManual.push_back(manual_[j]);
                break;
            }
    }
    if (reordered.size() == queue_.size()) {
        queue_ = std::move(reordered);
        manual_ = std::move(reorderedManual);
    }
}

void Engine::removeAt(int i) {
    if (i < 0 || i >= (int)queue_.size()) return;
    std::string idx = std::to_string(i);
    const char* cmd[] = {"playlist-remove", idx.c_str(), nullptr};
    command(cmd);
    queue_.erase(queue_.begin() + i);
    manual_.erase(manual_.begin() + i);
}

void Engine::moveUp(int i) {
    if (i <= 0 || i >= (int)queue_.size()) return;
    movePlaylist(i, i - 1); // mpv: playlist-move <from> <to-before-index>
    std::swap(queue_[i], queue_[i - 1]);
    std::swap(manual_[i], manual_[i - 1]);
}

void Engine::moveDown(int i) {
    if (i < 0 || i >= (int)queue_.size() - 1) return;
    movePlaylist(i, i + 2);
    std::swap(queue_[i], queue_[i + 1]);
    std::swap(manual_[i], manual_[i + 1]);
}

void Engine::playIndex(int i) {
    log("CMD playIndex(" + std::to_string(i) + ")");
    if (i < 0 || i >= (int)queue_.size()) return;
    int64_t v = i;
    mpv_set_property(mpv_, "playlist-pos", MPV_FORMAT_INT64, &v);
}

bool Engine::paused() const {
    return getFlag("pause") == 1;
}

int Engine::volume() const {
    double v = getDouble("volume");
    return v < 0 ? 100 : static_cast<int>(v);
}

double Engine::position() const { return getDouble("time-pos"); }
double Engine::duration() const { return getDouble("duration"); }

int Engine::playlistPos() const {
    int64_t v = 0;
    if (mpv_get_property(mpv_, "playlist-pos", MPV_FORMAT_INT64, &v) < 0)
        return -1;
    return static_cast<int>(v);
}

bool Engine::idle() const {
    return getFlag("idle-active") == 1 || queue_.empty();
}

bool Engine::repeat() const {
    char* v = nullptr;
    if (mpv_get_property(mpv_, "loop-playlist", MPV_FORMAT_STRING, &v) < 0 || !v)
        return false;
    std::string s = v;
    mpv_free(v);
    return s != "no" && s != "0";
}

const Track* Engine::current() const {
    int pos = playlistPos();
    if (pos < 0 || pos >= static_cast<int>(queue_.size())) return nullptr;
    return &queue_[pos];
}

void Engine::pump() {
    // Drain without blocking. We don't need the events themselves yet, but
    // pumping keeps mpv's core advancing and lets it free finished files.
    while (true) {
        mpv_event* ev = mpv_wait_event(mpv_, 0.0);
        if (!ev || ev->event_id == MPV_EVENT_NONE) break;

        switch (ev->event_id) {
        case MPV_EVENT_LOG_MESSAGE: {
            // mpv's own log (ytdl_hook etc.) — the gold for "why did it skip".
            auto* m = static_cast<mpv_event_log_message*>(ev->data);
            if (m) {
                std::string txt = m->text ? m->text : "";
                while (!txt.empty() && (txt.back() == '\n' || txt.back() == '\r'))
                    txt.pop_back();
                log(std::string("  [mpv ") + (m->prefix ? m->prefix : "") + "/" +
                    (m->level ? m->level : "") + "] " + txt);
            }
            break;
        }
        case MPV_EVENT_END_FILE: {
            auto* ef = static_cast<mpv_event_end_file*>(ev->data);
            int reason = ef ? ef->reason : -1;
            int err = ef ? ef->error : 0;
            log(std::string("EVENT END_FILE reason=") + endReason(reason) +
                " pos=" + std::to_string(playlistPos()) +
                (err ? std::string("  err=") + mpv_error_string(err) : ""));
            if (ef && ef->reason == MPV_END_FILE_REASON_ERROR)
                lastError_ = mpv_error_string(ef->error);
            break;
        }
        case MPV_EVENT_START_FILE:
            lastError_.clear(); // a new file started cleanly
            log("EVENT START_FILE  pos=" + std::to_string(playlistPos()));
            break;
        case MPV_EVENT_FILE_LOADED:
            log("EVENT FILE_LOADED  pos=" + std::to_string(playlistPos()) +
                "  dur=" + std::to_string(duration()));
            break;
        case MPV_EVENT_IDLE:
            log("EVENT IDLE");
            break;
        default:
            // log other events tersely (skip the noisy nothing)
            log(std::string("EVENT ") + eventName(ev->event_id));
            break;
        }
    }
}
