#include "video/design_render.hpp"
#include "ncap/live.hpp"

#include <windows.h>
#include <d2d1.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <memory>
#include <mutex>

namespace ncap::video {

using Microsoft::WRL::ComPtr;
using namespace ncap::design;

namespace {

constexpr float kPi = 3.14159265358979f;

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

D2D1_COLOR_F col(const Rgba& c, float alpha = 1) { return D2D1::ColorF(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f * alpha); }

struct Ctx {
    ComPtr<ID2D1Factory> d2;
    ComPtr<IDWriteFactory> dw;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IDWriteFontCollection> fonts;   // the system's and the brand kit's font files (null: the system's)
};

bool makeCtx(Ctx& c) {
    return SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, c.d2.GetAddressOf())) &&
           SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(c.dw.GetAddressOf()))) &&
           SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&c.wic)));
}

// A brush for a fill across a box (gradients run corner to corner along their angle, like borders).
ComPtr<ID2D1Brush> brushFor(ID2D1RenderTarget* rt, const Fill& f, const D2D1_RECT_F& box, float alpha = 1) {
    ComPtr<ID2D1Brush> out;
    if (f.type == Fill::Solid) {
        ComPtr<ID2D1SolidColorBrush> b;
        if (SUCCEEDED(rt->CreateSolidColorBrush(col(f.colour, alpha), &b))) out = b;
    } else if ((f.type == Fill::Linear || f.type == Fill::Radial) && f.stops.size() >= 2) {
        // Gradients are drawn from a picture of their colours over the box (worked out with
        // design::colourAt, so they match the tests exactly, see-through stops included), at most 512
        // pixels a side and stretched smoothly to the box: no brush rotation or edge repeating needed.
        const bool radial = f.type == Fill::Radial;
        const float w = std::max(1.0f, box.right - box.left), h = std::max(1.0f, box.bottom - box.top);
        const float k = std::min(1.0f, 512.0f / std::max(w, h));
        const UINT n = (UINT)std::clamp((int)std::ceil(w * k), 2, 512), rows = (UINT)std::clamp((int)std::ceil(h * k), 2, 512);
        const float a = f.angle * kPi / 180, dx = std::cos(a), dy = std::sin(a);
        const float half = (std::fabs(dx) * w + std::fabs(dy) * h) / 2;
        std::vector<std::uint8_t> px((size_t)n * rows * 4);
        for (UINT y = 0; y < rows; ++y)
            for (UINT x = 0; x < n; ++x) {
                // The pixel's place in the box (box units, from its centre).
                const float bx = ((x + 0.5f) / n - 0.5f) * w, by = ((y + 0.5f) / rows - 0.5f) * h;
                float t;
                if (radial) {
                    const float ux = bx / (w / 2), uy = by / (h / 2);
                    t = std::sqrt(ux * ux + uy * uy);
                } else {
                    t = half > 0 ? (bx * dx + by * dy) / (2 * half) + 0.5f : 0.0f;
                }
                const Rgba c = colourAt(f, t);
                const float al = c.a / 255.0f * alpha;
                std::uint8_t* p = &px[((size_t)y * n + x) * 4];
                p[0] = (std::uint8_t)std::lround(c.b * al);
                p[1] = (std::uint8_t)std::lround(c.g * al);
                p[2] = (std::uint8_t)std::lround(c.r * al);
                p[3] = (std::uint8_t)std::lround(255 * al);
            }
        ComPtr<ID2D1Bitmap> bmp;
        if (FAILED(rt->CreateBitmap(D2D1::SizeU(n, rows), px.data(), n * 4,
                                    D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &bmp)))
            return out;
        const D2D1_MATRIX_3X2_F m = D2D1::Matrix3x2F::Scale(w / n, h / rows) * D2D1::Matrix3x2F::Translation(box.left, box.top);
        ComPtr<ID2D1BitmapBrush> b;
        if (SUCCEEDED(rt->CreateBitmapBrush(bmp.Get(),
                                            D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR),
                                            D2D1::BrushProperties(1.0f, m), &b)))
            out = b;
    }
    return out;
}

ComPtr<ID2D1Bitmap> loadPicture(Ctx& c, ID2D1RenderTarget* rt, const std::string& file) {
    ComPtr<ID2D1Bitmap> bmp;
    if (file.empty()) return bmp;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> fr;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(c.wic->CreateDecoderFromFilename(widen(file).c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &fr)) || FAILED(c.wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(fr.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut)))
        return bmp;
    rt->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &bmp);
    return bmp;
}

bool hasFamily(IDWriteFontCollection* coll, const std::wstring& name) {
    UINT32 idx = 0;
    BOOL exists = FALSE;
    return coll && SUCCEEDED(coll->FindFamilyName(name.c_str(), &idx, &exists)) && exists;
}

// A font that's there: the one asked for, or (0.17) the nearest kind that is, serif for serif, so a design made on
// another PC never loses its words.
std::wstring familyFor(Ctx& c, const std::string& font) {
    const std::wstring want = widen(font);
    ComPtr<IDWriteFontCollection> sys;
    c.dw->GetSystemFontCollection(&sys);
    if (hasFamily(c.fonts.Get(), want) || hasFamily(sys.Get(), want) || !sys) return want;
    static const wchar_t* serifs[] = {L"Georgia", L"Palatino Linotype", L"Book Antiqua", L"Cambria", L"Constantia", L"Garamond", L"Times New Roman"};
    bool serif = want.find(L"Serif") != std::wstring::npos;
    for (auto* f : serifs) serif = serif || want == f;
    static const wchar_t* serifChain[] = {L"Georgia", L"Palatino Linotype", L"Cambria", L"Times New Roman", L"Liberation Serif", L"DejaVu Serif"};
    static const wchar_t* sansChain[] = {L"Segoe UI", L"Arial", L"Tahoma", L"Liberation Sans", L"DejaVu Sans"};
    if (serif)
        for (auto* f : serifChain)
            if (hasFamily(sys.Get(), f)) return f;
    for (auto* f : sansChain)
        if (hasFamily(sys.Get(), f)) return f;
    return want;
}

ComPtr<IDWriteTextLayout> layoutFor(Ctx& c, const Element& e, const std::wstring& text) {
    ComPtr<IDWriteTextFormat> fmt;
    ComPtr<IDWriteTextLayout> lay;
    // The kit's collection only when it has the font: otherwise Windows' own, so missing fonts fall back.
    const std::wstring family = familyFor(c, e.font);
    IDWriteFontCollection* coll = hasFamily(c.fonts.Get(), family) ? c.fonts.Get() : nullptr;
    if (FAILED(c.dw->CreateTextFormat(family.c_str(), coll, e.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                      e.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, e.size, L"en-gb", &fmt)) &&
        FAILED(c.dw->CreateTextFormat(L"Segoe UI", nullptr, e.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                      e.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, e.size, L"en-gb", &fmt)))
        return lay;
    fmt->SetTextAlignment(e.align == "centre" ? DWRITE_TEXT_ALIGNMENT_CENTER : e.align == "right" ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    c.dw->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt.Get(), e.w, e.h, &lay);
    return lay;
}

