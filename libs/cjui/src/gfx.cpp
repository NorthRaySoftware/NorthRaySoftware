#include "cjui/gfx.hpp"

#include <vector>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace cjui {

template <class T> static void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

Color hexColor(const std::string& hex, Color fallback) {
    unsigned v = 0;
    if (hex.size() != 7 || hex[0] != '#' || std::sscanf(hex.c_str() + 1, "%x", &v) != 1) return fallback;
    return {((v >> 16) & 0xFF) / 255.0f, ((v >> 8) & 0xFF) / 255.0f, (v & 0xFF) / 255.0f, 1.0f};
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

bool Gfx::init(HWND hwnd) {
    hwnd_ = hwnd;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&dw_))))
        return false;
    return true;
}

void Gfx::shutdown() {
    discardTarget();
    for (auto& [k, f] : formats_) release(f);
    formats_.clear();
    release(ellipsis_);
    release(wic_);
    release(dw_);
    release(d2d_);
}

void Gfx::setDpi(UINT dpi) {
    dpi_ = dpi ? dpi : 96;
    if (rt_) rt_->SetDpi((float)dpi_, (float)dpi_);
}

bool Gfx::createTarget() {
    if (rt_) return true;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
    props.dpiX = props.dpiY = (float)dpi_;
    HRESULT hr = d2d_->CreateHwndRenderTarget(
        props, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top)), &rt_);
    if (FAILED(hr)) return false;
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    rt_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &brush_);
    return true;
}

void Gfx::discardTarget() {
    for (auto& [p, img] : images_) release(img.bitmap);
    images_.clear();  // bitmaps belong to the render target
    release(brush_);
    release(rt_);
}

void Gfx::resize(UINT w, UINT h) {
    if (rt_) rt_->Resize(D2D1::SizeU(w, h));
}

bool Gfx::begin() {
    if (!createTarget()) return false;
    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    return true;
}

void Gfx::end() {
    if (rt_->EndDraw() == (HRESULT)D2DERR_RECREATE_TARGET) discardTarget();
}

// ------------------------------------------------------------------ images

Gfx::CachedImage* Gfx::loadImage(const std::wstring& path) {
    auto it = images_.find(path);
    if (it != images_.end()) return it->second.failed ? nullptr : &it->second;
    CachedImage& img = images_[path];
    img.failed = true;
    if (!rt_ || path.empty()) return nullptr;
    if (!wic_ && FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_))))
        return nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* conv = nullptr;
    bool ok = SUCCEEDED(wic_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&img.w, &img.h)) && img.w && img.h;
    IWICBitmapSource* src = frame;
    if (ok) {
        // Screen use only: keep at most ~1600 px on the long side to save memory.
        const unsigned longest = std::max(img.w, img.h);
        if (longest > 1600 && SUCCEEDED(wic_->CreateBitmapScaler(&scaler))) {
            const double k = 1600.0 / longest;
            if (SUCCEEDED(scaler->Initialize(frame, (UINT)std::max(1.0, img.w * k), (UINT)std::max(1.0, img.h * k),
                                             WICBitmapInterpolationModeFant)))
                src = scaler;
        }
        ok = SUCCEEDED(wic_->CreateFormatConverter(&conv)) &&
             SUCCEEDED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                        WICBitmapPaletteTypeMedianCut)) &&
             SUCCEEDED(rt_->CreateBitmapFromWicBitmap(conv, nullptr, &img.bitmap));
    }
    release(conv);
    release(scaler);
    release(frame);
    release(dec);
    if (!ok) { release(img.bitmap); return nullptr; }
    img.failed = false;
    return &img;
}

