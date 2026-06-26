#pragma once

#include <string>

#include "core/util.hpp"

// Shared data type passed across all three layers.
// Resolver produces these; Engine plays them; UI renders them.
struct Track {
    std::string title;
    std::string id;        // YouTube video id, or a list id when playlist=true
    std::string channel;   // uploader / channel name ("" if unknown)
    int duration = 0;      // seconds, 0 if unknown
    bool playlist = false; // true = a Mix / playlist (id is a list id), not a video

    // The canonical URL. mpv's ytdl hook re-resolves this at load time, so we
    // never store an (expiring) stream URL.
    //
    // A Mix/playlist points at the list (it must be expanded into videos, not
    // played as one entry). Auto-generated "RD<seed>" Mixes are *unviewable* as
    // a bare list — YouTube requires the seed video in the URL — so for those
    // (id = "RD" + an 11-char video id) we use the watch?v=…&list=… form.
    std::string url() const {
        if (!playlist) return "https://www.youtube.com/watch?v=" + id;
        if (id.size() == 13 && id.compare(0, 2, "RD") == 0)
            return "https://www.youtube.com/watch?v=" + id.substr(2) +
                   "&list=" + id;
        return "https://www.youtube.com/playlist?list=" + id;
    }

    // mm:ss, or h:mm:ss for long tracks. "--:--" when unknown.
    std::string durationStr() const {
        return fmtTime(duration <= 0 ? -1.0 : duration);
    }
};
