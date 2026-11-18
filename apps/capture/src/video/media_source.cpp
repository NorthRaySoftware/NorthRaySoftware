// NorthRay Capture: video and sound files as a source (system 2: "Video sources", "Media playlists").
//
// One file plays at a time (a list of files plays in order, or shuffled, and loops if asked). Each
// file is opened twice through Media Foundation's source reader, once for its pictures (as 32-bit
// RGB) and once for its sound (as 48 kHz stereo float, converted by the reader), each on its own
// thread and both timed against the same start, so a slow picture never holds up the sound. The
// sound goes into a ring the mixer reads like any other sound source; it's a channel in the mixer
// with its own volume and routing.
//
// Pausing stops both clocks; restarting and skipping open the file again.
#include "video/sources.hpp"
#include "video/design_render.hpp"

#include "ncap/sourceutil.hpp"

#include "nlohmann/json.hpp"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

namespace ncap::video {

using nlohmann::json;
namespace su = sourceutil;

namespace {

// A sound file's title and artist: its tags (through Windows' property system), else its file name.
su::TrackInfo trackInfo(const std::wstring& path) {
    su::TrackInfo t;
    ComPtr<IPropertyStore> store;
    if (SUCCEEDED(SHGetPropertyStoreFromParsingName(path.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&store)))) {
        auto get = [&](const PROPERTYKEY& k) {
            std::string out;
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(store->GetValue(k, &v)) && v.vt != VT_EMPTY) {
                PWSTR s = nullptr;
                if (SUCCEEDED(PropVariantToStringAlloc(v, &s)) && s) {
                    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
                    if (n > 1) {
                        out.resize((size_t)n - 1);
                        WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
                    }
                    CoTaskMemFree(s);
                }
            }
            PropVariantClear(&v);
            return out.substr(0, 200);
        };
        t.title = get(PKEY_Title);
        t.artist = get(PKEY_Music_Artist);
        if (t.artist.empty()) t.artist = get(PKEY_Music_AlbumArtist);
    }
    if (t.title.empty()) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string p((size_t)std::max(n - 1, 0), '\0');
        if (n > 1) WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, p.data(), n, nullptr, nullptr);
        const su::TrackInfo f = su::trackFromFileName(p);
        t.title = f.title;
        if (t.artist.empty()) t.artist = f.artist;
    }
    return t;
}


std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
std::wstring fileName(const std::wstring& path) {
    const std::size_t s = path.find_last_of(L"\\/");
    return s == std::wstring::npos ? path : path.substr(s + 1);
}
LONGLONG nowHns() {   // 100 ns units, steady
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (LONGLONG)((double)c.QuadPart * 1e7 / (double)f.QuadPart);
}

// How a reader hands its output over: the picture's layout, or the sound's rate and channels.
enum class Pix { Rgb32, Nv12, I420, Yuy2 };
struct Format {
    Pix pix = Pix::Rgb32;
    UINT32 w = 0, h = 0;
    INT32 stride = 0;
    UINT32 rate = audio::kRate, channels = audio::kChannels;
};

