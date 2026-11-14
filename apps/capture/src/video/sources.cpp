#include "video/sources.hpp"

#include "video/devices.hpp"
#include "video/wgc.hpp"

#include "nlohmann/json.hpp"

#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <cstring>
#include <thread>
#include <vector>

#ifndef MF_E_HW_MFT_FAILED_START_STREAMING
#define MF_E_HW_MFT_FAILED_START_STREAMING _HRESULT_TYPEDEF_(0xC00D3704L)
#endif

namespace ncap::video {

using nlohmann::json;

// ------------------------------------------------------------------ the base

std::wstring VideoSource::status() const {
    std::lock_guard<std::mutex> l(m_);
    return status_;
}

void VideoSource::setStatus(const std::wstring& s, bool problem) {
    std::lock_guard<std::mutex> l(m_);
    status_ = s;
    problem_ = problem;
}

void VideoSource::setSettings(const std::string& j) {
    std::lock_guard<std::mutex> l(m_);
    pending_ = j;
    changed_ = true;
}

bool VideoSource::takeSettings(std::string& j) {
    std::lock_guard<std::mutex> l(m_);
    if (!changed_) return false;
    changed_ = false;
    j = pending_;
    return true;
}

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string str(const json& j, const char* k, const std::string& fallback = {}) {
    return j.is_object() && j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : fallback;
}
bool flag(const json& j, const char* k, bool fallback) { return j.is_object() && j.contains(k) && j[k].is_boolean() ? j[k].get<bool>() : fallback; }

std::wstring sizeText(int w, int h) { return std::to_wstring(w) + L"×" + std::to_wstring(h); }

// A picture made on another thread, handed to the video thread.
struct CpuPicture {
    std::mutex m;
    std::vector<std::uint8_t> bgra;
    int w = 0, h = 0, stride = 0;
    std::uint64_t serial = 0;
};

// Copies the newest CPU picture into the target if it's newer than `seen`.
bool upload(Gpu& gpu, CpuPicture& p, std::uint64_t& seen, Target& out) {
    std::lock_guard<std::mutex> l(p.m);
    if (p.serial == seen || p.w <= 0 || p.h <= 0) return false;
    seen = p.serial;
    if (!out.ensure(gpu.device(), p.w, p.h)) return false;
    gpu.context()->UpdateSubresource(out.tex.Get(), 0, nullptr, p.bgra.data(), (UINT)p.stride, 0);
    return true;
}

// A thread that runs `grab` about every `ms` until stopped (the GDI methods).
class Ticker {
public:
    ~Ticker() { stop(); }
    template <class F>
    void start(int ms, F grab) {
        stop();
        quit_ = false;
        t_ = std::thread([this, ms, grab]() mutable {
            while (!quit_) {
                const ULONGLONG t0 = GetTickCount64();
                grab();
                const ULONGLONG spent = GetTickCount64() - t0;
                for (ULONGLONG waited = spent; waited < (ULONGLONG)ms && !quit_; waited += 5) Sleep(5);
            }
        });
    }
    void stop() {
        quit_ = true;
        if (t_.joinable()) t_.join();
    }
    bool running() const { return t_.joinable(); }

private:
    std::thread t_;
    std::atomic<bool> quit_{false};
};

// GDI: copies part of a device context into a CPU picture (compatibility methods).
struct GdiCopy {
    HDC mem = nullptr;
    HBITMAP bmp = nullptr;
    void* bits = nullptr;
    int w = 0, h = 0;
    ~GdiCopy() { release(); }
    void release() {
        if (bmp) DeleteObject(bmp);
        if (mem) DeleteDC(mem);
        bmp = nullptr;
        mem = nullptr;
        bits = nullptr;
        w = h = 0;
    }
    bool ensure(int width, int height) {
        if (bmp && width == w && height == h) return true;
        release();
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = width;
        bi.bmiHeader.biHeight = -height;   // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        HDC screen = GetDC(nullptr);
        mem = CreateCompatibleDC(screen);
        bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, screen);
        if (!mem || !bmp) {
            release();
            return false;
        }
        SelectObject(mem, bmp);
        w = width;
        h = height;
        return true;
    }
    // Nothing but black (sampled): the copy didn't work.
    bool blank() {
        GdiFlush();
        const std::uint32_t* px = static_cast<const std::uint32_t*>(bits);
        for (int i = 0; i < 97; ++i) {
            const int x = (int)((long long)w * ((i * 37) % 97) / 97), y = (int)((long long)h * i / 97);
            if (px[(size_t)y * w + x] & 0x00FFFFFFu) return false;
        }
        return true;
    }
    void drawCursor(int originX, int originY) {
        CURSORINFO ci{};
        ci.cbSize = sizeof ci;
        if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return;
        ICONINFO ii{};
        if (!GetIconInfo(ci.hCursor, &ii)) return;
        DrawIconEx(mem, ci.ptScreenPos.x - originX - (int)ii.xHotspot, ci.ptScreenPos.y - originY - (int)ii.yHotspot, ci.hCursor, 0, 0, 0,
                   nullptr, DI_NORMAL);
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
    }
    void publish(CpuPicture& p) {
        GdiFlush();
        std::lock_guard<std::mutex> l(p.m);
        p.w = w;
        p.h = h;
        p.stride = w * 4;
        p.bgra.resize((size_t)w * h * 4);
        std::memcpy(p.bgra.data(), bits, p.bgra.size());
        ++p.serial;
    }
};