bool Gfx::drawImage(const std::wstring& path, const Rect& dst, bool cover, float opacity) {
    CachedImage* img = loadImage(path);
    if (!img || !img->bitmap || dst.w <= 0 || dst.h <= 0) return false;
    const D2D1_SIZE_F bs = img->bitmap->GetSize();
    D2D1_RECT_F srcR = D2D1::RectF(0, 0, bs.width, bs.height);
    D2D1_RECT_F dstR = D2D1::RectF(dst.x, dst.y, dst.right(), dst.bottom());
    const float ia = bs.width / bs.height, da = dst.w / dst.h;
    if (cover) {
        if (ia > da) { const float w = bs.height * da; srcR.left = (bs.width - w) / 2; srcR.right = srcR.left + w; }
        else { const float h = bs.width / da; srcR.top = (bs.height - h) / 2; srcR.bottom = srcR.top + h; }
    } else {
        if (ia > da) { const float h = dst.w / ia; dstR.top = dst.y + (dst.h - h) / 2; dstR.bottom = dstR.top + h; }
        else { const float w = dst.h * ia; dstR.left = dst.x + (dst.w - w) / 2; dstR.right = dstR.left + w; }
    }
    rt_->DrawBitmap(img->bitmap, dstR, opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, srcR);
    return true;
}

std::pair<unsigned, unsigned> Gfx::imageSize(const std::wstring& path) {
    CachedImage* img = loadImage(path);
    return img ? std::pair<unsigned, unsigned>{img->w, img->h} : std::pair<unsigned, unsigned>{0, 0};
}

bool Gfx::drawPixels(const std::wstring& key, int w, int h, const std::uint8_t* rgba, const Rect& dst) {
    if (!rt_) return false;
    auto it = images_.find(key);
    if (it == images_.end() || !it->second.bitmap) {
        if (w <= 0 || h <= 0 || !rgba) return false;
        std::vector<std::uint8_t> bgra((size_t)w * h * 4);
        for (size_t i = 0, n = (size_t)w * h; i < n; ++i) {
            bgra[i * 4] = rgba[i * 4 + 2];
            bgra[i * 4 + 1] = rgba[i * 4 + 1];
            bgra[i * 4 + 2] = rgba[i * 4];
            bgra[i * 4 + 3] = 255;
        }
        CachedImage img;
        const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(rt_->CreateBitmap(D2D1::SizeU(w, h), bgra.data(), (UINT32)w * 4, props, &img.bitmap))) return false;
        img.w = (unsigned)w;
        img.h = (unsigned)h;
        img.failed = false;
        if (it != images_.end()) release(it->second.bitmap);
        images_[key] = img;
        it = images_.find(key);
    }
    rt_->DrawBitmap(it->second.bitmap, D2D1::RectF(dst.x, dst.y, dst.right(), dst.bottom()), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    return true;
}

void Gfx::forgetImage(const std::wstring& path) {
    auto it = images_.find(path);
    if (it == images_.end()) return;
    release(it->second.bitmap);
    images_.erase(it);
}

ID2D1SolidColorBrush* Gfx::brush(Color c) {
    brush_->SetColor(D2D1::ColorF(c.r, c.g, c.b, c.a));
    return brush_;
}

void Gfx::clear(Color c) { rt_->Clear(D2D1::ColorF(c.r, c.g, c.b, c.a)); }
void Gfx::fill(const Rect& r, Color c) { rt_->FillRectangle(r.d2d(), brush(c)); }
void Gfx::stroke(const Rect& r, Color c, float w) { rt_->DrawRectangle(r.inset(w / 2).d2d(), brush(c), w); }

void Gfx::fillRounded(const Rect& r, float rad, Color c) {
    rt_->FillRoundedRectangle(D2D1::RoundedRect(r.d2d(), rad, rad), brush(c));
}