// A reader for one stream of the file. Pictures: 32-bit RGB if the reader can convert, else a YUV
// layout converted here. Sound: 48 kHz stereo float if the reader can convert, else float at the
// file's own rate and channels, converted here.
ComPtr<IMFSourceReader> openReader(const std::wstring& path, bool video, Format& f) {
    ComPtr<IMFAttributes> a;
    MFCreateAttributes(&a, 2);
    if (video) a->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    ComPtr<IMFSourceReader> r;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), a.Get(), &r))) return nullptr;
    r->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    const DWORD stream = video ? (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM : (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM;
    if (FAILED(r->SetStreamSelection(stream, TRUE))) return nullptr;
    auto tryType = [&](const GUID& major, const GUID& sub, bool full) {
        ComPtr<IMFMediaType> t;
        MFCreateMediaType(&t);
        t->SetGUID(MF_MT_MAJOR_TYPE, major);
        t->SetGUID(MF_MT_SUBTYPE, sub);
        if (full) {
            t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, audio::kChannels);
            t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, audio::kRate);
            t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
            t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, audio::kChannels * 4);
            t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, audio::kRate * audio::kChannels * 4);
        }
        return SUCCEEDED(r->SetCurrentMediaType(stream, nullptr, t.Get()));
    };
    bool ok = false;
    if (video) {
        const std::pair<GUID, Pix> ways[] = {{MFVideoFormat_RGB32, Pix::Rgb32}, {MFVideoFormat_NV12, Pix::Nv12}, {MFVideoFormat_I420, Pix::I420},
                                             {MFVideoFormat_YUY2, Pix::Yuy2}};
        for (auto& [g, p] : ways)
            if (tryType(MFMediaType_Video, g, false)) {
                f.pix = p;
                ok = true;
                break;
            }
    } else {
        ok = tryType(MFMediaType_Audio, MFAudioFormat_Float, true) || tryType(MFMediaType_Audio, MFAudioFormat_Float, false);
    }
    if (!ok) return nullptr;
    ComPtr<IMFMediaType> actual;
    if (FAILED(r->GetCurrentMediaType(stream, &actual))) return nullptr;
    if (video) {
        MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &f.w, &f.h);
        const UINT32 bpp = f.pix == Pix::Rgb32 ? 4 : f.pix == Pix::Yuy2 ? 2 : 1;
        f.stride = (INT32)MFGetAttributeUINT32(actual.Get(), MF_MT_DEFAULT_STRIDE, f.w * bpp);
        if (!f.w || !f.h || f.w > 8192 || f.h > 8192) return nullptr;
    } else {
        f.rate = MFGetAttributeUINT32(actual.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, audio::kRate);
        f.channels = MFGetAttributeUINT32(actual.Get(), MF_MT_AUDIO_NUM_CHANNELS, audio::kChannels);
        if (!f.rate || !f.channels || f.channels > 32) return nullptr;
    }
    return r;
}

// One YUV pixel to BGRA (BT.709 limited range for HD, BT.601 for smaller pictures).
inline void yuvToBgra(int y, int u, int v, bool hd, std::uint8_t* out) {
    const float Y = 1.164f * (float)(y - 16), U = (float)(u - 128), V = (float)(v - 128);
    float r, g, b;
    if (hd) {
        r = Y + 1.793f * V;
        g = Y - 0.213f * U - 0.533f * V;
        b = Y + 2.112f * U;
    } else {
        r = Y + 1.596f * V;
        g = Y - 0.392f * U - 0.813f * V;
        b = Y + 2.017f * U;
    }
    out[0] = (std::uint8_t)std::clamp((int)(b + 0.5f), 0, 255);
    out[1] = (std::uint8_t)std::clamp((int)(g + 0.5f), 0, 255);
    out[2] = (std::uint8_t)std::clamp((int)(r + 0.5f), 0, 255);
    out[3] = 255;
}

