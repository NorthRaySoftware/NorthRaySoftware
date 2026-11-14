#include "output/streamer.hpp"

#include "ncap/rtmp.hpp"

#include <objbase.h>

#include <algorithm>

namespace ncap::output {

bool Streamer::start(video::Engine& engine, audio::Mixer& mixer, const StreamCommon& c, const DestinationStart& d, std::wstring& error) {
    rtmp::Url url;
    if (d.url.empty()) {
        error = L"There's no server address yet. Enter it in Settings › Streaming.";
        return false;
    }
    if (!rtmp::parseUrl(d.url, url)) {
        error = L"The server address isn't an RTMP address (it should start with rtmp:// or rtmps://).";
        return false;
    }
    if (d.key.empty()) {
        error = L"There's no stream key. Enter it in Settings › Streaming.";
        return false;
    }
    if (!engine.running()) {
        error = L"Video capture isn't running, so there's nothing to stream.";
        return false;
    }
    std::lock_guard<std::mutex> l(m_);
    if (auto it = dests_.find(d.id); it != dests_.end()) {
        if (it->second.second->active()) return true;
        dests_.erase(it);   // a failed one being started again
    }
    engine_ = &engine;
    mixer_ = &mixer;
    // The encode for this bitrate: shared if one is running, else a new one.
    Path* path = nullptr;
    if (auto it = paths_.find(d.videoKbps); it != paths_.end()) path = it->second.get();
    else {
        auto p = std::make_unique<Path>();
        p->kbps = d.videoKbps;
        p->fps = c.fps;
        encode::VideoSettings vs;
        vs.width = c.width;
        vs.height = c.height;
        vs.fps = c.fps;
        vs.bitrateKbps = d.videoKbps;
        vs.encoder = c.encoder;
        vs.constantBitrate = true;   // platforms want CBR
        p->video = std::make_unique<encode::VideoEncoder>();
        if (!p->video->open(engine.gpu(), vs, error)) return false;
        p->audio = std::make_unique<encode::AudioEncoder>();
        if (!p->audio->open(c.audioKbps, error)) {
            p->video->close();
            return false;
        }
        LARGE_INTEGER f, q;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&q);
        p->freq = f.QuadPart;
        p->t0 = q.QuadPart;
        p->tap = std::make_shared<audio::Tap>(0);   // the Stream bus
        mixer.addTap(p->tap);
        p->sink = "stream:" + std::to_string(d.videoKbps);
        Path* raw = p.get();
        p->thread = std::thread([raw] { raw->run(); });
        engine.setFrameSink(p->sink, [raw](ID3D11Texture2D* canvas, LONGLONG qpc) { raw->frame(canvas, qpc); });
        path = raw;
        paths_[d.videoKbps] = std::move(p);
    }
    auto dest = std::make_shared<Destination>();
    Destination::Settings ds;
    ds.name = d.name;
    ds.url = d.url;
    ds.key = d.key;
    ds.width = c.width;
    ds.height = c.height;
    ds.fps = c.fps;
    ds.videoKbps = d.videoKbps;
    ds.audioKbps = c.audioKbps;
    ds.longestWaitSeconds = c.longestWaitSeconds;
    dest->start(ds);
    {
        std::lock_guard<std::mutex> pl(path->m);
        if (!path->avcC.empty()) dest->configure(path->avcC, path->aac);
        path->dests.push_back(dest);
    }
    dests_[d.id] = {d.videoKbps, dest};
    return true;
}

void Streamer::stop(const std::string& id) {
    std::shared_ptr<Destination> d;
    int kbps = 0;
    {
        std::lock_guard<std::mutex> l(m_);
        auto it = dests_.find(id);
        if (it == dests_.end()) return;
        kbps = it->second.first;
        d = it->second.second;
        dests_.erase(it);
        if (auto p = paths_.find(kbps); p != paths_.end()) {
            std::lock_guard<std::mutex> pl(p->second->m);
            auto& v = p->second->dests;
            v.erase(std::remove(v.begin(), v.end(), d), v.end());
        }
    }
    d->stop();
    std::lock_guard<std::mutex> l(m_);
    bool used = false;
    for (auto& [_, e] : dests_)
        if (e.first == kbps) used = true;
    if (!used) stopPath(kbps);
}

void Streamer::stop() {
    std::vector<std::string> all = ids();
    for (auto& id : all) stop(id);
    std::lock_guard<std::mutex> l(m_);
    while (!paths_.empty()) stopPath(paths_.begin()->first);
}

