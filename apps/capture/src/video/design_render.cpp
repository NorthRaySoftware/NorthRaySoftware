#include "video/design_render.hpp"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
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

ComPtr<IDWriteTextLayout> layoutFor(Ctx& c, const Element& e, const std::wstring& text) {
    ComPtr<IDWriteTextFormat> fmt;
    ComPtr<IDWriteTextLayout> lay;
    if (FAILED(c.dw->CreateTextFormat(widen(e.font).c_str(), nullptr, e.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                      e.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, e.size, L"en-gb", &fmt)) &&
        FAILED(c.dw->CreateTextFormat(L"Segoe UI", nullptr, e.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                      e.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, e.size, L"en-gb", &fmt)))
        return lay;
    fmt->SetTextAlignment(e.align == "centre" ? DWRITE_TEXT_ALIGNMENT_CENTER : e.align == "right" ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    c.dw->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt.Get(), e.w, e.h, &lay);
    return lay;
}

// One element's shape, in its own box at (e.x, e.y), with the brush given (the shadow pass uses black).
void drawShape(Ctx& c, ID2D1RenderTarget* rt, const Element& e, const Placeholders& values, float alpha, bool shadowPass, bool showMissing) {
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
        case Kind::Text: {
            const std::wstring t = widen(fillPlaceholders(e.text, values, showMissing));
            ComPtr<IDWriteTextLayout> lay = layoutFor(c, e, t.empty() ? L" " : t);
            if (!lay) break;
            if (stroke) {   // an outline: the text drawn around itself, under the letters
                const int steps = std::max(12, (int)(e.strokeWidth * 4));
                for (int i = 0; i < steps; ++i) {
                    const float a = 2 * kPi * i / steps;
                    rt->DrawTextLayout(D2D1::Point2F(e.x + std::cos(a) * e.strokeWidth, e.y + std::sin(a) * e.strokeWidth), lay.Get(), stroke.Get());
                }
            }
            if (fill && e.fill.type != Fill::None) rt->DrawTextLayout(D2D1::Point2F(e.x, e.y), lay.Get(), fill.Get());
            break;
        }
        case Kind::Image: {
            ComPtr<ID2D1Bitmap> bmp = loadPicture(c, rt, e.file);
            if (!bmp) {
                if (!shadowPass) {   // a placeholder so it can be seen and picked
                    ComPtr<ID2D1SolidColorBrush> ph;
                    rt->CreateSolidColorBrush(D2D1::ColorF(0.5f, 0.55f, 0.62f, 0.35f * alpha), &ph);
                    rt->FillRoundedRectangle(D2D1::RoundedRect(box, r, r), ph.Get());
                }
                break;
            }
            const D2D1_SIZE_F s = bmp->GetSize();
            const float k = e.cover ? std::max(e.w / s.width, e.h / s.height) : std::min(e.w / s.width, e.h / s.height);
            const float dw = s.width * k, dh = s.height * k;
            const D2D1_RECT_F dst = D2D1::RectF(e.x + (e.w - dw) / 2, e.y + (e.h - dh) / 2, e.x + (e.w + dw) / 2, e.y + (e.h + dh) / 2);
            ComPtr<ID2D1RoundedRectangleGeometry> clip;
            c.d2->CreateRoundedRectangleGeometry(D2D1::RoundedRect(box, r, r), &clip);
            rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.Get()), nullptr);
            if (shadowPass) rt->FillRectangle(dst, black.Get());
            else rt->DrawBitmap(bmp.Get(), dst, alpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            rt->PopLayer();
            if (stroke)
                rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(box.left + e.strokeWidth / 2, box.top + e.strokeWidth / 2, box.right - e.strokeWidth / 2,
                                                                       box.bottom - e.strokeWidth / 2),
                                                           r, r),
                                         stroke.Get(), e.strokeWidth);
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
Placeholders gValues;
unsigned gValuesVersion = 1;

}  // namespace

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

