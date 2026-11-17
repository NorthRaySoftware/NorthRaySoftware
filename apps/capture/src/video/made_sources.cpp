// NorthRay Capture: the picture sources made in Capture itself (system 2, Milestone 8):
//
//   Image      a file (PNG, JPEG, BMP, GIF, TIFF, and WebP/HEIC where Windows has those codecs),
//              through WIC; animated GIFs play (frame delays, positions and disposal honoured).
//              The file is looked at every two seconds and reloaded when it changes.
//   Text       DirectWrite: font, size, colour, bold, italic, alignment, wrapping, an outline.
//   Colour     a solid colour (any alpha) of a chosen size.
//   Slideshow  the images in a folder, one after another, each fitted into a fixed size.
//   Region     part of a monitor: a display capture underneath, with the region copied out on the
//              graphics card.
//
// Files are read on a worker thread; the video thread only uploads finished pictures.
#include "video/sources.hpp"

#include "video/devices.hpp"
#include "video/design_render.hpp"

#include "ncap/aurora_sky.hpp"
#include "ncap/visualiser.hpp"
#include "ncap/sourceutil.hpp"

#include "nlohmann/json.hpp"

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>

#include <algorithm>
#include <mutex>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

namespace ncap::video {

using nlohmann::json;
namespace su = sourceutil;

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
std::string str(const json& j, const char* k, const std::string& fallback = {}) {
    return j.is_object() && j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : fallback;
}
double num(const json& j, const char* k, double fallback) { return j.is_object() && j.contains(k) && j[k].is_number() ? j[k].get<double>() : fallback; }
bool flag(const json& j, const char* k, bool fallback) { return j.is_object() && j.contains(k) && j[k].is_boolean() ? j[k].get<bool>() : fallback; }
std::wstring sizeText(int w, int h) { return std::to_wstring(w) + L"×" + std::to_wstring(h); }
std::wstring fileName(const std::wstring& path) {
    const std::size_t s = path.find_last_of(L"\\/");
    return s == std::wstring::npos ? path : path.substr(s + 1);
}

constexpr int kMaxSide = 8192;
constexpr std::size_t kMaxAnimationBytes = 512u * 1024 * 1024;

// A straight-alpha BGRA picture.
struct Picture {
    std::vector<std::uint8_t> bgra;
    int w = 0, h = 0;
    int delayMs = 0;   // animation frames
};

bool uploadPicture(Gpu& gpu, const Picture& p, Target& out) {
    if (p.w <= 0 || p.h <= 0 || p.bgra.size() < (std::size_t)p.w * (std::size_t)p.h * 4) return false;
    if (!out.ensure(gpu.device(), p.w, p.h)) return false;
    gpu.context()->UpdateSubresource(out.tex.Get(), 0, nullptr, p.bgra.data(), (UINT)p.w * 4, 0);
    return true;
}

// WIC, once per thread that uses it (COM is initialised by the caller).
ComPtr<IWICImagingFactory> wic() {
    ComPtr<IWICImagingFactory> f;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    return f;
}

bool toPicture(IWICImagingFactory* f, IWICBitmapSource* src, Picture& out, int fitW = 0, int fitH = 0) {
    UINT w = 0, h = 0;
    if (FAILED(src->GetSize(&w, &h)) || !w || !h || w > (UINT)kMaxSide || h > (UINT)kMaxSide) return false;
    ComPtr<IWICBitmapSource> s = src;
    if (fitW > 0 && fitH > 0) {   // scaled to fit inside fitW×fitH, keeping its shape
        const double k = std::min((double)fitW / w, (double)fitH / h);
        const UINT nw = std::max(1u, (UINT)std::lround(w * k)), nh = std::max(1u, (UINT)std::lround(h * k));
        ComPtr<IWICBitmapScaler> sc;
        if (SUCCEEDED(f->CreateBitmapScaler(&sc)) && SUCCEEDED(sc->Initialize(s.Get(), nw, nh, WICBitmapInterpolationModeFant))) {
            s = sc;
            w = nw;
            h = nh;
        }
    }
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(f->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(s.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return false;
    out.w = (int)w;
    out.h = (int)h;
    out.bgra.resize((std::size_t)w * h * 4);
    return SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)out.bgra.size(), out.bgra.data()));
}

