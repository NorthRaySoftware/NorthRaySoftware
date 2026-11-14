#include "output/streamer.hpp"

#include "output/net.hpp"

#include "ncap/rtmp.hpp"

#include <objbase.h>

#include <algorithm>
#include <deque>

namespace ncap::output {

namespace {
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
}  // namespace

bool Streamer::start(video::Engine& engine, audio::Mixer& mixer, const StreamSettings& s, std::wstring& error) {
    if (running_) return true;
    rtmp::Url url;
    if (!rtmp::parseUrl(s.url, url)) {
        error = L"The server address isn't an RTMP address (it should start with rtmp:// or rtmps://).";
        return false;
    }
    if (s.key.empty()) {
        error = L"There's no stream key. Enter it in Settings › Streaming.";
        return false;
    }
    if (!engine.running()) {
        error = L"Video capture isn't running, so there's nothing to stream.";
        return false;
    }
    engine_ = &engine;
    mixer_ = &mixer;
    s_ = s;
    s_.video.constantBitrate = true;   // platforms want CBR
    video_ = std::make_unique<encode::VideoEncoder>();
    if (!video_->open(engine.gpu(), s_.video, error)) {
        video_.reset();
        return false;
    }
    audio_ = std::make_unique<encode::AudioEncoder>();
    if (!audio_->open(s_.audioKbps, error)) {
        video_->close();
        video_.reset();
        audio_.reset();
        return false;
    }
    {
        std::lock_guard<std::mutex> l(m_);
        st_ = StreamStatus{};
        st_.state = StreamStatus::Connecting;
        st_.message = L"Connecting to " + wide(s_.name) + L"…";
        st_.encoder = video_->name();
        st_.hardware = video_->hardware();
    }
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    freq_ = f.QuadPart;
    t0_ = c.QuadPart;
    lastFrame_ = -1;
    stopping_ = false;
    running_ = true;
    tap_ = std::make_shared<audio::Tap>(0);   // the Stream bus
    mixer.addTap(tap_);
    engine.setFrameSink("stream", [this](ID3D11Texture2D* canvas, LONGLONG qpc) { frame(canvas, qpc); });
    thread_ = std::thread([this] { run(); });
    return true;
}

void Streamer::frame(ID3D11Texture2D* canvas, LONGLONG qpc) {
    std::lock_guard<std::mutex> busy(frameM_);
    if (stopping_ || !video_) return;
    const std::int64_t index = (std::int64_t)((double)(qpc - t0_) * s_.video.fps / (double)freq_ + 0.5);
    if (index <= lastFrame_) return;
    lastFrame_ = index;
    video_->submit(canvas, index * 1000000 / s_.video.fps);
}

void Streamer::stop() {
    if (!running_) return;
    stopping_ = true;
    if (engine_) engine_->setFrameSink("stream", nullptr);
    if (thread_.joinable()) thread_.join();
    if (tap_) mixer_->removeTap(tap_);
    tap_.reset();
    running_ = false;
    std::lock_guard<std::mutex> l(m_);
    st_.state = StreamStatus::Off;
}

StreamStatus Streamer::status() const {
    std::lock_guard<std::mutex> l(m_);
    return st_;
}

void Streamer::setState(StreamStatus::State s, const std::wstring& message) {
    std::lock_guard<std::mutex> l(m_);
    st_.state = s;
    st_.message = message;
}

void Streamer::run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    rtmp::Url url;
    rtmp::parseUrl(s_.url, url);
    media::Bytes avcC;                     // from the encoder's first keyframe
    std::deque<media::Packet> queue;       // encoded, waiting to be sent (in time order)
    media::Interleaver il(2, 300000);
    std::vector<float> block((size_t)1024 * audio::kChannels);
    bool anchored = false;
    int attempt = 0;
    ULONGLONG retryAt = 0;
    std::unique_ptr<net::Tcp> tcp;
    std::unique_ptr<net::Tls> tls;
    std::unique_ptr<rtmp::Client> client;
    bool needKeyframe = true;
    std::int64_t baseUs = -1;              // this connection's first packet time (its timestamps start at 0)
    double liveBefore = 0;                 // live seconds of earlier connections
    ULONGLONG liveSince = 0, rateAt = GetTickCount64();
    std::uint64_t rateBytes = 0, dropped = 0;

    auto disconnect = [&](bool polite) {
        if (client && polite) client->unpublish(s_.key);
        client.reset();
        tls.reset();
        tcp.reset();
        if (liveSince) liveBefore += (double)(GetTickCount64() - liveSince) / 1000.0;
        liveSince = 0;
    };
    auto scheduleRetry = [&](const std::wstring& why) {
        ++attempt;
        const int wait = std::min(60, 1 << std::min(attempt, 6));   // 2, 4, 8, 16, 32, 60 s
        retryAt = GetTickCount64() + (ULONGLONG)wait * 1000;
        std::lock_guard<std::mutex> l(m_);
        st_.state = StreamStatus::Reconnecting;
        st_.message = why;
        st_.attempt = attempt;
    };