// Drawn in bands and rings of solid colour rather than with gradient brushes,
// so it looks the same on every Direct2D (gradient-stop alpha isn't honoured everywhere).
void Gfx::fillGradient(const Rect& r, float rad, Color a, Color b, bool vertical) {
    if (!rt_ || r.w <= 0 || r.h <= 0) return;
    const float len = vertical ? r.h : r.w;
    const int n = std::clamp((int)(len / 3), 2, 96);
    const bool clip = rad > 0;
    if (clip) {
        // Round the corners by clipping to a rounded layer.
        ID2D1RoundedRectangleGeometry* geo = nullptr;
        ID2D1Factory* f = nullptr;
        rt_->GetFactory(&f);
        if (f && SUCCEEDED(f->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r.d2d(), rad, rad), &geo))) {
            ID2D1Layer* layer = nullptr;
            if (SUCCEEDED(rt_->CreateLayer(nullptr, &layer))) {
                rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), geo), layer);
                fillGradient(r, 0, a, b, vertical);
                rt_->PopLayer();
                layer->Release();
            }
            geo->Release();
        }
        if (f) f->Release();
        return;
    }
    for (int i = 0; i < n; ++i) {
        const float t0 = (float)i / n, t1 = (float)(i + 1) / n;
        const Color c = a.mix(b, (t0 + t1) / 2);
        const Rect band = vertical ? Rect{r.x, r.y + r.h * t0, r.w, r.h * (t1 - t0) + 0.6f}
                                   : Rect{r.x + r.w * t0, r.y, r.w * (t1 - t0) + 0.6f, r.h};
        rt_->FillRectangle(band.d2d(), brush(c));
    }
}

void Gfx::glow(float cx, float cy, float rx, float ry, Color c) {
    if (!rt_ || rx <= 0 || ry <= 0) return;
    // Rings from the outside in, each adding a little: a soft falloff to clear.
    const int n = 28;
    const float each = 1.0f - std::pow(1.0f - std::clamp(c.a, 0.0f, 1.0f), 1.0f / n);
    for (int i = 0; i < n; ++i) {
        const float k = 1.0f - (float)i / n;
        const float shaped = k * k * (3 - 2 * k);   // smooth at the edge
        rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rx * (0.15f + 0.85f * shaped), ry * (0.15f + 0.85f * shaped)),
                         brush(c.withAlpha(each)));
    }
}

void Gfx::strokeRounded(const Rect& r, float rad, Color c, float w) {
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(r.inset(w / 2).d2d(), rad, rad), brush(c), w);
}

void Gfx::line(float x1, float y1, float x2, float y2, Color c, float w) {
    rt_->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush(c), w);
}

void Gfx::circle(float cx, float cy, float r, Color c, bool filled, float w) {
    D2D1_ELLIPSE e = D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r);
    if (filled) rt_->FillEllipse(e, brush(c));
    else rt_->DrawEllipse(e, brush(c), w);
}

void Gfx::arc(float cx, float cy, float r, float a0, float a1, Color c, float w) {
    ID2D1PathGeometry* g = nullptr;
    if (FAILED(d2d_->CreatePathGeometry(&g))) return;
    ID2D1GeometrySink* s = nullptr;
    g->Open(&s);
    s->BeginFigure(D2D1::Point2F(cx + r * std::cos(a0), cy + r * std::sin(a0)), D2D1_FIGURE_BEGIN_HOLLOW);
    s->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx + r * std::cos(a1), cy + r * std::sin(a1)), D2D1::SizeF(r, r), 0,
                               D2D1_SWEEP_DIRECTION_CLOCKWISE,
                               (a1 - a0) > 3.14159f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    s->EndFigure(D2D1_FIGURE_END_OPEN);
    s->Close();
    rt_->DrawGeometry(g, brush(c), w);
    s->Release();
    g->Release();
}

void Gfx::curve(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, Color c, float w) {
    ID2D1PathGeometry* g = nullptr;
    if (FAILED(d2d_->CreatePathGeometry(&g))) return;
    ID2D1GeometrySink* s = nullptr;
    g->Open(&s);
    s->BeginFigure(D2D1::Point2F(x0, y0), D2D1_FIGURE_BEGIN_HOLLOW);
    s->AddBezier(D2D1::BezierSegment(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), D2D1::Point2F(x3, y3)));
    s->EndFigure(D2D1_FIGURE_END_OPEN);
    s->Close();
    ID2D1StrokeStyle* style = nullptr;
    d2d_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0, &style);
    rt_->DrawGeometry(g, brush(c), w, style);
    if (style) style->Release();
    s->Release();
    g->Release();
}