// Pictures with filters (0.16): decoded to pixels once per file (and its modification time), and filtered once per
// file, filters and blur size; a few of each are kept, since designs redraw often (timers, chat) and photos are big.
struct Pixels {
    std::vector<std::uint8_t> rgba;   // straight alpha
    int w = 0, h = 0;
};
std::string fileStamp(const std::string& file) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(widen(file).c_str(), GetFileExInfoStandard, &a)) return {};
    return std::to_string(((std::uint64_t)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime) + ":" +
           std::to_string(((std::uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow);
}
std::mutex gPixelsM;
std::vector<std::pair<std::string, std::shared_ptr<const Pixels>>> gDecoded, gFiltered;   // newest last
std::shared_ptr<const Pixels> cached(std::vector<std::pair<std::string, std::shared_ptr<const Pixels>>>& list, const std::string& key) {
    std::lock_guard<std::mutex> l(gPixelsM);
    for (auto& [k, v] : list)
        if (k == key) return v;
    return nullptr;
}
void keep(std::vector<std::pair<std::string, std::shared_ptr<const Pixels>>>& list, const std::string& key, std::shared_ptr<const Pixels> p) {
    std::lock_guard<std::mutex> l(gPixelsM);
    list.push_back({key, std::move(p)});
    if (list.size() > 6) list.erase(list.begin());
}
std::shared_ptr<const Pixels> decodePicture(Ctx& c, const std::string& file, const std::string& stamp) {
    const std::string key = file + "|" + stamp;
    if (auto p = cached(gDecoded, key)) return p;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> fr;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(c.wic->CreateDecoderFromFilename(widen(file).c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &fr)) || FAILED(c.wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(fr.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut)))
        return nullptr;
    UINT w = 0, h = 0;
    if (FAILED(conv->GetSize(&w, &h)) || w == 0 || h == 0 || (std::uint64_t)w * h > 64ull * 1024 * 1024) return nullptr;
    auto p = std::make_shared<Pixels>();
    p->w = (int)w, p->h = (int)h;
    p->rgba.resize((size_t)w * h * 4);
    if (FAILED(conv->CopyPixels(nullptr, w * 4, (UINT)p->rgba.size(), p->rgba.data()))) return nullptr;
    keep(gDecoded, key, p);
    return p;
}
// The filtered picture as a bitmap; `shown` is how many page pixels one picture pixel covers (for the blur's size).
ComPtr<ID2D1Bitmap> filteredPicture(Ctx& c, ID2D1RenderTarget* rt, const std::string& file, const Element& e) {
    ComPtr<ID2D1Bitmap> bmp;
    const std::string stamp = fileStamp(file);
    auto src = decodePicture(c, file, stamp);
    if (!src) return bmp;
    // How big the picture shows on the page, so the blur is in page pixels whatever the picture's size.
    float shown;
    if (e.cover) {
        const Area a = pictureArea((float)src->w, (float)src->h, e.w, e.h, e.picZoom, e.picX, e.picY);
        shown = a.w > 0 ? e.w / a.w : 1;
    } else {
        shown = std::min(e.w / src->w, e.h / src->h);
    }
    const float blurPx = shown > 0 ? e.filters.blur / shown : 0;
    const Filters& f = e.filters;
    char key[256];
    snprintf(key, sizeof key, "|%.1f,%.1f,%.1f,%.1f,%d,%d,%d,%.1f|%.0f", f.brightness, f.contrast, f.saturation, f.blur, f.tint.r, f.tint.g, f.tint.b,
             f.tintAmount, std::round(blurPx));
    const std::string fkey = file + "|" + stamp + key;
    auto done = cached(gFiltered, fkey);
    if (!done) {
        auto p = std::make_shared<Pixels>(*src);
        applyFilters(p->rgba, p->w, p->h, f, blurPx);
        keep(gFiltered, fkey, p);
        done = p;
    }
    std::vector<std::uint8_t> bgra(done->rgba.size());   // premultiplied BGRA for Direct2D
    for (size_t i = 0; i < bgra.size(); i += 4) {
        const unsigned a = done->rgba[i + 3];
        bgra[i] = (std::uint8_t)(done->rgba[i + 2] * a / 255);
        bgra[i + 1] = (std::uint8_t)(done->rgba[i + 1] * a / 255);
        bgra[i + 2] = (std::uint8_t)(done->rgba[i] * a / 255);
        bgra[i + 3] = (std::uint8_t)a;
    }
    rt->CreateBitmap(D2D1::SizeU((UINT)done->w, (UINT)done->h), bgra.data(), (UINT)done->w * 4,
                     D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &bmp);
    return bmp;
}

// The shape a picture is cut to (0.16): its mask's outline, or the box with its rounded corners.
ComPtr<ID2D1Geometry> pictureShape(Ctx& c, const Element& e, const D2D1_RECT_F& at, float r) {
    ComPtr<ID2D1Geometry> out;
    const float x0 = at.left, y0 = at.top, w = at.right - at.left, h = at.bottom - at.top;
    const auto pts = maskOutline(e.mask, w, h);
    if (pts.size() >= 3) {
        ComPtr<ID2D1PathGeometry> path;
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(c.d2->CreatePathGeometry(&path)) && SUCCEEDED(path->Open(&sink))) {
            sink->BeginFigure(D2D1::Point2F(x0 + pts[0].first, y0 + pts[0].second), D2D1_FIGURE_BEGIN_FILLED);
            for (size_t i = 1; i < pts.size(); ++i) sink->AddLine(D2D1::Point2F(x0 + pts[i].first, y0 + pts[i].second));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            if (SUCCEEDED(sink->Close())) out = path;
        }
        if (out) return out;
    }
    ComPtr<ID2D1RoundedRectangleGeometry> rr;
    r = std::min(r, std::min(w, h) / 2);
    if (SUCCEEDED(c.d2->CreateRoundedRectangleGeometry(D2D1::RoundedRect(at, r, r), &rr))) out = rr;
    return out;
}

// Text as one outline shape (0.15): a text renderer that, instead of drawing, writes each run of glyphs into one path at
// its place. Outlines then follow the letters exactly (DrawGeometry with round joins) and a gradient can be fitted to the
// words' own bounds. Null if DirectWrite or Direct2D can't (the caller falls back to drawing text).
//
// GetGlyphRunOutline draws each run at the origin, so a small sink in between moves its points to the run's place.
class MovedSink final : public ID2D1SimplifiedGeometrySink {
public:
    MovedSink(ID2D1SimplifiedGeometrySink* to, float dx, float dy) : to_(to), dx_(dx), dy_(dy) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (id == __uuidof(IUnknown) || id == __uuidof(ID2D1SimplifiedGeometrySink)) {
            *out = static_cast<ID2D1SimplifiedGeometrySink*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }   // lives on the stack
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    void STDMETHODCALLTYPE SetFillMode(D2D1_FILL_MODE) override {}
    void STDMETHODCALLTYPE SetSegmentFlags(D2D1_PATH_SEGMENT f) override { to_->SetSegmentFlags(f); }
    void STDMETHODCALLTYPE BeginFigure(D2D1_POINT_2F p, D2D1_FIGURE_BEGIN b) override { to_->BeginFigure(D2D1::Point2F(p.x + dx_, p.y + dy_), b); }
    void STDMETHODCALLTYPE AddLines(const D2D1_POINT_2F* pts, UINT32 n) override {
        std::vector<D2D1_POINT_2F> m(pts, pts + n);
        for (auto& q : m) q.x += dx_, q.y += dy_;
        to_->AddLines(m.data(), n);
    }
    void STDMETHODCALLTYPE AddBeziers(const D2D1_BEZIER_SEGMENT* bz, UINT32 n) override {
        std::vector<D2D1_BEZIER_SEGMENT> m(bz, bz + n);
        for (auto& b : m) {
            b.point1.x += dx_, b.point1.y += dy_;
            b.point2.x += dx_, b.point2.y += dy_;
            b.point3.x += dx_, b.point3.y += dy_;
        }
        to_->AddBeziers(m.data(), n);
    }
    void STDMETHODCALLTYPE EndFigure(D2D1_FIGURE_END e) override { to_->EndFigure(e); }
    HRESULT STDMETHODCALLTYPE Close() override { return S_OK; }   // the real sink is closed once, after every run

private:
    ID2D1SimplifiedGeometrySink* to_;
    float dx_, dy_;
};

