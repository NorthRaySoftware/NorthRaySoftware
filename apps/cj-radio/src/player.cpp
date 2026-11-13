#include "player.hpp"

#include "cjui/http.hpp"

#define MA_NO_ENCODING
#define MA_NO_FLAC
#define MA_NO_WAV
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio/miniaudio.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

namespace cjr {

static const ma_uint32 kRate = 44100;
static const ma_uint32 kChannels = 2;
static const ma_uint32 kRingFrames = kRate * 6;        // up to 6 s decoded audio
static const ma_uint32 kPrebufferFrames = kRate * 3 / 2; // 1.5 s before sound starts
static const size_t kMaxQueuedBytes = 512 * 1024;

struct StreamPlayer::Impl {
    ma_device device{};
    bool deviceReady = false;
    ma_pcm_rb ring{};
    std::atomic<bool> prebuffered{false};
    std::atomic<unsigned long long> framesPlayed{0};
    float gain = 0.8f;  // smoothed, audio thread only
};

static void dataCallback(ma_device* dev, void* out, const void*, ma_uint32 frameCount) {
    static_cast<StreamPlayer*>(dev->pUserData)->fill(static_cast<float*>(out), frameCount);
}

StreamPlayer::StreamPlayer() : impl_(new Impl) {
    ma_pcm_rb_init(ma_format_f32, kChannels, kRingFrames, nullptr, nullptr, &impl_->ring);
}

StreamPlayer::~StreamPlayer() {
    stop();
    if (impl_->deviceReady) ma_device_uninit(&impl_->device);
    ma_pcm_rb_uninit(&impl_->ring);
    delete impl_;
}

void StreamPlayer::setState(State s, const std::wstring& err) {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        error_ = err;
    }
    if (state_.exchange(s) != s || !err.empty())
        if (notify_) PostMessageW(notify_, message_, 0, 0);
}

std::wstring StreamPlayer::error() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return error_;
}

std::string StreamPlayer::url() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return url_;
}

double StreamPlayer::secondsPlayed() const { return impl_->framesPlayed.load() / double(kRate); }

void StreamPlayer::setVolume(float v) { volume_ = std::clamp(v, 0.0f, 1.0f); }

void StreamPlayer::play(const std::string& url, bool live) {
    stop();
    paused_ = false;
    if (!impl_->deviceReady) {
        ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
        cfg.playback.format = ma_format_f32;
        cfg.playback.channels = kChannels;
        cfg.sampleRate = kRate;
        cfg.dataCallback = dataCallback;
        cfg.pUserData = this;
        if (ma_device_init(nullptr, &cfg, &impl_->device) != MA_SUCCESS) {
            setState(State::Error, L"No audio output device was found. Check that speakers or headphones are connected.");
            return;
        }
        impl_->deviceReady = true;
    }
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        url_ = url;
    }
    live_ = live;
    cancel_ = false;
    {
        std::lock_guard<std::mutex> lock(bytesMutex_);
        bytes_.clear();
        netDone_ = false;
    }
    ma_pcm_rb_reset(&impl_->ring);
    impl_->prebuffered = false;
    impl_->framesPlayed = 0;
    const unsigned gen = ++generation_;
    setState(State::Connecting);
    if (ma_device_start(&impl_->device) != MA_SUCCESS) {
        setState(State::Error, L"Windows couldn't start audio playback.");
        return;
    }
    net_ = std::thread([this, url, live, gen] { networkLoop(url, live, gen); });
    decode_ = std::thread([this, gen] { decodeLoop(gen); });
}

void StreamPlayer::shutdownThreads() {
    cancel_ = true;
    bytesCv_.notify_all();
    if (net_.joinable()) net_.join();
    if (decode_.joinable()) decode_.join();
}

void StreamPlayer::stop() {
    shutdownThreads();
    if (impl_->deviceReady) ma_device_stop(&impl_->device);
    impl_->prebuffered = false;
    level_ = 0;
    if (state_.load() != State::Stopped) setState(State::Stopped);
}

// ------------------------------------------------------------------ network

// A recording that ships with the app: read the file at the speed it plays.
void StreamPlayer::fileLoop(const std::string& path) {
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wpath(wlen > 0 ? wlen - 1 : 0, L'\0');
    if (wlen > 1) MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wlen);
    HANDLE f = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        setState(State::Error, L"That recording's file couldn't be opened. It may have been moved or deleted.");
    } else {
        std::vector<uint8_t> buf(64 * 1024);
        DWORD got = 0;
        while (!cancel_.load() && ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr) && got > 0) {
            std::unique_lock<std::mutex> lock(bytesMutex_);
            bytesCv_.wait(lock, [&] { return cancel_.load() || bytes_.size() < kMaxQueuedBytes; });
            if (cancel_.load()) break;
            bytes_.insert(bytes_.end(), buf.data(), buf.data() + got);
            bytesCv_.notify_all();
        }
        CloseHandle(f);
    }
    std::lock_guard<std::mutex> lock(bytesMutex_);
    netDone_ = true;
    bytesCv_.notify_all();
}