// ------------------------------------------------------------------ Windows.Graphics.Capture

class WgcSession {
public:
    ~WgcSession() { close(); }
    // The item's size, or {0,0} on failure (with `why`).
    bool open(Gpu& gpu, HWND window, HMONITOR monitor, bool cursor, std::wstring& why) {
        close();
        if (!wgc::supported()) {
            why = L"Windows Graphics Capture isn't available on this version of Windows";
            return false;
        }
        HRESULT hr = window ? wgc::itemForWindow(window, &item_) : wgc::itemForMonitor(monitor, &item_);
        if (FAILED(hr) || !item_) {
            why = L"Windows wouldn't capture it";
            return false;
        }
        if (FAILED(wgc::wrapDevice(gpu.device(), &device_)) || FAILED(item_->get_Size(&size_)) ||
            FAILED(wgc::createFramePool(device_, size_, &pool_)) || FAILED(pool_->CreateCaptureSession(item_, &session_))) {
            why = L"Windows Graphics Capture couldn't start";
            close();
            return false;
        }
        wgc::IGraphicsCaptureSession2* s2 = nullptr;
        if (SUCCEEDED(session_->QueryInterface(__uuidof(wgc::IGraphicsCaptureSession2), reinterpret_cast<void**>(&s2)))) {
            s2->put_IsCursorCaptureEnabled(cursor);
            s2->Release();
        }
        // Windows 11 draws a yellow border round what's captured unless told it isn't needed.
        wgc::IGraphicsCaptureSession3* s3 = nullptr;
        if (SUCCEEDED(session_->QueryInterface(__uuidof(wgc::IGraphicsCaptureSession3), reinterpret_cast<void**>(&s3)))) {
            s3->put_IsBorderRequired(false);
            s3->Release();
        }
        if (FAILED(session_->StartCapture())) {
            why = L"Windows Graphics Capture couldn't start";
            close();
            return false;
        }
        return true;
    }
    // Copies the newest frame into `out`; true if there was one.
    bool pull(Gpu& gpu, Target& out) {
        if (!pool_) return false;
        wgc::IDirect3D11CaptureFrame* frame = nullptr;
        for (;;) {   // only the newest matters
            wgc::IDirect3D11CaptureFrame* next = nullptr;
            if (FAILED(pool_->TryGetNextFrame(&next)) || !next) break;
            if (frame) closeFrame(frame);
            frame = next;
        }
        if (!frame) return false;
        bool got = false;
        wgc::SizeInt32 content{};
        IInspectable* surface = nullptr;
        if (SUCCEEDED(frame->get_ContentSize(&content)) && SUCCEEDED(frame->get_Surface(&surface)) && surface) {
            wgc::IDirect3DDxgiInterfaceAccess* access = nullptr;
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(surface->QueryInterface(__uuidof(wgc::IDirect3DDxgiInterfaceAccess), reinterpret_cast<void**>(&access))) &&
                SUCCEEDED(access->GetInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) {
                D3D11_TEXTURE2D_DESC d{};
                tex->GetDesc(&d);
                const int w = std::min<int>(content.Width, (int)d.Width), h = std::min<int>(content.Height, (int)d.Height);
                if (w > 0 && h > 0 && out.ensure(gpu.device(), w, h)) {
                    D3D11_BOX box{0, 0, 0, (UINT)w, (UINT)h, 1};
                    gpu.context()->CopySubresourceRegion(out.tex.Get(), 0, 0, 0, 0, tex, 0, &box);
                    got = true;
                }
            }
            if (tex) tex->Release();
            if (access) access->Release();
            surface->Release();
        }
        closeFrame(frame);
        // The window was resized: the pool follows, so the next frames are whole.
        if (content.Width > 0 && content.Height > 0 && (content.Width != size_.Width || content.Height != size_.Height)) {
            size_ = content;
            pool_->Recreate(device_, wgc::kPixelFormatBGRA8, 2, size_);
        }
        return got;
    }
    void close() {
        auto closeIt = [](IInspectable* p) {
            if (!p) return;
            wgc::IClosable* c = nullptr;
            if (SUCCEEDED(p->QueryInterface(__uuidof(wgc::IClosable), reinterpret_cast<void**>(&c)))) {
                c->Close();
                c->Release();
            }
        };
        closeIt(session_);
        closeIt(pool_);
        if (session_) session_->Release();
        if (pool_) pool_->Release();
        if (item_) item_->Release();
        if (device_) device_->Release();
        session_ = nullptr;
        pool_ = nullptr;
        item_ = nullptr;
        device_ = nullptr;
    }
    bool isOpen() const { return pool_ != nullptr; }

private:
    static void closeFrame(wgc::IDirect3D11CaptureFrame* f) {
        wgc::IClosable* c = nullptr;
        if (SUCCEEDED(f->QueryInterface(__uuidof(wgc::IClosable), reinterpret_cast<void**>(&c)))) {
            c->Close();
            c->Release();
        }
        f->Release();
    }
    wgc::IGraphicsCaptureItem* item_ = nullptr;
    IInspectable* device_ = nullptr;
    wgc::IDirect3D11CaptureFramePool* pool_ = nullptr;
    wgc::IGraphicsCaptureSession* session_ = nullptr;
    wgc::SizeInt32 size_{};
};

