#pragma once

#include <string>
#include <vector>
#include <stdexcept>

#include "Track.hpp"

// The yt-dlp boundary. The ONLY component that knows yt-dlp exists.
// In: a search string or URL. Out: a vector<Track>.
//
// Quarantines the lone Python dependency behind a single popen() call.
// Deliberately avoids any JSON library: yt-dlp is asked for tab-delimited
// fields and we split on '\t'.
class Resolver {
public:
    // Thrown when yt-dlp itself fails (missing binary, no network).
    // An empty result (zero matches) is NOT an error — returns {}.
    struct Error : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    // Reads JARVIS_COOKIES_BROWSER (e.g. "firefox", "chrome", "brave",
    // "firefox:profile") to authenticate yt-dlp as your YouTube account.
    explicit Resolver(int maxResults = 8);

    // Free-text search via ytsearch. Returns up to maxResults tracks.
    std::vector<Track> search(const std::string& query) const;

    // Load a yt-dlp playlist/alias (e.g. ":ytfav", ":ytwatchlater", a URL).
    // Needs cookies for personal sources. Up to `limit` tracks.
    std::vector<Track> library(const std::string& source, int limit = 50) const;

    // Personal libraries — keep the yt-dlp alias tokens inside this boundary
    // (the UI names intent, not yt-dlp encodings). Both need cookies.
    std::vector<Track> likedSongs(int limit = 50) const {
        return library(":ytfav", limit);
    }
    std::vector<Track> watchLater(int limit = 50) const {
        return library(":ytwatchlater", limit);
    }
    // Personalized YouTube home / recommendations feed.
    std::vector<Track> homeFeed(int limit = 50) const {
        return library(":ytrec", limit);
    }

    // Empty when no cookies configured; otherwise the browser string.
    const std::string& cookieBrowser() const { return cookieBrowser_; }

    // YouTube Mix ("radio") for a video — related autoplay tracks. The seed
    // itself is usually the first entry. Returns up to `limit` tracks.
    std::vector<Track> radio(const std::string& videoId, int limit = 20) const;

    // YouTube search autocomplete suggestions. Never throws — returns {} on
    // any failure (it's a nicety, not a hard dependency).
    std::vector<std::string> suggest(const std::string& query) const;

private:
    int maxResults_;
    std::string cookieBrowser_;   // browser name for --cookies-from-browser
    std::string cookieArg_;       // ready-to-inject yt-dlp flag (or empty)

    // Assemble a flat-playlist yt-dlp command for `target`; negative
    // playlistEnd omits the --playlist-end cap.
    std::string buildCmd(const std::string& target, int playlistEnd = -1) const;
    // Runs the command, returns stdout. Throws Error on non-zero exit.
    static std::string run(const std::string& cmd);
    // Parse tab-delimited yt-dlp --print output into tracks.
    static std::vector<Track> parse(const std::string& output);
};
