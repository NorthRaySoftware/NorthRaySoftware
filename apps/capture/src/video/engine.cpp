#include "video/engine.hpp"

#include <objbase.h>
#include <timeapi.h>

#include <algorithm>
#include <cmath>
#include <set>

namespace ncap::video {

namespace {

constexpr wchar_t kPreviewClass[] = L"NorthRayCapturePreview";

LRESULT CALLBACK previewProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_NCHITTEST: return HTTRANSPARENT;   // the main window handles the mouse
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: ValidateRect(h, nullptr); return 0;
        default: return DefWindowProcW(h, m, w, l);
    }
}

LONGLONG now() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}
LONGLONG frequency() {
    static const LONGLONG f = [] {
        LARGE_INTEGER x;
        QueryPerformanceFrequency(&x);
        return x.QuadPart;
    }();
    return f;
}

// Texture coordinates for an item: its crop, then its mirroring.
void itemUv(const Transform& t, float sw, float sh, Pt uv[4]) {
    float u0 = t.cropLeft / sw, u1 = 1 - t.cropRight / sw, v0 = t.cropTop / sh, v1 = 1 - t.cropBottom / sh;
    if (t.flipH) std::swap(u0, u1);
    if (t.flipV) std::swap(v0, v1);
    uv[0] = {u0, v0};
    uv[1] = {u1, v0};
    uv[2] = {u1, v1};
    uv[3] = {u0, v1};
}

}  // namespace

Views layoutViews(float cw, float ch, float aw, float ah, bool studio) {
    Views v;
    if (!studio) {
        v.edit = v.program = fitCanvas(cw, ch, aw, ah, 0);
        return v;
    }
    v.gap = std::max(8.0f, std::floor(aw * 0.015f));
    v.half = std::floor((aw - v.gap) / 2);
    v.edit = fitCanvas(cw, ch, v.half, ah, 0);
    v.program = fitCanvas(cw, ch, v.half, ah, 0);
    v.program.x += v.half + v.gap;
    return v;
}

Fit fitCanvas(float cw, float ch, float aw, float ah, float margin) {
    Fit f;
    if (cw <= 0 || ch <= 0) return f;
    const float w = std::max(1.0f, aw - margin * 2), h = std::max(1.0f, ah - margin * 2);
    f.scale = std::min(w / cw, h / ch);
    f.x = std::floor((aw - cw * f.scale) / 2);
    f.y = std::floor((ah - ch * f.scale) / 2);
    return f;
}

bool Engine::start(HWND parent, int outputFps, std::wstring& error) {
    if (running()) return true;
    if (!gpu_.create(error)) return false;
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = previewProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kPreviewClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        registered = RegisterClassW(&wc) != 0;
    }
    preview_ = CreateWindowExW(WS_EX_NOPARENTNOTIFY, kPreviewClass, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 16, 16, parent, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    if (!preview_) {
        error = L"The preview window couldn't be created.";
        return false;
    }
    for (auto& t : timing_) {
        D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        D3D11_QUERY_DESC ts{D3D11_QUERY_TIMESTAMP, 0};
        gpu_.device()->CreateQuery(&q, &t.disjoint);
        gpu_.device()->CreateQuery(&ts, &t.begin);
        gpu_.device()->CreateQuery(&ts, &t.end);
    }
    {
        std::lock_guard<std::mutex> l(m_);
        stats_.adapter = gpu_.adapterName();
    }
    outputFps_ = std::max(1, outputFps);
    quit_ = false;
    thread_ = std::thread([this] { run(); });
    return true;
}

void Engine::stop() {
    quit_ = true;
    if (thread_.joinable()) thread_.join();
    if (preview_) DestroyWindow(preview_);
    preview_ = nullptr;
}

void Engine::setSnapshot(Snapshot s) {
    std::lock_guard<std::mutex> l(m_);
    snap_ = std::move(s);
}