// A decoded picture (in the reader's layout) to top-down BGRA.
void toBgra(const Format& f, const BYTE* data, std::size_t len, std::vector<std::uint8_t>& out) {
    const UINT32 w = f.w, h = f.h;
    out.resize((std::size_t)w * h * 4);
    const bool hd = h >= 720;
    switch (f.pix) {
        case Pix::Rgb32: {
            const std::size_t row = (std::size_t)w * 4, pitch = (std::size_t)std::abs(f.stride);
            if (len < pitch * (h - 1) + row) return;
            for (UINT32 y = 0; y < h; ++y) {
                const BYTE* src = f.stride < 0 ? data + (std::size_t)(h - 1 - y) * pitch : data + (std::size_t)y * pitch;
                std::memcpy(out.data() + (std::size_t)y * row, src, row);
            }
            for (std::size_t i = 3; i < out.size(); i += 4) out[i] = 255;   // RGB32's fourth byte isn't alpha
            break;
        }
        case Pix::Nv12:
        case Pix::I420: {
            const std::size_t pitch = (std::size_t)std::max<INT32>(f.stride, (INT32)w);
            const std::size_t ySize = pitch * h;
            const std::size_t need = f.pix == Pix::Nv12 ? ySize + pitch * ((h + 1) / 2) : ySize + 2 * (pitch / 2) * ((h + 1) / 2);
            if (len < need) return;
            const BYTE* Y = data;
            for (UINT32 y = 0; y < h; ++y)
                for (UINT32 x = 0; x < w; ++x) {
                    int u, v;
                    if (f.pix == Pix::Nv12) {
                        const BYTE* uv = data + ySize + (std::size_t)(y / 2) * pitch + (x / 2) * 2;
                        u = uv[0];
                        v = uv[1];
                    } else {
                        const std::size_t cp = pitch / 2, cs = cp * ((h + 1) / 2);
                        u = data[ySize + (std::size_t)(y / 2) * cp + x / 2];
                        v = data[ySize + cs + (std::size_t)(y / 2) * cp + x / 2];
                    }
                    yuvToBgra(Y[(std::size_t)y * pitch + x], u, v, hd, &out[((std::size_t)y * w + x) * 4]);
                }
            break;
        }
        case Pix::Yuy2: {
            const std::size_t pitch = (std::size_t)std::max<INT32>(f.stride, (INT32)w * 2);
            if (len < pitch * h) return;
            for (UINT32 y = 0; y < h; ++y)
                for (UINT32 x = 0; x + 1 < w + 1; x += 2) {
                    const BYTE* p = data + (std::size_t)y * pitch + (std::size_t)x * 2;
                    yuvToBgra(p[0], p[1], p[3], hd, &out[((std::size_t)y * w + x) * 4]);
                    if (x + 1 < w) yuvToBgra(p[2], p[1], p[3], hd, &out[((std::size_t)y * w + x + 1) * 4]);
                }
            break;
        }
    }
}

// Sound at the file's rate and channels to 48 kHz stereo (linear interpolation; the first two
// channels, or the one copied to both).
class Resampler {
public:
    void reset(UINT32 rate, UINT32 channels) {
        rate_ = rate;
        channels_ = channels;
        pos_ = 0;
        last_[0] = last_[1] = 0;
    }
    void run(const float* in, std::size_t frames, std::vector<float>& out) {
        out.clear();
        if (!frames) return;
        auto frame = [&](std::size_t i, int ch) -> float {
            const float* p = in + i * channels_;
            return channels_ == 1 ? p[0] : p[ch];
        };
        if (rate_ == (UINT32)audio::kRate) {
            out.reserve(frames * 2);
            for (std::size_t i = 0; i < frames; ++i) {
                out.push_back(frame(i, 0));
                out.push_back(frame(i, 1));
            }
            return;
        }
        const double step = (double)rate_ / audio::kRate;
        // pos_ is relative to the start of this block; -1..0 interpolates from the previous block's last frame.
        while (pos_ < (double)frames - 1) {
            const long i = (long)std::floor(pos_);
            const double t = pos_ - (double)i;
            for (int ch = 0; ch < 2; ++ch) {
                const float a = i < 0 ? last_[ch] : frame((std::size_t)i, ch);
                const float b = frame((std::size_t)(i + 1), ch);
                out.push_back((float)(a + (b - a) * t));
            }
            pos_ += step;
        }
        pos_ -= (double)frames;
        last_[0] = frame(frames - 1, 0);
        last_[1] = frame(frames - 1, 1);
    }

private:
    UINT32 rate_ = audio::kRate, channels_ = 2;
    double pos_ = 0;
    float last_[2] = {0, 0};
};

double durationOf(IMFSourceReader* r) {
    PROPVARIANT v;
    PropVariantInit(&v);
    double s = 0;
    if (SUCCEEDED(r->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &v)) && v.vt == VT_UI8)
        s = (double)v.uhVal.QuadPart / 1e7;
    PropVariantClear(&v);
    return s;
}

