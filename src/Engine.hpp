#pragma once

#include <fstream>
#include <string>
#include <vector>

#include "Track.hpp"

struct mpv_handle; // fwd-decl: keep <mpv/client.h> out of the public header

// The libmpv boundary. Owns playback: the playlist, play/pause, volume, seek.
// Knows nothing about the terminal or about searching.
//
// mpv owns the *real* playlist (via loadfile append-play and ytdl=yes, which
// re-resolves stream URLs at load time so expiry is a non-issue). We keep a
// parallel vector<Track> purely so the UI has titles to render.
class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Manually queue a track. Placed AHEAD of any autoplay (radio) tracks so
    // user picks always take priority. Starts playing if idle.
    void enqueue(const Track& t);
    // Append an autoplay/radio track at the very end (lowest priority).
    void appendAuto(const Track& t);
    // Replace the queue with this single track and play it now.
    void playNow(const Track& t);

    bool isManual(int i) const;   // for UI marker
    int  upcoming() const;        // queue entries after the current one

    void togglePause();
    void next();
    void prev();
    void stop();                 // clear playlist, stop playback

    void volumeUp(int step = 5);
    void volumeDown(int step = 5);
    void setVolume(int vol);      // clamped 0..130
    void seekRelative(double seconds);

    void toggleRepeat();          // loop the whole playlist on/off
    void shuffle();               // randomize remaining order

    // queue editing
    void removeAt(int i);         // drop one track
    void moveUp(int i);           // swap with previous
    void moveDown(int i);         // swap with next
    void playIndex(int i);        // jump to and play a queue entry

    // --- state queries: safe to call at any time, never block ---
    bool   paused() const;
    int    volume() const;
    double position() const;      // seconds into current track, -1 if none
    double duration() const;      // length of current track, -1 if unknown
    int    playlistPos() const;   // index of current track, -1 if none
    bool   idle() const;          // nothing loaded / playback finished
    bool   repeat() const;        // loop-playlist active?

    const std::vector<Track>& queue() const { return queue_; }
    const Track* current() const;             // nullptr if none

    // Drain mpv's event queue (non-blocking). Call each UI tick so mpv's
    // internal state stays current and finished/failed files advance.
    void pump();

    // Last playback error (e.g. a stream that failed to resolve), "" if none.
    const std::string& lastError() const { return lastError_; }

private:
    mpv_handle* mpv_ = nullptr;
    std::string lastError_;
    std::ofstream log_;            // debug log (path from JARVIS_LOG)
    void log(const std::string& msg);
    std::vector<Track> queue_;
    std::vector<char> manual_; // parallel to queue_: 1 = user-added, 0 = radio

    void   command(const char** args) const;
    void   loadfile(const Track& t, const char* mode); // mpv loadfile <url> mode
    void   movePlaylist(int from, int to);             // mpv playlist-move
    double getDouble(const char* prop) const;
    int    getFlag(const char* prop) const;
};