int metaInt(IWICMetadataQueryReader* q, const wchar_t* name, int fallback) {
    if (!q) return fallback;
    PROPVARIANT v;
    PropVariantInit(&v);
    int r = fallback;
    if (SUCCEEDED(q->GetMetadataByName(name, &v))) {
        if (v.vt == VT_UI1) r = v.bVal;
        else if (v.vt == VT_UI2) r = v.uiVal;
        else if (v.vt == VT_UI4) r = (int)v.ulVal;
        else if (v.vt == VT_I4) r = v.lVal;
    }
    PropVariantClear(&v);
    return r;
}

// Every frame of a file: one for most; an animated GIF's frames put together as they show.
bool loadFrames(const std::wstring& path, std::vector<Picture>& frames, std::wstring& error) {
    frames.clear();
    auto f = wic();
    ComPtr<IWICBitmapDecoder> dec;
    if (!f || FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) {
        error = GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES ? L"The file isn't there any more"
                                                                             : L"Windows can't open this kind of image";
        return false;
    }
    UINT count = 0;
    dec->GetFrameCount(&count);
    GUID fmt{};
    dec->GetContainerFormat(&fmt);
    if (count <= 1 || fmt != GUID_ContainerFormatGif) {
        ComPtr<IWICBitmapFrameDecode> fr;
        Picture p;
        if (FAILED(dec->GetFrame(0, &fr)) || !toPicture(f.Get(), fr.Get(), p)) {
            error = L"The image couldn't be read (or it's bigger than 8192 pixels)";
            return false;
        }
        frames.push_back(std::move(p));
        return true;
    }
    // An animated GIF.
    ComPtr<IWICMetadataQueryReader> top;
    dec->GetMetadataQueryReader(&top);
    const int W = metaInt(top.Get(), L"/logscrdesc/Width", 0), H = metaInt(top.Get(), L"/logscrdesc/Height", 0);
    if (W <= 0 || H <= 0 || W > kMaxSide || H > kMaxSide) {
        error = L"The GIF's size couldn't be read";
        return false;
    }
    std::vector<std::uint8_t> canvas((std::size_t)W * H * 4, 0), previous;
    std::size_t total = 0;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IWICBitmapFrameDecode> fr;
        if (FAILED(dec->GetFrame(i, &fr))) break;
        ComPtr<IWICMetadataQueryReader> q;
        fr->GetMetadataQueryReader(&q);
        const int left = metaInt(q.Get(), L"/imgdesc/Left", 0), topY = metaInt(q.Get(), L"/imgdesc/Top", 0);
        const int delay = su::gifDelayMs(metaInt(q.Get(), L"/grctlext/Delay", 10));
        const int disposal = metaInt(q.Get(), L"/grctlext/Disposal", 0);
        Picture part;
        if (!toPicture(f.Get(), fr.Get(), part)) break;
        if (disposal == 3) previous = canvas;
        // Draw the frame's part over the canvas (its transparent pixels leave what's there).
        for (int y = 0; y < part.h; ++y) {
            const int cy = topY + y;
            if (cy < 0 || cy >= H) continue;
            for (int x = 0; x < part.w; ++x) {
                const int cx = left + x;
                if (cx < 0 || cx >= W) continue;
                const std::uint8_t* s = &part.bgra[((std::size_t)y * part.w + x) * 4];
                if (s[3] == 0) continue;
                std::memcpy(&canvas[((std::size_t)cy * W + cx) * 4], s, 4);
            }
        }
        Picture out;
        out.w = W;
        out.h = H;
        out.bgra = canvas;
        out.delayMs = delay;
        total += out.bgra.size();
        frames.push_back(std::move(out));
        if (total > kMaxAnimationBytes) break;   // very long, very big animations: the start only
        // What happens before the next frame.
        if (disposal == 2) {
            for (int y = std::max(0, topY); y < std::min(H, topY + part.h); ++y)
                std::memset(&canvas[((std::size_t)y * W + std::max(0, left)) * 4], 0,
                            (std::size_t)(std::min(W, left + part.w) - std::max(0, left)) * 4);
        } else if (disposal == 3 && !previous.empty()) {
            canvas = previous;
        }
    }
    if (frames.empty()) {
        error = L"The GIF couldn't be read";
        return false;
    }
    return true;
}