void Engine::setPreview(RECT r, bool show, int fps) {
    previewFps_ = fps;
    const bool visible = show && fps > 0 && r.right > r.left && r.bottom > r.top;
    previewShown_ = visible;
    if (!preview_) return;
    if (visible) {
        RECT have{};
        GetWindowRect(preview_, &have);
        MapWindowPoints(nullptr, GetParent(preview_), reinterpret_cast<POINT*>(&have), 2);
        if (!EqualRect(&have, &r) || !IsWindowVisible(preview_))
            SetWindowPos(preview_, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else if (IsWindowVisible(preview_)) {
        ShowWindow(preview_, SW_HIDE);
    }
}

void Engine::setFrameSink(const std::string& name, FrameSink sink) {
    std::lock_guard<std::mutex> calls(sinkCallM_);
    std::lock_guard<std::mutex> l(m_);
    if (sink) sinks_[name] = std::move(sink);
    else sinks_.erase(name);
    outputsActive_ = !sinks_.empty();
}

EngineStats Engine::stats() const {
    std::lock_guard<std::mutex> l(m_);
    return stats_;
}

void Engine::run() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    timeBeginPeriod(1);
    LONGLONG next = now();
    LONGLONG secondStart = next;
    int frames = 0, late = 0;
    double spent = 0;
    while (!quit_) {
        const LONGLONG interval = frequency() / outputFps_.load();
        next += interval;
        LONGLONG t = now();
        if (t > next + interval) {   // fell behind (the PC was busy): skip ahead, count it
            late += (int)((t - next) / interval);
            next = t + interval;
        }
        const LONGLONG wait = next - t;
        if (wait > 0) {
            LARGE_INTEGER due;
            due.QuadPart = -(wait * 10000000 / frequency());
            if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 1000);
            else Sleep((DWORD)(wait * 1000 / frequency()));
        }
        const LONGLONG f0 = now();
        frame();
        spent += (double)(now() - f0) * 1000.0 / (double)frequency();
        ++frames;
        if (now() - secondStart >= frequency()) {
            const double secs = (double)(now() - secondStart) / (double)frequency();
            std::lock_guard<std::mutex> l(m_);
            stats_.fps = frames / secs;
            stats_.renderMs = frames ? spent / frames : 0;
            stats_.late = late;
            secondStart = now();
            frames = 0;
            spent = 0;
        }
    }
    timeEndPeriod(1);
    if (timer) CloseHandle(timer);
    // Sources may be released here last; their own threads stop in their destructors.
    {
        std::lock_guard<std::mutex> l(m_);
        snap_.items.clear();
    }
    backbuffer_.Reset();
    swap_.Reset();
    if (SUCCEEDED(co)) CoUninitialize();
}