void StreamPlayer::networkLoop(std::string url, bool live, unsigned gen) {
    if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) {
        fileLoop(url);
        return;
    }
    int backoffSeconds = 2;
    while (!cancel_.load() && generation_.load() == gen) {
        std::wstring err;
        std::string contentType;
        bool gotData = false;
        const int status = cjui::httpStream(
            url,
            [&](const uint8_t* data, size_t n) {
                std::unique_lock<std::mutex> lock(bytesMutex_);
                // Recordings download faster than they play; wait for room.
                bytesCv_.wait(lock, [&] { return cancel_.load() || bytes_.size() < kMaxQueuedBytes; });
                if (cancel_.load()) return false;
                bytes_.insert(bytes_.end(), data, data + n);
                gotData = true;
                bytesCv_.notify_all();
                return true;
            },
            cancel_, err, &contentType);
        if (cancel_.load()) break;

        if (status == 200 && !contentType.empty() && contentType.find("mpeg") == std::string::npos &&
            contentType.find("mp3") == std::string::npos && contentType.find("octet-stream") == std::string::npos) {
            setState(State::Error, L"This stream isn't in MP3 format, which CJ Radio plays. The station needs an MP3 mount.");
            break;
        }
        if (!live) {
            if (status != 200 && !gotData)
                setState(State::Error, status == 404 ? L"That recording couldn't be found." : err);
            break;  // a recording ends when its data ends
        }
        if (status == 404 || status == 403) {
            // Icecast answers 404 when nothing is on that mount.
            setState(State::Error, L"The station isn't sending audio right now.");
            break;
        }
        // The connection dropped mid-broadcast: try again, waiting a little longer each time.
        setState(State::Reconnecting, err.empty() ? L"Reconnecting..." : err);
        std::unique_lock<std::mutex> lock(bytesMutex_);
        bytesCv_.wait_for(lock, std::chrono::seconds(backoffSeconds), [&] { return cancel_.load(); });
        backoffSeconds = std::min(backoffSeconds * 2, 30);
    }
    std::lock_guard<std::mutex> lock(bytesMutex_);
    netDone_ = true;
    bytesCv_.notify_all();
}

// ------------------------------------------------------------------ decoding

// A network stream can't rewind, but the MP3 decoder peeks at the first bytes
// and seeks back. Recently read bytes are kept so short rewinds replay from
// memory; forward seeks just read and discard.
struct DecodeContext {
    std::mutex* mutex;
    std::condition_variable* cv;
    std::deque<unsigned char>* bytes;
    bool* netDone;
    std::atomic<bool>* cancel;
    unsigned long long cursor = 0;
    std::deque<unsigned char> history;  // the last bytes handed to the decoder
};

static const size_t kHistoryBytes = 64 * 1024;

static size_t takeBytes(DecodeContext* ctx, unsigned char* out, size_t want) {
    std::unique_lock<std::mutex> lock(*ctx->mutex);
    ctx->cv->wait(lock, [&] { return !ctx->bytes->empty() || *ctx->netDone || ctx->cancel->load(); });
    const size_t n = std::min(want, ctx->bytes->size());
    std::copy_n(ctx->bytes->begin(), n, out);
    ctx->history.insert(ctx->history.end(), ctx->bytes->begin(), ctx->bytes->begin() + n);
    ctx->bytes->erase(ctx->bytes->begin(), ctx->bytes->begin() + n);
    if (ctx->history.size() > kHistoryBytes) ctx->history.erase(ctx->history.begin(), ctx->history.end() - kHistoryBytes);
    ctx->cursor += n;
    ctx->cv->notify_all();
    return n;
}

static ma_result onRead(ma_decoder* dec, void* out, size_t bytesToRead, size_t* bytesRead) {
    auto* ctx = static_cast<DecodeContext*>(dec->pUserData);
    *bytesRead = takeBytes(ctx, static_cast<unsigned char*>(out), bytesToRead);
    return *bytesRead == 0 ? MA_AT_END : MA_SUCCESS;
}