class OutlineCollector final : public IDWriteTextRenderer {
public:
    explicit OutlineCollector(ID2D1GeometrySink* sink) : sink_(sink) {}
    int runs = 0;
    bool failed = false;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (id == __uuidof(IUnknown) || id == __uuidof(IDWritePixelSnapping) || id == __uuidof(IDWriteTextRenderer)) {
            *out = static_cast<IDWriteTextRenderer*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }   // lives on the stack
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* no) override {
        *no = TRUE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* m) override {
        *m = DWRITE_MATRIX{1, 0, 0, 1, 0, 0};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* p) override {
        *p = 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE, const DWRITE_GLYPH_RUN* run, const DWRITE_GLYPH_RUN_DESCRIPTION*,
                                           IUnknown*) override {
        if (!run || !run->fontFace || run->glyphCount == 0) return S_OK;
        MovedSink moved(sink_, x, y);
        if (FAILED(run->fontFace->GetGlyphRunOutline(run->fontEmSize, run->glyphIndices, run->glyphAdvances, run->glyphOffsets, run->glyphCount,
                                                     run->isSideways, run->bidiLevel % 2, &moved)))
            failed = true;
        else ++runs;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override { return S_OK; }

private:
    ID2D1GeometrySink* sink_;
};

ComPtr<ID2D1Geometry> textGeometry(Ctx& c, IDWriteTextLayout* lay, float x, float y) {
    ComPtr<ID2D1Geometry> out;
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(c.d2->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return out;
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
    OutlineCollector col(sink.Get());
    const HRESULT drawn = lay->Draw(nullptr, &col, x, y);
    if (FAILED(sink->Close()) || FAILED(drawn) || col.failed || col.runs == 0) return out;
    out = path;
    return out;
}

// One element's shape, in its own box at (e.x, e.y), with the brush given (the shadow pass uses black).
void drawShape(Ctx& c, ID2D1RenderTarget* rt, const Element& e, const Placeholders& values, float alpha, bool shadowPass, bool showMissing,
               const Clock& clock) {
    const D2D1_RECT_F box = D2D1::RectF(e.x, e.y, e.x + e.w, e.y + e.h);
    ComPtr<ID2D1SolidColorBrush> black;
    if (shadowPass) rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &black);
    ComPtr<ID2D1Brush> fill = shadowPass ? ComPtr<ID2D1Brush>(black) : brushFor(rt, e.fill, box, alpha);
    ComPtr<ID2D1SolidColorBrush> stroke;
    if (e.strokeWidth > 0 && e.stroke.a > 0) {
        if (shadowPass) stroke = black;
        else rt->CreateSolidColorBrush(col(e.stroke, alpha), &stroke);
    }
    const float r = std::min(e.radius, std::min(e.w, e.h) / 2);
    switch (e.kind) {
        case Kind::Rect: {
            const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(box, r, r);
            if (fill && e.fill.type != Fill::None) rt->FillRoundedRectangle(rr, fill.Get());
            if (stroke) {
                const float s = e.strokeWidth / 2;
                rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(box.left + s, box.top + s, box.right - s, box.bottom - s), std::max(0.0f, r - s),
                                                           std::max(0.0f, r - s)),
                                         stroke.Get(), e.strokeWidth);
            }
            break;
        }
        case Kind::Ellipse: {
            const D2D1_ELLIPSE el = D2D1::Ellipse(D2D1::Point2F(e.x + e.w / 2, e.y + e.h / 2), e.w / 2, e.h / 2);
            if (fill && e.fill.type != Fill::None) rt->FillEllipse(el, fill.Get());
            if (stroke) rt->DrawEllipse(D2D1::Ellipse(el.point, std::max(0.0f, el.radiusX - e.strokeWidth / 2), std::max(0.0f, el.radiusY - e.strokeWidth / 2)),
                                        stroke.Get(), e.strokeWidth);
            break;
        }
        case Kind::Timer:
        case Kind::Text: {
            const std::wstring t = widen(e.kind == Kind::Timer ? fillPlaceholders(timerText(e, clock).text, values, showMissing)
                                                               : fillPlaceholders(e.text, values, showMissing));
            ComPtr<IDWriteTextLayout> lay = layoutFor(c, e, t.empty() ? L" " : t);
            if (!lay) break;
            if (stroke) {   // an outline without its own picture (see renderDesign): the text drawn around itself, under the letters
                const int steps = std::max(12, (int)(e.strokeWidth * 4));
                for (int i = 0; i < steps; ++i) {
                    const float a = 2 * kPi * i / steps;
                    rt->DrawTextLayout(D2D1::Point2F(e.x + std::cos(a) * e.strokeWidth, e.y + std::sin(a) * e.strokeWidth), lay.Get(), stroke.Get());
                }
            }
            if (e.fill.type == Fill::None || !fill) break;
            // The letters as shapes (0.15), so a gradient spans the words themselves rather than the whole box.
            if (ComPtr<ID2D1Geometry> g = textGeometry(c, lay.Get(), e.x, e.y)) {
                ComPtr<ID2D1Brush> words = fill;
                D2D1_RECT_F ink;
                if (!shadowPass && e.fill.type != Fill::Solid && SUCCEEDED(g->GetBounds(nullptr, &ink)) && ink.right > ink.left && ink.bottom > ink.top)
                    words = brushFor(rt, e.fill, ink, alpha);
                if (words) rt->FillGeometry(g.Get(), words.Get());
                break;
            }
            rt->DrawTextLayout(D2D1::Point2F(e.x, e.y), lay.Get(), fill.Get());
            break;
        }
        case Kind::Chat: {
            // The newest messages at the bottom, older ones above until the box is full; each on its own
            // rounded card (optional), the name in the viewer's colour.
            std::vector<chat::Message> all = designChat();
            const std::int64_t now = (std::int64_t)GetTickCount64();
            if (all.empty() && showMissing) all = chat::samples(now);   // the designer: something to see
            const auto msgs = chat::recent(all, (std::size_t)std::max(1, e.chatCount), e.chatFade, now);
            const float pad = e.chatBubbles ? e.size * 0.45f : 0, padY = e.chatBubbles ? e.size * 0.3f : 0, gap = e.size * 0.3f;
            const float radius = e.size * 0.55f;
            ComPtr<ID2D1SolidColorBrush> card;
            if (e.chatBubbles && e.chatBubble.a) rt->CreateSolidColorBrush(shadowPass ? D2D1::ColorF(0, 0, 0, alpha) : col(e.chatBubble, alpha), &card);
            const Brand brand = designBrand();
            const BrandColour* main = brandColour(brand, "primary");
            rt->PushAxisAlignedClip(box, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            float bottom = e.y + e.h;
            for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
                const std::wstring name = widen(it->name), body = widen(it->text);
                const std::wstring t = name + (it->action ? L" " : L": ") + body;
                // Which side: the chosen one, or (mine) yours on the right and viewers' on the left; none chosen
                // follows Align, as before. Placed bubbles are as wide as their message (at most 80% of the box when
                // they alternate), the text starting at the bubble's left.
                const bool placed = !e.chatSide.empty();
                const bool mine = std::find(it->badges.begin(), it->badges.end(), "broadcaster") != it->badges.end();
                const bool onRight = e.chatSide == "right" || (e.chatSide == "mine" && mine);
                Element lay = e;
                lay.w = std::max(10.f, (e.chatSide == "mine" ? e.w * 0.8f : e.w) - 2 * pad);
                lay.h = 100000;
                if (placed) lay.align = "left";
                ComPtr<IDWriteTextLayout> L = layoutFor(c, lay, t);
                if (!L) break;
                // The name: bold, in the viewer's colour (made lighter if it would be hard to read).
                Rgba nc = e.chatNameColours ? (it->colour.a ? it->colour : chat::defaultColour(it->login)) : (main ? main->colour : Rgba{63, 207, 142, 255});
                const float lum = (0.2126f * nc.r + 0.7152f * nc.g + 0.0722f * nc.b) / 255.f;
                // On light cards (parchment, 0.17) names are darkened instead, like coloured inks.
                const Rgba bc = e.chatBubble;
                const bool lightCards = e.chatBubbles && bc.a > 120 && (0.2126f * bc.r + 0.7152f * bc.g + 0.0722f * bc.b) / 255.f > 0.6f;
                if (lightCards) {
                    float l = lum;
                    for (int k = 0; k < 6 && l > 0.32f; ++k) {
                        nc = {(std::uint8_t)(nc.r * 0.7f), (std::uint8_t)(nc.g * 0.7f), (std::uint8_t)(nc.b * 0.7f), 255};
                        l = (0.2126f * nc.r + 0.7152f * nc.g + 0.0722f * nc.b) / 255.f;
                    }
                } else if (lum < 0.45f)
                    nc = {(std::uint8_t)(nc.r + (255 - nc.r) * 0.45f), (std::uint8_t)(nc.g + (255 - nc.g) * 0.45f), (std::uint8_t)(nc.b + (255 - nc.b) * 0.45f), 255};
                ComPtr<ID2D1SolidColorBrush> nb;
                rt->CreateSolidColorBrush(shadowPass ? D2D1::ColorF(0, 0, 0, alpha) : col(nc, alpha), &nb);
                const DWRITE_TEXT_RANGE nameRange{0, (UINT32)(it->action ? t.size() : name.size())};
                L->SetDrawingEffect(nb.Get(), nameRange);
                L->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_RANGE{0, (UINT32)name.size()});
                if (it->action) L->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, DWRITE_TEXT_RANGE{0, (UINT32)t.size()});
                DWRITE_TEXT_METRICS m{};
                L->GetMetrics(&m);
                const float h = m.height + 2 * padY;
                const float top = bottom - h;
                if (top < e.y && it != msgs.rbegin()) break;   // no room for this one (the newest always shows)
                const float bw = std::min(e.w, m.widthIncludingTrailingWhitespace + 2 * pad + 1);
                float left = e.x;
                if (placed ? onRight : e.align == "right") left = e.x + e.w - bw;
                else if (!placed && e.align == "centre") left = e.x + (e.w - bw) / 2;
                if (card) rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(left, top, left + bw, bottom), radius, radius), card.Get());
                if (fill && e.fill.type != Fill::None) rt->DrawTextLayout(D2D1::Point2F((placed ? left : e.x) + pad, top + padY), L.Get(), fill.Get());
                bottom = top - gap;
            }
            rt->PopAxisAlignedClip();
            break;
        }
        case Kind::Qr: {
            // Dark squares on a light square with a quiet border (fill: the light; outline colour: the dark).
            const auto m = qrModules(fillPlaceholders(e.qr, values, false));
            if (m.empty()) {
                ComPtr<ID2D1SolidColorBrush> ph;
                rt->CreateSolidColorBrush(D2D1::ColorF(0.5f, 0.55f, 0.62f, 0.35f * alpha), &ph);
                rt->FillRectangle(box, ph.Get());
                break;
            }
            const float side = std::min(e.w, e.h), quiet = 2;
            const float cell = side / (m.size() + 2 * quiet);
            const float ox = e.x + (e.w - side) / 2 + quiet * cell, oy = e.y + (e.h - side) / 2 + quiet * cell;
            if (fill && e.fill.type != Fill::None)
                rt->FillRectangle(D2D1::RectF(e.x + (e.w - side) / 2, e.y + (e.h - side) / 2, e.x + (e.w + side) / 2, e.y + (e.h + side) / 2), fill.Get());
            ComPtr<ID2D1SolidColorBrush> dark;
            if (shadowPass) dark = black;
            else rt->CreateSolidColorBrush(e.stroke.a ? col(e.stroke, alpha) : D2D1::ColorF(0.04f, 0.07f, 0.12f, alpha), &dark);
            const D2D1_ANTIALIAS_MODE was = rt->GetAntialiasMode();
            rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);   // crisp squares that touch
            for (std::size_t y = 0; y < m.size(); ++y)
                for (std::size_t x = 0; x < m.size(); ++x)
                    if (m[y][x]) rt->FillRectangle(D2D1::RectF(ox + x * cell, oy + y * cell, ox + (x + 1) * cell + 0.01f, oy + (y + 1) * cell + 0.01f), dark.Get());
            rt->SetAntialiasMode(was);
            break;
        }
        case Kind::Icon: {
            const Icon* ic = iconById(e.icon);
            if (!ic) break;
            ComPtr<ID2D1PathGeometry> g;
            ComPtr<ID2D1GeometrySink> sink;
            if (FAILED(c.d2->CreatePathGeometry(&g)) || FAILED(g->Open(&sink))) break;
            sink->SetFillMode(D2D1_FILL_MODE_WINDING);
            const float side = std::min(e.w, e.h), k = side / 24, ox = e.x + (e.w - side) / 2, oy = e.y + (e.h - side) / 2;
            for (const auto& f : iconFigures(ic->path)) {
                if (f.size() < 3) continue;
                sink->BeginFigure(D2D1::Point2F(ox + f[0].first * k, oy + f[0].second * k), D2D1_FIGURE_BEGIN_FILLED);
                for (std::size_t i = 1; i < f.size(); ++i) sink->AddLine(D2D1::Point2F(ox + f[i].first * k, oy + f[i].second * k));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            }
            sink->Close();
            if (fill && e.fill.type != Fill::None) rt->FillGeometry(g.Get(), fill.Get());
            if (stroke) rt->DrawGeometry(g.Get(), stroke.Get(), e.strokeWidth);
            break;
        }
        case Kind::Image: {
            // A value can name the file ("{cover}": the book Writing Studio says is being written).
            const std::string file = e.file.find('{') == std::string::npos ? e.file : fillPlaceholders(e.file, values, false);
            ComPtr<ID2D1Bitmap> bmp;
            if (!file.empty()) bmp = e.filters.any() ? filteredPicture(c, rt, file, e) : loadPicture(c, rt, file);
            if (!bmp && file.empty() && !showMissing) break;   // e.g. no brand logo or cover yet: nothing on stream
            if (!bmp) {
                ComPtr<ID2D1Geometry> shape = pictureShape(c, e, box, r);
                if (!shadowPass && shape) {   // a placeholder so it can be seen and picked
                    ComPtr<ID2D1SolidColorBrush> ph;
                    rt->CreateSolidColorBrush(D2D1::ColorF(0.5f, 0.55f, 0.62f, 0.35f * alpha), &ph);
                    rt->FillGeometry(shape.Get(), ph.Get());
                }
                break;
            }
            // The picture fills its shape as a brush (so the shape cuts it on any graphics driver): filling the box, it's
            // zoomed and moved as cropped (0.16); fitting inside, the shape is cut from the fitted picture.
            const D2D1_SIZE_F s = bmp->GetSize();
            D2D1_RECT_F at = box;
            D2D1_MATRIX_3X2_F m;
            if (e.cover) {
                const Area a = pictureArea(s.width, s.height, e.w, e.h, e.picZoom, e.picX, e.picY);
                if (!(a.w > 0) || !(a.h > 0)) break;
                const float sx = e.w / a.w, sy = e.h / a.h;
                m = D2D1::Matrix3x2F::Scale(sx, sy) * D2D1::Matrix3x2F::Translation(e.x - a.x * sx, e.y - a.y * sy);
            } else {
                const float k = std::min(e.w / s.width, e.h / s.height);
                const float dw = s.width * k, dh = s.height * k;
                at = D2D1::RectF(e.x + (e.w - dw) / 2, e.y + (e.h - dh) / 2, e.x + (e.w + dw) / 2, e.y + (e.h + dh) / 2);
                m = D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(at.left, at.top);
            }
            ComPtr<ID2D1Geometry> shape = pictureShape(c, e, at, r);
            if (!shape) break;
            if (shadowPass) rt->FillGeometry(shape.Get(), black.Get());
            else {
                ComPtr<ID2D1BitmapBrush> pb;
                if (SUCCEEDED(rt->CreateBitmapBrush(bmp.Get(),
                                                    D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR),
                                                    D2D1::BrushProperties(alpha, m), &pb)))
                    rt->FillGeometry(shape.Get(), pb.Get());
            }
            // The outline, just inside the edge. A shape's (mask's) outline is a picture of its own from renderDesign, so
            // here only the box's: a rounded rectangle half the thickness in.
            if (stroke && e.mask.empty()) {
                const float t = e.strokeWidth / 2, rr = std::max(0.0f, std::min(r, std::min(at.right - at.left, at.bottom - at.top) / 2) - t);
                rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(at.left + t, at.top + t, at.right - t, at.bottom - t), rr, rr), stroke.Get(), e.strokeWidth);
            }
            break;
        }
    }
}