ULONGLONG lastWrite(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return 0;
    return ((ULONGLONG)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
}

// A worker thread with COM, stopped with the source.
class Worker {
public:
    ~Worker() { stop(); }
    template <class F>
    void start(F body) {
        stop();
        quit_ = false;
        t_ = std::thread([this, body]() mutable {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            body(quit_);
            CoUninitialize();
        });
    }
    void stop() {
        quit_ = true;
        if (t_.joinable()) t_.join();
    }

private:
    std::thread t_;
    std::atomic<bool> quit_{false};
};

bool sleepUnless(const std::atomic<bool>& quit, int ms) {
    for (int t = 0; t < ms && !quit; t += 20) Sleep(20);
    return !quit;
}

// ------------------------------------------------------------------ image

class ImageSource : public VideoSource {
public:
    explicit ImageSource(const std::string& settings) { setSettings(settings); }
    ~ImageSource() override { worker_.stop(); }
    bool hasAlpha() const override { return true; }

    void update(Gpu& gpu) override {
        wanted();
        std::string j;
        if (takeSettings(j)) {
            const json s = json::parse(j, nullptr, false);
            path_ = widen(str(s, "file"));
            start();
        }
        std::lock_guard<std::mutex> l(m_);
        if (frames_.empty()) return;
        // Which frame now (animations loop).
        std::size_t want = 0;
        if (frames_.size() > 1) {
            const ULONGLONG t = (GetTickCount64() - startedAt_) % std::max<ULONGLONG>(1, totalMs_);
            ULONGLONG acc = 0;
            for (std::size_t i = 0; i < frames_.size(); ++i) {
                acc += (ULONGLONG)frames_[i].delayMs;
                if (t < acc) {
                    want = i;
                    break;
                }
            }
        }
        if (serial_ != shownSerial_ || want != shown_) {
            if (uploadPicture(gpu, frames_[want], out_)) {
                hasPicture_ = true;
                w_ = frames_[want].w;
                h_ = frames_[want].h;
            }
            shown_ = want;
            shownSerial_ = serial_;
        }
    }

private:
    void start() {
        const std::wstring path = path_;
        worker_.start([this, path](std::atomic<bool>& quit) {
            ULONGLONG seenWrite = ~0ULL;
            while (!quit) {
                const ULONGLONG wtime = lastWrite(path);
                if (wtime != seenWrite) {
                    seenWrite = wtime;
                    std::vector<Picture> frames;
                    std::wstring error;
                    if (path.empty()) setStatus(L"No image chosen yet", true);
                    else if (loadFrames(path, frames, error)) {
                        ULONGLONG total = 0;
                        for (auto& f : frames) total += (ULONGLONG)f.delayMs;
                        const int w = frames[0].w, h = frames[0].h;
                        const std::size_t n = frames.size();
                        {
                            std::lock_guard<std::mutex> l(m_);
                            frames_ = std::move(frames);
                            totalMs_ = total;
                            startedAt_ = GetTickCount64();
                            ++serial_;
                        }
                        setStatus(fileName(path) + L" · " + sizeText(w, h) + (n > 1 ? L" · " + std::to_wstring(n) + L" frames" : L""));
                    } else {
                        setStatus(fileName(path) + L": " + error, true);
                    }
                }
                sleepUnless(quit, 2000);   // then look whether the file has changed
            }
        });
    }
    std::wstring path_;
    Worker worker_;
    std::mutex m_;
    std::vector<Picture> frames_;
    ULONGLONG totalMs_ = 0, startedAt_ = 0;
    std::uint64_t serial_ = 0, shownSerial_ = ~0ULL;
    std::size_t shown_ = 0;
};

// ------------------------------------------------------------------ colour

class ColourSource : public VideoSource {
public:
    explicit ColourSource(const std::string& settings) { setSettings(settings); }
    bool hasAlpha() const override { return true; }
    void update(Gpu& gpu) override {
        std::string j;
        if (!takeSettings(j) && hasPicture_) return;
        const json s = json::parse(j.empty() ? "{}" : j, nullptr, false);
        if (!j.empty()) {
            if (!su::parseColour(str(s, "colour", "#1E2A44"), c_)) c_ = {30, 42, 68, 255};
            w_ = std::clamp((int)num(s, "width", 1920), 1, kMaxSide);
            h_ = std::clamp((int)num(s, "height", 1080), 1, kMaxSide);
        }
        if (!out_.ensure(gpu.device(), w_, h_)) return;
        gpu.clear(out_.rtv.Get(), c_.r / 255.0f, c_.g / 255.0f, c_.b / 255.0f, c_.a / 255.0f);
        hasPicture_ = true;
        setStatus(widen(su::colourText(c_)) + L" · " + sizeText(w_, h_));
    }

private:
    su::Rgba c_{30, 42, 68, 255};
};

// ------------------------------------------------------------------ text

class TextSource : public VideoSource {
public:
    explicit TextSource(const std::string& settings) { setSettings(settings); }
    bool hasAlpha() const override { return true; }
    void update(Gpu& gpu) override {
        std::string j;
        if (takeSettings(j)) {
            Picture p;
            std::wstring error;
            if (render(json::parse(j, nullptr, false), p, error)) {
                if (uploadPicture(gpu, p, out_)) {
                    hasPicture_ = true;
                    w_ = p.w;
                    h_ = p.h;
                }
                setStatus(L"Text · " + sizeText(p.w, p.h));
            } else {
                setStatus(error, true);
            }
        }
    }

private:
    static bool render(const json& s, Picture& out, std::wstring& error) {
        std::wstring text = widen(str(s, "text", "Text"));
        if (text.empty()) text = L" ";
        const std::wstring font = widen(str(s, "font", "Segoe UI"));
        const float size = (float)std::clamp(num(s, "size", 64), 6.0, 600.0);
        su::Rgba colour, outline;
        if (!su::parseColour(str(s, "colour", "#FFFFFF"), colour)) colour = {255, 255, 255, 255};
        if (!su::parseColour(str(s, "outlineColour", "#000000"), outline)) outline = {0, 0, 0, 255};
        const float outlineW = (float)std::clamp(num(s, "outline", 0), 0.0, 40.0);
        const float wrap = (float)std::clamp(num(s, "wrap", 0), 0.0, (double)kMaxSide);
        const std::string align = str(s, "align", "left");

        ComPtr<IDWriteFactory> dw;
        ComPtr<ID2D1Factory> d2;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.GetAddressOf()))) ||
            FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2.GetAddressOf()))) {
            error = L"DirectWrite isn't available";
            return false;
        }
        ComPtr<IDWriteTextFormat> fmt;
        if (FAILED(dw->CreateTextFormat(font.c_str(), nullptr, flag(s, "bold", false) ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                        flag(s, "italic", false) ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                                        L"en-gb", &fmt))) {
            error = L"The font couldn't be used";
            return false;
        }
        fmt->SetTextAlignment(align == "centre" ? DWRITE_TEXT_ALIGNMENT_CENTER : align == "right" ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
        fmt->SetWordWrapping(wrap > 0 ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dw->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt.Get(), wrap > 0 ? wrap : 100000.0f, 100000.0f, &layout))) {
            error = L"The text couldn't be laid out";
            return false;
        }
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        const float boxW = wrap > 0 ? wrap : m.widthIncludingTrailingWhitespace;
        if (wrap <= 0) layout->SetMaxWidth(std::max(1.0f, boxW));
        const float pad = std::ceil(outlineW) + 2;
        const int W = std::clamp((int)std::ceil(boxW + pad * 2), 1, kMaxSide), H = std::clamp((int)std::ceil(m.height + pad * 2), 1, kMaxSide);

        auto f = wic();
        ComPtr<IWICBitmap> bmp;
        ComPtr<ID2D1RenderTarget> rt;
        const D2D1_RENDER_TARGET_PROPERTIES props =
            D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (!f || FAILED(f->CreateBitmap((UINT)W, (UINT)H, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp)) ||
            FAILED(d2->CreateWicBitmapRenderTarget(bmp.Get(), &props, &rt))) {
            error = L"The text couldn't be drawn";
            return false;
        }
        ComPtr<ID2D1SolidColorBrush> fill, edge;
        rt->CreateSolidColorBrush(D2D1::ColorF(colour.r / 255.0f, colour.g / 255.0f, colour.b / 255.0f, colour.a / 255.0f), &fill);
        rt->CreateSolidColorBrush(D2D1::ColorF(outline.r / 255.0f, outline.g / 255.0f, outline.b / 255.0f, outline.a / 255.0f), &edge);
        rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);   // no coloured fringes on a see-through picture
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0, 0, 0, 0));
        if (outlineW > 0) {   // the outline: the text drawn around itself in a circle, under the fill
            const int steps = std::max(12, (int)(outlineW * 4));
            for (int i = 0; i < steps; ++i) {
                const float a = 6.2831853f * i / steps;
                rt->DrawTextLayout(D2D1::Point2F(pad + std::cos(a) * outlineW, pad + std::sin(a) * outlineW), layout.Get(), edge.Get());
            }
        }
        rt->DrawTextLayout(D2D1::Point2F(pad, pad), layout.Get(), fill.Get());
        if (FAILED(rt->EndDraw())) {
            error = L"The text couldn't be drawn";
            return false;
        }
        out.w = W;
        out.h = H;
        out.bgra.resize((std::size_t)W * H * 4);
        if (FAILED(bmp->CopyPixels(nullptr, (UINT)W * 4, (UINT)out.bgra.size(), out.bgra.data()))) return false;
        // Premultiplied to straight alpha (what the compositor blends).
        for (std::size_t i = 0; i < out.bgra.size(); i += 4) {
            const int a = out.bgra[i + 3];
            if (a == 0 || a == 255) continue;
            for (int c = 0; c < 3; ++c) out.bgra[i + (std::size_t)c] = (std::uint8_t)std::min(255, out.bgra[i + (std::size_t)c] * 255 / a);
        }
        return true;
    }
};