void Engine::frame() {
    // Nobody is looking and nothing is being streamed or recorded: no work at all.
    if (!previewShown_ && !outputsActive_) return;
    Snapshot s;
    {
        std::lock_guard<std::mutex> l(m_);
        s = snap_;
    }
    // GPU timing (when the driver supports timestamps).
    Timing& tm = timing_[timingIndex_];
    timingIndex_ = (timingIndex_ + 1) % 4;
    Timing& oldest = timing_[timingIndex_];
    if (oldest.pending) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        UINT64 b = 0, e = 0;
        auto* ctx = gpu_.context();
        if (ctx->GetData(oldest.disjoint.Get(), &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
            ctx->GetData(oldest.begin.Get(), &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
            ctx->GetData(oldest.end.Get(), &e, sizeof e, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
            oldest.pending = false;
            if (!dj.Disjoint && dj.Frequency && e >= b) {
                const double ms = (double)(e - b) * 1000.0 / (double)dj.Frequency;
                std::lock_guard<std::mutex> l(m_);
                stats_.gpuMs = stats_.gpuMs < 0 ? ms : stats_.gpuMs * 0.9 + ms * 0.1;
            }
        }
    }
    const bool timed = tm.disjoint && !tm.pending;
    if (timed) {
        gpu_.context()->Begin(tm.disjoint.Get());
        gpu_.context()->End(tm.begin.Get());
    }

    // A new program scene: start the transition from what was live.
    if (s.programScene != lastProgram_) {
        if (!lastProgram_.empty() && s.transition.kind != transition::Kind::Cut && s.transition.durationMs > 0) {
            fromItems_ = lastItems_;   // switching again mid-way starts from the scene it was going to
            transitioning_ = true;
            transStart_ = now();
            trans_ = s.transition;
            durationMs_ = trans_.durationMs;
            if (trans_.kind == transition::Kind::Stinger) {
                if (s.stinger) {
                    s.stinger->command("start");
                    const double len = s.stinger->length();
                    if (len > 0.1) durationMs_ = (int)(len * 1000);
                }
                durationMs_ = std::max(durationMs_, trans_.cutMs + 50);
            }
        } else {
            transitioning_ = false;
        }
        lastProgram_ = s.programScene;
    }
    if (transitioning_ && (double)(now() - transStart_) * 1000.0 / (double)frequency() >= durationMs_) {
        transitioning_ = false;
        fromItems_.clear();
    }
    lastItems_ = s.items;
    {
        std::lock_guard<std::mutex> l(m_);
        stats_.transitioning = transitioning_;
        stats_.program = s.programScene;
    }

    // Bring in each shown source's newest picture (once, however many items show it).
    std::set<VideoSource*> done;
    updateSources(s.items, done, 0);
    if (transitioning_) updateSources(fromItems_, done, 0);
    if (s.studio && previewShown_) updateSources(s.previewItems, done, 0);
    if (s.stinger) s.stinger->update(gpu_);   // kept ready, so it starts on cue
    // A custom transition shader: compiled when its code changes.
    if (s.shaderCode != customCode_) {
        customCode_ = s.shaderCode;
        custom_.Reset();
        std::string err;
        if (!customCode_.empty()) custom_ = gpu_.compilePixelShader(transition::shaderSource(customCode_), err);
        std::lock_guard<std::mutex> l(m_);
        stats_.shaderError = err;
    }

    compose(s);
    {
        std::lock_guard<std::mutex> calls(sinkCallM_);
        std::vector<FrameSink> sinks;
        {
            std::lock_guard<std::mutex> l(m_);
            for (auto& [n, f] : sinks_) sinks.push_back(f);
        }
        const LONGLONG at = now();
        if (canvas_.tex)
            for (auto& f : sinks) f(canvas_.tex.Get(), at);
    }
    const LONGLONG t = now();
    if (previewShown_ && previewFps_ > 0 && (t - lastPreview_) * previewFps_ >= frequency() - frequency() / 200) {
        lastPreview_ = t;
        present(s);
    }
    if (timed) {
        gpu_.context()->End(tm.end.Get());
        gpu_.context()->End(tm.disjoint.Get());
        tm.pending = true;
    }
}

void Engine::updateSources(const Items& items, std::set<VideoSource*>& done, int depth) {
    for (auto& it : items) {
        if (!it.visible) continue;
        if (it.source && done.insert(it.source.get()).second) it.source->update(gpu_);
        if (it.nestedItems && depth < 4) updateSources(*it.nestedItems, done, depth + 1);
    }
}

void Engine::composeItems(const Items& items, Target& into, int w, int h, int depth) {
    if (!into.ensure(gpu_.device(), w, h)) return;
    // Scenes shown inside this one first (each once a frame, however often it's shown), since
    // composing them switches the render target.
    for (auto& it : items)
        if (it.visible && it.nestedItems && depth < 4 && nestedDone_.insert(it.nested).second)
            composeItems(*it.nestedItems, nested_[it.nested], w, h, depth + 1);
    gpu_.clear(into.rtv.Get(), 0, 0, 0, depth ? 0.0f : 1.0f);   // a nested scene is see-through where it has nothing
    gpu_.begin(into.rtv.Get(), into.w, into.h);
    for (auto& it : items) {
        if (!it.visible) continue;
        ID3D11ShaderResourceView* v = nullptr;
        float sw = 0, sh = 0;
        bool opaque = true;
        if (it.nestedItems) {
            auto n = nested_.find(it.nested);
            if (n == nested_.end() || !n->second.srv || depth >= 4) continue;
            v = n->second.srv.Get();
            sw = (float)w;
            sh = (float)h;
            opaque = false;
        } else if (it.source) {
            v = it.source->view();
            sw = (float)it.source->width();
            sh = (float)it.source->height();
            opaque = !it.source->hasAlpha();
        }
        if (!v || sw <= 0 || sh <= 0) continue;
        Vec2 c[4];
        cornersOf(it.transform, sw, sh, c);
        const Pt corners[4] = {{c[0].x, c[0].y}, {c[1].x, c[1].y}, {c[2].x, c[2].y}, {c[3].x, c[3].y}};
        Pt uv[4];
        itemUv(it.transform, sw, sh, uv);
        gpu_.drawTexture(v, corners, uv, std::clamp(it.transform.opacity, 0.0f, 1.0f), opaque);
    }
}

void Engine::compose(const Snapshot& s) {
    nestedDone_.clear();
    if (!transitioning_) {
        composeItems(s.items, canvas_, s.canvasW, s.canvasH, 0);
    } else {
        // Both scenes, then the program is made from them.
        composeItems(fromItems_, from_, s.canvasW, s.canvasH, 0);
        composeItems(s.items, to_, s.canvasW, s.canvasH, 0);
        if (!canvas_.ensure(gpu_.device(), s.canvasW, s.canvasH) || !from_.srv || !to_.srv) return;
        const float W = (float)s.canvasW, H = (float)s.canvasH;
        const double elapsedMs = (double)(now() - transStart_) * 1000.0 / (double)frequency();
        const float t = (float)(elapsedMs / std::max(1, durationMs_));
        const float p = transition::ease(trans_.easing, t);
        gpu_.clear(canvas_.rtv.Get(), 0, 0, 0, 1);
        gpu_.begin(canvas_.rtv.Get(), canvas_.w, canvas_.h);
        const Pt uv[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        auto quad = [&](float ox, float oy, Pt out[4]) {
            out[0] = {ox, oy};
            out[1] = {ox + W, oy};
            out[2] = {ox + W, oy + H};
            out[3] = {ox, oy + H};
        };
        Pt full[4];
        quad(0, 0, full);
        switch (trans_.kind) {
        case transition::Kind::Fade:
            gpu_.drawTexture(from_.srv.Get(), full, uv, 1, true);
            gpu_.drawTexture(to_.srv.Get(), full, uv, p, true);
            break;
        case transition::Kind::Slide:
        case transition::Kind::Swipe: {
            const transition::Offsets o = transition::offsets(trans_.kind, trans_.direction, p, W, H);
            Pt a[4], b[4];
            quad(o.fromX, o.fromY, a);
            quad(o.toX, o.toY, b);
            gpu_.drawTexture(from_.srv.Get(), a, uv, 1, true);
            gpu_.drawTexture(to_.srv.Get(), b, uv, 1, true);
            break;
        }
        case transition::Kind::LumaWipe: {
            // The pattern at a quarter of the canvas size (it's smooth; the edge is softened anyway).
            const int mw = std::max(16, s.canvasW / 4), mh = std::max(16, s.canvasH / 4);
            if (!mask_ || maskPattern_ != trans_.pattern || maskW_ != mw || maskH_ != mh) {
                const auto px = transition::pattern(trans_.pattern, mw, mh);
                mask_ = gpu_.makeMask(px.data(), mw, mh);
                maskPattern_ = trans_.pattern;
                maskW_ = mw;
                maskH_ = mh;
            }
            gpu_.drawTexture(from_.srv.Get(), full, uv, 1, true);
            gpu_.drawLumaWipe(to_.srv.Get(), mask_.Get(), full, p, trans_.softness, trans_.invert);
            break;
        }
        case transition::Kind::Stinger: {
            // The old scene until the cut point, the new one after; the stinger's video over both.
            gpu_.drawTexture(elapsedMs < trans_.cutMs ? from_.srv.Get() : to_.srv.Get(), full, uv, 1, true);
            if (s.stinger)
                if (ID3D11ShaderResourceView* v = s.stinger->view()) gpu_.drawTexture(v, full, uv, 1, !s.stinger->hasAlpha());
            break;
        }
        case transition::Kind::Shader:
            if (custom_) gpu_.drawWith(custom_.Get(), from_.srv.Get(), to_.srv.Get(), full, p, W, H);
            else {   // it didn't compile: a fade instead
                gpu_.drawTexture(from_.srv.Get(), full, uv, 1, true);
                gpu_.drawTexture(to_.srv.Get(), full, uv, p, true);
            }
            break;
        case transition::Kind::Cut: gpu_.drawTexture(to_.srv.Get(), full, uv, 1, true); break;
        }
    }
    // Studio mode: the scene being prepared, for the preview only.
    if (s.studio && previewShown_) composeItems(s.previewItems, studio_, s.canvasW, s.canvasH, 0);
}

void Engine::drawFit(Target& t, const Fit& f) {
    if (!t.srv) return;
    const float cw = t.w * f.scale, ch = t.h * f.scale;
    const Pt corners[4] = {{f.x, f.y}, {f.x + cw, f.y}, {f.x + cw, f.y + ch}, {f.x, f.y + ch}};
    const Pt uv[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    gpu_.drawTexture(t.srv.Get(), corners, uv, 1, true);
}

bool Engine::ensureSwapChain(int w, int h) {
    if (swap_ && w == swapW_ && h == swapH_) return true;
    backbuffer_.Reset();
    if (swap_) {
        if (FAILED(swap_->ResizeBuffers(0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, 0))) swap_.Reset();
    }
    if (!swap_) {
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        if (FAILED(gpu_.device()->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter)) ||
            FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
            return false;
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = (UINT)w;
        d.Height = (UINT)h;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.Scaling = DXGI_SCALING_STRETCH;
        // Flip model where Windows has it (less copying); the older blt model otherwise.
        for (DXGI_SWAP_EFFECT effect : {DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, DXGI_SWAP_EFFECT_DISCARD}) {
            d.SwapEffect = effect;
            d.BufferCount = effect == DXGI_SWAP_EFFECT_DISCARD ? 1 : 2;
            if (SUCCEEDED(factory->CreateSwapChainForHwnd(gpu_.device(), preview_, &d, nullptr, nullptr, &swap_))) break;
        }
        if (!swap_) return false;
        factory->MakeWindowAssociation(preview_, DXGI_MWA_NO_ALT_ENTER);
    }
    ComPtr<ID3D11Texture2D> buf;
    if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&buf))) || FAILED(gpu_.device()->CreateRenderTargetView(buf.Get(), nullptr, &backbuffer_))) {
        swap_.Reset();
        return false;
    }
    swapW_ = w;
    swapH_ = h;
    return true;
}

