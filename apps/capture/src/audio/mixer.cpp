#include "audio/mixer.hpp"

#include "audio/devices.hpp"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <timeapi.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ncap::audio {

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------- the monitor output

void MonitorOutput::start(const std::wstring& device) {
    stop();
    quit_ = false;
    ring_.skip(ring_.available());
    thread_ = std::thread([this, device] { run(device); });
}

void MonitorOutput::stop() {
    quit_ = true;
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> l(m_);
    status_.clear();
}

void MonitorOutput::push(const float* frames, std::size_t count) {
    if (!thread_.joinable()) return;
    // Keep the delay short: if the device has fallen behind, drop what it hasn't played.
    if (ring_.available() > (std::size_t)kBlock * 8) ring_.skip(ring_.available() - (std::size_t)kBlock * 2);
    ring_.write(frames, count);
}

std::wstring MonitorOutput::status() const {
    std::lock_guard<std::mutex> l(m_);
    return status_;
}

void MonitorOutput::run(std::wstring device) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    auto setStatus = [&](const std::wstring& s) {
        std::lock_guard<std::mutex> l(m_);
        status_ = s;
    };
    std::vector<float> tmp;
    while (!quit_) {
        std::wstring id = device.empty() || device == L"default" ? defaultDevice(false) : device;
        ComPtr<IMMDeviceEnumerator> e;
        ComPtr<IMMDevice> dev;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioRenderClient> render;
        HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        WAVEFORMATEX fmt{};
        fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        fmt.nChannels = kChannels;
        fmt.nSamplesPerSec = kRate;
        fmt.wBitsPerSample = 32;
        fmt.nBlockAlign = kChannels * 4;
        fmt.nAvgBytesPerSec = kRate * fmt.nBlockAlign;
        HRESULT hr = id.empty() ? HRESULT_FROM_WIN32(ERROR_NOT_FOUND) : CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&e));
        if (SUCCEEDED(hr)) hr = e->GetDevice(id.c_str(), &dev);
        if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
        if (SUCCEEDED(hr))
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                    400000 /* 40 ms */, 0, &fmt, nullptr);
        UINT32 size = 0;
        if (SUCCEEDED(hr)) hr = client->SetEventHandle(ready);
        if (SUCCEEDED(hr)) hr = client->GetBufferSize(&size);
        if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&render));
        if (SUCCEEDED(hr)) hr = client->Start();
        if (FAILED(hr)) {
            setStatus(L"The monitor device isn't available");
            CloseHandle(ready);
            for (int i = 0; i < 40 && !quit_; ++i) Sleep(50);
            continue;
        }
        setStatus(L"Monitoring on " + deviceName(id));
        ULONGLONG lastCheck = GetTickCount64();
        while (!quit_) {
            WaitForSingleObject(ready, 50);
            UINT32 padding = 0;
            if (FAILED(hr = client->GetCurrentPadding(&padding))) break;
            const UINT32 room = size - padding;
            if (room == 0) continue;
            BYTE* data = nullptr;
            if (FAILED(hr = render->GetBuffer(room, &data))) break;
            const std::size_t got = ring_.read(reinterpret_cast<float*>(data), room);
            if (got < room) std::memset(data + got * kChannels * 4, 0, (room - got) * kChannels * 4);
            render->ReleaseBuffer(room, 0);
            const ULONGLONG now = GetTickCount64();
            if ((device.empty() || device == L"default") && now - lastCheck > 1000) {
                lastCheck = now;
                if (defaultDevice(false) != id) break;   // follow the new default
            }
        }
        client->Stop();
        CloseHandle(ready);
    }
    if (SUCCEEDED(co)) CoUninitialize();
}

// ---------------------------------------------------------------- the mixer

void Mixer::start() {
    if (thread_.joinable()) return;
    quit_ = false;
    buf_.assign((size_t)kBlock * kChannels, 0);
    for (auto& b : bus_) b.assign((size_t)kBlock * kChannels, 0);
    thread_ = std::thread([this] { run(); });
}

void Mixer::stop() {
    quit_ = true;
    if (thread_.joinable()) thread_.join();
    monitor_.stop();
    std::lock_guard<std::mutex> l(m_);
    if (clip_) {
        std::fclose(clip_);
        clip_ = nullptr;
    }
    sources_.clear();
}

void Mixer::setSources(std::vector<MixSource> s) {
    std::lock_guard<std::mutex> l(m_);
    sources_ = std::move(s);
}

void Mixer::setMonitorDevice(const std::wstring& device) {
    if (device == monitorDevice_) return;
    monitorDevice_ = device;
    if (device == L"off") monitor_.stop();
    else monitor_.start(device);
}

std::map<Id, Levels> Mixer::takeLevels() {
    std::lock_guard<std::mutex> l(m_);
    std::map<Id, Levels> out;
    out.swap(levels_);
    return out;
}

bool Mixer::startClip(const Id& key, const std::wstring& path, int seconds) {
    std::lock_guard<std::mutex> l(m_);
    if (clip_) return false;
    clip_ = _wfopen(path.c_str(), L"wb");
    if (!clip_) return false;
    const auto header = wavHeader(kRate, kChannels, 0);   // the sizes are filled in at the end
    std::fwrite(header.data(), 1, header.size(), clip_);
    clipKey_ = key;
    clipPath_ = path;
    clipFrames_ = 0;
    clipWanted_ = (std::uint32_t)(seconds * kRate);
    return true;
}

