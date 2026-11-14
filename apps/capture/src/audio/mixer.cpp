#include "audio/mixer.hpp"

#include <windows.h>
#include <timeapi.h>

#include <algorithm>
#include <cstring>

namespace ncap::audio {

void Mixer::start() {
    if (thread_.joinable()) return;
    quit_ = false;
    buf_.assign((size_t)kBlock * kChannels, 0);
    mix_.assign((size_t)kBlock * kChannels, 0);
    thread_ = std::thread([this] { run(); });
}

void Mixer::stop() {
    quit_ = true;
    if (thread_.joinable()) thread_.join();
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

std::map<Id, Levels> Mixer::takeLevels() {
    std::lock_guard<std::mutex> l(m_);
    std::map<Id, Levels> out;
    out.swap(levels_);
    return out;
}

bool Mixer::startClip(const Id& source, const std::wstring& path, int seconds) {
    std::lock_guard<std::mutex> l(m_);
    if (clip_) return false;
    clip_ = _wfopen(path.c_str(), L"wb");
    if (!clip_) return false;
    const auto header = wavHeader(kRate, kChannels, 0);   // the sizes are filled in at the end
    std::fwrite(header.data(), 1, header.size(), clip_);
    clipSource_ = source;
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
        if (c.QuadPart > next + 5 * interval) next = c.QuadPart;   // after a long stall, don't rush to catch up
        const LONGLONG wait = next - c.QuadPart;
        if (wait > 0) {
            LARGE_INTEGER due;
            due.QuadPart = -(wait * 10000000 / f.QuadPart);
            if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 100);
            else Sleep(10);
        }
        block();
    }
    timeEndPeriod(1);
    if (timer) CloseHandle(timer);
}

void Mixer::block() {
    std::vector<MixSource> sources;
    {
        std::lock_guard<std::mutex> l(m_);
        sources = sources_;
    }
    std::fill(mix_.begin(), mix_.end(), 0.0f);
    std::map<Id, Levels> peaks;
    for (auto& s : sources) {
        if (!s.capture) continue;
        Ring& ring = s.capture->ring();
        // Keep each source's delay small: if it has run more than 100 ms ahead, drop to 20 ms.
        const size_t have = ring.available();
        if (have > (size_t)kBlock * 10) ring.skip(have - (size_t)kBlock * 2);
        const size_t got = ring.read(buf_.data(), kBlock);
        if (got < (size_t)kBlock) std::fill(buf_.begin() + (long)(got * kChannels), buf_.end(), 0.0f);
        {
            std::lock_guard<std::mutex> l(m_);
            if (clip_ && s.id == clipSource_) {
                std::fwrite(buf_.data(), sizeof(float), buf_.size(), clip_);
                clipFrames_ += kBlock;
                if (clipFrames_ >= clipWanted_) {
                    const std::uint32_t bytes = clipFrames_ * kChannels * 4;
                    const auto header = wavHeader(kRate, kChannels, bytes);
                    std::fseek(clip_, 0, SEEK_SET);
                    std::fwrite(header.data(), 1, header.size(), clip_);
                    finishedOk_ = std::ferror(clip_) == 0;
                    std::fclose(clip_);
                    clip_ = nullptr;
                    finishedClip_ = clipPath_;
                }
            }
        }
        const float gain = s.muted ? 0.0f : s.gain;
        Levels& lv = peaks[s.id];
        lv.left = peakOf(buf_.data(), kBlock, 0) * gain;
        lv.right = peakOf(buf_.data(), kBlock, 1) * gain;
        mixInto(mix_.data(), buf_.data(), mix_.size(), gain);
    }
    Levels& master = peaks[Id{}];
    master.left = peakOf(mix_.data(), kBlock, 0);
    master.right = peakOf(mix_.data(), kBlock, 1);
    std::lock_guard<std::mutex> l(m_);
    for (auto& [id, p] : peaks) {
        Levels& have = levels_[id];
        have.left = std::max(have.left, p.left);
        have.right = std::max(have.right, p.right);
    }
}

}  // namespace ncap::audio
