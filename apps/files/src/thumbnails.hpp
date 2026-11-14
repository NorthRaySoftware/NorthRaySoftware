// NorthRay Files: pictures of files, for the Icons view and the preview pane.
//
// Decoded with Windows Imaging Component (Windows' own image codecs: JPEG, PNG,
// GIF, BMP, TIFF, ICO, and WebP, HEIF or AVIF where their Windows codecs are
// installed) on one background thread, scaled down as they're decoded, turned
// the right way up (EXIF orientation), never stretched. No third-party
// thumbnail handlers are loaded. Online-only (cloud) files are skipped, so
// browsing never downloads them, and so are files too big to be worth it.
//
// Kept in memory only (at most kMaxKept); a disk cache comes with previews.
#pragma once

#include <windows.h>

#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace nrf {

constexpr UINT WM_APP_THUMB = WM_APP + 42;   // lParam: ThumbResult*

struct Thumb {
    int w = 0, h = 0;
    std::vector<unsigned char> rgba;   // straight alpha
    bool failed = false;
    std::uint64_t serial = 0;          // a new number for each picture, for the drawing cache's key
};

struct ThumbResult {
    std::wstring key;
    Thumb thumb;
};

class Thumbnails {
public:
    explicit Thumbnails(HWND notify);
    ~Thumbnails();
    Thumbnails(const Thumbnails&) = delete;
    Thumbnails& operator=(const Thumbnails&) = delete;

    // Whether a name looks like a picture Windows can decode.
    static bool isImage(const std::wstring& name);
    // What a picture of the file would come from (0.4): Windows' image codecs, Photoshop's own
    // picture, the PNG inside a Procreate/Krita/OpenRaster file, or Windows' thumbnail handlers
    // (videos, PDFs: only when Windows has a handler for them).
    enum class PictureKind { None, Image, Photoshop, ZipArt, Shell };
    static PictureKind kindOf(const std::wstring& name);
    static bool hasPicture(const std::wstring& name) { return kindOf(name) != PictureKind::None; }

    // The picture, if it's ready (nullptr while it's being made, or when it
    // couldn't be). Asking for one that isn't ready queues it; the window gets
    // WM_APP_THUMB when it is. `px` is the size it'll be drawn at, in pixels.
    const Thumb* get(const std::wstring& path, std::uint64_t modified, std::uint64_t size, int px);
    // Whether it was tried and no picture could be made.
    bool failed(const std::wstring& path, std::uint64_t modified, std::uint64_t size, int px) const;
    void received(ThumbResult* r);   // on the UI thread, from WM_APP_THUMB
    void forgetQueue();              // a new folder: what was asked for no longer matters
    // Pictures dropped from memory since last asked: their drawing bitmaps can go too.
    std::vector<std::uint64_t> takeEvicted();

    static constexpr std::size_t kMaxKept = 600;
    static constexpr std::uint64_t kMaxBytes = 256ull * 1024 * 1024;   // larger files keep their icon

private:
    struct Shared;
    std::shared_ptr<Shared> shared_;
    std::unordered_map<std::wstring, Thumb> kept_;
    std::list<std::wstring> order_;   // most recently used first
    std::unordered_map<std::wstring, std::list<std::wstring>::iterator> where_;
    std::vector<std::uint64_t> evicted_;
    std::uint64_t serial_ = 0;
};

}  // namespace nrf