void Engine::present(const Snapshot& s) {
    RECT rc{};
    GetClientRect(preview_, &rc);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0 || !ensureSwapChain(w, h)) return;
    gpu_.begin(backbuffer_.Get(), w, h);
    gpu_.clear(backbuffer_.Get(), s.background[0], s.background[1], s.background[2], 1);
    const float u = s.uiScale;
    const Views views = layoutViews((float)s.canvasW, (float)s.canvasH, (float)w, (float)h, s.studio);
    const Fit f = views.edit;
    if (s.studio) {
        drawFit(studio_, views.edit);
        drawFit(canvas_, views.program);
        // The program's frame: red, as on a studio's live monitor.
        const Fit& g = views.program;
        const float pw = s.canvasW * g.scale, ph = s.canvasH * g.scale, t = 2 * u;
        gpu_.fillRect(g.x, g.y, pw, t, 0.88f, 0.34f, 0.42f, 1);   // inside the picture's edge, so it's always seen
        gpu_.fillRect(g.x, g.y + ph - t, pw, t, 0.88f, 0.34f, 0.42f, 1);
        gpu_.fillRect(g.x, g.y, t, ph, 0.88f, 0.34f, 0.42f, 1);
        gpu_.fillRect(g.x + pw - t, g.y, t, ph, 0.88f, 0.34f, 0.42f, 1);
    } else {
        drawFit(canvas_, f);
    }
    auto toPreview = [&](Vec2 p) { return Pt{f.x + p.x * f.scale, f.y + p.y * f.scale}; };
    // Snapping guides.
    for (float gx : s.guidesX) gpu_.line(toPreview({gx, 0}), toPreview({gx, (float)s.canvasH}), 1 * u, 0.93f, 0.36f, 0.55f, 0.9f);
    for (float gy : s.guidesY) gpu_.line(toPreview({0, gy}), toPreview({(float)s.canvasW, gy}), 1 * u, 0.93f, 0.36f, 0.55f, 0.9f);
    // The selection and the item under the pointer (in the view that's edited).
    for (auto& it : s.studio ? s.previewItems : s.items) {
        const bool sel = it.id == s.selected, hov = it.id == s.hovered;
        if ((!sel && !hov) || !it.visible || (!it.source && !it.nestedItems)) continue;
        const float sw = it.nestedItems ? (float)s.canvasW : (float)it.source->width();
        const float sh = it.nestedItems ? (float)s.canvasH : (float)it.source->height();
        if (frameOf(it.transform, sw, sh).w <= 0) continue;
        Vec2 c[4];
        cornersOf(it.transform, sw, sh, c);
        const bool locked = sel && s.selectedLocked;
        const float r = locked ? 0.62f : 0.31f, g = locked ? 0.66f : 0.88f, b = locked ? 0.72f : 0.69f;
        const float thick = (sel ? 2.0f : 1.0f) * u, alpha = sel ? 1.0f : 0.6f;
        for (int i = 0; i < 4; ++i) gpu_.line(toPreview(c[i]), toPreview(c[(i + 1) % 4]), thick, r, g, b, alpha);
        if (sel && !locked) {
            const Handle hs[] = {Handle::TopLeft, Handle::Top, Handle::TopRight, Handle::Right, Handle::BottomRight, Handle::Bottom, Handle::BottomLeft, Handle::Left};
            const float half = 4.5f * u;
            for (Handle hd : hs) {
                const Pt p = toPreview(handlePoint(it.transform, sw, sh, hd));
                gpu_.fillRect(p.x - half, p.y - half, half * 2, half * 2, r, g, b, 1);
                gpu_.fillRect(p.x - half + u * 1.5f, p.y - half + u * 1.5f, half * 2 - u * 3, half * 2 - u * 3, 1, 1, 1, 1);
            }
        }
    }
    const HRESULT hr = swap_->Present(0, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        std::lock_guard<std::mutex> l(m_);
        stats_.error = L"The graphics driver restarted. Restart NorthRay Capture to get the picture back.";
    }
}

}  // namespace ncap::video
