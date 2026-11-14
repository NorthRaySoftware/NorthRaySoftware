#include "nrf/exif.hpp"

#include <cstdint>

namespace nrf {

namespace {

struct Reader {
    const unsigned char* p;
    std::size_t n;
    bool little;
    bool u16(std::size_t at, unsigned& out) const {
        if (at + 2 > n || at + 2 < at) return false;
        out = little ? (unsigned)(p[at] | (p[at + 1] << 8)) : (unsigned)((p[at] << 8) | p[at + 1]);
        return true;
    }
    bool u32(std::size_t at, std::uint32_t& out) const {
        if (at + 4 > n || at + 4 < at) return false;
        out = little ? (std::uint32_t)p[at] | ((std::uint32_t)p[at + 1] << 8) | ((std::uint32_t)p[at + 2] << 16) | ((std::uint32_t)p[at + 3] << 24)
                     : ((std::uint32_t)p[at] << 24) | ((std::uint32_t)p[at + 1] << 16) | ((std::uint32_t)p[at + 2] << 8) | (std::uint32_t)p[at + 3];
        return true;
    }
};

int fromTiff(const unsigned char* t, std::size_t n) {
    if (n < 8) return 1;
    Reader r{t, n, false};
    if (t[0] == 'I' && t[1] == 'I') r.little = true;
    else if (!(t[0] == 'M' && t[1] == 'M')) return 1;
    unsigned magic = 0;
    std::uint32_t ifd = 0;
    if (!r.u16(2, magic) || magic != 42 || !r.u32(4, ifd)) return 1;
    unsigned count = 0;
    if (!r.u16(ifd, count)) return 1;
    for (unsigned i = 0; i < count && i < 512; ++i) {
        const std::size_t e = (std::size_t)ifd + 2 + (std::size_t)i * 12;
        unsigned tag = 0, type = 0, value = 0;
        if (!r.u16(e, tag) || !r.u16(e + 2, type)) return 1;
        if (tag == 274) {
            if (type != 3 || !r.u16(e + 8, value)) return 1;   // SHORT
            return value >= 1 && value <= 8 ? (int)value : 1;
        }
    }
    return 1;
}

}  // namespace

void applyOrientation(std::vector<unsigned char>& px, int& w, int& h, int o) {
    if (o < 2 || o > 8 || w <= 0 || h <= 0 || px.size() != (std::size_t)w * (std::size_t)h * 4) return;
    const bool swap = o >= 5;
    const int nw = swap ? h : w, nh = swap ? w : h;
    std::vector<unsigned char> out(px.size());
    for (int y = 0; y < nh; ++y)
        for (int x = 0; x < nw; ++x) {
            // Where the shown pixel (x, y) comes from in the stored picture.
            int sx = 0, sy = 0;
            switch (o) {
                case 2: sx = w - 1 - x; sy = y; break;              // mirrored
                case 3: sx = w - 1 - x; sy = h - 1 - y; break;      // upside down
                case 4: sx = x; sy = h - 1 - y; break;              // mirrored, upside down
                case 5: sx = y; sy = x; break;                      // transposed
                case 6: sx = y; sy = h - 1 - x; break;              // turn clockwise
                case 7: sx = w - 1 - y; sy = h - 1 - x; break;      // transversed
                case 8: sx = w - 1 - y; sy = x; break;              // turn anticlockwise
            }
            const std::size_t from = ((std::size_t)sy * (std::size_t)w + (std::size_t)sx) * 4;
            const std::size_t to = ((std::size_t)y * (std::size_t)nw + (std::size_t)x) * 4;
            for (int k = 0; k < 4; ++k) out[to + k] = px[from + k];
        }
    px.swap(out);
    w = nw;
    h = nh;
}

int jpegOrientation(const unsigned char* d, std::size_t n) {
    if (!d || n < 4 || d[0] != 0xFF || d[1] != 0xD8) return 1;
    std::size_t i = 2;
    while (i + 4 <= n) {
        if (d[i] != 0xFF) return 1;
        const unsigned char marker = d[i + 1];
        if (marker == 0xFF) { ++i; continue; }                       // padding
        if (marker == 0xD9 || marker == 0xDA) return 1;              // end, or the picture itself
        if (marker >= 0xD0 && marker <= 0xD7) { i += 2; continue; }  // no length
        const std::size_t len = ((std::size_t)d[i + 2] << 8) | d[i + 3];
        if (len < 2) return 1;
        const std::size_t body = i + 4, bodyLen = len - 2;
        if (marker == 0xE1 && bodyLen >= 6 && body + 6 <= n && d[body] == 'E' && d[body + 1] == 'x' && d[body + 2] == 'i' &&
            d[body + 3] == 'f' && d[body + 4] == 0 && d[body + 5] == 0) {
            const std::size_t start = body + 6;
            std::size_t avail = start < n ? n - start : 0;
            if (bodyLen - 6 < avail) avail = bodyLen - 6;
            return fromTiff(d + start, avail);
        }
        i += 2 + len;
    }
    return 1;
}

}  // namespace nrf