std::wstring Mixer::takeFinishedClip(bool* ok) {
    std::lock_guard<std::mutex> l(m_);
    std::wstring p;
    p.swap(finishedClip_);
    if (ok) *ok = finishedOk_;
    return p;
}

void Mixer::run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* high resolution */, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    timeBeginPeriod(1);
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    const LONGLONG interval = f.QuadPart / 100;
    LONGLONG next = c.QuadPart;
    while (!quit_) {
        next += interval;
        QueryPerformanceCounter(&c);
        // After a stall (the PC was busy), mix the missed blocks at once rather than skipping them,
        // so a recording's sound never loses time against its picture; beyond half a second, skip.
        if (c.QuadPart > next + 50 * interval) next = c.QuadPart - interval;
        const LONGLONG wait = next - c.QuadPart;
        if (wait > 0) {
            LARGE_INTEGER due;
            due.QuadPart = -(wait * 10000000 / f.QuadPart);
            if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 100);
            else Sleep(10);
        }
        blockQpc_ = next;
        block();
    }
    timeEndPeriod(1);
    if (timer) CloseHandle(timer);
}

// Under the lock.
void Mixer::clipWrite(const Id& key, const float* frames) {
    if (!clip_ || key != clipKey_) return;
    std::fwrite(frames, sizeof(float), (size_t)kBlock * kChannels, clip_);
    clipFrames_ += kBlock;
    if (clipFrames_ < clipWanted_) return;
    const auto header = wavHeader(kRate, kChannels, clipFrames_ * kChannels * 4);
    std::fseek(clip_, 0, SEEK_SET);
    std::fwrite(header.data(), 1, header.size(), clip_);
    finishedOk_ = std::ferror(clip_) == 0;
    std::fclose(clip_);
    clip_ = nullptr;
    finishedClip_ = clipPath_;
}

void Mixer::addTap(std::shared_ptr<Tap> tap) {
    std::lock_guard<std::mutex> l(m_);
    tap->firstQpc = 0;
    taps_.push_back(std::move(tap));
}

void Mixer::removeTap(const std::shared_ptr<Tap>& tap) {
    std::lock_guard<std::mutex> l(m_);
    taps_.erase(std::remove(taps_.begin(), taps_.end(), tap), taps_.end());
}

void Mixer::block() {
    std::vector<MixSource> sources;
    {
        std::lock_guard<std::mutex> l(m_);
        sources = sources_;
    }
    for (auto& b : bus_) std::fill(b.begin(), b.end(), 0.0f);
    std::map<Id, Levels> peaks;
    for (auto& s : sources) {
        if (s.syncTone) {
            // The beep starts exactly on each second of the performance counter.
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            const double t0 = (double)blockQpc_ / (double)f.QuadPart;
            for (int i = 0; i < kBlock; ++i) {
                const double t = t0 + (double)i / kRate;
                const double inSecond = t - std::floor(t);
                const float v = inSecond < 0.05 ? (float)(0.5 * std::sin(2 * 3.14159265358979 * 1000.0 * inSecond)) : 0.0f;
                buf_[(size_t)i * 2] = buf_[(size_t)i * 2 + 1] = v;
            }
        } else {
            if (!s.capture) continue;
            Ring& ring = s.capture->ring();
            // Keep each source's delay small: if it has run more than 100 ms ahead, drop to 20 ms.
            const size_t have = ring.available();
            if (have > (size_t)kBlock * 10) ring.skip(have - (size_t)kBlock * 2);
            const size_t got = ring.read(buf_.data(), kBlock);
            if (got < (size_t)kBlock) std::fill(buf_.begin() + (long)(got * kChannels), buf_.end(), 0.0f);
        }
        {
            std::lock_guard<std::mutex> l(m_);
            clipWrite(s.id, buf_.data());
        }
        const float gain = s.muted ? 0.0f : s.gain;
        Levels& lv = peaks[s.id];
        lv.left = peakOf(buf_.data(), kBlock, 0) * gain;
        lv.right = peakOf(buf_.data(), kBlock, 1) * gain;
        if (gain == 0) continue;
        for (int b = 0; b < kBuses; ++b)
            if (s.buses & busBit(b)) mixInto(bus_[(size_t)b].data(), buf_.data(), buf_.size(), gain);
    }
    for (int b = 0; b < kBuses; ++b) {
        Levels& lv = peaks[busKey(b)];
        lv.left = peakOf(bus_[(size_t)b].data(), kBlock, 0);
        lv.right = peakOf(bus_[(size_t)b].data(), kBlock, 1);
    }
    monitor_.push(bus_[7].data(), kBlock);
    std::lock_guard<std::mutex> l(m_);
    for (auto& t : taps_) {
        if (t->bus < 0 || t->bus >= kBuses) continue;
        if (t->firstQpc == 0) t->firstQpc = blockQpc_;
        const std::size_t wrote = t->ring.write(bus_[(size_t)t->bus].data(), kBlock);
        if (wrote < (std::size_t)kBlock) t->overflowed += kBlock - wrote;
    }
    for (int b = 0; b < kBuses; ++b) clipWrite(busKey(b), bus_[(size_t)b].data());
    for (auto& [id, p] : peaks) {
        Levels& have = levels_[id];
        have.left = std::max(have.left, p.left);
        have.right = std::max(have.right, p.right);
    }
}

}  // namespace ncap::audio
