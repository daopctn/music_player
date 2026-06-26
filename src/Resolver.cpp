#include "Resolver.hpp"

#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "util.hpp"

namespace {

// Tab-separated --print template; parse() below relies on this field order.
// live_status lets us drop livestreams (no fixed duration → mpv chokes/streams
// forever): "is_live", "is_upcoming", "post_live", "was_live", "not_live", NA.
constexpr const char* kPrintFmt =
    "%(title)s\t%(id)s\t%(duration)s\t%(channel)s\t%(live_status)s\t%(ie_key)s";

// Wrap a string in single quotes for /bin/sh, escaping any embedded quote.
// foo'bar -> 'foo'\''bar'
std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

// Trim trailing CR/LF so Windows-ish line endings don't poison the last field.
void rstrip(std::string& s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
}

} // namespace

Resolver::Resolver(int maxResults) : maxResults_(maxResults) {
    if (const char* b = std::getenv("JARVIS_COOKIES_BROWSER")) {
        cookieBrowser_ = b;
        if (!cookieBrowser_.empty())
            cookieArg_ = " --cookies-from-browser " + shellQuote(cookieBrowser_);
    }
}

std::string Resolver::run(const std::string& cmd) {
    int status = 0;
    std::string out = slurp(cmd, &status);
    if (status == -1) throw Error("failed to launch yt-dlp (popen)");

    // 127 from the shell means "command not found".
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == 127)
            throw Error("yt-dlp not found on PATH — install it and retry");
        // Any other non-zero with no usable output: surface it.
        if (code != 0 && out.empty())
            throw Error("yt-dlp exited with code " + std::to_string(code) +
                        " (network down? bad query?)");
    }
    return out;
}

// Assemble a flat-playlist yt-dlp invocation. `target` is the trailing source
// token (an ytsearch spec, URL, or :alias, already shell-quoted as needed).
// --flat-playlist: don't resolve each video (fast, one yt-dlp call).
// --print kPrintFmt: tab-separated fields, JSON-lib-free on purpose. yt-dlp
// does NOT interpret backslash escapes in the template, so the separator is a
// literal TAB (0x09). A negative playlistEnd omits the --playlist-end cap.
std::string Resolver::buildCmd(const std::string& target,
                               int playlistEnd) const {
    std::ostringstream cmd;
    cmd << "yt-dlp --quiet --no-warnings --flat-playlist" << cookieArg_ << " ";
    if (playlistEnd >= 0) cmd << "--playlist-end " << playlistEnd << " ";
    cmd << "--print '" << kPrintFmt << "' " << target << " 2>/dev/null";
    return cmd.str();
}

std::vector<Track> Resolver::search(const std::string& query) const {
    if (query.empty()) return {};
    std::string target = "ytsearch" + std::to_string(maxResults_) + ":" +
                         shellQuote(query);
    return parse(run(buildCmd(target)));
}

std::vector<Track> Resolver::library(const std::string& source,
                                     int limit) const {
    if (source.empty()) return {};
    return parse(run(buildCmd(shellQuote(source), limit)));
}

std::vector<Track> Resolver::radio(const std::string& videoId,
                                   int limit) const {
    if (videoId.empty()) return {};
    // RD<id> is YouTube's auto-generated Mix playlist seeded by the video.
    std::string url = "https://www.youtube.com/watch?v=" + videoId +
                      "&list=RD" + videoId;
    return parse(run(buildCmd(shellQuote(url), limit)));
}

namespace {
unsigned hex4(const std::string& s, size_t i) {
    unsigned v = 0;
    for (int k = 0; k < 4 && i + k < s.size(); ++k) {
        char c = s[i + k];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
    }
    return v;
}
std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += c;
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}
} // namespace

std::vector<std::string> Resolver::suggest(const std::string& query) const {
    if (query.empty()) return {};
    std::string url =
        "https://suggestqueries.google.com/complete/search?client=firefox"
        "&ds=yt&q=" + urlEncode(query);
    std::string cmd = "curl -s --max-time 5 '" + url + "' 2>/dev/null";

    std::string out = slurp(cmd);

    // Response: ["query",["sug1","sug2",...],[],{...}]. Isolate the second
    // array (the suggestions) so the trailing object's keys can't leak in.
    size_t a = out.find(",[");
    if (a == std::string::npos) return {};
    size_t b = out.find(']', a);
    if (b == std::string::npos) return {};
    std::string seg = out.substr(a + 2, b - (a + 2));

    std::vector<std::string> all;
    bool in = false;
    std::string cur;
    for (size_t i = 0; i < seg.size(); ++i) {
        char c = seg[i];
        if (!in) {
            if (c == '"') { in = true; cur.clear(); }
        } else {
            if (c == '\\' && i + 1 < seg.size()) {
                char e = seg[++i];
                if (e == 'u' && i + 4 < seg.size()) {
                    appendUtf8(cur, hex4(seg, i + 1));
                    i += 4;
                } else {
                    cur += e;
                }
                continue;
            }
            if (c == '"') { in = false; all.push_back(cur); }
            else cur += c;
        }
    }
    if (all.size() > 8) all.resize(8);
    return all;
}

std::vector<Track> Resolver::parse(const std::string& output) {
    std::vector<Track> tracks;
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        rstrip(line);
        if (line.empty()) continue;

        // Split on tabs into at most 6 fields.
        std::string fields[6];
        int fi = 0;
        for (char c : line) {
            if (c == '\t' && fi < 5) { ++fi; continue; }
            fields[fi] += c;
        }

        // Drop livestreams / premieres: no fixed duration, mpv would stream
        // them forever or fail. was_live (a finished stream / VOD) is fine.
        const std::string& live = fields[4];
        if (live == "is_live" || live == "is_upcoming" || live == "post_live")
            continue;

        Track t;
        t.title = fields[0];
        t.id    = fields[1];
        if (t.id.empty()) continue;          // malformed row, skip
        try { t.duration = std::stoi(fields[2]); }
        catch (...) { t.duration = 0; }
        if (fields[3] != "NA") t.channel = fields[3];
        // ie_key "YoutubeTab" => the row is a playlist / Mix, not a video.
        t.playlist = (fields[5] == "YoutubeTab");

        tracks.push_back(std::move(t));
    }
    return tracks;
}