// ------------------------------------------------------------------ slideshow

class SlideshowSource : public VideoSource {
public:
    explicit SlideshowSource(const std::string& settings) { setSettings(settings); }
    ~SlideshowSource() override { worker_.stop(); }
    bool hasAlpha() const override { return true; }
    void update(Gpu& gpu) override {
        wanted();
        std::string j;
        if (takeSettings(j)) {
            const json s = json::parse(j, nullptr, false);
            folder_ = widen(str(s, "folder"));
            seconds_ = std::clamp((int)num(s, "seconds", 5), 1, 3600);
            shuffle_ = flag(s, "shuffle", false);
            boxW_ = std::clamp((int)num(s, "width", 1920), 16, kMaxSide);
            boxH_ = std::clamp((int)num(s, "height", 1080), 16, kMaxSide);
            w_ = boxW_;
            h_ = boxH_;
            start();
        }
        std::lock_guard<std::mutex> l(m_);
        if (serial_ == shown_ || slide_.w <= 0) return;
        // The slide fitted in the middle of the box; the rest see-through.
        if (!out_.ensure(gpu.device(), boxW_, boxH_)) return;
        gpu.clear(out_.rtv.Get(), 0, 0, 0, 0);
        const int x = (boxW_ - slide_.w) / 2, y = (boxH_ - slide_.h) / 2;
        D3D11_BOX box{(UINT)x, (UINT)y, 0, (UINT)(x + slide_.w), (UINT)(y + slide_.h), 1};
        gpu.context()->UpdateSubresource(out_.tex.Get(), 0, &box, slide_.bgra.data(), (UINT)slide_.w * 4, 0);
        hasPicture_ = true;
        shown_ = serial_;
    }

private:
    void start() {
        const std::wstring folder = folder_;
        const int seconds = seconds_, bw = boxW_, bh = boxH_;
        const bool shuffle = shuffle_;
        worker_.start([this, folder, seconds, shuffle, bw, bh](std::atomic<bool>& quit) {
            auto f = wic();
            std::uint32_t round = GetTickCount();
            while (!quit) {
                std::vector<std::string> names;
                std::error_code ec;
                if (!folder.empty())
                    for (auto& e : std::filesystem::directory_iterator(std::filesystem::path(folder), ec))
                        if (e.is_regular_file(ec) && su::isImageFile(narrow(e.path().filename().wstring()))) names.push_back(narrow(e.path().filename().wstring()));
                if (folder.empty()) setStatus(L"No folder chosen yet", true);
                else if (ec) setStatus(L"The folder can't be read", true);
                else if (names.empty()) setStatus(L"No images in " + fileName(folder), true);
                if (names.empty()) {
                    if (!sleepUnless(quit, 3000)) return;
                    continue;
                }
                const auto order = su::slideOrder(names, shuffle, round++);
                int shownCount = 0;
                for (std::size_t i = 0; i < order.size() && !quit; ++i) {
                    const std::wstring path = folder + L"\\" + widen(order[i]);
                    ComPtr<IWICBitmapDecoder> dec;
                    ComPtr<IWICBitmapFrameDecode> fr;
                    Picture p;
                    if (!f || FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) ||
                        FAILED(dec->GetFrame(0, &fr)) || !toPicture(f.Get(), fr.Get(), p, bw, bh))
                        continue;   // a file Windows can't read: skip it
                    {
                        std::lock_guard<std::mutex> l(m_);
                        slide_ = std::move(p);
                        ++serial_;
                    }
                    ++shownCount;
                    setStatus(fileName(folder) + L" · " + std::to_wstring(i + 1) + L" of " + std::to_wstring(order.size()) + L": " +
                              widen(order[i]));
                    if (!sleepUnless(quit, seconds * 1000)) return;
                }
                if (!shownCount) {
                    setStatus(L"None of the images in " + fileName(folder) + L" could be read", true);
                    if (!sleepUnless(quit, 3000)) return;
                }
            }
        });
    }
    std::wstring folder_;
    int seconds_ = 5, boxW_ = 1920, boxH_ = 1080;
    bool shuffle_ = false;
    Worker worker_;
    std::mutex m_;
    Picture slide_;
    std::uint64_t serial_ = 0, shown_ = 0;
};

