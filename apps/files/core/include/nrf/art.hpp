// NorthRay Files: pictures inside art files that Windows can't show by itself (0.4).
//
// Photoshop (.psd, and .psb for big documents) keeps the flattened picture after its layers (when
// it's saved with "Maximise compatibility", Photoshop's default) and usually a small JPEG preview
// among its resources. Both are read here, portably, so the rules are tested everywhere. (Procreate,
// Krita and OpenRaster files are zip archives holding a PNG; the Windows layer reads those.)
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nrf::art {

struct Image {
    int w = 0, h = 0;
    std::vector<unsigned char> rgba;   // straight alpha
};

struct PsdInfo {
    bool ok = false;
    bool psb = false;
    std::uint32_t width = 0, height = 0;
    int channels = 0, depth = 0, mode = 0;   // mode: 1 grey, 3 RGB, 4 CMYK (others aren't drawn)
    std::uint64_t resourcesStart = 0, resourcesLength = 0;
    std::uint64_t imageDataStart = 0;        // 0 when the bytes given end before it's known
};

// What the first bytes say. `n` may be just the start of the file: the header, colour data and
// resources tell the size and where the preview is; the image data's place needs the layer section's
// length too.
PsdInfo psdInfo(const unsigned char* data, std::size_t n);

// The small JPEG preview Photoshop stores (resource 1036), or "" if there isn't one in the bytes given.
std::string psdPreviewJpeg(const unsigned char* data, std::size_t n);

// The flattened picture (8- or 16-bit grey, RGB or CMYK; raw or RLE), scaled down to fit a `maxPx`
// square (each pixel the average of the ones it covers). `data` is the whole file. False if it can't.
bool psdComposite(const unsigned char* data, std::size_t n, int maxPx, Image& out);

// Text files: decoded for showing (UTF-8 with or without its mark, UTF-16 with its mark, else the
// bytes as Windows-1252). Lines past `maxLines` are left off; `truncated` says so.
std::wstring decodeText(const unsigned char* data, std::size_t n, std::size_t maxLines, bool* truncated = nullptr);
// Whether the bytes look like text at all (no NUL bytes outside UTF-16, few control characters).
bool looksLikeText(const unsigned char* data, std::size_t n);

}  // namespace nrf::art