class MediaSource : public VideoSource {
public:
    explicit MediaSource(const std::string& settings) : sound_(std::make_shared<audio::Ring>((std::size_t)audio::kRate / 2)) { setSettings(settings); }
    ~MediaSource() override {
        stopPlaying();
        clearNowPlaying(this);
    }
    std::shared_ptr<audio::Ring> sound() override { return sound_; }
    double length() const override { return length_.load(); }

    void command(const std::string& c) override {
        if (c == "pause") pause(true);
        else if (c == "play") pause(false);
        else if (c == "restart") skip_ = 0;
        else if (c == "start") {   // from the beginning, playing now (a stinger)
            std::lock_guard<std::mutex> l(clockM_);
            if (atStart_) {   // already open and waiting at its first picture: just go
                atStart_ = false;
                paused_ = false;
                startedAt_ = nowHns();
            } else {
                immediate_ = true;
                skip_ = 0;
                paused_ = false;
            }
        }
        else if (c == "next") skip_ = 1;
    }

    void update(Gpu& gpu) override {
        wanted();
        std::string j;
        if (takeSettings(j)) {
            const json s = json::parse(j, nullptr, false);
            std::vector<std::wstring> files;
            if (s.is_object() && s.contains("files") && s["files"].is_array())
                for (auto& f : s["files"])
                    if (f.is_string()) files.push_back(widen(f.get<std::string>()));
            const bool loop = s.is_object() && s.value("loop", true);
            const bool shuffle = s.is_object() && s.value("shuffle", false);
            cue_ = s.is_object() && s.value("startPaused", false);
            if (cue_) {   // a stinger: waits at its start until asked
                std::lock_guard<std::mutex> l(clockM_);
                paused_ = true;
            }
            startPlaying(files, loop, shuffle);
        }
        std::lock_guard<std::mutex> l(pic_.m);
        if (pic_.serial == seen_ || pic_.w <= 0) return;
        seen_ = pic_.serial;
        if (!out_.ensure(gpu.device(), pic_.w, pic_.h)) return;
        gpu.context()->UpdateSubresource(out_.tex.Get(), 0, nullptr, pic_.bgra.data(), (UINT)pic_.w * 4, 0);
        hasPicture_ = true;
        w_ = pic_.w;
        h_ = pic_.h;
    }

private:
    struct Pic {
        std::mutex m;
        std::vector<std::uint8_t> bgra;
        int w = 0, h = 0;
        std::uint64_t serial = 0;
    };

    void pause(bool on) {
        std::lock_guard<std::mutex> l(clockM_);
        if (on == paused_) return;
        if (on) pausedAt_ = nowHns();
        else startedAt_ += nowHns() - pausedAt_;   // the clock picks up where it stopped
        paused_ = on;
    }
    // Where the file is now (100 ns), and whether it's paused.
    LONGLONG position(bool& paused) {
        std::lock_guard<std::mutex> l(clockM_);
        paused = paused_;
        return (paused_ ? pausedAt_ : nowHns()) - startedAt_;
    }

    void stopPlaying() {
        quit_ = true;
        if (control_.joinable()) control_.join();
    }

    void startPlaying(std::vector<std::wstring> files, bool loop, bool shuffle) {
        stopPlaying();
        quit_ = false;
        control_ = std::thread([this, files, loop, shuffle]() mutable {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const bool mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
            play(files, loop, shuffle);
            if (mf) MFShutdown();
            CoUninitialize();
        });
    }