// ------------------------------------------------------------------ the mouse pointer (Duplication)

// Desktop Duplication hands over the pointer separately: its shape (in one of three formats) and
// where it is. It's drawn on top in two passes: the coloured part, then the part that inverts
// what's under it (the text cursor, for one).
struct PointerShape {
    ComPtr<ID3D11ShaderResourceView> colour, invert;
    int w = 0, h = 0;
    POINT hotspot{};
    bool anyInvert = false;

    static ComPtr<ID3D11ShaderResourceView> texture(ID3D11Device* dev, const std::vector<std::uint32_t>& px, int w, int h) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = (UINT)w;
        d.Height = (UINT)h;
        d.MipLevels = d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_IMMUTABLE;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{px.data(), (UINT)w * 4, 0};
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> srv;
        if (SUCCEEDED(dev->CreateTexture2D(&d, &init, &tex))) dev->CreateShaderResourceView(tex.Get(), nullptr, &srv);
        return srv;
    }

    void set(ID3D11Device* dev, const DXGI_OUTDUPL_POINTER_SHAPE_INFO& info, const std::vector<std::uint8_t>& buf) {
        colour.Reset();
        invert.Reset();
        anyInvert = false;
        hotspot = info.HotSpot;
        const bool mono = info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
        w = (int)info.Width;
        h = mono ? (int)info.Height / 2 : (int)info.Height;
        if (w <= 0 || h <= 0) return;
        std::vector<std::uint32_t> col((size_t)w * h, 0), inv((size_t)w * h, 0xFF000000u);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                std::uint32_t& c = col[(size_t)y * w + x];
                std::uint32_t& v = inv[(size_t)y * w + x];
                if (mono) {
                    const std::uint8_t andBit = (buf[(size_t)y * info.Pitch + x / 8] >> (7 - x % 8)) & 1;
                    const std::uint8_t xorBit = (buf[(size_t)(y + h) * info.Pitch + x / 8] >> (7 - x % 8)) & 1;
                    if (!andBit) c = xorBit ? 0xFFFFFFFFu : 0xFF000000u;
                    else if (xorBit) { v = 0xFFFFFFFFu; anyInvert = true; }
                } else {
                    std::uint32_t p;
                    std::memcpy(&p, buf.data() + (size_t)y * info.Pitch + (size_t)x * 4, 4);
                    if (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) c = p;
                    else {   // masked colour: alpha 0 = this colour; 0xFF = XOR with the screen
                        if ((p >> 24) == 0) c = p | 0xFF000000u;
                        else if (p & 0x00FFFFFFu) { v = 0xFF000000u | (p & 0x00FFFFFFu); anyInvert = true; }
                    }
                }
            }
        colour = texture(dev, col, w, h);
        if (anyInvert) invert = texture(dev, inv, w, h);
    }
};

