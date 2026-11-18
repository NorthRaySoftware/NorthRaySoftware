#include "thumbnails.hpp"

#include "nrf/art.hpp"
#include "nrf/exif.hpp"
#include "nrf/paths.hpp"

#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include "miniz.h"
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

bool decodeWith(IWICImagingFactory* f, IWICBitmapDecoder* decIn, const std::wstring& path, int px, Thumb& out);

// Decodes `path` scaled to fit a px-by-px square (never larger than the picture itself).
bool decode(IWICImagingFactory* f, const std::wstring& path, int px, Thumb& out) {
    Com<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromFilename(extendedPath(path).c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, dec.put())))
        return false;
    return decodeWith(f, dec.p, path, px, out);
}

// A picture held in memory (a PNG or JPEG out of an art file).
bool decodeBytes(IWICImagingFactory* f, const std::string& bytes, int px, Thumb& out) {
    if (bytes.empty() || bytes.size() > 0x7fffffff) return false;
    Com<IWICStream> st;
    if (FAILED(f->CreateStream(st.put())) || FAILED(st->InitializeFromMemory((BYTE*)bytes.data(), (DWORD)bytes.size()))) return false;
    Com<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromStream(st.p, nullptr, WICDecodeMetadataCacheOnDemand, dec.put()))) return false;
    return decodeWith(f, dec.p, L"", px, out);
}

bool decodeWith(IWICImagingFactory* f, IWICBitmapDecoder* decIn, const std::wstring& path, int px, Thumb& out) {
    struct { IWICBitmapDecoder* p; IWICBitmapDecoder* operator->() const { return p; } } dec{decIn};
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
    if (orientation == 0) orientation = container == kJpegContainer && !path.empty() ? jpegOrientationOfFile(path) : 1;
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


std::wstring lowerExt(const std::wstring& path) { return extensionOf(path); }

bool readRange(HANDLE h, std::uint64_t at, void* buf, std::size_t n) {
    OVERLAPPED o{};
    o.Offset = (DWORD)at;
    o.OffsetHigh = (DWORD)(at >> 32);
    DWORD got = 0;
    return ReadFile(h, buf, (DWORD)n, &got, &o) && got == n;
}

HANDLE openForReading(const std::wstring& path) {
    return CreateFileW(extendedPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                       FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
}

void fromImage(art::Image& img, Thumb& out) {
    out.w = img.w;
    out.h = img.h;
    out.rgba = std::move(img.rgba);
}

// Photoshop: the flattened picture, or (for small pictures, very big files or when that isn't there)
// the JPEG preview Photoshop keeps near the start.
bool decodePsd(IWICImagingFactory* f, const std::wstring& path, int px, Thumb& out) {
    HANDLE h = openForReading(path);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    const std::uint64_t total = (std::uint64_t)size.QuadPart;
    std::vector<unsigned char> head((std::size_t)std::min<std::uint64_t>(total, 8ull << 20));
    bool ok = !head.empty() && readRange(h, 0, head.data(), head.size());
    std::string jpeg = ok ? art::psdPreviewJpeg(head.data(), head.size()) : std::string();
    if (ok && (px > 256 || jpeg.empty()) && total <= 1024ull * 1024 * 1024) {
        std::vector<unsigned char> all((std::size_t)total);
        if (readRange(h, 0, all.data(), all.size())) {
            art::Image img;
            if (art::psdComposite(all.data(), all.size(), px, img)) {
                CloseHandle(h);
                fromImage(img, out);
                return true;
            }
        }
    }
    CloseHandle(h);
    return !jpeg.empty() && decodeBytes(f, jpeg, px, out);
}

// Procreate, Krita and OpenRaster files are zip archives with a PNG of the picture inside.
bool decodeZipArt(IWICImagingFactory* f, const std::wstring& path, int px, Thumb& out) {
    const std::wstring ext = lowerExt(path);
    std::vector<const char*> names;
    if (ext == L".procreate") names = {"QuickLook/Thumbnail.png"};
    else if (ext == L".kra") names = px > 256 ? std::vector<const char*>{"mergedimage.png", "preview.png"} : std::vector<const char*>{"preview.png", "mergedimage.png"};
    else names = px > 256 ? std::vector<const char*>{"mergedimage.png", "Thumbnails/thumbnail.png"} : std::vector<const char*>{"Thumbnails/thumbnail.png", "mergedimage.png"};
    HANDLE h = openForReading(path);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    mz_zip_archive z{};
    z.m_pIO_opaque = &h;
    z.m_pRead = [](void* opaque, mz_uint64 ofs, void* buf, size_t n) -> size_t {
        return readRange(*static_cast<HANDLE*>(opaque), ofs, buf, n) ? n : 0;
    };
    bool ok = false;
    if (mz_zip_reader_init(&z, (mz_uint64)size.QuadPart, 0)) {
        for (const char* name : names) {
            const int i = mz_zip_reader_locate_file(&z, name, nullptr, 0);
            mz_zip_archive_file_stat st{};
            if (i < 0 || !mz_zip_reader_file_stat(&z, (mz_uint)i, &st) || st.m_uncomp_size > 300ull * 1024 * 1024) continue;
            size_t n = 0;
            void* p = mz_zip_reader_extract_to_heap(&z, (mz_uint)i, &n, 0);
            if (!p) continue;
            const std::string bytes(static_cast<const char*>(p), n);
            mz_free(p);
            if (decodeBytes(f, bytes, px, out)) { ok = true; break; }
        }
        mz_zip_reader_end(&z);
    }
    CloseHandle(h);
    return ok;
}

// Videos and PDFs: the picture Windows' own thumbnail handlers make (the same as File Explorer's).
bool decodeShell(const std::wstring& path, int px, Thumb& out) {
    Com<IShellItemImageFactory> fac;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(fac.put())))) return false;
    HBITMAP bmp = nullptr;
    if (FAILED(fac->GetImage(SIZE{px, px}, SIIGBF_THUMBNAILONLY, &bmp)) || !bmp) return false;
    BITMAP bm{};
    GetObjectW(bmp, sizeof bm, &bm);
    bool ok = false;
    if (bm.bmWidth > 0 && bm.bmHeight > 0) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = bm.bmWidth;
        bi.bmiHeader.biHeight = -bm.bmHeight;   // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        out.w = bm.bmWidth;
        out.h = bm.bmHeight;
        out.rgba.resize((std::size_t)out.w * out.h * 4);
        HDC dc = GetDC(nullptr);
        ok = GetDIBits(dc, bmp, 0, (UINT)out.h, out.rgba.data(), &bi, DIB_RGB_COLORS) == out.h;
        ReleaseDC(nullptr, dc);
        bool anyAlpha = false;
        for (std::size_t i = 0; i < out.rgba.size(); i += 4) {
            std::swap(out.rgba[i], out.rgba[i + 2]);   // BGRA -> RGBA
            anyAlpha = anyAlpha || out.rgba[i + 3] != 0;
        }
        if (!anyAlpha)
            for (std::size_t i = 3; i < out.rgba.size(); i += 4) out.rgba[i] = 255;
        else   // premultiplied: back to straight alpha
            for (std::size_t i = 0; i < out.rgba.size(); i += 4) {
                const unsigned a = out.rgba[i + 3];
                if (a > 0 && a < 255)
                    for (int c = 0; c < 3; ++c) out.rgba[i + (std::size_t)c] = (unsigned char)std::min(255u, out.rgba[i + (std::size_t)c] * 255u / a);
            }
    }
    DeleteObject(bmp);
    return ok;
}

