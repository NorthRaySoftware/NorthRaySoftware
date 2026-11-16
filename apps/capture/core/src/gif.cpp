#include "ncap/gif.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace ncap::gif {

namespace {

struct Rgb {
    std::uint8_t r, g, b;
};

// Median cut: split the box with the widest channel at its median until there are `want` boxes; each
// box's average is a palette colour.
std::vector<Rgb> medianCut(std::vector<std::uint32_t>& px, std::size_t want) {
    struct Box {
        std::size_t a, b;   // [a, b) in px
    };
    auto ch = [](std::uint32_t c, int i) { return (int)((c >> (16 - 8 * i)) & 0xFF); };
    std::vector<Box> boxes{{0, px.size()}};
    while (boxes.size() < want) {
        // The box with the widest range (of those with more than one colour).
        int best = -1, bestRange = 0, bestCh = 0;
        for (std::size_t i = 0; i < boxes.size(); ++i) {
            if (boxes[i].b - boxes[i].a < 2) continue;
            for (int c = 0; c < 3; ++c) {
                int lo = 255, hi = 0;
                for (std::size_t k = boxes[i].a; k < boxes[i].b; ++k) lo = std::min(lo, ch(px[k], c)), hi = std::max(hi, ch(px[k], c));
                if (hi - lo > bestRange) bestRange = hi - lo, best = (int)i, bestCh = c;
            }
        }
        if (best < 0 || bestRange == 0) break;
        Box bx = boxes[(size_t)best];
        const std::size_t mid = bx.a + (bx.b - bx.a) / 2;
        std::nth_element(px.begin() + (long)bx.a, px.begin() + (long)mid, px.begin() + (long)bx.b,
                         [&](std::uint32_t x, std::uint32_t y) { return ch(x, bestCh) < ch(y, bestCh); });
        boxes[(size_t)best] = {bx.a, mid};
        boxes.push_back({mid, bx.b});
    }
    std::vector<Rgb> pal;
    for (auto& bx : boxes) {
        if (bx.b == bx.a) continue;
        std::uint64_t s[3] = {0, 0, 0};
        for (std::size_t k = bx.a; k < bx.b; ++k)
            for (int c = 0; c < 3; ++c) s[c] += (std::uint64_t)ch(px[k], c);
        const std::uint64_t n = bx.b - bx.a;
        pal.push_back({(std::uint8_t)(s[0] / n), (std::uint8_t)(s[1] / n), (std::uint8_t)(s[2] / n)});
    }
    return pal;
}

class Lzw {
public:
    Lzw(std::vector<std::uint8_t>& out, int minCode) : out_(out), min_(minCode) {}
    void encode(const std::vector<std::uint8_t>& idx) {
        const int clear = 1 << min_, end = clear + 1;
        reset();
        put(clear);
        if (idx.empty()) {
            put(end);
            flush();
            return;
        }
        int cur = idx[0];
        for (std::size_t i = 1; i < idx.size(); ++i) {
            const std::uint32_t k = ((std::uint32_t)cur << 8) | idx[i];
            auto it = dict_.find(k);
            if (it != dict_.end()) {
                cur = it->second;
                continue;
            }
            put(cur);
            if (next_ < 4096) {
                dict_[k] = next_++;
                if (next_ > (1 << size_) && size_ < 12) ++size_;
            } else {   // the table is full: start again
                put(clear);
                reset();
            }
            cur = idx[i];
        }
        put(cur);
        put(end);
        flush();
    }

private:
    void reset() {
        dict_.clear();
        size_ = min_ + 1;
        next_ = (1 << min_) + 2;
    }
    void put(int code) {
        bits_ |= (std::uint32_t)code << nbits_;
        nbits_ += size_;
        while (nbits_ >= 8) {
            byte((std::uint8_t)(bits_ & 0xFF));
            bits_ >>= 8;
            nbits_ -= 8;
        }
    }
    void byte(std::uint8_t b) {
        block_.push_back(b);
        if (block_.size() == 255) emit();
    }
    void emit() {
        if (block_.empty()) return;
        out_.push_back((std::uint8_t)block_.size());
        out_.insert(out_.end(), block_.begin(), block_.end());
        block_.clear();
    }
    void flush() {
        if (nbits_ > 0) byte((std::uint8_t)(bits_ & 0xFF));
        bits_ = 0, nbits_ = 0;
        emit();
        out_.push_back(0);   // end of the image data
    }
    std::vector<std::uint8_t>& out_;
    int min_, size_ = 0, next_ = 0;
    std::unordered_map<std::uint32_t, int> dict_;
    std::vector<std::uint8_t> block_;
    std::uint32_t bits_ = 0;
    int nbits_ = 0;
};

void u16(std::vector<std::uint8_t>& o, int v) {
    o.push_back((std::uint8_t)(v & 0xFF));
    o.push_back((std::uint8_t)((v >> 8) & 0xFF));
}

}  // namespace