void Gfx::bookmark(const Rect& r, Color c) {
    ID2D1PathGeometry* g = nullptr;
    if (FAILED(d2d_->CreatePathGeometry(&g))) return;
    ID2D1GeometrySink* s = nullptr;
    g->Open(&s);
    const float notch = r.w * 0.55f;
    s->BeginFigure(D2D1::Point2F(r.x, r.y), D2D1_FIGURE_BEGIN_FILLED);
    s->AddLine(D2D1::Point2F(r.right(), r.y));
    s->AddLine(D2D1::Point2F(r.right(), r.bottom()));
    s->AddLine(D2D1::Point2F(r.x + r.w / 2, r.bottom() - notch));
    s->AddLine(D2D1::Point2F(r.x, r.bottom()));
    s->EndFigure(D2D1_FIGURE_END_CLOSED);
    s->Close();
    rt_->FillGeometry(g, brush(c));
    s->Release();
    g->Release();
}

void Gfx::fillTriangle(float x1, float y1, float x2, float y2, float x3, float y3, Color c) {
    ID2D1PathGeometry* g = nullptr;
    if (FAILED(d2d_->CreatePathGeometry(&g))) return;
    ID2D1GeometrySink* s = nullptr;
    g->Open(&s);
    s->BeginFigure(D2D1::Point2F(x1, y1), D2D1_FIGURE_BEGIN_FILLED);
    s->AddLine(D2D1::Point2F(x2, y2));
    s->AddLine(D2D1::Point2F(x3, y3));
    s->EndFigure(D2D1_FIGURE_END_CLOSED);
    s->Close();
    rt_->FillGeometry(g, brush(c));
    s->Release();
    g->Release();
}

IDWriteTextFormat* Gfx::format(Font f) {
    auto it = formats_.find(f);
    if (it != formats_.end()) return it->second;

    const wchar_t* ui = L"Segoe UI Variable Text";
    const wchar_t* display = L"Segoe UI Variable Display";
    // Fall back to Segoe UI on Windows 10, which doesn't ship the Variable family.
    auto has = [&](const wchar_t* name) {
        IDWriteFontCollection* sys = nullptr;
        dw_->GetSystemFontCollection(&sys, FALSE);
        UINT32 idx = 0;
        BOOL exists = FALSE;
        if (sys) { sys->FindFamilyName(name, &idx, &exists); sys->Release(); }
        return exists == TRUE;
    };
    static int variable = -1;
    if (variable < 0) variable = has(ui) ? 1 : 0;
    if (!variable) ui = display = L"Segoe UI";

    const wchar_t* family = ui;
    float size = 14;
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_REGULAR;
    switch (f) {
        case Font::Body: break;
        case Font::BodyStrong: weight = DWRITE_FONT_WEIGHT_SEMI_BOLD; break;
        case Font::Small: size = 12.5f; break;
        case Font::SmallStrong: size = 12.5f; weight = DWRITE_FONT_WEIGHT_SEMI_BOLD; break;
        case Font::Label: size = 11; weight = DWRITE_FONT_WEIGHT_SEMI_BOLD; break;
        case Font::Heading: size = 17; weight = DWRITE_FONT_WEIGHT_SEMI_BOLD; family = display; break;
        case Font::Title: size = 26; weight = DWRITE_FONT_WEIGHT_SEMI_BOLD; family = display; break;
        case Font::Status: size = 22; weight = DWRITE_FONT_WEIGHT_SEMI_BOLD; family = display; break;
        case Font::Wordmark: size = 19; weight = DWRITE_FONT_WEIGHT_BOLD; family = display; break;
        case Font::WordmarkLight: size = 19; family = display; break;
        case Font::Numeric: size = 13; break;
        case Font::Monogram: size = 30; weight = DWRITE_FONT_WEIGHT_BOLD; family = display; break;
        case Font::Serif: size = 15; weight = DWRITE_FONT_WEIGHT_SEMI_BOLD; break;
        case Font::SerifTitle: size = 28; weight = DWRITE_FONT_WEIGHT_SEMI_BOLD; break;
        case Font::SerifSmall: size = 12.5f; break;
    }
    DWRITE_FONT_STYLE style = DWRITE_FONT_STYLE_NORMAL;
    if (f == Font::Serif || f == Font::SerifTitle || f == Font::SerifSmall) {
        static const wchar_t* serif = has(L"Cambria") ? L"Cambria" : has(L"Georgia") ? L"Georgia" : L"Times New Roman";
        family = serif;
        if (f == Font::SerifSmall) style = DWRITE_FONT_STYLE_ITALIC;
    }
    IDWriteTextFormat* tf = nullptr;
    // A family that isn't installed would draw nothing: fall back to the UI face.
    if (!has(family)) family = ui;
    dw_->CreateTextFormat(family, nullptr, weight, style, DWRITE_FONT_STRETCH_NORMAL, size, L"en-gb", &tf);
    formats_[f] = tf;
    return tf;
}