    while (!stopping_) {
        Sleep(5);
        // Encoded sound and pictures, whatever the connection's doing.
        while (tap_->ring.available() >= 1024) {
            const std::size_t n = tap_->ring.read(block.data(), 1024);
            if (!anchored) {
                anchored = true;
                audio_->encode(block.data(), n, std::max<std::int64_t>(0, (std::int64_t)((double)(tap_->firstQpc.load() - t0_) * 1e6 / (double)freq_)));
            } else {
                audio_->encode(block.data(), n, 0);
            }
        }
        media::Packet p;
        while (audio_->take(p, 1)) il.push(std::move(p));
        while (video_->take(p)) {
            if (avcC.empty() && p.keyframe) {
                media::Bytes sps, pps;
                if (media::findParameterSets(p.data.data(), p.data.size(), sps, pps)) avcC = media::avcC(sps, pps);
            }
            p.data = media::annexBToLengthPrefixed(p.data.data(), p.data.size(), true);
            il.push(std::move(p));
        }
        while (il.pop(p)) queue.push_back(std::move(p));

        // Never let the backlog grow: keep at most ~3 s, dropping from the front up to a keyframe.
        if (!queue.empty() && queue.back().dts - queue.front().dts > 3000000) {
            std::size_t cut = queue.size();
            for (std::size_t i = queue.size(); i-- > 0;)
                if (queue[i].stream == 0 && queue[i].keyframe) {
                    cut = i;
                    break;
                }
            for (std::size_t i = 0; i < cut; ++i)
                if (queue[i].stream == 0) ++dropped;
            queue.erase(queue.begin(), queue.begin() + (long)cut);
        }

        const ULONGLONG now = GetTickCount64();
        // Connect (again).
        if (!client && now >= retryAt && !avcC.empty()) {
            setState(attempt ? StreamStatus::Reconnecting : StreamStatus::Connecting, L"Connecting to " + wide(s_.name) + L"…");
            std::string err;
            tcp = std::make_unique<net::Tcp>();
            rtmp::Transport* t = tcp.get();
            bool ok = tcp->open(url.host, url.port, 10000, err);
            if (ok && url.tls) {
                tls = std::make_unique<net::Tls>(*tcp);
                ok = tls->handshake(url.host, 10000, err);
                t = tls.get();
            }
            if (ok) {
                client = std::make_unique<rtmp::Client>(*t);
                ok = client->publish(url, s_.key, err);
            }
            if (ok)
                ok = client->metadata(s_.video.width, s_.video.height, s_.video.fps, s_.video.bitrateKbps, s_.audioKbps) &&
                     client->videoConfig(avcC) && client->audioConfig(audio_->config());
            if (!ok) {
                disconnect(false);
                const bool badKey = err.find("Publishing failed") != std::string::npos || err.find("refused") != std::string::npos;
                if (badKey && attempt == 0) {   // a wrong key or a closed channel: retrying won't help
                    setState(StreamStatus::Failed, wide(err));
                    break;
                }
                scheduleRetry(wide(err) + L" Trying again soon.");
                continue;
            }
            attempt = 0;
            needKeyframe = true;
            baseUs = -1;
            liveSince = GetTickCount64();
            setState(StreamStatus::Live, L"Live on " + wide(s_.name));
        }
        // Send.
        if (client) {
            bool ok = true;
            std::string err;
            while (ok && !queue.empty()) {
                media::Packet& q = queue.front();
                if (needKeyframe) {   // a viewer's player can only start at a keyframe
                    if (q.stream == 0 && q.keyframe) needKeyframe = false;
                    else {
                        queue.pop_front();
                        continue;
                    }
                }
                if (baseUs < 0) baseUs = q.dts;
                const std::uint32_t ms = (std::uint32_t)std::max<std::int64_t>(0, (q.dts - baseUs) / 1000);
                ok = q.stream == 0 ? client->video(ms, q.keyframe, q.data) : client->audio(ms, q.data);
                queue.pop_front();
            }
            if (ok) ok = client->service(err, 0);
            if (!ok) {
                disconnect(false);
                scheduleRetry((err.empty() ? std::wstring(L"The connection was lost.") : wide(err)) + L" Reconnecting.");
            }
        } else if (queue.size() > 2000) {
            queue.clear();   // not connected for a long time: nothing old is worth sending
        }
        // Status, once a second or so.
        if (now - rateAt >= 1000) {
            const std::uint64_t sent = client ? client->bytesSent() : 0;
            const double kbps = client ? (double)(sent - std::min(sent, rateBytes)) * 8.0 / (double)(now - rateAt) : 0;
            rateBytes = sent;
            rateAt = now;
            std::lock_guard<std::mutex> l(m_);
            st_.kbps = kbps;
            st_.droppedFrames = dropped + video_->dropped();
            st_.seconds = liveBefore + (liveSince ? (double)(GetTickCount64() - liveSince) / 1000.0 : 0);
            st_.retryIn = client ? 0 : (int)((retryAt > now ? retryAt - now : 0) / 1000);
        }
    }
    disconnect(true);
    if (engine_) engine_->setFrameSink("stream", nullptr);
    { std::lock_guard<std::mutex> wait(frameM_); }
    video_->close();
    video_.reset();
    audio_.reset();
    {
        std::lock_guard<std::mutex> l(m_);
        if (st_.state != StreamStatus::Failed) st_.state = StreamStatus::Off;
    }
    CoUninitialize();
}

}  // namespace ncap::output