    void play(std::vector<std::wstring>& files, bool loop, bool shuffle) {
        if (files.empty()) {
            setStatus(L"No files chosen yet", true);
            return;
        }
        std::uint32_t round = GetTickCount();
        for (;;) {
            std::vector<std::wstring> order = files;
            if (shuffle && order.size() > 1) {
                std::vector<std::string> keys;
                for (std::size_t i = 0; i < order.size(); ++i) keys.push_back(std::to_string(i));
                keys = su::slideOrder(keys, true, round++);
                std::vector<std::wstring> o;
                for (auto& k : keys) o.push_back(files[(std::size_t)std::stoul(k)]);
                order = o;
            }
            int played = 0;
            for (std::size_t i = 0; i < order.size() && !quit_; ++i) {
                const int r = playFile(order[i], i + 1, order.size());
                if (r > 0) ++played;
                if (r == 2) --i;   // restart this one
            }
            if (quit_) return;
            if (!played) {
                setStatus(order.size() == 1 ? fileName(order[0]) + L": Windows can't play this file" : L"None of these files could be played", true);
                return;
            }
            if (cue_) {   // a stinger: back to its start, open and paused, ready for the next time
                std::lock_guard<std::mutex> l(clockM_);
                paused_ = true;
                continue;
            }
            if (!loop) {   // stays on its last picture until it's started again
                setStatus(L"Finished (" + fileName(order.back()) + L")");
                clearNowPlaying(this);   // nothing's playing now
                skip_ = -1;
                while (!quit_ && skip_ != 0) Sleep(10);
                if (quit_) return;
            }
        }
    }

