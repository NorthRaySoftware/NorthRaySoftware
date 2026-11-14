#include "nrf/art.hpp"

#include <algorithm>
#include <cstring>

namespace nrf::art {

namespace {

struct Reader {
    const unsigned char* d;
    std::size_t n;
    std::uint64_t at = 0;
    bool fail = false;
    bool has(std::uint64_t k) const { return !fail && at + k <= n; }
    std::uint64_t be(int bytes) {
        if (!has((std::uint64_t)bytes)) { fail = true; return 0; }
        std::uint64_t v = 0;
        for (int i = 0; i < bytes; ++i) v = (v << 8) | d[at + (std::uint64_t)i];
        at += (std::uint64_t)bytes;
        return v;
    }
    void skip(std::uint64_t k) {
        if (!has(k)) { fail = true; at = n; return; }
        at += k;
    }
};

}  // namespace

PsdInfo psdInfo(const unsigned char* data, std::size_t n) {
    PsdInfo info;
    Reader r{data, n};
    if (n < 26 || std::memcmp(data, "8BPS", 4) != 0) return info;
    r.skip(4);
    const auto version = r.be(2);
    if (version != 1 && version != 2) return info;
    info.psb = version == 2;
    r.skip(6);
    info.channels = (int)r.be(2);
    info.height = (std::uint32_t)r.be(4);
    info.width = (std::uint32_t)r.be(4);
    info.depth = (int)r.be(2);
    info.mode = (int)r.be(2);
    if (r.fail || info.channels < 1 || info.channels > 56 || info.width == 0 || info.height == 0) return info;
    const auto colourLen = r.be(4);
    r.skip(colourLen);
    if (r.fail) return info;
    info.resourcesLength = r.be(4);
    info.resourcesStart = r.at;
    info.ok = !r.fail;
    if (!info.ok) return info;
    // The layers come next; the flattened picture after them.
    Reader l = r;
    l.at = info.resourcesStart + info.resourcesLength;
    if (l.at > n) return info;
    const auto layersLen = l.be(info.psb ? 8 : 4);
    if (!l.fail && l.at + layersLen <= n) info.imageDataStart = l.at + layersLen;
    return info;
}

std::string psdPreviewJpeg(const unsigned char* data, std::size_t n) {
    const PsdInfo info = psdInfo(data, n);
    if (!info.ok) return {};
    Reader r{data, n};
    r.at = info.resourcesStart;
    const std::uint64_t end = std::min<std::uint64_t>(n, info.resourcesStart + info.resourcesLength);
    while (r.at + 12 <= end) {
        if (std::memcmp(data + r.at, "8BIM", 4) != 0) break;
        r.skip(4);
        const auto id = r.be(2);
        const auto nameLen = r.be(1);
        r.skip(nameLen + ((nameLen + 1) % 2));   // the name and its length byte, padded to even
        const auto size = r.be(4);
        if (r.fail || r.at + size > end) break;
        if (id == 1036 && size > 28) {
            // format, width, height, widthbytes, total size, compressed size, bits, planes; then the JPEG
            const std::uint64_t jpegAt = r.at + 28;
            const std::uint64_t jpegLen = size - 28;
            if (data[jpegAt] == 0xFF && data[jpegAt + 1] == 0xD8) return std::string((const char*)data + jpegAt, (std::size_t)jpegLen);
        }
        r.skip(size + (size % 2));
    }
    return {};
}

bool psdComposite(const unsigned char* data, std::size_t n, int maxPx, Image& out) {
    const PsdInfo info = psdInfo(data, n);
    if (!info.ok || !info.imageDataStart) return false;
    if (info.depth != 8 && info.depth != 16) return false;
    int colours = 0;
    if (info.mode == 1) colours = 1;
    else if (info.mode == 3) colours = 3;
    else if (info.mode == 4) colours = 4;
    else return false;
    if (info.channels < colours) return false;
    const std::uint64_t w = info.width, h = info.height;
    if (w * h > 1000ull * 1000 * 1000) return false;
    Reader r{data, n};
    r.at = info.imageDataStart;
    const auto compression = r.be(2);
    if (r.fail || (compression != 0 && compression != 1)) return false;
    const std::uint64_t bytesPerSample = (std::uint64_t)info.depth / 8;
    const std::uint64_t rowBytes = w * bytesPerSample;

    // Where each row of each channel starts (RLE rows are of different lengths).
    const std::uint64_t rows = (std::uint64_t)colours * h;
    std::vector<std::uint64_t> rowLen;
    if (compression == 1) {
        const int countBytes = info.psb ? 4 : 2;
        // The table covers every channel in the file, not just the colours drawn.
        const std::uint64_t allRows = (std::uint64_t)info.channels * h;
        if (!r.has(allRows * (std::uint64_t)countBytes)) return false;
        rowLen.resize((std::size_t)rows);
        for (std::uint64_t i = 0; i < allRows; ++i) {
            const auto len = r.be(countBytes);
            if (i < rows) rowLen[(std::size_t)i] = len;
        }
    }

    const double scale = std::min(1.0, (double)maxPx / (double)std::max(w, h));
    const int ow = std::max(1, (int)(w * scale + 0.5)), oh = std::max(1, (int)(h * scale + 0.5));
    std::vector<std::uint32_t> sums((std::size_t)ow * oh * 4, 0);
    std::vector<std::uint32_t> counts((std::size_t)ow * oh, 0);
    std::vector<int> colOf((std::size_t)w);
    for (std::uint64_t x = 0; x < w; ++x) colOf[(std::size_t)x] = (int)std::min<std::uint64_t>((std::uint64_t)ow - 1, x * (std::uint64_t)ow / w);
    for (std::uint64_t y = 0; y < h; ++y) {
        const std::uint64_t oy = std::min<std::uint64_t>((std::uint64_t)oh - 1, y * (std::uint64_t)oh / h);
        for (std::uint64_t x = 0; x < w; ++x) ++counts[(std::size_t)(oy * (std::uint64_t)ow) + (std::size_t)colOf[(std::size_t)x]];
    }

    std::vector<unsigned char> row((std::size_t)rowBytes);
    for (int c = 0; c < colours; ++c) {
        for (std::uint64_t y = 0; y < h; ++y) {
            if (compression == 0) {
                if (!r.has(rowBytes)) return false;
                std::memcpy(row.data(), data + r.at, (std::size_t)rowBytes);
                r.at += rowBytes;
            } else {
                // PackBits.
                const std::uint64_t len = rowLen[(std::size_t)((std::uint64_t)c * h + y)];
                if (!r.has(len)) return false;
                const unsigned char* p = data + r.at;
                const unsigned char* pe = p + len;
                std::uint64_t o = 0;
                while (p < pe && o < rowBytes) {
                    const int k = (signed char)*p++;
                    if (k >= 0) {
                        const std::uint64_t cnt = std::min<std::uint64_t>((std::uint64_t)k + 1, rowBytes - o);
                        if ((std::uint64_t)(pe - p) < cnt) return false;
                        std::memcpy(row.data() + o, p, (std::size_t)cnt);
                        p += k + 1;
                        o += cnt;
                    } else if (k != -128) {
                        if (p >= pe) return false;
                        const std::uint64_t cnt = std::min<std::uint64_t>((std::uint64_t)(1 - k), rowBytes - o);
                        std::memset(row.data() + o, *p++, (std::size_t)cnt);
                        o += cnt;
                    }
                }
                if (o < rowBytes) std::memset(row.data() + o, 0, (std::size_t)(rowBytes - o));
                r.at += len;
            }
            const std::uint64_t oy = std::min<std::uint64_t>((std::uint64_t)oh - 1, y * (std::uint64_t)oh / h);
            std::uint32_t* line = sums.data() + (std::size_t)(oy * (std::uint64_t)ow * 4);
            for (std::uint64_t x = 0; x < w; ++x) {
                const unsigned v = row[(std::size_t)(x * bytesPerSample)];   // 16-bit: the high byte
                line[(std::size_t)colOf[(std::size_t)x] * 4 + (std::size_t)c] += v;
            }
        }
    }
    out.w = ow;
    out.h = oh;
    out.rgba.assign((std::size_t)ow * oh * 4, 255);
    for (std::size_t i = 0; i < (std::size_t)ow * oh; ++i) {
        const std::uint32_t k = std::max<std::uint32_t>(1, counts[i]);
        unsigned char* px = out.rgba.data() + i * 4;
        const std::uint32_t* s = sums.data() + i * 4;
        if (colours == 1) {
            px[0] = px[1] = px[2] = (unsigned char)(s[0] / k);
        } else if (colours == 3) {
            px[0] = (unsigned char)(s[0] / k);
            px[1] = (unsigned char)(s[1] / k);
            px[2] = (unsigned char)(s[2] / k);
        } else {
            // CMYK as Photoshop stores it (255 = no ink).
            const std::uint32_t C = s[0] / k, M = s[1] / k, Y = s[2] / k, K = s[3] / k;
            px[0] = (unsigned char)(C * K / 255);
            px[1] = (unsigned char)(M * K / 255);
            px[2] = (unsigned char)(Y * K / 255);
        }
    }
    return true;
}

bool looksLikeText(const unsigned char* d, std::size_t n) {
    if (n >= 2 && ((d[0] == 0xFF && d[1] == 0xFE) || (d[0] == 0xFE && d[1] == 0xFF))) return true;
    std::size_t control = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (d[i] == 0) return false;
        if (d[i] < 32 && d[i] != '\n' && d[i] != '\r' && d[i] != '\t' && d[i] != 12 && d[i] != 27) ++control;
    }
    return control * 100 <= n;   // a few stray ones are fine
}