// Three box blurs of the alpha channel (close to a Gaussian), radius r.
void blurAlpha(std::vector<std::uint8_t>& a, int w, int h, int r) {
    if (r <= 0) return;
    std::vector<std::uint8_t> t(a.size());
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < h; ++y) {   // across
            int sum = 0;
            const std::uint8_t* row = &a[(size_t)y * w];
            for (int x = -r; x <= r; ++x) sum += row[std::clamp(x, 0, w - 1)];
            for (int x = 0; x < w; ++x) {
                t[(size_t)y * w + x] = (std::uint8_t)(sum / (2 * r + 1));
                sum += row[std::min(x + r + 1, w - 1)] - row[std::max(x - r, 0)];
            }
        }
        for (int x = 0; x < w; ++x) {   // down
            int sum = 0;
            for (int y = -r; y <= r; ++y) sum += t[(size_t)std::clamp(y, 0, h - 1) * w + x];
            for (int y = 0; y < h; ++y) {
                a[(size_t)y * w + x] = (std::uint8_t)(sum / (2 * r + 1));
                sum += t[(size_t)std::min(y + r + 1, h - 1) * w + x] - t[(size_t)std::max(y - r, 0) * w + x];
            }
        }
    }
}

std::mutex gValuesM;
std::atomic<std::int64_t> gLiveMs{-1};
std::atomic<ULONGLONG> gLiveAt{0};
std::atomic<std::int64_t> gGoLiveAt{-1}, gPlannedMs{-1};
std::atomic<std::int64_t> gSprintEnd{-1};      // epoch ms
std::atomic<ULONGLONG> gCelebrateAt{0};        // GetTickCount64 when the last celebration began
std::mutex gWritingM;
std::string gCelebration, gWorldColour;
std::mutex gNextM;
std::string gNextText;
Placeholders gValues;
unsigned gValuesVersion = 1;
Brand gBrand = defaultBrand();
std::vector<chat::Message> gChat;
const void* gPlayingOwner = nullptr;
sourceutil::TrackInfo gPlaying;