// ------------------------------------------------------------------ display capture

enum class Method { Duplication, Wgc, Gdi };

class DisplaySource : public VideoSource {
public:
    explicit DisplaySource(const std::string& settings) { setSettings(settings); }
    ~DisplaySource() override { close(); }

    void update(Gpu& gpu) override {
        wanted();
        std::string j;
        if (takeSettings(j)) {
            const json s = json::parse(j, nullptr, false);
            device_ = widen(str(s, "monitor"));
            method_ = str(s, "method", "auto");
            cursor_ = flag(s, "cursor", true);
            close();
            retryAt_ = 0;
        }
        if (!open_ && GetTickCount64() >= retryAt_) {
            if (!open(gpu)) retryAt_ = GetTickCount64() + 2000;
        }
        if (!open_) return;
        switch (using_) {
            case Method::Duplication: pullDuplication(gpu); break;
            case Method::Wgc:
                if (wgc_.pull(gpu, out_)) picture();
                break;
            case Method::Gdi:
                if (upload(gpu, cpu_, seen_, out_)) picture();
                break;
        }
    }

private:
    void picture() {
        hasPicture_ = true;
        if (w_ != out_.w || h_ != out_.h) {
            w_ = out_.w;
            h_ = out_.h;
            describe();
        }
    }
    void describe() {
        const wchar_t* how = using_ == Method::Duplication ? L"Desktop Duplication" : using_ == Method::Wgc ? L"Windows Graphics Capture"
                                                                                                           : L"Compatibility mode (GDI, up to 30 fps)";
        setStatus(std::wstring(how) + L" · " + sizeText(w_, h_));
    }

    bool open(Gpu& gpu) {
        close();
        MonitorInfo mon{};
        bool found = false;
        for (auto& m : listMonitors())
            if (m.device == device_ || (device_.empty() && m.primary)) {
                mon = m;
                found = true;
                break;
            }
        if (!found) {
            setStatus(L"This display isn't connected", true);
            return false;
        }
        rect_ = mon.rect;
        std::wstring why;
        const bool any = method_ == "auto";
        if ((any || method_ == "duplication") && openDuplication(gpu, why)) using_ = Method::Duplication;
        else if ((any || method_ == "wgc") && wgc_.open(gpu, nullptr, mon.handle, cursor_, why)) using_ = Method::Wgc;
        else if (any || method_ == "gdi") {
            using_ = Method::Gdi;
            const RECT r = rect_;
            const bool cursor = cursor_;
            ticker_.start(33, [this, r, cursor] {
                if (!recentlyWanted()) return;
                const int w = r.right - r.left, h = r.bottom - r.top;
                if (!gdi_.ensure(w, h)) return;
                HDC screen = GetDC(nullptr);
                BitBlt(gdi_.mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT);
                ReleaseDC(nullptr, screen);
                if (cursor) gdi_.drawCursor(r.left, r.top);
                gdi_.publish(cpu_);
            });
        } else {
            setStatus(L"Can't capture this display with the chosen method: " + why, true);
            return false;
        }
        open_ = true;
        w_ = 0;   // describe() on the first picture
        setStatus(L"Starting…");
        return true;
    }

