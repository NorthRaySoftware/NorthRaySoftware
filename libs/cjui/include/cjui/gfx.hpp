// Thin wrapper over Direct2D + DirectWrite. All coordinates are in DIPs
// (device-independent pixels); the render target's DPI handles scaling.
#pragma once

#include <cstdint>

#include <d2d1.h>
#include <wincodec.h>
#include <dwrite.h>
#include <windows.h>

#include <map>
#include <string>

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
    void setDpi(UINT dpi);
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

    // Images (PNG, JPEG, GIF, BMP, TIFF, WebP where Windows has the codec), decoded
    // with WIC and cached per path. `cover` fills dst and crops the overflow
    // (centre); otherwise the whole image fits inside dst. Returns false if the
    // file can't be read; nothing is drawn then.
    bool drawImage(const std::wstring& path, const Rect& dst, bool cover = true, float opacity = 1.0f);
    // Pixel size of the original file (0x0 if unreadable).
    std::pair<unsigned, unsigned> imageSize(const std::wstring& path);
    void forgetImage(const std::wstring& path);
    // Pixels made by the program (straight RGBA, opaque), drawn into dst and kept
    // under `key` until forgetImage(key). Returns false if the bitmap couldn't be made.
    bool drawPixels(const std::wstring& key, int w, int h, const std::uint8_t* rgba, const Rect& dst);

    void pushClip(const Rect& r);
    void popClip();

    ID2D1HwndRenderTarget* target() const { return rt_; }

private:
    IDWriteTextFormat* format(Font f);
    IDWriteTextLayout* layout(const std::wstring& s, float w, float h, Font f, const TextOpts& o);
    ID2D1SolidColorBrush* brush(Color c);
    bool createTarget();
    void discardTarget();

    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    ID2D1Factory* d2d_ = nullptr;
    IDWriteFactory* dw_ = nullptr;
    ID2D1HwndRenderTarget* rt_ = nullptr;
    ID2D1SolidColorBrush* brush_ = nullptr;
    std::map<Font, IDWriteTextFormat*> formats_;
    struct CachedImage { ID2D1Bitmap* bitmap = nullptr; unsigned w = 0, h = 0; bool failed = false; };
    std::map<std::wstring, CachedImage> images_;
    IWICImagingFactory* wic_ = nullptr;
    CachedImage* loadImage(const std::wstring& path);
    IDWriteInlineObject* ellipsis_ = nullptr;
    IDWriteTextFormat* ellipsisOwner_ = nullptr;
};

}  // namespace cjui