// The brand kit's font files with the system's fonts, made once per list of files (DirectWrite's font
// sets: Windows 10 1703 and later).
std::mutex gFontsM;
std::vector<std::string> gFontsFor;
ComPtr<IDWriteFontCollection> gFonts;
std::wstring gFontsError;

ComPtr<IDWriteFontCollection> fontsFor(IDWriteFactory* dw, const std::vector<std::string>& files) {
    std::lock_guard<std::mutex> l(gFontsM);
    if (files == gFontsFor && (gFonts || !gFontsError.empty() || files.empty())) return gFonts;
    gFontsFor = files;
    gFonts.Reset();
    gFontsError.clear();
    if (files.empty()) return gFonts;
    ComPtr<IDWriteFactory5> f5;
    ComPtr<IDWriteFontSetBuilder1> builder;
    ComPtr<IDWriteFontSet> system;
    if (FAILED(dw->QueryInterface(IID_PPV_ARGS(&f5))) || FAILED(f5->CreateFontSetBuilder(&builder)) || FAILED(f5->GetSystemFontSet(&system))) {
        gFontsError = L"This version of Windows can't use font files directly: install the fonts in Windows instead.";
        return gFonts;
    }
    builder->AddFontSet(system.Get());
    std::wstring bad;
    for (auto& f : files) {
        ComPtr<IDWriteFontFile> ff;
        if (FAILED(f5->CreateFontFileReference(widen(f).c_str(), nullptr, &ff)) || FAILED(builder->AddFontFile(ff.Get())))
            bad += (bad.empty() ? L"" : L", ") + widen(f.substr(f.find_last_of("\\/") + 1));
    }
    ComPtr<IDWriteFontSet> set;
    ComPtr<IDWriteFontCollection1> coll;
    if (FAILED(builder->CreateFontSet(&set)) || FAILED(f5->CreateFontCollectionFromFontSet(set.Get(), &coll))) {
        gFontsError = L"The font files couldn't be read.";
        return gFonts;
    }
    gFonts = coll;
    if (!bad.empty()) gFontsError = L"Couldn't read: " + bad;
    return gFonts;
}

}  // namespace

void setDesignBrand(const Brand& b) {
    std::lock_guard<std::mutex> l(gValuesM);
    if (b == gBrand) return;
    gBrand = b;
    ++gValuesVersion;
}

void setDesignChat(std::vector<chat::Message> m) {
    std::lock_guard<std::mutex> l(gValuesM);
    gChat = std::move(m);
    ++gValuesVersion;
}

void setNowPlaying(const void* owner, const sourceutil::TrackInfo& track) {
    std::lock_guard<std::mutex> l(gValuesM);
    if (owner == gPlayingOwner && track == gPlaying) return;
    gPlayingOwner = owner;
    gPlaying = track;
    ++gValuesVersion;
}

void clearNowPlaying(const void* owner) {
    std::lock_guard<std::mutex> l(gValuesM);
    if (owner != gPlayingOwner) return;
    gPlayingOwner = nullptr;
    gPlaying = {};
    ++gValuesVersion;
}

sourceutil::TrackInfo nowPlaying() {
    std::lock_guard<std::mutex> l(gValuesM);
    return gPlaying;
}

std::vector<chat::Message> designChat() {
    std::lock_guard<std::mutex> l(gValuesM);
    return gChat;
}

Brand designBrand() {
    std::lock_guard<std::mutex> l(gValuesM);
    return gBrand;
}

