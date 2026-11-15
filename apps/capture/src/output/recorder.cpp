#include "output/recorder.hpp"

#include "ncap/mkv.hpp"
#include "ncap/profiles.hpp"

#include <objbase.h>
#include <shlobj.h>

#include <cstring>
#include <ctime>

namespace ncap::output {

namespace {

// A file written through a 1 MB buffer; earlier bytes can be patched (cluster and segment sizes).
class FileSink : public mkv::Sink {
public:
    explicit FileSink(const std::wstring& path) {
        h_ = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        buf_.reserve(1 << 20);
    }
    ~FileSink() override { close(); }
    bool ok() const { return h_ != INVALID_HANDLE_VALUE && !failed_; }
    bool write(const std::uint8_t* d, std::size_t n) override {
        buf_.insert(buf_.end(), d, d + n);
        pos_ += n;
        return buf_.size() < (1 << 20) || flush();
    }
    bool writeAt(std::uint64_t at, const std::uint8_t* d, std::size_t n) override {
        if (at >= written_) {   // still in the buffer
            if (at - written_ + n > buf_.size()) return false;
            std::memcpy(buf_.data() + (at - written_), d, n);
            return true;
        }
        if (!flush()) return false;
        LARGE_INTEGER p;
        p.QuadPart = (LONGLONG)at;
        DWORD w = 0;
        const bool ok = SetFilePointerEx(h_, p, nullptr, FILE_BEGIN) && WriteFile(h_, d, (DWORD)n, &w, nullptr) && w == n;
        p.QuadPart = 0;
        SetFilePointerEx(h_, p, nullptr, FILE_END);
        if (!ok) failed_ = true;
        return ok;
    }
    std::uint64_t position() const override { return pos_; }
    bool flush() {
        if (h_ == INVALID_HANDLE_VALUE || failed_) return false;
        if (buf_.empty()) return true;
        DWORD w = 0;
        if (!WriteFile(h_, buf_.data(), (DWORD)buf_.size(), &w, nullptr) || w != buf_.size()) {
            failed_ = true;
            return false;
        }
        written_ += buf_.size();
        buf_.clear();
        return true;
    }
    void close() {
        if (h_ == INVALID_HANDLE_VALUE) return;
        flush();
        FlushFileBuffers(h_);
        CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
    }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    std::vector<std::uint8_t> buf_;
    std::uint64_t pos_ = 0, written_ = 0;
    bool failed_ = false;
};

std::uint64_t freeSpace(const std::wstring& folder) {
    ULARGE_INTEGER free{};
    return GetDiskFreeSpaceExW(folder.c_str(), &free, nullptr, nullptr) ? free.QuadPart : 0;
}

}  // namespace

bool Recorder::start(video::Engine& engine, audio::Mixer& mixer, const RecordSettings& s, std::wstring& error) {
    if (running_) return true;
    if (!engine.running()) {
        error = L"Video capture isn't running, so there's nothing to record.";
        return false;
    }
    engine_ = &engine;
    mixer_ = &mixer;
    s_ = s;
    if (s_.tracks.empty()) s_.tracks = {1};
    if (s_.tracks.size() > 6) s_.tracks.resize(6);
    SHCreateDirectoryExW(nullptr, s_.folder.c_str(), nullptr);
    const std::uint64_t free = freeSpace(s_.folder);
    if (free && free < 500ull * 1024 * 1024) {
        error = L"There's less than 500 MB free where recordings go. Free some space or choose another folder in Settings › Recording.";
        return false;
    }
    // Encoders first: if they can't start, nothing else is touched.
    video_ = std::make_unique<encode::VideoEncoder>();
    if (!video_->open(engine.gpu(), s_.video, error)) {
        video_.reset();
        return false;
    }
    audio_.clear();
    for (std::size_t i = 0; i < s_.tracks.size(); ++i) {
        auto a = std::make_unique<encode::AudioEncoder>();
        if (!a->open(s_.audioKbps, error)) {
            video_->close();
            video_.reset();
            audio_.clear();
            return false;
        }
        audio_.push_back(std::move(a));
    }
    const std::time_t now = std::time(nullptr);
    std::tm t{};
    localtime_s(&t, &now);
    std::wstring base = s_.folder + L"\\" +
                        [&] {
                            const std::string n = profiles::recordingName(s_.profile, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
                            const int len = MultiByteToWideChar(CP_UTF8, 0, n.data(), (int)n.size(), nullptr, 0);
                            std::wstring w((size_t)len, L'\0');
                            MultiByteToWideChar(CP_UTF8, 0, n.data(), (int)n.size(), w.data(), len);
                            return w;
                        }();
    std::wstring path = base + L".mkv";
    for (int k = 2; GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES && k < 100; ++k) path = base + L" (" + std::to_wstring(k) + L").mkv";
    {
        std::lock_guard<std::mutex> l(m_);
        st_ = RecordStatus{};
        st_.active = true;
        st_.path = path;
        st_.encoder = video_->name();
        st_.hardware = video_->hardware();
        st_.freeBytes = free;
    }
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    freq_ = f.QuadPart;
    t0_ = c.QuadPart;
    lastFrame_ = -1;
    stopping_ = false;
    running_ = true;
    // Sound from the next mixer block, pictures from the next composed frame.
    taps_.clear();
    for (int track : s_.tracks) {
        auto tap = std::make_shared<audio::Tap>(track);   // bus index = track number
        taps_.push_back(tap);
        mixer.addTap(tap);
    }
    engine.setFrameSink("record", [this](ID3D11Texture2D* canvas, LONGLONG qpc) { frame(canvas, qpc); });
    thread_ = std::thread([this] { run(); });
    return true;
}

// Video thread.
void Recorder::frame(ID3D11Texture2D* canvas, LONGLONG qpc) {
    std::lock_guard<std::mutex> busy(frameM_);   // the recorder thread waits for this before closing the encoder
    if (stopping_ || !video_) return;
    // The frame's place on the constant grid.
    const std::int64_t index = (std::int64_t)((double)(qpc - t0_) * s_.video.fps / (double)freq_ + 0.5);
    if (index <= lastFrame_) return;   // composed twice in one frame's time
    std::uint64_t gap = lastFrame_ < 0 ? 0 : (std::uint64_t)(index - lastFrame_ - 1);
    lastFrame_ = index;
    video_->submit(canvas, index * 1000000 / s_.video.fps);
    std::lock_guard<std::mutex> l(m_);
    st_.droppedFrames += gap;
    ++st_.frames;
}

void Recorder::stop() {
    if (!running_) return;
    stopping_ = true;
    if (engine_) engine_->setFrameSink("record", nullptr);   // no more frames (waits for the video thread's lock)
    if (thread_.joinable()) thread_.join();
    for (auto& t : taps_) mixer_->removeTap(t);
    taps_.clear();
    running_ = false;
}

RecordStatus Recorder::status() const {
    std::lock_guard<std::mutex> l(m_);
    return st_;
}

std::wstring Recorder::takeFinished(bool* ok, std::wstring* problem) {
    std::lock_guard<std::mutex> l(m_);
    std::wstring p;
    p.swap(finished_);
    if (ok) *ok = finishedOk_;
    if (problem) *problem = finishedProblem_;
    return p;
}

void Recorder::run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::wstring path;
    {
        std::lock_guard<std::mutex> l(m_);
        path = st_.path;
    }
    FileSink file(path);
    mkv::Writer writer;
    media::Interleaver il(1 + (int)s_.tracks.size());
    bool begun = false, failed = !file.ok();
    std::wstring problem = failed ? L"The recording file couldn't be created in " + s_.folder : L"";
    std::vector<float> block((size_t)1024 * audio::kChannels);
    std::vector<bool> anchored(s_.tracks.size(), false);
    ULONGLONG lastSpaceCheck = GetTickCount64();

    auto writePacket = [&](media::Packet& p) {
        if (failed) return;
        const bool ok = p.stream == 0 ? writer.video(p.pts, p.keyframe, p.data) : writer.audio(p.stream - 1, p.pts, p.data);
        if (!ok) {
            failed = true;
            problem = L"Writing the recording failed (is the disk full or unplugged?). What was written so far is kept.";
        }
    };
    auto takeAudio = [&](bool all) {
        for (std::size_t k = 0; k < taps_.size(); ++k) {
            auto& tap = *taps_[k];
            while (tap.ring.available() >= 1024 || (all && tap.ring.available() > 0)) {
                const std::size_t n = tap.ring.read(block.data(), 1024);
                if (!anchored[k]) {
                    anchored[k] = true;
                    // The first block's time on the recording's clock; samples are counted from it.
                    const std::int64_t first = (std::int64_t)((double)(tap.firstQpc.load() - t0_) * 1e6 / (double)freq_);
                    audio_[k]->encode(block.data(), n, std::max<std::int64_t>(0, first));
                } else {
                    audio_[k]->encode(block.data(), n, 0);
                }
            }
            media::Packet p;
            while (audio_[k]->take(p, 1 + (int)k)) il.push(std::move(p));
        }
    };
    auto takeVideo = [&] {
        media::Packet p;
        while (video_->take(p)) {
            if (!begun) {
                // The file starts with the first keyframe: it carries the SPS and PPS for the header.
                media::Bytes sps, pps;
                if (!p.keyframe || !media::findParameterSets(p.data.data(), p.data.size(), sps, pps)) continue;
                mkv::VideoTrack v;
                v.width = s_.video.width;
                v.height = s_.video.height;
                v.fps = s_.video.fps;
                v.avcC = media::avcC(sps, pps);
                std::vector<mkv::AudioTrack> at;
                for (std::size_t k = 0; k < s_.tracks.size(); ++k)
                    at.push_back({k < s_.trackNames.size() ? s_.trackNames[k] : "Track " + std::to_string(s_.tracks[k]), audio::kRate, audio::kChannels,
                                  audio_[k]->config()});
                if (!failed && !writer.begin(&file, v, at, "NorthRay Capture")) {
                    failed = true;
                    problem = L"The recording file couldn't be written.";
                }
                begun = true;
            }
            p.data = media::annexBToLengthPrefixed(p.data.data(), p.data.size(), true);
            il.push(std::move(p));
        }
    };

    while (!stopping_) {
        Sleep(5);
        takeAudio(false);
        takeVideo();
        media::Packet p;
        while (begun && il.pop(p)) writePacket(p);
        const std::wstring encErr = video_->error();
        const ULONGLONG now = GetTickCount64();
        std::uint64_t free = 0;
        if (now - lastSpaceCheck > 5000) {
            lastSpaceCheck = now;
            free = freeSpace(s_.folder);
            if (free && free < 200ull * 1024 * 1024) {   // stop cleanly before the disk is full
                problem = L"The disk was nearly full, so recording stopped. The file is complete up to that point.";
                stopping_ = true;
            }
        }
        const std::uint64_t encDropped = video_->dropped();
        std::lock_guard<std::mutex> l(m_);
        st_.encoderDropped = encDropped;
        st_.seconds = (double)writer.durationUs() / 1e6;
        st_.bytes = file.position();
        if (free) st_.freeBytes = free;
        if (!encErr.empty()) st_.error = encErr;
        if (failed) {
            st_.error = problem;
            stopping_ = true;
        }
    }
    // The end: no more frames, the encoders' last packets, everything in order, and the file closed.
    if (engine_) engine_->setFrameSink("record", nullptr);
    { std::lock_guard<std::mutex> wait(frameM_); }   // a frame being handed over finishes first
    video_->finish();
    takeVideo();
    takeAudio(true);
    for (std::size_t k = 0; k < audio_.size(); ++k) {
        audio_[k]->finish();
        media::Packet p;
        while (audio_[k]->take(p, 1 + (int)k)) il.push(std::move(p));
    }
    media::Packet p;
    while (il.drain(p))
        if (begun) writePacket(p);
    if (begun) writer.finish();
    file.close();
    video_->close();
    if (!begun) {
        DeleteFileW(path.c_str());   // nothing was ever encoded
        if (problem.empty()) problem = L"The encoder produced no pictures, so no file was kept. " + video_->error();
        failed = true;
    }
    {
        std::lock_guard<std::mutex> l(m_);
        st_.active = false;
        if (!problem.empty()) st_.error = problem;
        finished_ = path;
        finishedOk_ = !failed && begun;
        finishedProblem_ = problem;
    }
    video_.reset();
    audio_.clear();
    CoUninitialize();
}

}  // namespace ncap::output