std::wstring decodeText(const unsigned char* d, std::size_t n, std::size_t maxLines, bool* truncated) {
    std::wstring s;
    if (n >= 2 && d[0] == 0xFF && d[1] == 0xFE) {
        for (std::size_t i = 2; i + 1 < n; i += 2) s += (wchar_t)(d[i] | (d[i + 1] << 8));
    } else if (n >= 2 && d[0] == 0xFE && d[1] == 0xFF) {
        for (std::size_t i = 2; i + 1 < n; i += 2) s += (wchar_t)((d[i] << 8) | d[i + 1]);
    } else {
        std::size_t i = (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) ? 3 : 0;
        // UTF-8 if it all decodes (a sequence cut off at the very end is allowed); else Windows-1252.
        std::wstring u;
        bool utf8 = true;
        for (std::size_t k = i; k < n && utf8;) {
            const unsigned char c = d[k];
            int extra = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
            if (extra < 0) { utf8 = false; break; }
            if (k + (std::size_t)extra >= n && extra > 0) break;   // cut off at the end
            std::uint32_t cp = extra == 0 ? c : extra == 1 ? (c & 0x1F) : extra == 2 ? (c & 0x0F) : (c & 0x07);
            for (int e = 1; e <= extra; ++e) {
                if ((d[k + (std::size_t)e] & 0xC0) != 0x80) { utf8 = false; break; }
                cp = (cp << 6) | (d[k + (std::size_t)e] & 0x3F);
            }
            if (!utf8) break;
            if (cp >= 0x10000 && sizeof(wchar_t) == 2) {
                cp -= 0x10000;
                u += (wchar_t)(0xD800 + (cp >> 10));
                u += (wchar_t)(0xDC00 + (cp & 0x3FF));
            } else {
                u += (wchar_t)cp;
            }
            k += (std::size_t)extra + 1;
        }
        if (utf8) s = std::move(u);
        else {
            static const unsigned short cp1252[32] = {0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                                      0x2039, 0x0152, 0x8D, 0x017D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                                      0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178};
            for (std::size_t k = i; k < n; ++k) s += d[k] >= 0x80 && d[k] < 0xA0 ? (wchar_t)cp1252[d[k] - 0x80] : (wchar_t)d[k];
        }
    }
    // Line ends as \n; tabs as spaces; only the first lines.
    std::wstring o;
    std::size_t lines = 0;
    bool cut = false;
    for (std::size_t k = 0; k < s.size(); ++k) {
        wchar_t c = s[k];
        if (c == L'\r') {
            if (k + 1 < s.size() && s[k + 1] == L'\n') continue;
            c = L'\n';
        }
        if (c == L'\n' && ++lines >= maxLines) { cut = k + 1 < s.size(); break; }
        if (c == L'\t') { o += L"    "; continue; }
        o += c;
    }
    if (truncated) *truncated = cut;
    return o;
}

}  // namespace nrf::art