// ------------------------------------------------------------------ region

class RegionSource : public VideoSource {
public:
    explicit RegionSource(const std::string& settings) { setSettings(settings); }
    void update(Gpu& gpu) override {
        wanted();
        std::string j;
        if (takeSettings(j)) {
            const json s = json::parse(j, nullptr, false);
            rx_ = (int)num(s, "x", 0);
            ry_ = (int)num(s, "y", 0);
            rw_ = std::clamp((int)num(s, "w", 640), 2, kMaxSide);
            rh_ = std::clamp((int)num(s, "h", 360), 2, kMaxSide);
            json d = {{"monitor", str(s, "monitor")}, {"method", str(s, "method", "auto")}, {"cursor", flag(s, "cursor", true)}};
            display_ = createVideoSource(SourceKind::Display, d.dump());
            monitor_ = widen(str(s, "monitor"));
        }
        if (!display_) return;
        display_->update(gpu);
        ID3D11Texture2D* src = display_->texture();
        if (!src) {
            setStatus(display_->status(), display_->problem());
            return;
        }
        D3D11_TEXTURE2D_DESC d{};
        src->GetDesc(&d);
        // The part of the region that's on the monitor (it may have shrunk since).
        const int x0 = std::clamp(rx_, 0, (int)d.Width), y0 = std::clamp(ry_, 0, (int)d.Height);
        const int x1 = std::clamp(rx_ + rw_, 0, (int)d.Width), y1 = std::clamp(ry_ + rh_, 0, (int)d.Height);
        if (x1 - x0 < 2 || y1 - y0 < 2) {
            setStatus(L"The region is outside the display now", true);
            return;
        }
        if (!out_.ensure(gpu.device(), x1 - x0, y1 - y0)) return;
        const D3D11_BOX box{(UINT)x0, (UINT)y0, 0, (UINT)x1, (UINT)y1, 1};
        gpu.context()->CopySubresourceRegion(out_.tex.Get(), 0, 0, 0, 0, src, 0, &box);
        hasPicture_ = true;
        if (w_ != out_.w || h_ != out_.h) {
            w_ = out_.w;
            h_ = out_.h;
        }
        setStatus(L"Part of the display · " + sizeText(w_, h_) + L" at " + std::to_wstring(x0) + L", " + std::to_wstring(y0));
    }

private:
    std::shared_ptr<VideoSource> display_;
    std::wstring monitor_;
    int rx_ = 0, ry_ = 0, rw_ = 640, rh_ = 360;
};

}  // namespace