bool makePicture(IWICImagingFactory* f, const std::wstring& path, int px, Thumb& out) {
    switch (Thumbnails::kindOf(path)) {
        case Thumbnails::PictureKind::Image: return f && decode(f, path, px, out);
        case Thumbnails::PictureKind::Photoshop: return f && decodePsd(f, path, px, out);
        case Thumbnails::PictureKind::ZipArt: return f && decodeZipArt(f, path, px, out);
        case Thumbnails::PictureKind::Shell: return decodeShell(path, px, out);
        default: return false;
    }
}

// ---- Kept on disk (0.9): %LOCALAPPDATA%\NorthRay\Files\Thumbnails, one small JPEG (PNG when it has
// transparency) per file, named by a hash of its path, modified time and picture size, so a changed file gets a
// new one. Pruned oldest first when the folder passes kDiskMax.
const GUID kPngContainer = {0x1b7cfaf4, 0x713f, 0x473c, {0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf}};
const GUID kBgr24 = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0c}};
constexpr std::uint64_t kDiskMax = 300ull * 1024 * 1024, kDiskKeep = 220ull * 1024 * 1024;

std::wstring cacheFolder() {
    static const std::wstring dir = [] {
        PWSTR p = nullptr;
        std::wstring d;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)) && p) d = std::wstring(p) + L"\\NorthRay\\Files\\Thumbnails";
        CoTaskMemFree(p);
        if (!d.empty()) SHCreateDirectoryExW(nullptr, d.c_str(), nullptr);
        return d;
    }();
    return dir;
}

std::wstring cacheFile(const std::wstring& key, bool png) {
    const std::wstring dir = cacheFolder();
    if (dir.empty()) return L"";
    std::uint64_t h = 1469598103934665603ull;   // FNV-1a over the key in lower case
    for (wchar_t c : key) {
        const unsigned v = (unsigned)towlower(c);
        h = (h ^ (v & 0xff)) * 1099511628211ull;
        h = (h ^ (v >> 8)) * 1099511628211ull;
    }
    wchar_t name[40];
    swprintf(name, 40, L"\\%016llx.%ls", (unsigned long long)h, png ? L"png" : L"jpg");
    return dir + name;
}

void touch(const std::wstring& file);