static ma_result onSeek(ma_decoder* dec, ma_int64 offset, ma_seek_origin origin) {
    auto* ctx = static_cast<DecodeContext*>(dec->pUserData);
    long long target;
    if (origin == ma_seek_origin_start) target = offset;
    else if (origin == ma_seek_origin_current) target = (long long)ctx->cursor + offset;
    else return MA_NOT_IMPLEMENTED;  // a live stream has no end
    if (target < 0) return MA_INVALID_ARGS;
    if ((unsigned long long)target < ctx->cursor) {
        const size_t back = (size_t)(ctx->cursor - target);
        if (back > ctx->history.size()) return MA_NOT_IMPLEMENTED;
        std::lock_guard<std::mutex> lock(*ctx->mutex);
        ctx->bytes->insert(ctx->bytes->begin(), ctx->history.end() - back, ctx->history.end());
        ctx->history.erase(ctx->history.end() - back, ctx->history.end());
        ctx->cursor = target;
        ctx->cv->notify_all();
        return MA_SUCCESS;
    }
    unsigned char discard[4096];
    while (ctx->cursor < (unsigned long long)target) {
        const size_t want = (size_t)std::min<unsigned long long>(sizeof discard, target - ctx->cursor);
        if (takeBytes(ctx, discard, want) == 0) return MA_AT_END;
    }
    return MA_SUCCESS;
}

void StreamPlayer::decodeLoop(unsigned gen) {
    DecodeContext ctx{&bytesMutex_, &bytesCv_, &bytes_, &netDone_, &cancel_};
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, kChannels, kRate);
    cfg.encodingFormat = ma_encoding_format_mp3;
    ma_decoder dec;
    if (ma_decoder_init(onRead, onSeek, &ctx, &cfg, &dec) != MA_SUCCESS) {
        if (!cancel_.load() && state_.load() != State::Error)
            setState(State::Error, live_ ? L"Couldn't read the station's audio." : L"Couldn't read that recording.");
        return;
    }
    std::vector<float> buf(4096 * kChannels);
    bool ended = false;
    while (!cancel_.load() && generation_.load() == gen) {
        const ma_uint32 space = ma_pcm_rb_available_write(&impl_->ring);
        if (space < 4096) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } else {
            ma_uint64 read = 0;
            ma_decoder_read_pcm_frames(&dec, buf.data(), 4096, &read);
            if (read == 0) { ended = true; break; }
            ma_uint32 remaining = (ma_uint32)read;
            const float* src = buf.data();
            while (remaining > 0) {
                ma_uint32 n = remaining;
                void* dst = nullptr;
                if (ma_pcm_rb_acquire_write(&impl_->ring, &n, &dst) != MA_SUCCESS || n == 0) break;
                std::memcpy(dst, src, n * kChannels * sizeof(float));
                ma_pcm_rb_commit_write(&impl_->ring, n);
                src += n * kChannels;
                remaining -= n;
            }
        }
        if (!impl_->prebuffered && ma_pcm_rb_available_read(&impl_->ring) >= kPrebufferFrames) {
            impl_->prebuffered = true;
            setState(State::Playing);
        } else if (!impl_->prebuffered && state_.load() == State::Playing) {
            setState(State::Buffering);
        }
    }
    ma_decoder_uninit(&dec);
    if (ended && !cancel_.load()) {
        // Let what's buffered finish playing, then report the end.
        impl_->prebuffered = true;
        while (!cancel_.load() && ma_pcm_rb_available_read(&impl_->ring) > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (!cancel_.load() && state_.load() != State::Error)
            setState(live_ ? State::Error : State::Finished, live_ ? L"The broadcast stream ended." : L"");
    }
}

// ------------------------------------------------------------- audio thread

// Runs on the audio thread: never blocks, never allocates.
void StreamPlayer::fill(float* out, unsigned frameCount) {
    const unsigned samples = frameCount * kChannels;
    std::fill(out, out + samples, 0.0f);
    if (!impl_->prebuffered) { level_ = level_.load() * 0.9f; return; }

    const bool paused = paused_.load();
    const float target = (muted_.load() || paused) ? 0.0f : volume_.load() * volume_.load();  // perceptual curve
    // Paused and already faded out: hold the position (nothing is read).
    if (paused && impl_->gain < 0.0005f) { impl_->gain = 0; level_ = level_.load() * 0.9f; return; }
    unsigned done = 0;
    float peak = 0;
    while (done < frameCount) {
        ma_uint32 n = frameCount - done;
        void* src = nullptr;
        if (ma_pcm_rb_acquire_read(&impl_->ring, &n, &src) != MA_SUCCESS || n == 0) break;
        const float* in = static_cast<const float*>(src);
        float* dst = out + done * kChannels;
        for (ma_uint32 i = 0; i < n * kChannels; ++i) {
            impl_->gain += (target - impl_->gain) * (paused ? 0.002f : 0.0005f);  // short glides, no clicks
            const float v = in[i] * impl_->gain;
            dst[i] = v;
            peak = std::max(peak, std::fabs(v));
        }
        ma_pcm_rb_commit_read(&impl_->ring, n);
        done += n;
    }
    impl_->framesPlayed += done;
    level_ = std::max(peak, level_.load() * 0.92f);
    if (done < frameCount && live_ && state_.load() == State::Playing) {
        // Ran dry: wait for the network to catch up before playing again.
        impl_->prebuffered = false;
        state_ = State::Buffering;
        if (notify_) PostMessageW(notify_, message_, 0, 0);
    }
}

}  // namespace cjr