    // 0: couldn't be played; 1: played to its end (or skipped); 2: restart asked for.
    int playFile(const std::wstring& path, std::size_t n, std::size_t of) {
        Format vf, af;
        ComPtr<IMFSourceReader> video = openReader(path, true, vf), sound = openReader(path, false, af);
        if (!video && !sound) return 0;
        // Music (sound-only files, not a stinger) is what designs show as {song} and {artist}.
        if (!video && sound && !cue_) setNowPlaying(this, trackInfo(path));
        const double length = durationOf(video ? video.Get() : sound.Get());
        length_ = length;
        {
            std::lock_guard<std::mutex> l(clockM_);
            // A fifth of a second to get going, except when it's started on cue (a stinger).
            startedAt_ = nowHns() + (paused_ || immediate_.exchange(false) ? 0 : 2000000);
            atStart_ = paused_;
            pausedAt_ = startedAt_;
        }
        skip_ = -1;
        std::atomic<bool> videoDone{!video}, soundDone{!sound};
        const std::wstring name = fileName(path) + (of > 1 ? L" (" + std::to_wstring(n) + L" of " + std::to_wstring(of) + L")" : L"");

        std::thread vt([&] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            while (video && !quit_ && skip_ < 0) {
                DWORD flags = 0;
                LONGLONG ts = 0;
                ComPtr<IMFSample> sample;
                if (FAILED(video->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &sample)) ||
                    (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)))
                    break;
                if (!sample) continue;
                // Wait for its moment (or drop it if it's already late by more than a frame or two).
                bool paused = false;
                for (;;) {
                    const LONGLONG pos = position(paused);
                    if (quit_ || skip_ >= 0) break;
                    if (!paused && pos >= ts) break;
                    Sleep(paused ? 20 : (DWORD)std::clamp<LONGLONG>((ts - pos) / 10000, 1, 20));
                }
                if (quit_ || skip_ >= 0) break;
                bool p2 = false;
                if (position(p2) - ts > 1000000) continue;   // more than 100 ms late: skip it
                ComPtr<IMFMediaBuffer> buf;
                BYTE* data = nullptr;
                DWORD len = 0;
                if (FAILED(sample->ConvertToContiguousBuffer(&buf)) || FAILED(buf->Lock(&data, nullptr, &len))) continue;
                if (recentlyWanted()) {
                    std::vector<std::uint8_t> bgra;
                    toBgra(vf, data, len, bgra);
                    if (!bgra.empty()) {
                        std::lock_guard<std::mutex> l(pic_.m);
                        pic_.w = (int)vf.w;
                        pic_.h = (int)vf.h;
                        pic_.bgra.swap(bgra);
                        ++pic_.serial;
                    }
                }
                buf->Unlock();
            }
            videoDone = true;
            CoUninitialize();
        });
        std::thread at([&] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            std::vector<float> pending;
            LONGLONG pendingTs = 0;
            Resampler rs;
            rs.reset(af.rate, af.channels);
            while (sound && !quit_ && skip_ < 0) {
                if (pending.empty()) {
                    DWORD flags = 0;
                    LONGLONG ts = 0;
                    ComPtr<IMFSample> sample;
                    if (FAILED(sound->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, &ts, &sample)) ||
                        (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)))
                        break;
                    if (!sample) continue;
                    ComPtr<IMFMediaBuffer> buf;
                    BYTE* data = nullptr;
                    DWORD len = 0;
                    if (FAILED(sample->ConvertToContiguousBuffer(&buf)) || FAILED(buf->Lock(&data, nullptr, &len))) continue;
                    rs.run(reinterpret_cast<const float*>(data), len / sizeof(float) / af.channels, pending);
                    buf->Unlock();
                    pendingTs = ts;
                }
                // Hand it over a little at a time, up to kLead ahead of its moment, so the mixer always has
                // some waiting but never more than kMost. (Before 0.10.30 a whole decoded block went in at once:
                // FLAC, WAV and resampled sound come in blocks of 100 ms or more, the mixer cut anything over
                // 100 ms back to 20 ms to keep the delay down, and the music skipped many times a second.)
                constexpr LONGLONG kLead = 500000;                            // 50 ms, in 100 ns units
                constexpr std::size_t kMost = (std::size_t)audio::kBlock * 8;   // 80 ms (the mixer trims past 100)
                bool paused = false;
                const LONGLONG pos = position(paused);
                if (paused) {
                    Sleep(5);
                    continue;
                }
                const std::size_t frames = pending.size() / audio::kChannels;
                if (pos - pendingTs > 2000000) {   // fell 200 ms behind (the PC was busy): drop it
                    pending.clear();
                    continue;
                }
                const LONGLONG ahead = pos + kLead - pendingTs;   // how much of it may go now
                const std::size_t have = sound_->available();
                if (ahead <= 0 || have >= kMost) {
                    Sleep(2);
                    continue;
                }
                const std::size_t allowed = std::min({frames, (std::size_t)(ahead * audio::kRate / 10000000) + 1, kMost - have});
                const std::size_t wrote = sound_->write(pending.data(), allowed);
                pending.erase(pending.begin(), pending.begin() + (long)(wrote * audio::kChannels));
                pendingTs += (LONGLONG)wrote * 10000000 / audio::kRate;
                if (wrote < frames) Sleep(2);
            }
            soundDone = true;
            CoUninitialize();
        });
        // Status while it plays.
        while (!(videoDone && soundDone) && !quit_ && skip_ < 0) {
            bool paused = false;
            const double at = std::max<LONGLONG>(0, position(paused)) / 1e7;
            std::wstring s = name + L" · " + widen(su::clockText(at));
            if (length > 0) s += L" / " + widen(su::clockText(length));
            if (paused) s += L" · paused";
            if (!video) s += L" · sound only";
            setStatus(s);
            Sleep(250);
        }
        const int asked = skip_;
        skip_ = 1;   // let both threads go
        vt.join();
        at.join();
        if (asked == 0) return 2;
        return 1;
    }

    std::shared_ptr<audio::Ring> sound_;
    std::thread control_;
    std::atomic<bool> quit_{false};
    std::atomic<int> skip_{-1};   // -1 playing; 0 restart; 1 next
    std::atomic<double> length_{0};
    std::atomic<bool> immediate_{false};
    bool atStart_ = false;   // (clockM_) open, paused at the very start
    bool cue_ = false;       // played on cue (a stinger)
    std::mutex clockM_;
    LONGLONG startedAt_ = 0, pausedAt_ = 0;
    bool paused_ = false;
    Pic pic_;
    std::uint64_t seen_ = 0;
};

}  // namespace

std::shared_ptr<VideoSource> createMediaSource(const std::string& settings) { return std::make_shared<MediaSource>(settings); }

double mediaLength(const std::wstring& path) {
    // Its own COM and Media Foundation start-up: called on a worker thread (the Library's lengths).
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    double s = 0;
    if (SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        ComPtr<IMFSourceReader> r;
        if (SUCCEEDED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &r))) s = durationOf(r.Get());
        r.Reset();
        MFShutdown();
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return s;
}

}  // namespace ncap::video
