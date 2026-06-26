#include "Thumbnail.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "third_party/stb_image.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "util.hpp"

using namespace ftxui;

namespace {

std::atomic<int> g_nextKittyId{1};

// Kitty "row/column" diacritics — index N encodes coordinate N. Only the first
// chunk is needed for a modest art box. (From the Kitty graphics spec.)
const unsigned int kDiacritics[] = {
    0x0305, 0x030D, 0x030E, 0x0310, 0x0312, 0x033D, 0x033E, 0x033F, 0x0346,
    0x034A, 0x034B, 0x034C, 0x0350, 0x0351, 0x0352, 0x0357, 0x035B, 0x0363,
    0x0364, 0x0365, 0x0366, 0x0367, 0x0368, 0x0369, 0x036A, 0x036B, 0x036C,
    0x036D, 0x036E, 0x036F, 0x0483, 0x0484, 0x0485, 0x0486, 0x0487, 0x0592,
    0x0593, 0x0594, 0x0595, 0x0597, 0x0598, 0x0599, 0x059C, 0x059D, 0x059E,
    0x059F, 0x05A0, 0x05A1, 0x05A8, 0x05A9, 0x05AB, 0x05AC, 0x05AF, 0x05C4,
    0x0610, 0x0611, 0x0612, 0x0613, 0x0614, 0x0615, 0x0616, 0x0617, 0x0657,
    0x0658};
constexpr int kDiacriticsN = sizeof(kDiacritics) / sizeof(kDiacritics[0]);

std::string base64(const unsigned char* p, size_t n) {
    static const char* T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        unsigned v = (p[i] << 16) | (p[i + 1] << 8) | p[i + 2];
        out += T[(v >> 18) & 63];
        out += T[(v >> 12) & 63];
        out += T[(v >> 6) & 63];
        out += T[v & 63];
    }
    if (i < n) {
        unsigned v = p[i] << 16;
        if (i + 1 < n) v |= p[i + 1] << 8;
        out += T[(v >> 18) & 63];
        out += T[(v >> 12) & 63];
        out += (i + 1 < n) ? T[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

bool detectKitty() {
    if (std::getenv("JARVIS_NO_KITTY")) return false;
    if (std::getenv("KITTY_WINDOW_ID")) return true;
    if (std::getenv("GHOSTTY_RESOURCES_DIR") || std::getenv("GHOSTTY_BIN_DIR"))
        return true;
    if (const char* tp = std::getenv("TERM_PROGRAM"))
        if (std::strstr(tp, "ghostty") || std::strstr(tp, "kitty")) return true;
    if (const char* t = std::getenv("TERM"))
        if (std::strstr(t, "kitty") || std::strstr(t, "ghostty")) return true;
    return false;
}

bool safeId(const std::string& id) {
    if (id.empty() || id.size() > 24) return false;
    for (char c : id)
        if (!(std::isalnum((unsigned char)c) || c == '_' || c == '-'))
            return false;
    return true;
}

} // namespace

ThumbnailCache::ThumbnailCache() : kitty_(detectKitty()) {}

void ThumbnailCache::fetch(const std::string& key, const std::string& id) {
    auto img = std::make_shared<Img>();
    std::string url = "https://i.ytimg.com/vi/" + id + "/mqdefault.jpg";
    std::string bytes = slurp("curl -s --max-time 8 '" + url + "'");

    if (!bytes.empty()) {
        int w = 0, h = 0, comp = 0;
        unsigned char* px = stbi_load_from_memory(
            reinterpret_cast<const unsigned char*>(bytes.data()),
            (int)bytes.size(), &w, &h, &comp, 3);
        if (px) {
            img->w = w;
            img->h = h;
            img->rgb.assign(px, px + (size_t)w * h * 3);
            stbi_image_free(px);
        } else {
            img->failed = true;
        }
    } else {
        img->failed = true;
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        cache_[key] = img;
    }
    if (refresh_) refresh_();
}

// Send raw RGB to the terminal once (Kitty f=24, transmit-only) and create a
// virtual placement the unicode placeholders will reference.
void ThumbnailCache::transmit(Img& img) {
    // 24-bit id: the Unicode placeholder carries it in the cell's RGB fg
    // (see placeholders()), so we have the full 1..0xFFFFFF range. (The old
    // code capped at 255 — blue channel only — which wrapped and made distinct
    // thumbnails collide onto one id, showing the wrong picture.)
    img.kittyId = g_nextKittyId.fetch_add(1);
    if (img.kittyId > 0xFFFFFF) img.kittyId = ((img.kittyId - 1) % 0xFFFFFF) + 1;

    std::string b64 = base64(img.rgb.data(), img.rgb.size());

    // q=2 suppresses the terminal's responses so nothing leaks into the TUI.
    constexpr size_t kChunk = 4096;
    size_t off = 0;
    bool first = true;
    while (off < b64.size()) {
        size_t len = std::min(kChunk, b64.size() - off);
        bool last = (off + len >= b64.size());
        std::string esc = "\033_G";
        if (first) {
            esc += "q=2,a=t,f=24,s=" + std::to_string(img.w) +
                   ",v=" + std::to_string(img.h) +
                   ",i=" + std::to_string(img.kittyId) + ",t=d";
            first = false;
        } else {
            esc += "q=2";
        }
        esc += ",m=";
        esc += last ? "0" : "1";
        esc += ";";
        esc.append(b64, off, len);
        esc += "\033\\";
        std::fwrite(esc.data(), 1, esc.size(), stdout);
        off += len;
    }
    img.transmitted = true;
    std::fflush(stdout);
}

Element ThumbnailCache::placeholders(const Img& img, int cols, int rows) {
    if (cols > kDiacriticsN) cols = kDiacriticsN;
    if (rows > kDiacriticsN) rows = kDiacriticsN;
    int id = img.kittyId;
    // Kitty reads the cell's 24-bit fg as the image id — spread id over R,G,B.
    Color fg = Color::RGB((id >> 16) & 0xFF, (id >> 8) & 0xFF, id & 0xFF);

    Elements lines;
    for (int r = 0; r < rows; ++r) {
        std::string row;
        for (int c = 0; c < cols; ++c) {
            appendUtf8(row, 0x10EEEE);          // placeholder base glyph
            appendUtf8(row, kDiacritics[r]);    // row coordinate
            appendUtf8(row, kDiacritics[c]);    // column coordinate
        }
        lines.push_back(text(row) | color(fg));
    }
    return vbox(std::move(lines));
}

Element ThumbnailCache::halfBlocks(const Img& img, int cols) {
    if (cols < 8) cols = 8;
    int outW = cols;
    int outRows = std::max(1, (img.h * outW) / (img.w * 2));
    int outH = outRows * 2;
    auto at = [&](int x, int y) -> const unsigned char* {
        int sx = std::min(x * img.w / outW, img.w - 1);
        int sy = std::min(y * img.h / outH, img.h - 1);
        return &img.rgb[((size_t)sy * img.w + sx) * 3];
    };
    Elements rows;
    for (int ry = 0; ry < outRows; ++ry) {
        Elements cells;
        for (int x = 0; x < outW; ++x) {
            const unsigned char* t = at(x, ry * 2);
            const unsigned char* b = at(x, ry * 2 + 1);
            cells.push_back(text("▀") | color(Color::RGB(t[0], t[1], t[2])) |
                            bgcolor(Color::RGB(b[0], b[1], b[2])));
        }
        rows.push_back(hbox(std::move(cells)));
    }
    return vbox(std::move(rows));
}

Element ThumbnailCache::get(const std::string& id, int cols) {
    if (!safeId(id)) return text("no art") | dim | center;

    std::string key = id + "@" + std::to_string(cols);
    std::shared_ptr<Img> img;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = cache_.find(key);
        if (it == cache_.end()) {
            cache_[key] = nullptr;
            std::thread(&ThumbnailCache::fetch, this, key, id).detach();
            return text("loading art…") | dim | center;
        }
        img = it->second;
    }
    if (!img) return text("loading art…") | dim | center;
    if (img->failed || img->rgb.empty()) return text("no art") | dim | center;

    int rows = std::max(1, (img->h * cols) / (img->w * 2));

    if (kitty_) {
        int ccols = std::min(cols, kDiacriticsN);
        int crows = std::min(rows, kDiacriticsN);
        std::lock_guard<std::mutex> lk(mu_);
        if (!img->transmitted) {
            transmit(*img);
            // Create the Unicode virtual placement the placeholder cells
            // reference. WITHOUT this the cells point at nothing — no image.
            std::string esc = "\033_Gq=2,a=p,U=1,i=" +
                              std::to_string(img->kittyId) +
                              ",c=" + std::to_string(ccols) +
                              ",r=" + std::to_string(crows) + "\033\\";
            std::fwrite(esc.data(), 1, esc.size(), stdout);
            std::fflush(stdout);
        }
        return placeholders(*img, cols, rows);
    }
    return halfBlocks(*img, cols);
}