std::shared_ptr<VideoSource> createMediaSource(const std::string& settings);   // media_source.cpp

// ------------------------------------------------------------------ overlay design

// A design from the overlay designer: drawn on a worker thread when it changes (or its placeholders
// do), and every 33 ms while its elements are coming in after its scene is shown ("enter").
class DesignSource : public VideoSource {
public:
    explicit DesignSource(const std::string& settings) {
        setSettings(settings);
        worker_.start([this](std::atomic<bool>& quit) { run(quit); });
    }
    ~DesignSource() override { worker_.stop(); }
    bool hasAlpha() const override { return true; }
    void command(const std::string& c) override {
        if (c == "enter" || c == "start" || c == "restart") {
            std::lock_guard<std::mutex> l(m_);
            enteredAt_ = GetTickCount64();
            ++changes_;
        }
    }
    void update(Gpu& gpu) override {
        wanted();
        std::string j;
        if (takeSettings(j)) {
            std::lock_guard<std::mutex> l(m_);
            doc_ = design::fromJson(j);
            w_ = doc_.width;
            h_ = doc_.height;
            ++changes_;
        }
        std::lock_guard<std::mutex> l(m_);
        if (picSerial_ == shownSerial_ || pic_.w <= 0) return;
        Picture p;
        p.w = pic_.w;
        p.h = pic_.h;
        p.bgra.swap(pic_.bgra);
        if (uploadPicture(gpu, p, out_)) hasPicture_ = true;
        shownSerial_ = picSerial_;
    }

private:
    void run(std::atomic<bool>& quit) {
        std::uint64_t done = 0;
        unsigned valuesSeen = 0;
        while (!quit) {
            unsigned vv = 0;
            const Placeholders values = designValues(&vv);
            design::Document doc;
            float t = 1e9f;
            bool redo = false;
            design::Clock clock;
            {
                std::lock_guard<std::mutex> l(m_);
                doc = doc_;
                if (enteredAt_) t = (float)(GetTickCount64() - enteredAt_);
                clock = clockNow(enteredAt_ ? (std::int64_t)(GetTickCount64() - enteredAt_) : 0);
                // Timers: every second (every frame or two while one pulses).
                if (design::usesTime(doc)) {
                    std::string shown;
                    float pulse = 1;
                    for (auto& e : doc.elements)
                        if (e.visible && e.kind == design::Kind::Timer) {
                            const auto ts = design::timerText(e, clock);
                            shown += ts.text + "|";
                            pulse = std::max(pulse, ts.pulse);
                        }
                    if (shown != lastTimers_ || pulse > 1.0001f || lastPulse_) {
                        lastTimers_ = shown;
                        ++changes_;
                    }
                    lastPulse_ = pulse > 1.0001f;
                }
                // Again while elements are coming in, and once more after the last picture was mid-entrance.
                redo = changes_ != done || vv != valuesSeen || (enteredAt_ && design::animating(doc, t - 40)) || lastMoving_;
                done = changes_;
            }
            if (redo && (recentlyWanted() || !hasDrawn_)) {
                valuesSeen = vv;
                DesignPicture pic;
                std::wstring error;
                lastMoving_ = enteredAt_ && design::animating(doc, t);
                if (renderDesign(doc, t, 1.0f, values, pic, error, false, clock)) {
                    std::lock_guard<std::mutex> l(m_);
                    pic_ = std::move(pic);
                    ++picSerial_;
                    hasDrawn_ = true;
                    setStatus(L"Design · " + std::to_wstring(doc.elements.size()) + (doc.elements.size() == 1 ? L" element · " : L" elements · ") +
                              sizeText(doc.width, doc.height));
                } else {
                    setStatus(error, true);
                }
            } else if (redo) {
                std::lock_guard<std::mutex> l(m_);
                --done;   // try again when it's shown
            }
            sleepUnless(quit, 33);
        }
    }

