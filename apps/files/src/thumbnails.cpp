#include "thumbnails.hpp"

#include "nrf/exif.hpp"
#include "nrf/paths.hpp"

#include <objbase.h>
#include <propvarutil.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <thread>

namespace nrf {

namespace {

// 32bpp RGBA, straight alpha (declared here in case the SDK's import library lacks it).
const GUID kRgba = {0xf5c7ad2d, 0x6a8d, 0x43dd, {0xa7, 0xa8, 0xa2, 0x99, 0x35, 0x26, 0x1a, 0xe9}};
const GUID kJpegContainer = {0x19e4a5aa, 0x5662, 0x4fc5, {0xa0, 0xc0, 0x17, 0x58, 0x02, 0x8e, 0x10, 0x57}};
const GUID kIcoContainer = {0xa3a860c4, 0x338f, 0x4c17, {0x91, 0x9a, 0xfb, 0xa4, 0xb5, 0x62, 0x8f, 0x21}};

template <class T>
struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T** put() { if (p) { p->Release(); p = nullptr; } return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

struct Job {
    std::wstring key, path;
    int px = 0;
    std::uint64_t generation = 0;
};

// 0 when Windows' metadata reader has no answer.
int orientationOf(IWICBitmapFrameDecode* frame) {
    Com<IWICMetadataQueryReader> q;
    if (FAILED(frame->GetMetadataQueryReader(q.put())) || !q) return 0;
    PROPVARIANT v;
    PropVariantInit(&v);
    int o = 0;
    for (const wchar_t* name : {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}"}) {
        if (SUCCEEDED(q->GetMetadataByName(name, &v))) {
            if (v.vt == VT_UI2) o = v.uiVal;
            PropVariantClear(&v);
            break;
        }
    }
    return o >= 1 && o <= 8 ? o : 0;
}

// Reads the orientation from the file's first bytes (for when the reader above has no answer).
int jpegOrientationOfFile(const std::wstring& path) {
    HANDLE h = CreateFileW(extendedPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 1;
    std::vector<unsigned char> head(128 * 1024);
    DWORD got = 0;
    const BOOL ok = ReadFile(h, head.data(), (DWORD)head.size(), &got, nullptr);
    CloseHandle(h);
    return ok ? jpegOrientation(head.data(), got) : 1;
}

// Decodes `path` scaled to fit a px-by-px square (never larger than the picture itself).
bool decode(IWICImagingFactory* f, const std::wstring& path, int px, Thumb& out) {
    Com<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromFilename(extendedPath(path).c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, dec.put())))
        return false;
    // An icon file holds several sizes: take the largest.
    UINT frames = 0, pick = 0;
    dec->GetFrameCount(&frames);
    GUID container{};
    dec->GetContainerFormat(&container);
    if (container == kIcoContainer && frames > 1) {
        UINT best = 0;
        for (UINT i = 0; i < frames && i < 32; ++i) {
            Com<IWICBitmapFrameDecode> fr;
            UINT w = 0, h = 0;
            if (SUCCEEDED(dec->GetFrame(i, fr.put())) && SUCCEEDED(fr->GetSize(&w, &h)) && w * h > best) { best = w * h; pick = i; }
        }
    }
    Com<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(pick, frame.put()))) return false;
    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0) return false;
    if ((std::uint64_t)w * h > 400ull * 1000 * 1000) return false;   // absurd sizes keep their icon

    int orientation = orientationOf(frame.p);
    if (orientation == 0) orientation = container == kJpegContainer ? jpegOrientationOfFile(path) : 1;
    const bool sideways = orientation >= 5;
    // Fit the (turned) picture inside the square, keeping its shape.
    const double shownW = sideways ? h : w, shownH = sideways ? w : h;
    const double scale = std::min(1.0, std::min(px / shownW, px / shownH));
    UINT sw = std::max<UINT>(1, (UINT)std::lround(w * scale)), sh = std::max<UINT>(1, (UINT)std::lround(h * scale));

    IWICBitmapSource* src = frame.p;
    Com<IWICBitmapScaler> scaler;
    if (sw != w || sh != h) {
        if (FAILED(f->CreateBitmapScaler(scaler.put())) || FAILED(scaler->Initialize(frame.p, sw, sh, WICBitmapInterpolationModeFant)))
            return false;
        src = scaler.p;
    }
    Com<IWICFormatConverter> conv;
    if (FAILED(f->CreateFormatConverter(conv.put())) ||
        FAILED(conv->Initialize(src, kRgba, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        return false;
    out.w = (int)sw;
    out.h = (int)sh;
    out.rgba.resize((size_t)sw * sh * 4);
    if (FAILED(conv->CopyPixels(nullptr, sw * 4, (UINT)out.rgba.size(), out.rgba.data()))) return false;
    applyOrientation(out.rgba, out.w, out.h, orientation);   // the right way up
    return true;
}

}  // namespace

struct Thumbnails::Shared {
    std::mutex m;
    std::condition_variable cv;
    std::deque<Job> jobs;
    std::set<std::wstring> pending;
    std::uint64_t generation = 0;
    bool stop = false;
    HWND notify = nullptr;
};

Thumbnails::Thumbnails(HWND notify) : shared_(std::make_shared<Shared>()) {
    shared_->notify = notify;
    std::shared_ptr<Shared> s = shared_;
    std::thread([s] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        Com<IWICImagingFactory> factory;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put()));
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(s->m);
                s->cv.wait(lock, [&] { return s->stop || !s->jobs.empty(); });
                if (s->stop) break;
                // The newest request first: what's on screen now.
                job = std::move(s->jobs.back());
                s->jobs.pop_back();
            }
            auto* r = new ThumbResult;
            r->key = job.key;
            if (!factory || !decode(factory.p, job.path, job.px, r->thumb)) {
                r->thumb = {};
                r->thumb.failed = true;
            }
            bool posted = false;
            {
                std::lock_guard<std::mutex> lock(s->m);
                s->pending.erase(job.key);
                if (!s->stop && job.generation == s->generation)
                    posted = PostMessageW(s->notify, WM_APP_THUMB, 0, reinterpret_cast<LPARAM>(r)) != FALSE;
            }
            if (!posted) delete r;
        }
        factory.put();   // released before COM goes
        CoUninitialize();
    }).detach();   // never holds up closing the window: a slow file finishes on its own
}