std::vector<std::wstring> brandFontFamilies(std::wstring& error) {
    std::vector<std::wstring> out;
    ComPtr<IDWriteFactory> dw;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.GetAddressOf())))) return out;
    const Brand b = designBrand();
    ComPtr<IDWriteFontCollection> all = fontsFor(dw.Get(), b.fontFiles);
    {
        std::lock_guard<std::mutex> l(gFontsM);
        error = gFontsError;
    }
    if (!all) dw->GetSystemFontCollection(&all);
    if (!all) return out;
    const UINT32 n = all->GetFontFamilyCount();
    for (UINT32 i = 0; i < n; ++i) {
        ComPtr<IDWriteFontFamily> fam;
        ComPtr<IDWriteLocalizedStrings> names;
        if (FAILED(all->GetFontFamily(i, &fam)) || FAILED(fam->GetFamilyNames(&names))) continue;
        UINT32 idx = 0;
        BOOL ok = FALSE;
        names->FindLocaleName(L"en-us", &idx, &ok);
        if (!ok) idx = 0;
        UINT32 len = 0;
        if (FAILED(names->GetStringLength(idx, &len))) continue;
        std::wstring nm(len + 1, L'\0');
        if (FAILED(names->GetString(idx, nm.data(), len + 1))) continue;
        nm.resize(len);
        out.push_back(nm);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<std::wstring> fontFileFamilies(const std::wstring& file) {
    std::vector<std::wstring> out;
    ComPtr<IDWriteFactory> dw;
    ComPtr<IDWriteFactory5> f5;
    ComPtr<IDWriteFontSetBuilder1> builder;
    ComPtr<IDWriteFontFile> ff;
    ComPtr<IDWriteFontSet> set;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.GetAddressOf()))) ||
        FAILED(dw->QueryInterface(IID_PPV_ARGS(&f5))) || FAILED(f5->CreateFontSetBuilder(&builder)) ||
        FAILED(f5->CreateFontFileReference(file.c_str(), nullptr, &ff)) || FAILED(builder->AddFontFile(ff.Get())) ||
        FAILED(builder->CreateFontSet(&set)))
        return out;
    for (UINT32 i = 0; i < set->GetFontCount(); ++i) {
        BOOL exists = FALSE;
        ComPtr<IDWriteLocalizedStrings> names;
        if (FAILED(set->GetPropertyValues(i, DWRITE_FONT_PROPERTY_ID_WIN32_FAMILY_NAME, &exists, &names)) || !exists || !names) continue;
        UINT32 idx = 0;
        BOOL ok = FALSE;
        names->FindLocaleName(L"en-us", &idx, &ok);
        if (!ok) idx = 0;
        UINT32 len = 0;
        if (FAILED(names->GetStringLength(idx, &len))) continue;
        std::wstring nm(len + 1, L'\0');
        if (FAILED(names->GetString(idx, nm.data(), len + 1))) continue;
        nm.resize(len);
        if (std::find(out.begin(), out.end(), nm) == out.end()) out.push_back(nm);
    }
    return out;
}

void setDesignValues(const Placeholders& v) {
    std::lock_guard<std::mutex> l(gValuesM);
    if (v == gValues) return;
    gValues = v;
    ++gValuesVersion;
}

Placeholders designValues(unsigned* version) {
    std::lock_guard<std::mutex> l(gValuesM);
    if (version) *version = gValuesVersion;
    return gValues;
}

void setLiveTime(std::int64_t ms) {
    gLiveMs = ms;
    gLiveAt = GetTickCount64();
}

void setStreamPlan(std::int64_t goLiveAt, std::int64_t plannedMs, const std::string& nextText) {
    gGoLiveAt = goLiveAt;
    gPlannedMs = plannedMs;
    std::lock_guard<std::mutex> l(gNextM);
    gNextText = nextText;
}

void setWritingSprint(std::int64_t endsAt) { gSprintEnd = endsAt; }

void celebrate(const std::string& text) {
    {
        std::lock_guard<std::mutex> l(gWritingM);
        gCelebration = text;
    }
    gCelebrateAt = GetTickCount64();
    std::lock_guard<std::mutex> l(gValuesM);
    ++gValuesVersion;
}

namespace {
std::mutex gThemeM;
std::vector<Theme> gOwnThemes;
std::string gThemeId = "northray";
bool gWorldAccent = true;
}  // namespace

void setDesignThemes(const std::vector<Theme>& own, const std::string& current, bool worldAccent) {
    {
        std::lock_guard<std::mutex> l(gThemeM);
        gOwnThemes = own;
        gThemeId = current;
        gWorldAccent = worldAccent;
    }
    std::lock_guard<std::mutex> l(gValuesM);
    ++gValuesVersion;
}

void setWorldColour(const std::string& hex) {
    {
        std::lock_guard<std::mutex> l(gWritingM);
        if (hex == gWorldColour) return;
        gWorldColour = hex;
    }
    std::lock_guard<std::mutex> l(gValuesM);
    ++gValuesVersion;
}

