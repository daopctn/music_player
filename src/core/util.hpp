#pragma once

#include <cstdio>
#include <string>

// Small helpers shared across the layers. Header-only: each is a pure
// function of its arguments with no state, so inlining avoids a .cpp.

// Run `cmd` via the shell and return its stdout. Best-effort: returns {} if the
// pipe can't open. When `status` is non-null it receives pclose()'s raw return
// (-1 on popen failure), so callers that care can inspect the exit code.
inline std::string slurp(const std::string& cmd, int* status = nullptr) {
    std::FILE* p = ::popen(cmd.c_str(), "r");
    if (!p) {
        if (status) *status = -1;
        return {};
    }
    std::string out;
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
    int rc = ::pclose(p);
    if (status) *status = rc;
    return out;
}

// seconds -> "m:ss", or "h:mm:ss" for long tracks. "--:--" when negative.
inline std::string fmtTime(double s) {
    if (s < 0) return "--:--";
    int t = static_cast<int>(s);
    int h = t / 3600, m = (t % 3600) / 60, sec = t % 60;
    char buf[16];
    if (h > 0) std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, sec);
    else       std::snprintf(buf, sizeof(buf), "%d:%02d", m, sec);
    return buf;
}

// Append Unicode codepoint cp to out, UTF-8 encoded (BMP + astral planes).
inline void appendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}