    Worker worker_;
    std::mutex m_;
    design::Document doc_;
    std::uint64_t changes_ = 0;
    ULONGLONG enteredAt_ = 0;
    DesignPicture pic_;
    std::uint64_t picSerial_ = 0, shownSerial_ = 0;
    std::atomic<bool> hasDrawn_{false};
    bool lastMoving_ = false;   // worker thread
    std::string lastTimers_;    // what the timers showed last time
    bool lastPulse_ = false;
};

// ------------------------------------------------------------------ the aurora sky

// Drawn by a pixel shader into its own picture every frame it's shown (the engine only updates sources in
// the scenes being composed). The shader is made from the settings (core/aurora_sky) and compiled again
// when they change; the time keeps running from when the source was made.
namespace {
std::mutex gBandsM;
std::vector<float> gBands;
float gLevel = 0;
}  // namespace

void setSoundBands(const std::vector<float>& bands, float level) {
    std::lock_guard<std::mutex> l(gBandsM);
    gBands = bands;
    gLevel = level;
}

class AuroraSkySource : public VideoSource {
public:
    explicit AuroraSkySource(const std::string& settings, bool pulse = false) : pulse_(pulse) {
        setSettings(settings);
        LARGE_INTEGER f, c;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&c);
        freq_ = (double)f.QuadPart;
        start_ = c.QuadPart;
    }
    void update(Gpu& gpu) override {
        std::string j;
        if (takeSettings(j)) {
            cfg_ = aurora_sky::fromJson(j);
            ps_.Reset();
            failed_ = false;
            w_ = cfg_.width;
            h_ = cfg_.height;
        }
        if (!ps_ && !failed_) {
            std::string err;
            ps_ = gpu.compilePixelShader(aurora_sky::shader(cfg_, pulse_), err);
            if (!ps_) {
                failed_ = true;
                setStatus(L"The aurora couldn't be drawn: " + widen(err.substr(0, 200)), true);
                return;
            }
            setStatus(pulse_ ? std::wstring(cfg_.wave ? L"Aurora wave" : L"Aurora pulse") + L" \u00B7 moves with the Stream mix \u00B7 " + sizeText(cfg_.width, cfg_.height)
                             : L"Aurora sky \u00B7 drawn on the graphics card \u00B7 " + sizeText(cfg_.width, cfg_.height));
        }
        if (!ps_ || !out_.ensure(gpu.device(), cfg_.width, cfg_.height)) return;
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        const double seconds = std::fmod((double)(c.QuadPart - start_) / freq_, 86400.0);
        const float W = (float)cfg_.width, H = (float)cfg_.height;
        const Pt corners[4] = {{0, 0}, {W, 0}, {W, H}, {0, H}};
        ID3D11ShaderResourceView* bands = pulse_ ? updateBands(gpu) : nullptr;
        gpu.begin(out_.rtv.Get(), cfg_.width, cfg_.height);
        gpu.drawWith(ps_.Get(), bands, nullptr, corners, (float)seconds, W, H);
        hasPicture_ = true;
    }