Thumbnails::~Thumbnails() {
    std::lock_guard<std::mutex> lock(shared_->m);
    shared_->stop = true;
    shared_->jobs.clear();
    shared_->cv.notify_all();
}

bool Thumbnails::isImage(const std::wstring& name) {
    static const wchar_t* exts[] = {L".jpg", L".jpeg", L".jpe", L".jfif", L".png", L".gif", L".bmp", L".dib", L".tif", L".tiff",
                                    L".ico", L".webp", L".heic", L".heif", L".avif", L".jxr", L".wdp", L".hdp"};
    const std::wstring e = extensionOf(name);
    for (const wchar_t* x : exts)
        if (e == x) return true;
    return false;
}

const Thumb* Thumbnails::get(const std::wstring& path, std::uint64_t modified, std::uint64_t size, int px) {
    if (size == 0 || size > kMaxBytes) return nullptr;
    const std::wstring key = path + L"|" + std::to_wstring(modified) + L"|" + std::to_wstring(px);
    auto it = kept_.find(key);
    if (it != kept_.end()) {
        order_.splice(order_.begin(), order_, where_[key]);   // recently used
        return it->second.failed ? nullptr : &it->second;
    }
    std::lock_guard<std::mutex> lock(shared_->m);
    if (shared_->pending.count(key)) return nullptr;
    shared_->pending.insert(key);
    shared_->jobs.push_back({key, path, px, shared_->generation});
    while (shared_->jobs.size() > 400) {   // asked for long ago and scrolled past
        shared_->pending.erase(shared_->jobs.front().key);
        shared_->jobs.pop_front();
    }
    shared_->cv.notify_one();
    return nullptr;
}

void Thumbnails::received(ThumbResult* r) {
    if (kept_.count(r->key)) return;
    r->thumb.serial = ++serial_;
    kept_[r->key] = std::move(r->thumb);
    order_.push_front(r->key);
    where_[r->key] = order_.begin();
    while (kept_.size() > kMaxKept) {
        const std::wstring old = order_.back();
        order_.pop_back();
        where_.erase(old);
        auto k = kept_.find(old);
        if (k != kept_.end()) {
            if (!k->second.failed) evicted_.push_back(k->second.serial);
            kept_.erase(k);
        }
    }
}

void Thumbnails::forgetQueue() {
    std::lock_guard<std::mutex> lock(shared_->m);
    ++shared_->generation;
    for (const Job& j : shared_->jobs) shared_->pending.erase(j.key);
    shared_->jobs.clear();
}

std::vector<std::uint64_t> Thumbnails::takeEvicted() {
    std::vector<std::uint64_t> out;
    out.swap(evicted_);
    return out;
}

}  // namespace nrf