bool loadCached(IWICImagingFactory* f, const std::wstring& key, int px, Thumb& out) {
    for (bool png : {false, true}) {
        const std::wstring file = cacheFile(key, png);
        if (file.empty() || GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        if (decode(f, file, px, out)) {
            touch(file);
            return true;
        }
        DeleteFileW(file.c_str());   // spoiled: made again
    }
    return false;
}

void saveCached(IWICImagingFactory* f, const std::wstring& key, const Thumb& t) {
    if (!f || t.w <= 0 || t.h <= 0 || t.rgba.size() != (std::size_t)t.w * t.h * 4) return;
    bool alpha = false;
    for (std::size_t i = 3; i < t.rgba.size() && !alpha; i += 4) alpha = t.rgba[i] != 255;
    const std::wstring file = cacheFile(key, alpha);
    if (file.empty()) return;
    const std::wstring tmp = file + L".new";
    Com<IWICBitmap> bmp;
    Com<IWICStream> st;
    Com<IWICBitmapEncoder> enc;
    Com<IWICBitmapFrameEncode> fr;
    Com<IPropertyBag2> bag;
    bool ok = SUCCEEDED(f->CreateBitmapFromMemory((UINT)t.w, (UINT)t.h, kRgba, (UINT)t.w * 4, (UINT)t.rgba.size(), (BYTE*)t.rgba.data(), bmp.put())) &&
              SUCCEEDED(f->CreateStream(st.put())) && SUCCEEDED(st->InitializeFromFilename(tmp.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(f->CreateEncoder(alpha ? kPngContainer : kJpegContainer, nullptr, enc.put())) &&
              SUCCEEDED(enc->Initialize(st.p, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(fr.put(), bag.put()));
    if (ok && !alpha && bag) {
        PROPBAG2 opt{};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = 0.88f;
        bag->Write(1, &opt, &v);
    }
    if (ok) {
        WICPixelFormatGUID pf = alpha ? kRgba : kBgr24;
        ok = SUCCEEDED(fr->Initialize(bag.p)) && SUCCEEDED(fr->SetSize((UINT)t.w, (UINT)t.h)) && SUCCEEDED(fr->SetPixelFormat(&pf)) &&
             SUCCEEDED(fr->WriteSource(bmp.p, nullptr)) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
    }
    fr.put();
    enc.put();
    st.put();   // closes the file
    if (!ok || !MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str());
}

// Oldest first until the folder is back under kDiskKeep, once it's past kDiskMax.
void pruneCache() {
    const std::wstring dir = cacheFolder();
    if (dir.empty()) return;
    struct F { std::wstring name; std::uint64_t size, when; };
    std::vector<F> files;
    std::uint64_t total = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::uint64_t sz = ((std::uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        const std::uint64_t when = ((std::uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        files.push_back({fd.cFileName, sz, when});
        total += sz;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (total <= kDiskMax) return;
    std::sort(files.begin(), files.end(), [](const F& a, const F& b) { return a.when < b.when; });
    for (const F& f : files) {
        if (total <= kDiskKeep) break;
        if (DeleteFileW((dir + L"\\" + f.name).c_str())) total -= f.size;
    }
}

// Pictures that were used again count as new, so pruning keeps them.
void touch(const std::wstring& file) {
    HANDLE h = CreateFileW(file.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    SetFileTime(h, nullptr, nullptr, &now);
    CloseHandle(h);
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
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);   // the Shell's thumbnail handlers want one
        Com<IWICImagingFactory> factory;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put()));
        std::uint64_t made = 0;
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
            if (++made % 300 == 1) pruneCache();   // at the start, then now and then
            if (loadCached(factory.p, job.key, job.px, r->thumb)) {
            } else if (makePicture(factory.p, job.path, job.px, r->thumb)) {
                saveCached(factory.p, job.key, r->thumb);
            } else {
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

Thumbnails::PictureKind Thumbnails::kindOf(const std::wstring& name) {
    if (isImage(name)) return PictureKind::Image;
    const std::wstring e = extensionOf(name);
    if (e == L".psd" || e == L".psb") return PictureKind::Photoshop;
    if (e == L".procreate" || e == L".kra" || e == L".ora") return PictureKind::ZipArt;
    static const wchar_t* shell[] = {L".mp4", L".m4v", L".mov", L".avi", L".wmv", L".mkv", L".webm", L".mpg", L".mpeg", L".3gp", L".ts", L".pdf"};
    for (const wchar_t* x : shell)
        if (e == x) return PictureKind::Shell;
    return PictureKind::None;
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
    // Pictures are decoded whole, so very big ones keep their icon; art files and videos only have a part read.
    const PictureKind kind = kindOf(path);
    if (size == 0 || kind == PictureKind::None || (kind == PictureKind::Image && size > kMaxBytes)) return nullptr;
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

bool Thumbnails::failed(const std::wstring& path, std::uint64_t modified, std::uint64_t size, int px) const {
    (void)size;
    auto it = kept_.find(path + L"|" + std::to_wstring(modified) + L"|" + std::to_wstring(px));
    return it != kept_.end() && it->second.failed;
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

void Thumbnails::clearDisk() {
    const std::wstring dir = cacheFolder();
    if (dir.empty()) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

std::vector<std::uint64_t> Thumbnails::takeEvicted() {
    std::vector<std::uint64_t> out;
    out.swap(evicted_);
    return out;
}

}  // namespace nrf