void Streamer::stopPath(int kbps) {   // m_ held
    auto it = paths_.find(kbps);
    if (it == paths_.end()) return;
    Path& p = *it->second;
    if (engine_) engine_->setFrameSink(p.sink, nullptr);
    p.stopping = true;
    { std::lock_guard<std::mutex> wait(p.frameM); }
    if (p.thread.joinable()) p.thread.join();
    if (mixer_ && p.tap) mixer_->removeTap(p.tap);
    p.video->close();
    paths_.erase(it);
}

bool Streamer::active() const {
    std::lock_guard<std::mutex> l(m_);
    for (auto& [_, e] : dests_)
        if (e.second->active()) return true;
    return false;
}

bool Streamer::active(const std::string& id) const {
    std::lock_guard<std::mutex> l(m_);
    auto it = dests_.find(id);
    return it != dests_.end() && it->second.second->active();
}

bool Streamer::known(const std::string& id) const {
    std::lock_guard<std::mutex> l(m_);
    return dests_.count(id) > 0;
}

void Streamer::clear(const std::string& id) {
    if (!active(id)) stop(id);
}

DestinationStatus Streamer::status(const std::string& id) const {
    std::lock_guard<std::mutex> l(m_);
    auto it = dests_.find(id);
    return it == dests_.end() ? DestinationStatus{} : it->second.second->status();
}

std::vector<std::string> Streamer::ids() const {
    std::lock_guard<std::mutex> l(m_);
    std::vector<std::string> v;
    for (auto& [id, _] : dests_) v.push_back(id);
    return v;
}

std::vector<EncodeStatus> Streamer::encodes() const {
    std::lock_guard<std::mutex> l(m_);
    std::vector<EncodeStatus> v;
    for (auto& [kbps, p] : paths_) {
        EncodeStatus e;
        e.videoKbps = kbps;
        e.encoder = p->video->name();
        e.hardware = p->video->hardware();
        e.encoderDropped = p->video->dropped();
        for (auto& [_, d] : dests_)
            if (d.first == kbps && d.second->active()) ++e.destinations;
        v.push_back(e);
    }
    return v;
}

void Streamer::tick() {
    // A destination that failed (a refused key) stops by itself; its encode isn't needed once no
    // running destination uses it.
    std::lock_guard<std::mutex> l(m_);
    std::vector<int> idle;
    for (auto& [kbps, p] : paths_) {
        bool used = false;
        for (auto& [_, d] : dests_)
            if (d.first == kbps && d.second->active()) used = true;
        if (!used) idle.push_back(kbps);
    }
    for (int k : idle) {
        for (auto& [_, d] : dests_)
            if (d.first == k) d.second->stop();   // joins the finished thread
        stopPath(k);
    }
}

// --- one encode ---

void Streamer::Path::frame(ID3D11Texture2D* canvas, LONGLONG qpc) {
    std::lock_guard<std::mutex> busy(frameM);
    if (stopping || !video) return;
    const std::int64_t index = (std::int64_t)((double)(qpc - t0) * fps / (double)freq + 0.5);
    if (index <= lastFrame) return;
    lastFrame = index;
    video->submit(canvas, index * 1000000 / fps);
}

void Streamer::Path::run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    media::Interleaver il(2, 300000);
    std::vector<float> block((size_t)1024 * audio::kChannels);
    bool anchored = false;
    while (!stopping) {
        Sleep(5);
        while (tap->ring.available() >= 1024) {
            const std::size_t n = tap->ring.read(block.data(), 1024);
            if (!anchored) {
                anchored = true;
                audio->encode(block.data(), n, std::max<std::int64_t>(0, (std::int64_t)((double)(tap->firstQpc.load() - t0) * 1e6 / (double)freq)));
            } else {
                audio->encode(block.data(), n, 0);
            }
        }
        media::Packet p;
        while (audio->take(p, 1)) il.push(std::move(p));
        while (video->take(p)) {
            if (p.keyframe) {
                bool first = false;
                {
                    std::lock_guard<std::mutex> l(m);
                    if (avcC.empty()) {
                        media::Bytes sps, pps;
                        if (media::findParameterSets(p.data.data(), p.data.size(), sps, pps)) {
                            avcC = media::avcC(sps, pps);
                            aac = audio->config();
                            first = true;
                        }
                    }
                }
                if (first) {
                    std::lock_guard<std::mutex> l(m);
                    for (auto& d : dests) d->configure(avcC, aac);
                }
            }
            p.data = media::annexBToLengthPrefixed(p.data.data(), p.data.size(), true);
            il.push(std::move(p));
        }
        std::vector<std::shared_ptr<Destination>> now;
        {
            std::lock_guard<std::mutex> l(m);
            now = dests;
        }
        while (il.pop(p))
            for (auto& d : now)
                if (d->active()) d->push(p);
    }
    CoUninitialize();
}

}  // namespace ncap::output
