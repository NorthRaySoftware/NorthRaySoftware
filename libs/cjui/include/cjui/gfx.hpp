// Thin wrapper over Direct2D + DirectWrite. All coordinates are in DIPs
// (device-independent pixels); the render target's DPI handles scaling.
#pragma once

#include <cstdint>

#include <d2d1.h>
#include <wincodec.h>
#include <dwrite.h>
#include <windows.h>

#include <map>
#include <unordered_map>
#include <string>
#include <vector>

namespace cjui {

struct Color {
    float r = 0, g = 0, b = 0, a = 1;
    Color withAlpha(float alpha) const { return {r, g, b, alpha}; }
    Color mix(const Color& o, float t) const {
        return {r + (o.r - r) * t, g + (o.g - g) * t, b + (o.b - b) * t, a + (o.a - a) * t};
    }
};
Color hexColor(const std::string& hex, Color fallback = {});

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    float right() const { return x + w; }
    float bottom() const { return y + h; }
    bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
    Rect inset(float d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
    Rect inset(float dx, float dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
    D2D1_RECT_F d2d() const { return D2D1::RectF(x, y, x + w, y + h); }
};

enum class Font {
    Body,         // 14 regular
    BodyStrong,   // 14 semibold
    Small,        // 12.5 regular
    SmallStrong,  // 12.5 semibold
    Label,        // 11 semibold, used upper-case for section labels
    Heading,      // 17 semibold
    Title,        // 26 semibold display
    Status,       // 22 semibold display, radio status
    Wordmark,     // 19 bold display
    WordmarkLight,// 19 regular display
    Numeric,      // 13 regular, tabular figures where the font supports it
    Monogram,     // 30 bold display
    Serif,        // 15 book serif (Georgia / Cambria): book titles on covers and shelves
    SerifTitle,   // 28 book serif, page titles in the Writing Studio
    SerifSmall,   // 12.5 book serif italic, cover subtitles and authors
};

enum class Align { Left, Center, Right };

struct TextOpts {
    Align align = Align::Left;
    bool wrap = true;
    bool ellipsis = false;
    bool centerVertically = false;
    bool strike = false;
};

std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& w);

class Gfx {
public:
    bool init(HWND hwnd);
    void shutdown();
    void resize(UINT pxWidth, UINT pxHeight);
    void setDpi(float dpi);   // the display's DPI times the interface zoom
    float dpiScale() const { return dpi_ / 96.0f; }

    bool begin();   // false if the target couldn't be created
    void end();

    void clear(Color c);
    void fill(const Rect& r, Color c);
    void fillRounded(const Rect& r, float radius, Color c);
    // Gradients (the NorthRay aurora): linear from `a` to `b` across the rect
    // (top to bottom, or left to right), and a soft radial glow fading to clear.
    void fillGradient(const Rect& r, float radius, Color a, Color b, bool vertical = true);
    void glow(float cx, float cy, float rx, float ry, Color c);
    void stroke(const Rect& r, Color c, float width = 1.0f);
    void strokeRounded(const Rect& r, float radius, Color c, float width = 1.0f);
    void line(float x1, float y1, float x2, float y2, Color c, float width = 1.0f);
    void circle(float cx, float cy, float radius, Color c, bool filled, float width = 1.0f);
    void arc(float cx, float cy, float radius, float a0, float a1, Color c, float width);
    void bookmark(const Rect& r, Color c);
    void fillTriangle(float x1, float y1, float x2, float y2, float x3, float y3, Color c);  // ribbon with a V-notch at the bottom
    void curve(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, Color c, float width);

    // Draws text and returns the height used.
    float text(const std::wstring& s, const Rect& r, Font f, Color c, TextOpts o = {});
    float measureHeight(const std::wstring& s, float width, Font f);
    float measureWidth(const std::wstring& s, Font f);
    // Text at any size (DIPs, 8-200) in the UI face, regular or semibold: for big reading text such as a
    // teleprompter. Returns the height used.
    float textSized(const std::wstring& s, const Rect& r, float size, bool strong, Color c, TextOpts o = {});
    float measureSized(const std::wstring& s, float width, float size, bool strong);

    // Images (PNG, JPEG, GIF, BMP, TIFF, WebP where Windows has the codec), decoded
    // with WIC and cached per path. `cover` fills dst and crops the overflow
    // (centre); otherwise the whole image fits inside dst. Returns false if the
    // file can't be read; nothing is drawn then.
    bool drawImage(const std::wstring& path, const Rect& dst, bool cover = true, float opacity = 1.0f);
    // The same, filling a rounded rectangle (a circle when `radius` is half the side): portraits in lists (0.87).
    bool drawImageRounded(const std::wstring& path, const Rect& dst, float radius, float opacity = 1.0f);
    // Pixel size of the original file (0x0 if unreadable).
    std::pair<unsigned, unsigned> imageSize(const std::wstring& path);
    void forgetImage(const std::wstring& path);
    // Pixels made by the program (straight RGBA), drawn into dst and kept under
    // `key` until forgetImage(key). Opaque unless `useAlpha` (then the alpha
    // channel is kept, e.g. for site icons). Returns false if the bitmap couldn't be made.
    bool drawPixels(const std::wstring& key, int w, int h, const std::uint8_t* rgba, const Rect& dst, bool useAlpha = false, float opacity = 1.0f);