IDWriteTextLayout* Gfx::layout(const std::wstring& s, float w, float h, Font f, const TextOpts& o) {
    IDWriteTextFormat* tf = format(f);
    IDWriteTextLayout* tl = nullptr;
    if (!tf || FAILED(dw_->CreateTextLayout(s.c_str(), (UINT32)s.size(), tf, std::max(1.0f, w), std::max(1.0f, h), &tl)))
        return nullptr;
    tl->SetWordWrapping(o.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    tl->SetTextAlignment(o.align == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                         : o.align == Align::Right ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                   : DWRITE_TEXT_ALIGNMENT_LEADING);
    if (o.centerVertically) tl->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (o.strike) tl->SetStrikethrough(TRUE, {0, (UINT32)s.size()});
    if (o.ellipsis) {
        if (!ellipsis_ || ellipsisOwner_ != tf) {
            release(ellipsis_);
            dw_->CreateEllipsisTrimmingSign(tf, &ellipsis_);
            ellipsisOwner_ = tf;
        }
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        tl->SetTrimming(&trim, ellipsis_);
    }
    return tl;
}

float Gfx::text(const std::wstring& s, const Rect& r, Font f, Color c, TextOpts o) {
    if (s.empty()) return 0;
    IDWriteTextLayout* tl = layout(s, r.w, o.centerVertically ? r.h : 10000.0f, f, o);
    if (!tl) return 0;
    DWRITE_TEXT_METRICS m{};
    tl->GetMetrics(&m);
    rt_->DrawTextLayout(D2D1::Point2F(r.x, r.y), tl, brush(c),
                        o.ellipsis ? D2D1_DRAW_TEXT_OPTIONS_CLIP : D2D1_DRAW_TEXT_OPTIONS_NONE);
    tl->Release();
    return m.height;
}

float Gfx::measureHeight(const std::wstring& s, float w, Font f) {
    if (s.empty()) return 0;
    IDWriteTextLayout* tl = layout(s, w, 10000.0f, f, {});
    if (!tl) return 0;
    DWRITE_TEXT_METRICS m{};
    tl->GetMetrics(&m);
    tl->Release();
    return m.height;
}

float Gfx::measureWidth(const std::wstring& s, Font f) {
    if (s.empty()) return 0;
    TextOpts o;
    o.wrap = false;
    IDWriteTextLayout* tl = layout(s, 10000.0f, 1000.0f, f, o);
    if (!tl) return 0;
    DWRITE_TEXT_METRICS m{};
    tl->GetMetrics(&m);
    tl->Release();
    return m.widthIncludingTrailingWhitespace;
}

void Gfx::pushClip(const Rect& r) { rt_->PushAxisAlignedClip(r.d2d(), D2D1_ANTIALIAS_MODE_ALIASED); }
void Gfx::popClip() { rt_->PopAxisAlignedClip(); }

}  // namespace cjui