std::vector<std::uint8_t> encode(int w, int h, const std::vector<Frame>& framesIn, bool dither) {
    std::vector<std::uint8_t> out;
    if (w <= 0 || h <= 0 || w > 65535 || h > 65535 || framesIn.empty()) return out;
    const std::size_t n = (std::size_t)w * h;
    for (auto& f : framesIn)
        if (f.bgra.size() != n * 4) return out;
    // Frames that repeat the one before just lengthen it.
    std::vector<Frame> frames;
    for (auto& f : framesIn) {
        if (!frames.empty() && frames.back().bgra == f.bgra) frames.back().delayMs += f.delayMs;
        else frames.push_back(f);
    }
    // The colours (a sample of every frame's opaque-enough pixels), and whether anything is see-through.
    bool clear = false;
    std::vector<std::uint32_t> sample;
    const std::size_t step = std::max<std::size_t>(1, n * frames.size() / 400000);
    std::size_t k = 0;
    for (auto& f : frames)
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint8_t* p = &f.bgra[i * 4];
            if (p[3] < 128) {
                clear = true;
                continue;
            }
            if (k++ % step == 0) sample.push_back(((std::uint32_t)p[2] << 16) | ((std::uint32_t)p[1] << 8) | p[0]);
        }
    std::vector<Rgb> pal = sample.empty() ? std::vector<Rgb>{{0, 0, 0}} : medianCut(sample, clear ? 255 : 256);
    const int clearIndex = clear ? (int)pal.size() : -1;
    if (clear) pal.push_back({0, 0, 0});
    int bits = 1;
    while ((1 << bits) < (int)pal.size()) ++bits;
    const int tableSize = 1 << bits;
    // The nearest palette colour, remembered per 5-bit-per-channel colour.
    std::vector<std::int16_t> cache(32768, -1);
    auto nearest = [&](int r, int g, int b) {
        const int key = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
        if (cache[(size_t)key] >= 0) return (int)cache[(size_t)key];
        int best = 0, bestD = 1 << 30;
        const int nOpaque = clear ? (int)pal.size() - 1 : (int)pal.size();
        for (int i = 0; i < nOpaque; ++i) {
            const int dr = pal[(size_t)i].r - r, dg = pal[(size_t)i].g - g, db = pal[(size_t)i].b - b;
            const int d = 2 * dr * dr + 4 * dg * dg + 3 * db * db;
            if (d < bestD) bestD = d, best = i;
        }
        cache[(size_t)key] = (std::int16_t)best;
        return best;
    };
    static const int kBayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

    // Header, screen, the global palette, looping.
    const char* sig = "GIF89a";
    out.insert(out.end(), sig, sig + 6);
    u16(out, w), u16(out, h);
    out.push_back((std::uint8_t)(0x80 | ((bits - 1) << 4) | (bits - 1)));
    out.push_back(0), out.push_back(0);
    for (int i = 0; i < tableSize; ++i) {
        const Rgb c = i < (int)pal.size() ? pal[(size_t)i] : Rgb{0, 0, 0};
        out.push_back(c.r), out.push_back(c.g), out.push_back(c.b);
    }
    if (frames.size() > 1) {
        const char* app = "NETSCAPE2.0";
        out.push_back(0x21), out.push_back(0xFF), out.push_back(11);
        out.insert(out.end(), app, app + 11);
        out.push_back(3), out.push_back(1), u16(out, 0), out.push_back(0);   // loop forever
    }
    std::vector<std::uint8_t> idx(n);
    for (auto& f : frames) {
        // Graphic control: the delay, the see-through entry, and clearing to the background before the next
        // frame (so see-through parts don't keep what was there).
        const int cs = std::max(2, (int)std::lround(f.delayMs / 10.0));
        out.push_back(0x21), out.push_back(0xF9), out.push_back(4);
        out.push_back((std::uint8_t)((clear ? 2 << 2 : 1 << 2) | (clear ? 1 : 0)));
        u16(out, std::min(cs, 65535));
        out.push_back((std::uint8_t)(clear ? clearIndex : 0));
        out.push_back(0);
        // The image: the whole frame.
        out.push_back(0x2C);
        u16(out, 0), u16(out, 0), u16(out, w), u16(out, h);
        out.push_back(0);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const std::size_t i = (std::size_t)y * w + x;
                const std::uint8_t* p = &f.bgra[i * 4];
                if (p[3] < 128) {
                    idx[i] = (std::uint8_t)clearIndex;
                    continue;
                }
                int r = p[2], g = p[1], b = p[0];
                if (dither) {
                    const int d = (kBayer[y & 3][x & 3] - 8);   // -8..7: a small, steady pattern
                    r = std::clamp(r + d, 0, 255), g = std::clamp(g + d, 0, 255), b = std::clamp(b + d, 0, 255);
                }
                idx[i] = (std::uint8_t)nearest(r, g, b);
            }
        const int minCode = std::max(2, bits);
        out.push_back((std::uint8_t)minCode);
        Lzw(out, minCode).encode(idx);
    }
    out.push_back(0x3B);
    return out;
}

}  // namespace ncap::gif