Clock clockNow(std::int64_t elapsedMs) {
    Clock c;
    {
        // Writing Studio's sprint: the time left (one that ended more than a few seconds ago, or never ended because
        // Writing Studio closed, counts as none).
        const std::int64_t end = gSprintEnd;
        if (end > 0) {
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            const std::int64_t nowMs = (std::int64_t)((((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000ULL) - 11644473600000LL;
            if (end - nowMs > -3000 && end - nowMs < 4LL * 3600 * 1000) c.sprintLeftMs = std::max<std::int64_t>(0, end - nowMs);
        }
        const ULONGLONG at = gCelebrateAt;
        if (at) c.celebrateMs = (std::int64_t)(GetTickCount64() - at);
    }
    c.elapsedMs = elapsedMs;
    SYSTEMTIME st;
    GetLocalTime(&st);
    c.localSeconds = st.wHour * 3600 + st.wMinute * 60 + st.wSecond;
    static const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    c.dateText = std::string(days[st.wDayOfWeek % 7]) + " " + std::to_string(st.wDay) + " " + months[(st.wMonth + 11) % 12];
    const std::int64_t live = gLiveMs;
    c.liveMs = live < 0 ? -1 : live + (std::int64_t)(GetTickCount64() - gLiveAt);
    const std::int64_t at = gGoLiveAt;
    if (at >= 0) c.goLiveInMs = std::max<std::int64_t>(0, at - live::civilMs(st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds));
    c.plannedMs = gPlannedMs;
    {
        std::lock_guard<std::mutex> l(gNextM);
        c.nextText = gNextText;
    }
    return c;
}

bool renderDesign(const Document& linked, float tMs, float scale, const Placeholders& profileValues, DesignPicture& out, std::wstring& error, bool showMissing,
                  const Clock& clock) {
    Ctx c;
    if (!makeCtx(c)) {
        error = L"Direct2D isn't available";
        return false;
    }
    Brand brand = designBrand();
    std::string celebration;
    std::vector<Theme> ownThemes;
    std::string themeId;
    bool worldAccent = false;
    {
        std::lock_guard<std::mutex> l(gThemeM);
        ownThemes = gOwnThemes;
        themeId = gThemeId;
        worldAccent = gWorldAccent;
    }
    const Theme streamTheme = themeById(ownThemes, themeId);
    sourceutil::Rgba world{0, 0, 0, 0};
    {
        // While writing a book in Writing Studio, its world's colour stands in for the kit's main colour (0.13); since
        // 0.21, when the stream's theme has its world colour switched on, and for the NorthRay theme (which is the kit).
        std::lock_guard<std::mutex> l(gWritingM);
        sourceutil::Rgba wc;
        if (worldAccent && !gWorldColour.empty() && sourceutil::parseColour(gWorldColour, wc)) world = {wc.r, wc.g, wc.b, 255};
        if (worldAccent && streamTheme.id == "northray" && !gWorldColour.empty() && sourceutil::parseColour(gWorldColour, wc)) {
            // Lightened until it reads on the designs' dark panels (a world's accent is often deep).
            for (int i = 0; i < 8; ++i) {
                const float lum = (0.2126f * wc.r + 0.7152f * wc.g + 0.0722f * wc.b) / 255.0f;
                if (lum >= 0.42f) break;
                wc.r = (std::uint8_t)(wc.r + (255 - wc.r) * 0.22f);
                wc.g = (std::uint8_t)(wc.g + (255 - wc.g) * 0.22f);
                wc.b = (std::uint8_t)(wc.b + (255 - wc.b) * 0.22f);
            }
            wc.a = 255;
            for (auto& bc : brand.colours)
                if (bc.key == "primary") bc.colour = wc;
        }
        celebration = gCelebration;
    }
    Document d = applyBrand(linked, brand);
    // Your details, and what's playing ({song}, {artist}), and the latest celebration's words.
    Placeholders values = profileValues;
    values.push_back({"celebration", celebration});
    {
        const sourceutil::TrackInfo t = nowPlaying();
        values.push_back({"song", t.title});
        values.push_back({"artist", t.artist});
    }
    // A Writing card (0.20) is laid out now from what there is to show. The designer shows every part, with stand-ins.
    Clock clk = clock;
    if (d.writing.on) {
        std::vector<std::string> hidden;
        if (showMissing) {
            auto have = [&](const char* k) {
                for (auto& v : values) if (v.first == k && !v.second.empty()) return true;
                return false;
            };
            const std::pair<const char*, const char*> samples[] = {
                {"book", "{book}"}, {"series", "{series}"}, {"chapter", "{chapter}"}, {"status", "{status}"}, {"place", "{place}"},
                {"storyday", "{storyday}"}, {"today", "{today}"}, {"viewpoint", "{viewpoint}"}, {"cast1", "{cast1}"}, {"cast2", "{cast2}"},
                {"cast3", "{cast3}"}, {"streamline", "{streamline}"}, {"celebration", "{celebration}"}, {"sprintline", "{sprintline}"}};
            for (auto& [k, v] : samples) if (!have(k)) values.push_back({k, v});
            if (clk.sprintLeftMs < 0) clk.sprintLeftMs = 25 * 60 * 1000;
            if (clk.celebrateMs < 0) clk.celebrateMs = 1000;
        } else {
            hidden = writingHidden();
        }
        const Theme cardTheme = themeById(ownThemes, d.writing.look);
        d = applyBrand(buildWritingCard(d, values, clk, hidden, &cardTheme), brand);
    }
    // The theme's main colour as the book's world colour (themes other than NorthRay, whose colours are the kit's).
    if (world.a && streamTheme.id != "northray" && d.theme == streamTheme.id) recolourAccent(d, streamTheme, world);
    // "Only while music plays": an empty, see-through page when nothing is (the designer still shows it).
    if (!showMissing && ((d.onlyWhilePlaying && nowPlaying().title.empty()) || !shownNow(d, clk))) {
        d.elements.clear();
        d.background = Fill::none();
    }
    c.fonts = fontsFor(c.dw.Get(), brand.fontFiles);
    scale = std::clamp(scale, 0.02f, 4.0f);
    const int W = std::clamp((int)std::lround(d.width * scale), 1, 8192), H = std::clamp((int)std::lround(d.height * scale), 1, 8192);
    const D2D1_RENDER_TARGET_PROPERTIES props =
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<IWICBitmap> bmp;
    ComPtr<ID2D1RenderTarget> rt;
    if (FAILED(c.wic->CreateBitmap((UINT)W, (UINT)H, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp)) ||
        FAILED(c.d2->CreateWicBitmapRenderTarget(bmp.Get(), &props, &rt))) {
        error = L"The design couldn't be drawn";
        return false;
    }
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    const D2D1_MATRIX_3X2_F base = D2D1::Matrix3x2F::Scale(scale, scale);

    // An element's placement: its entrance, then its rotation about its centre, then the page scale.
    auto placement = [&](const Element& e, const Pose& p) {
        const D2D1_POINT_2F centre = D2D1::Point2F(e.x + e.w / 2, e.y + e.h / 2);
        const float pulse = e.kind == Kind::Timer ? timerText(e, clk).pulse : 1.0f;   // the last ten seconds
        return D2D1::Matrix3x2F::Scale(p.scale * pulse, p.scale * pulse, centre) * D2D1::Matrix3x2F::Rotation(e.rotation, centre) *
               D2D1::Matrix3x2F::Translation(p.dx, p.dy) * base;
    };

    // Shadows and glows (0.15) first need each element's shape on its own: drawn in black to a separate bitmap
    // (once for both), blurred, then used as a mask in the effect's colour. Each is only as big as its element and
    // the blur around it.
    struct ShadowPic {
        ComPtr<ID2D1Bitmap> bmp;
        float x = 0, y = 0, w = 0, h = 0;   // where it goes, in output pixels (before the entrance and offset)
    };
    // Text outlines (0.15) are grown from the letters' own pixels (design::grow) into a picture of their own, drawn
    // under the letters: even all round at any thickness, on any graphics driver. `bare` is each element as drawn
    // over its outline picture (its outline taken off).
    std::vector<ShadowPic> shadows(d.elements.size()), glows(d.elements.size()), outlines(d.elements.size()), overs(d.elements.size());
    std::vector<Element> bare(d.elements.begin(), d.elements.end());
    for (std::size_t i = 0; i < d.elements.size(); ++i) {
        const Element& e = d.elements[i];
        const bool sh = e.visible && e.shadow.on && e.shadow.colour.a > 0, gl = e.visible && e.glow.on && e.glow.colour.a > 0;
        const bool ol = e.visible && (e.kind == Kind::Text || e.kind == Kind::Timer) && e.strokeWidth > 0 && e.stroke.a > 0;
        // A picture cut to a shape (0.16): its outline is a band just inside the shape, from the shape's own pixels.
        const bool band = e.visible && e.kind == Kind::Image && !e.mask.empty() && e.strokeWidth > 0 && e.stroke.a > 0;
        if (!sh && !gl && !ol && !band) continue;
        Element letters = e;
        if (ol || band) letters.strokeWidth = 0, letters.stroke.a = 0;
        const int shBlur = sh ? std::max(0, (int)std::lround(e.shadow.blur * scale / 2)) : 0;
        const int glBlur = gl ? std::max(1, (int)std::lround(e.glow.size * scale / 2)) : 0;
        const float margin = std::max(shBlur, glBlur) * 3.0f + 2 + e.strokeWidth * scale;
        const int bx = (int)std::floor(e.x * scale - margin), by = (int)std::floor(e.y * scale - margin);
        const int bw = std::clamp((int)std::ceil(e.w * scale + 2 * margin), 1, 8192), bh = std::clamp((int)std::ceil(e.h * scale + 2 * margin), 1, 8192);
        ComPtr<IWICBitmap> sb;
        ComPtr<ID2D1RenderTarget> srt;
        if (FAILED(c.wic->CreateBitmap((UINT)bw, (UINT)bh, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &sb)) ||
            FAILED(c.d2->CreateWicBitmapRenderTarget(sb.Get(), &props, &srt)))
            continue;
        srt->BeginDraw();
        srt->Clear(D2D1::ColorF(0, 0, 0, 0));
        srt->SetTransform(base * D2D1::Matrix3x2F::Translation((float)-bx, (float)-by));   // unposed: it moves with its element below
        drawShape(c, srt.Get(), letters, values, 1, true, showMissing, clk);
        if (FAILED(srt->EndDraw())) continue;
        std::vector<std::uint8_t> px((size_t)bw * bh * 4);
        if (FAILED(sb->CopyPixels(nullptr, (UINT)bw * 4, (UINT)px.size(), px.data()))) continue;
        std::vector<std::uint8_t> shape((size_t)bw * bh);
        for (size_t k = 0; k < shape.size(); ++k) shape[k] = px[k * 4 + 3];
        std::vector<std::uint8_t> grown;
        if (ol) {
            grown = grow(shape, bw, bh, e.strokeWidth * scale);
            shape = grown;   // shadows and glows go round the outline too
        }
        auto make = [&](int blur, Rgba colour, float strength, ShadowPic& out) {   // strength < 0: a shadow (as blurred)
            std::vector<std::uint8_t> a = shape;
            blurAlpha(a, bw, bh, blur);
            std::vector<std::uint8_t> q((size_t)bw * bh * 4);
            for (size_t k = 0; k < a.size(); ++k) {
                const float cov = strength < 0 ? a[k] / 255.0f : glowLevel(a[k] / 255.0f, strength);
                const unsigned al = (unsigned)std::lround(cov * colour.a);
                q[k * 4 + 0] = (std::uint8_t)(colour.b * al / 255);
                q[k * 4 + 1] = (std::uint8_t)(colour.g * al / 255);
                q[k * 4 + 2] = (std::uint8_t)(colour.r * al / 255);
                q[k * 4 + 3] = (std::uint8_t)al;
            }
            rt->CreateBitmap(D2D1::SizeU((UINT)bw, (UINT)bh), q.data(), (UINT)bw * 4,
                             D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &out.bmp);
            out.x = (float)bx, out.y = (float)by, out.w = (float)bw, out.h = (float)bh;
        };
        if (sh) make(shBlur, e.shadow.colour, -1, shadows[i]);
        if (gl) make(glBlur, e.glow.colour, e.glow.strength, glows[i]);
        if (ol) {
            make(0, e.stroke, -1, outlines[i]);
            if (outlines[i].bmp) bare[i] = letters;
        }
        if (band) {
            std::vector<std::uint8_t> outside(shape.size());
            for (size_t k = 0; k < shape.size(); ++k) outside[k] = (std::uint8_t)(255 - shape[k]);
            const auto reach = grow(outside, bw, bh, e.strokeWidth * scale);   // within the thickness of the outside
            const std::vector<std::uint8_t> keepShape = shape;
            for (size_t k = 0; k < shape.size(); ++k) shape[k] = std::min(keepShape[k], reach[k]);
            make(0, e.stroke, -1, overs[i]);
            shape = keepShape;
            if (overs[i].bmp) bare[i] = letters;
        }
    }

    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    rt->SetTransform(base);
    if (d.background.type != Fill::None) {
        const D2D1_RECT_F page = D2D1::RectF(0, 0, (float)d.width, (float)d.height);
        if (auto b = brushFor(rt.Get(), d.background, page)) rt->FillRectangle(page, b.Get());
    }
    for (std::size_t i = 0; i < d.elements.size(); ++i) {
        const Element& e = d.elements[i];
        if (!e.visible) continue;
        const Pose p = pose(e, tMs, d.width, d.height);
        const float alpha = std::clamp(e.opacity * p.opacity, 0.0f, 1.0f);
        if (alpha <= 0.001f || p.reveal <= 0.001f || p.scale <= 0.001f) continue;
        // A wipe shows the element from its left edge as it comes in.
        const bool wiping = p.reveal < 0.999f;
        // The shadow, then the glow, under the element: their bitmaps are in page pixels and move with its entrance
        // (the shadow by its offset too).
        auto under = [&](const ShadowPic& sp, float ox, float oy) {
            if (!sp.bmp) return;
            const D2D1_POINT_2F centre = D2D1::Point2F((e.x + e.w / 2) * scale, (e.y + e.h / 2) * scale);
            const float pulse = e.kind == Kind::Timer ? timerText(e, clk).pulse : 1.0f;   // as the element itself
            rt->SetTransform(D2D1::Matrix3x2F::Translation(-centre.x, -centre.y) * D2D1::Matrix3x2F::Scale(p.scale * pulse, p.scale * pulse) *
                             D2D1::Matrix3x2F::Rotation(e.rotation) *
                             D2D1::Matrix3x2F::Translation(centre.x + (p.dx + ox) * scale, centre.y + (p.dy + oy) * scale));
            if (wiping) rt->PushAxisAlignedClip(D2D1::RectF(e.x * scale, -1e5f, (e.x + e.w * p.reveal) * scale, 1e5f), D2D1_ANTIALIAS_MODE_ALIASED);
            rt->DrawBitmap(sp.bmp.Get(), D2D1::RectF(sp.x, sp.y, sp.x + sp.w, sp.y + sp.h), alpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            if (wiping) rt->PopAxisAlignedClip();
        };
        // Drawn only inside its clip box (0.20: the Writing card's scrolling cast row).
        const bool clipped = e.clipW > 0 && e.clipH > 0;
        if (clipped) {
            rt->SetTransform(base);
            rt->PushAxisAlignedClip(D2D1::RectF(e.clipX, e.clipY, e.clipX + e.clipW, e.clipY + e.clipH), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        }
        under(shadows[i], e.shadow.dx, e.shadow.dy);
        under(glows[i], 0, 0);
        under(outlines[i], 0, 0);
        rt->SetTransform(placement(e, p));
        if (wiping) rt->PushAxisAlignedClip(D2D1::RectF(e.x, e.y - 1e4f, e.x + e.w * p.reveal, e.y + e.h + 1e4f), D2D1_ANTIALIAS_MODE_ALIASED);
        drawShape(c, rt.Get(), bare[i], values, alpha, false, showMissing, clk);
        if (wiping) rt->PopAxisAlignedClip();
        under(overs[i], 0, 0);   // (over it, despite the name: a shaped picture's outline)
        if (clipped) rt->PopAxisAlignedClip();
    }
    if (FAILED(rt->EndDraw())) {
        error = L"The design couldn't be drawn";
        return false;
    }
    out.w = W;
    out.h = H;
    out.bgra.resize((size_t)W * H * 4);
    if (FAILED(bmp->CopyPixels(nullptr, (UINT)W * 4, (UINT)out.bgra.size(), out.bgra.data()))) return false;
    for (std::size_t i = 0; i < out.bgra.size(); i += 4) {   // to straight alpha
        const int a = out.bgra[i + 3];
        if (a == 0 || a == 255) continue;
        for (int k = 0; k < 3; ++k) out.bgra[i + (size_t)k] = (std::uint8_t)std::min(255, out.bgra[i + (size_t)k] * 255 / a);
    }
    return true;
}

namespace {
std::mutex gHiddenM;
std::vector<std::string> gHidden;
}  // namespace
void setWritingHidden(const std::vector<std::string>& parts) {
    std::lock_guard<std::mutex> l(gHiddenM);
    gHidden = parts;
}
std::vector<std::string> writingHidden() {
    std::lock_guard<std::mutex> l(gHiddenM);
    return gHidden;
}

bool savePng(const std::wstring& path, const DesignPicture& p) {
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(f->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) || FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(enc->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) || FAILED(frame->SetSize((UINT)p.w, (UINT)p.h)))
        return false;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&fmt)) || fmt != GUID_WICPixelFormat32bppBGRA) return false;
    std::vector<std::uint8_t> copy(p.bgra);
    return SUCCEEDED(frame->WritePixels((UINT)p.h, (UINT)p.w * 4, (UINT)copy.size(), copy.data())) && SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
}

}  // namespace ncap::video