bool renderDesign(const Document& d, float tMs, float scale, const Placeholders& values, DesignPicture& out, std::wstring& error, bool showMissing) {
    Ctx c;
    if (!makeCtx(c)) {
        error = L"Direct2D isn't available";
        return false;
    }
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
        return D2D1::Matrix3x2F::Scale(p.scale, p.scale, centre) * D2D1::Matrix3x2F::Rotation(e.rotation, centre) *
               D2D1::Matrix3x2F::Translation(p.dx, p.dy) * base;
    };

    // Shadows first need each element's shape on its own: drawn in black to a separate bitmap, blurred,
    // then used as a mask in the shadow's colour.
    // Each shadow is only as big as its element and the blur around it.
    struct ShadowPic {
        ComPtr<ID2D1Bitmap> bmp;
        float x = 0, y = 0, w = 0, h = 0;   // where it goes, in output pixels (before the entrance and offset)
    };
    std::vector<ShadowPic> shadows(d.elements.size());
    for (std::size_t i = 0; i < d.elements.size(); ++i) {
        const Element& e = d.elements[i];
        if (!e.visible || !e.shadow.on || e.shadow.colour.a == 0) continue;
        const int blur = std::max(0, (int)std::lround(e.shadow.blur * scale / 2));
        const float margin = blur * 3.0f + 2 + e.strokeWidth * scale;
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
        drawShape(c, srt.Get(), e, values, 1, true, showMissing);
        if (FAILED(srt->EndDraw())) continue;
        std::vector<std::uint8_t> px((size_t)bw * bh * 4);
        if (FAILED(sb->CopyPixels(nullptr, (UINT)bw * 4, (UINT)px.size(), px.data()))) continue;
        std::vector<std::uint8_t> a((size_t)bw * bh);
        for (size_t k = 0; k < a.size(); ++k) a[k] = px[k * 4 + 3];
        blurAlpha(a, bw, bh, blur);
        const Rgba sc = e.shadow.colour;
        for (size_t k = 0; k < a.size(); ++k) {
            const unsigned al = a[k] * sc.a / 255;
            px[k * 4 + 0] = (std::uint8_t)(sc.b * al / 255);
            px[k * 4 + 1] = (std::uint8_t)(sc.g * al / 255);
            px[k * 4 + 2] = (std::uint8_t)(sc.r * al / 255);
            px[k * 4 + 3] = (std::uint8_t)al;
        }
        rt->CreateBitmap(D2D1::SizeU((UINT)bw, (UINT)bh), px.data(), (UINT)bw * 4,
                         D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &shadows[i].bmp);
        shadows[i].x = (float)bx, shadows[i].y = (float)by, shadows[i].w = (float)bw, shadows[i].h = (float)bh;
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
        if (shadows[i].bmp) {
            // The shadow bitmap is in page pixels; it moves with its element's entrance and offset.
            const D2D1_POINT_2F centre = D2D1::Point2F((e.x + e.w / 2) * scale, (e.y + e.h / 2) * scale);
            rt->SetTransform(D2D1::Matrix3x2F::Translation(-centre.x, -centre.y) * D2D1::Matrix3x2F::Scale(p.scale, p.scale) *
                             D2D1::Matrix3x2F::Rotation(e.rotation) *
                             D2D1::Matrix3x2F::Translation(centre.x + (p.dx + e.shadow.dx) * scale, centre.y + (p.dy + e.shadow.dy) * scale));
            if (wiping) rt->PushAxisAlignedClip(D2D1::RectF(e.x * scale, -1e5f, (e.x + e.w * p.reveal) * scale, 1e5f), D2D1_ANTIALIAS_MODE_ALIASED);
            const ShadowPic& sp = shadows[i];
            rt->DrawBitmap(sp.bmp.Get(), D2D1::RectF(sp.x, sp.y, sp.x + sp.w, sp.y + sp.h), alpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            if (wiping) rt->PopAxisAlignedClip();
        }
        rt->SetTransform(placement(e, p));
        if (wiping) rt->PushAxisAlignedClip(D2D1::RectF(e.x, e.y - 1e4f, e.x + e.w * p.reveal, e.y + e.h + 1e4f), D2D1_ANTIALIAS_MODE_ALIASED);
        drawShape(c, rt.Get(), e, values, alpha, false, showMissing);
        if (wiping) rt->PopAxisAlignedClip();
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