    bool openDuplication(Gpu& gpu, std::wstring& why) {
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(gpu.device()->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter))) return false;
        ComPtr<IDXGIOutput> out;
        for (UINT i = 0; adapter->EnumOutputs(i, &out) != DXGI_ERROR_NOT_FOUND; ++i, out.Reset()) {
            DXGI_OUTPUT_DESC d{};
            if (SUCCEEDED(out->GetDesc(&d)) && (device_.empty() ? d.AttachedToDesktop != FALSE : device_ == d.DeviceName)) break;
        }
        if (!out) {
            why = L"it's on a different graphics card";
            return false;
        }
        ComPtr<IDXGIOutput1> out1;
        if (FAILED(out.As(&out1))) return false;
        const HRESULT hr = out1->DuplicateOutput(gpu.device(), &dup_);
        if (FAILED(hr)) {
            why = hr == E_ACCESSDENIED ? L"Windows is showing a protected screen" : L"Desktop Duplication isn't available";
            return false;
        }
        DXGI_OUTDUPL_DESC dd{};
        dup_->GetDesc(&dd);
        rotation_ = dd.Rotation;
        return true;
    }

    void pullDuplication(Gpu& gpu) {
        DXGI_OUTDUPL_FRAME_INFO fi{};
        ComPtr<IDXGIResource> res;
        const HRESULT hr = dup_->AcquireNextFrame(0, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return;
        if (FAILED(hr)) {
            // Lost (a mode change, a full-screen switch, the secure desktop): start again shortly.
            close();
            retryAt_ = GetTickCount64() + (hr == DXGI_ERROR_ACCESS_LOST ? 100 : 1000);
            return;
        }
        bool changed = false;
        if (fi.LastMouseUpdateTime.QuadPart != 0) {
            pointerVisible_ = fi.PointerPosition.Visible != FALSE;
            pointerAt_ = fi.PointerPosition.Position;
            changed = true;
        }
        if (fi.PointerShapeBufferSize > 0) {
            std::vector<std::uint8_t> buf(fi.PointerShapeBufferSize);
            UINT need = 0;
            DXGI_OUTDUPL_POINTER_SHAPE_INFO info{};
            if (SUCCEEDED(dup_->GetFramePointerShape((UINT)buf.size(), buf.data(), &need, &info))) pointer_.set(gpu.device(), info, buf);
        }
        if (fi.LastPresentTime.QuadPart != 0 || !desk_) {
            ComPtr<ID3D11Texture2D> tex;
            if (SUCCEEDED(res.As(&tex))) {
                D3D11_TEXTURE2D_DESC d{};
                tex->GetDesc(&d);
                D3D11_TEXTURE2D_DESC have{};
                if (desk_) desk_->GetDesc(&have);
                if (!desk_ || have.Width != d.Width || have.Height != d.Height || have.Format != d.Format) {
                    desk_.Reset();
                    deskView_.Reset();
                    D3D11_TEXTURE2D_DESC c = d;
                    c.Usage = D3D11_USAGE_DEFAULT;
                    c.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    c.CPUAccessFlags = 0;
                    c.MiscFlags = 0;
                    c.MipLevels = c.ArraySize = 1;
                    if (SUCCEEDED(gpu.device()->CreateTexture2D(&c, nullptr, &desk_))) gpu.device()->CreateShaderResourceView(desk_.Get(), nullptr, &deskView_);
                }
                if (desk_) {
                    gpu.context()->CopyResource(desk_.Get(), tex.Get());
                    deskW_ = (int)d.Width;
                    deskH_ = (int)d.Height;
                    changed = true;
                }
            }
        }
        dup_->ReleaseFrame();
        if (changed && deskView_) compose(gpu);
    }

    // The desktop (turned the way the monitor is) and the pointer, into this source's picture.
    void compose(Gpu& gpu) {
        const bool sideways = rotation_ == DXGI_MODE_ROTATION_ROTATE90 || rotation_ == DXGI_MODE_ROTATION_ROTATE270;
        const int w = sideways ? deskH_ : deskW_, h = sideways ? deskW_ : deskH_;
        if (!out_.ensure(gpu.device(), w, h)) return;
        gpu.begin(out_.rtv.Get(), w, h);
        const Pt corners[4] = {{0, 0}, {(float)w, 0}, {(float)w, (float)h}, {0, (float)h}};
        Pt uv[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        if (rotation_ == DXGI_MODE_ROTATION_ROTATE90) { uv[0] = {0, 1}; uv[1] = {0, 0}; uv[2] = {1, 0}; uv[3] = {1, 1}; }
        else if (rotation_ == DXGI_MODE_ROTATION_ROTATE180) { uv[0] = {1, 1}; uv[1] = {0, 1}; uv[2] = {0, 0}; uv[3] = {1, 0}; }
        else if (rotation_ == DXGI_MODE_ROTATION_ROTATE270) { uv[0] = {1, 0}; uv[1] = {1, 1}; uv[2] = {0, 1}; uv[3] = {0, 0}; }
        gpu.setBlend(Gpu::Blend::Alpha);
        gpu.drawTexture(deskView_.Get(), corners, uv, 1, true);
        if (cursor_ && pointerVisible_ && pointer_.colour) {
            const float x = (float)(pointerAt_.x), y = (float)(pointerAt_.y), pw = (float)pointer_.w, ph = (float)pointer_.h;
            const Pt pc[4] = {{x, y}, {x + pw, y}, {x + pw, y + ph}, {x, y + ph}};
            const Pt full[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
            gpu.drawTexture(pointer_.colour.Get(), pc, full, 1, false);
            if (pointer_.invert) {
                gpu.setBlend(Gpu::Blend::Invert);
                gpu.drawTexture(pointer_.invert.Get(), pc, full, 1, true);
                gpu.setBlend(Gpu::Blend::Alpha);
            }
        }
        picture();
    }

    void close() {
        ticker_.stop();
        wgc_.close();
        dup_.Reset();
        desk_.Reset();
        deskView_.Reset();
        open_ = false;
    }

    std::wstring device_;
    std::string method_ = "auto";
    bool cursor_ = true;
    bool open_ = false;
    ULONGLONG retryAt_ = 0;
    Method using_ = Method::Duplication;
    RECT rect_{};
    // Duplication
    ComPtr<IDXGIOutputDuplication> dup_;
    ComPtr<ID3D11Texture2D> desk_;
    ComPtr<ID3D11ShaderResourceView> deskView_;
    int deskW_ = 0, deskH_ = 0;
    DXGI_MODE_ROTATION rotation_ = DXGI_MODE_ROTATION_IDENTITY;
    PointerShape pointer_;
    bool pointerVisible_ = false;
    POINT pointerAt_{};
    // WGC
    WgcSession wgc_;
    // GDI
    Ticker ticker_;
    GdiCopy gdi_;
    CpuPicture cpu_;
    std::uint64_t seen_ = 0;
};

// ------------------------------------------------------------------ window capture

class WindowSource : public VideoSource {
public:
    explicit WindowSource(const std::string& settings) { setSettings(settings); }
    ~WindowSource() override { close(); }

    void update(Gpu& gpu) override {
        wanted();
        std::string j;
        if (takeSettings(j)) {
            const json s = json::parse(j, nullptr, false);
            wanted_.title = str(s, "title");
            wanted_.windowClass = str(s, "class");
            wanted_.exe = str(s, "exe");
            method_ = str(s, "method", "auto");
            cursor_ = flag(s, "cursor", true);
            close();
            retryAt_ = 0;
        }
        const ULONGLONG now = GetTickCount64();
        if (hwnd_ && !IsWindow(hwnd_)) {
            close();   // the program closed: keep the last picture until it's back
            retryAt_ = now + 500;
        }
        if (!hwnd_ && now >= retryAt_) {
            retryAt_ = now + 1000;
            HWND h = findWindow(wanted_);
            if (!h) {
                const std::string who = !wanted_.exe.empty() ? wanted_.exe : wanted_.title;
                setStatus(L"Waiting for " + widen(who) + L" to open");
            } else if (!open(gpu, h)) {
                retryAt_ = now + 3000;
            }
        }
        if (!hwnd_) return;
        if (IsIconic(hwnd_)) {
            if (!minimised_) {
                minimised_ = true;
                setStatus(L"Minimised: showing its last picture");
            }
            return;
        }
        if (minimised_) {
            minimised_ = false;
            w_ = 0;   // describe again
        }
        const bool got = using_ == Method::Wgc ? wgc_.pull(gpu, out_) : upload(gpu, cpu_, seen_, out_);
        if (got) {
            hasPicture_ = true;
            if (w_ != out_.w || h_ != out_.h) {
                w_ = out_.w;
                h_ = out_.h;
                setStatus(std::wstring(using_ == Method::Wgc ? L"Windows Graphics Capture" : L"Compatibility mode (GDI, up to 30 fps)") + L" · " +
                          sizeText(w_, h_));
            }
        }
    }

private:
    bool open(Gpu& gpu, HWND h) {
        std::wstring why;
        const bool any = method_ == "auto";
        if ((any || method_ == "wgc") && wgc_.open(gpu, h, nullptr, cursor_, why)) using_ = Method::Wgc;
        else if (any || method_ == "gdi") {
            using_ = Method::Gdi;
            const bool cursor = cursor_;
            ticker_.start(33, [this, h, cursor] {
                if (!recentlyWanted() || !IsWindow(h) || IsIconic(h)) return;
                RECT r{};
                GetWindowRect(h, &r);
                const int w = r.right - r.left, ht = r.bottom - r.top;
                if (w <= 0 || ht <= 0 || !gdi_.ensure(w, ht)) return;
                // PrintWindow asks the window to draw itself, so it works when covered. Some
                // windows (and some systems) answer with nothing; then copy what's on screen.
                if (!PrintWindow(h, gdi_.mem, 2 /* PW_RENDERFULLCONTENT */) || gdi_.blank()) {
                    HDC dc = GetWindowDC(h);
                    BitBlt(gdi_.mem, 0, 0, w, ht, dc, 0, 0, SRCCOPY);
                    ReleaseDC(h, dc);
                }
                if (cursor) gdi_.drawCursor(r.left, r.top);
                gdi_.publish(cpu_);
            });
        } else {
            setStatus(L"Can't capture this window with the chosen method: " + why, true);
            return false;
        }
        hwnd_ = h;
        minimised_ = false;
        w_ = 0;
        setStatus(L"Starting…");
        return true;
    }
    void close() {
        ticker_.stop();
        wgc_.close();
        hwnd_ = nullptr;
    }

    WindowIdentity wanted_;
    std::string method_ = "auto";
    bool cursor_ = true;
    HWND hwnd_ = nullptr;
    bool minimised_ = false;
    ULONGLONG retryAt_ = 0;
    Method using_ = Method::Wgc;
    WgcSession wgc_;
    Ticker ticker_;
    GdiCopy gdi_;
    CpuPicture cpu_;
    std::uint64_t seen_ = 0;
};

// ------------------------------------------------------------------ cameras

class CameraSource : public VideoSource {
public:
    explicit CameraSource(const std::string& settings) { setSettings(settings); }
    ~CameraSource() override { stop(); }

    void update(Gpu& gpu) override {
        wanted();
        std::string j;
        if (takeSettings(j)) {
            const json s = json::parse(j, nullptr, false);
            stop();
            link_ = widen(str(s, "device"));
            name_ = widen(str(s, "name"));
            quit_ = false;
            thread_ = std::thread([this] { run(); });
        }
        if (upload(gpu, cpu_, seen_, out_)) {
            hasPicture_ = true;
            w_ = out_.w;
            h_ = out_.h;
        }
    }

private:
    void stop() {
        quit_ = true;
        if (thread_.joinable()) thread_.join();
    }

    void run() {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
        while (!quit_ && mf) {
            const HRESULT hr = readUntilStopped();
            if (quit_) break;
            if (hr == E_ACCESSDENIED)
                setStatus(L"Windows' camera privacy settings are blocking desktop apps (Settings › Privacy & security › Camera)", true);
            else if (hr == MF_E_HW_MFT_FAILED_START_STREAMING || hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION))
                setStatus(L"Another program is using this camera", true);
            else
                setStatus(L"The camera isn't available (unplugged or turned off?); trying again", true);
            for (int i = 0; i < 60 && !quit_; ++i) Sleep(50);   // try again in 3 s
        }
        if (mf) MFShutdown();
        if (SUCCEEDED(co)) CoUninitialize();
    }

    HRESULT readUntilStopped() {
        ComPtr<IMFAttributes> attrs;
        ComPtr<IMFMediaSource> source;
        HRESULT hr = MFCreateAttributes(&attrs, 2);
        if (SUCCEEDED(hr)) hr = attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        if (SUCCEEDED(hr)) hr = attrs->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, link_.c_str());
        if (SUCCEEDED(hr)) hr = MFCreateDeviceSource(attrs.Get(), &source);
        if (FAILED(hr)) return hr;
        ComPtr<IMFAttributes> readerAttrs;
        ComPtr<IMFSourceReader> reader;
        MFCreateAttributes(&readerAttrs, 1);
        readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        hr = MFCreateSourceReaderFromMediaSource(source.Get(), readerAttrs.Get(), &reader);
        if (FAILED(hr)) {
            source->Shutdown();
            return hr;
        }
        const DWORD stream = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;
        // The camera's own format: the largest up to 1080p that manages 30 fps or more (a bigger
        // picture at 5 fps looks worse on stream than a smaller one at 30).
        ComPtr<IMFMediaType> best;
        double bestScore = -1;
        std::wstring bestDesc;
        for (DWORD i = 0;; ++i) {
            ComPtr<IMFMediaType> t;
            if (FAILED(reader->GetNativeMediaType(stream, i, &t))) break;
            UINT32 w = 0, h = 0, num = 0, den = 1;
            MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
            MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &num, &den);
            const double fps = den ? (double)num / den : 0;
            if (w == 0 || h == 0) continue;
            double score = (double)std::min<UINT32>(w * h, 1920 * 1080) / (1920.0 * 1080.0) * 10;
            if (w * h > 1920u * 1080u) score -= 2;
            if (fps >= 29) score += 20;
            score += std::min(fps, 60.0) / 60.0;
            if (score > bestScore) {
                bestScore = score;
                best = t;
                bestDesc = sizeText((int)w, (int)h) + L" at " + std::to_wstring((int)(fps + 0.5)) + L" fps";
            }
        }
        if (best) reader->SetCurrentMediaType(stream, nullptr, best.Get());
        // Ask for 32-bit RGB; the reader converts.
        ComPtr<IMFMediaType> rgb;
        MFCreateMediaType(&rgb);
        rgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        hr = reader->SetCurrentMediaType(stream, nullptr, rgb.Get());
        ComPtr<IMFMediaType> actual;
        if (SUCCEEDED(hr)) hr = reader->GetCurrentMediaType(stream, &actual);
        if (FAILED(hr)) {
            source->Shutdown();
            return hr;
        }
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &w, &h);
        INT32 stride = (INT32)MFGetAttributeUINT32(actual.Get(), MF_MT_DEFAULT_STRIDE, w * 4);
        setStatus(bestDesc.empty() ? sizeText((int)w, (int)h) : bestDesc);
        while (!quit_) {
            DWORD flags = 0;
            ComPtr<IMFSample> sample;
            hr = reader->ReadSample(stream, 0, nullptr, &flags, nullptr, &sample);
            if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
                if (SUCCEEDED(hr)) hr = E_FAIL;
                break;
            }
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
                actual.Reset();
                reader->GetCurrentMediaType(stream, &actual);
                MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &w, &h);
                stride = (INT32)MFGetAttributeUINT32(actual.Get(), MF_MT_DEFAULT_STRIDE, w * 4);
            }
            if (!sample) continue;
            ComPtr<IMFMediaBuffer> buf;
            if (FAILED(sample->ConvertToContiguousBuffer(&buf))) continue;
            BYTE* data = nullptr;
            DWORD len = 0;
            if (FAILED(buf->Lock(&data, nullptr, &len))) continue;
            const size_t row = (size_t)w * 4;
            if (len >= row * h && recentlyWanted()) {   // the camera stays open; copying waits until it's shown
                std::lock_guard<std::mutex> l(cpu_.m);
                cpu_.w = (int)w;
                cpu_.h = (int)h;
                cpu_.stride = (int)row;
                cpu_.bgra.resize(row * h);
                // A negative stride means the rows are stored bottom-up.
                for (UINT32 y = 0; y < h; ++y) {
                    const BYTE* src = stride < 0 ? data + (size_t)(h - 1 - y) * row : data + (size_t)y * (size_t)stride;
                    std::memcpy(cpu_.bgra.data() + (size_t)y * row, src, row);
                }
                ++cpu_.serial;
            }
            buf->Unlock();
        }
        source->Shutdown();
        return hr;
    }

    std::wstring link_, name_;
    std::thread thread_;
    std::atomic<bool> quit_{false};
    CpuPicture cpu_;
    std::uint64_t seen_ = 0;
};

}  // namespace

std::shared_ptr<VideoSource> createVideoSource(SourceKind kind, const std::string& settings) {
    switch (kind) {
        case SourceKind::Display: return std::make_shared<DisplaySource>(settings);
        case SourceKind::Window: return std::make_shared<WindowSource>(settings);
        case SourceKind::Camera: return std::make_shared<CameraSource>(settings);
        default: return nullptr;
    }
}

}  // namespace ncap::video