private:
    // The bands as a kMaxBands×1 texture (red the band, green the loudness), written each frame.
    ID3D11ShaderResourceView* updateBands(Gpu& gpu) {
        std::vector<float> b;
        float level;
        {
            std::lock_guard<std::mutex> l(gBandsM);
            b = gBands;
            level = gLevel;
        }
        const int n = std::clamp((int)b.size(), 1, visualiser::kMaxBands);
        if (!bandsTex_ || bandsW_ != n) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = (UINT)n;
            d.Height = 1;
            d.MipLevels = d.ArraySize = 1;
            d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;   // every card filters this (32-bit floats it may not)
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DYNAMIC;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            bandsTex_.Reset();
            bandsSrv_.Reset();
            if (FAILED(gpu.device()->CreateTexture2D(&d, nullptr, &bandsTex_)) ||
                FAILED(gpu.device()->CreateShaderResourceView(bandsTex_.Get(), nullptr, &bandsSrv_))) {
                setStatus(L"The aurora pulse couldn't make its sound texture", true);
                return nullptr;
            }
            bandsW_ = n;
        }
        ComPtr<ID3D11DeviceContext> ctx;
        gpu.device()->GetImmediateContext(&ctx);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(bandsTex_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return nullptr;
        std::uint8_t* p = static_cast<std::uint8_t*>(m.pData);
        auto byte = [](float v) { return (std::uint8_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255); };
        for (int i = 0; i < n; ++i) {
            p[4 * i] = byte(i < (int)b.size() ? b[(size_t)i] : 0.0f);   // red: the band
            p[4 * i + 1] = byte(level);                                    // green: the loudness
            p[4 * i + 2] = 0;
            p[4 * i + 3] = 255;
        }
        ctx->Unmap(bandsTex_.Get(), 0);
        return bandsSrv_.Get();
    }
    bool pulse_;
    ComPtr<ID3D11Texture2D> bandsTex_;
    ComPtr<ID3D11ShaderResourceView> bandsSrv_;
    int bandsW_ = 0;
    aurora_sky::Settings cfg_;
    ComPtr<ID3D11PixelShader> ps_;
    bool failed_ = false;
    double freq_ = 1;
    LONGLONG start_ = 0;
};

std::shared_ptr<VideoSource> createMadeSource(SourceKind kind, const std::string& settings) {
    switch (kind) {
        case SourceKind::Image: return std::make_shared<ImageSource>(settings);
        case SourceKind::Colour: return std::make_shared<ColourSource>(settings);
        case SourceKind::Text: return std::make_shared<TextSource>(settings);
        case SourceKind::Slideshow: return std::make_shared<SlideshowSource>(settings);
        case SourceKind::Region: return std::make_shared<RegionSource>(settings);
        case SourceKind::Media: return createMediaSource(settings);
        case SourceKind::Design: return std::make_shared<DesignSource>(settings);
        case SourceKind::AuroraSky: return std::make_shared<AuroraSkySource>(settings);
        case SourceKind::AuroraPulse: return std::make_shared<AuroraSkySource>(settings, true);
        default: return nullptr;
    }
}

}  // namespace ncap::video