    void pushClip(const Rect& r);
    void popClip();
    // Drawing scaled about (ox, oy) by `scale` and clipped to `area` (window DIPs) until endZoom (suite 0.173): what's drawn
    // between them is laid out in the zoomed space, where `area` is {ox, oy, area.w / scale, area.h / scale}.
    void beginZoom(const Rect& area, float scale);
    void endZoom();
    float zoomScale() const { return zoom_; }
    // Whether a rectangle lies wholly outside what's being drawn (the window, or the innermost clip): drawing it would
    // cost the time and show nothing (0.88).
    bool culled(const Rect& r) const;

    ID2D1HwndRenderTarget* target() const { return rt_; }

    // Where text was drawn on the previous frame (its ink, in DIPs), so decoration drawn before it
    // (the night sky's stars) can keep clear of it. Frames rarely move, so last frame's is right.
    const std::vector<Rect>& lastTextRects() const { return lastText_; }

private:
    std::vector<Rect> text_, lastText_;
    IDWriteTextFormat* format(Font f);
    IDWriteTextLayout* layout(const std::wstring& s, float w, float h, Font f, const TextOpts& o);
    IDWriteTextLayout* layoutWith(IDWriteTextFormat* tf, const std::wstring& s, float w, float h, const TextOpts& o);
    IDWriteTextFormat* sizedFormat(float size, bool strong);
    ID2D1SolidColorBrush* brush(Color c);
    bool createTarget();
    void discardTarget();

    HWND hwnd_ = nullptr;
    float dpi_ = 96;
    float zoom_ = 1;
    bool zoomed_ = false;
    float zoomX_ = 0, zoomY_ = 0;
    size_t zoomClips_ = 0;   // clips_ before beginZoom
    ID2D1Factory* d2d_ = nullptr;
    IDWriteFactory* dw_ = nullptr;
    ID2D1HwndRenderTarget* rt_ = nullptr;
    ID2D1SolidColorBrush* brush_ = nullptr;
    std::map<Font, IDWriteTextFormat*> formats_;
    std::map<int, IDWriteTextFormat*> sized_;   // by size in tenths, negative for semibold
    // `small` (0.87): a copy at most 192 px on its long side, drawn when the picture is shown small (a portrait in a
    // list), so a page of them isn't scaled down from full-size bitmaps every frame.
    struct CachedImage { ID2D1Bitmap* bitmap = nullptr; ID2D1Bitmap* small = nullptr; unsigned w = 0, h = 0; bool failed = false; };
    ID2D1Bitmap* bitmapFor(CachedImage& img, const Rect& dst);
    std::map<std::wstring, CachedImage> images_;
    IWICImagingFactory* wic_ = nullptr;
    CachedImage* loadImage(const std::wstring& path);
    IDWriteInlineObject* ellipsis_ = nullptr;
    IDWriteTextFormat* ellipsisOwner_ = nullptr;
    // Text layouts kept between frames (0.87): laying text out (shaping, line breaks) was most of a page's time, and a
    // page draws the same words frame after frame. Keyed by format, words, box and options; dropped when not used for a
    // few frames, and all of them when a format goes.
    struct LayoutKey {
        IDWriteTextFormat* tf;
        std::wstring s;
        int w, h, flags;
        bool operator==(const LayoutKey& o) const { return tf == o.tf && w == o.w && h == o.h && flags == o.flags && s == o.s; }
    };
    struct LayoutKeyHash {
        size_t operator()(const LayoutKey& k) const {
            return std::hash<std::wstring>()(k.s) ^ (std::hash<void*>()(k.tf) * 31) ^ ((size_t)k.w * 1315423911u) ^ ((size_t)k.h << 7) ^ (size_t)k.flags;
        }
    };
    struct CachedLayout { IDWriteTextLayout* tl = nullptr; unsigned long long used = 0; };
    std::unordered_map<LayoutKey, CachedLayout, LayoutKeyHash> layouts_;
    unsigned long long frame_ = 0;
    std::vector<Rect> clips_;   // the clips pushed this frame, each the intersection with the one before
    Rect view_{0, 0, 1e9f, 1e9f};
    void dropLayouts();
};

}  // namespace cjui
