#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>

// Async YouTube-thumbnail cache. Renders REAL images via the Kitty graphics
// protocol (unicode-placeholder mode) when the terminal supports it — ghostty
// and kitty do — and falls back to half-block art everywhere else.
//
// Placeholder mode is what makes this work inside FTXUI: the image is
// transmitted once, then drawn as ordinary U+10EEEE placeholder cells that
// FTXUI styles like any other text. The terminal composites the picture onto
// those cells, so it survives FTXUI's full repaints.
//
// get() never blocks: first request for an id spawns a detached worker
// (curl -> stb_image decode), returns a placeholder, and calls the refresh
// callback when the image lands.
class ThumbnailCache {
public:
    ThumbnailCache();

    // cols = target width in terminal cells.
    ftxui::Element get(const std::string& id, int cols);

    // Called from a worker thread when a fetch completes; wire to PostEvent.
    void setRefresh(std::function<void()> cb) { refresh_ = std::move(cb); }

    bool kitty() const { return kitty_; }

private:
    struct Img {
        int w = 0, h = 0;
        std::vector<unsigned char> rgb; // w*h*3
        bool failed = false;
        int kittyId = 0;                // 1..255, 0 = not yet assigned
        bool transmitted = false;       // image bytes sent to the terminal
    };

    bool kitty_ = false;
    std::mutex mu_;
    // Keyed by "id@cols": the Kitty placement is sized to the cell box, so the
    // same video at two sizes (16 in result rows, 38 in now-playing) needs two
    // entries — otherwise the second size renders against the first's placement
    // and looks squashed/overflowing.
    std::map<std::string, std::shared_ptr<Img>> cache_; // null => in flight
    std::function<void()> refresh_;

    void fetch(const std::string& key, const std::string& id);
    void transmit(Img& img);                          // Kitty: send pixels
    ftxui::Element placeholders(const Img& img, int cols, int rows);
    ftxui::Element halfBlocks(const Img& img, int cols);
};
