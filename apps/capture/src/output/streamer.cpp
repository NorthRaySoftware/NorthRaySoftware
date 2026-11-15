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
    if (d.bus < 0 || d.bus > 6) {
        error = L"That audio mix isn't one a stream can carry (the Stream bus or Tracks 1-6).";
        return false;
    }
    std::lock_guard<std::mutex> l(m_);
    if (auto it = dests_.find(d.id); it != dests_.end()) {
        if (it->second.dest->active()) return true;
        const Entry old = it->second;
        dests_.erase(it);   // a failed one being started again
        dropMixIfUnused(old.kbps, old.bus);
    }
    engine_ = &engine;
    mixer_ = &mixer;
    // The video encode for this bitrate: shared if one is running, else a new one.
    Path* path = nullptr;
    bool fresh = false;
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
        LARGE_INTEGER f, q;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&q);
        p->freq = f.QuadPart;
        p->t0 = q.QuadPart;
        p->sink = "stream:" + std::to_string(d.videoKbps);
        path = p.get();
        paths_[d.videoKbps] = std::move(p);
        fresh = true;
    }
    // The audio mix for this bus within it: shared if another destination carries it already.
    std::shared_ptr<Mix> mix;
    {
        std::lock_guard<std::mutex> pl(path->m);
        if (auto it = path->mixes.find(d.bus); it != path->mixes.end()) mix = it->second;
    }
    if (!mix) {
        mix = std::make_shared<Mix>();
        mix->bus = d.bus;
        mix->audio = std::make_unique<encode::AudioEncoder>();
        if (!mix->audio->open(c.audioKbps, error)) {
            if (fresh) {
                path->video->close();
                paths_.erase(d.videoKbps);
            }
            return false;
        }
        mix->il = std::make_unique<media::Interleaver>(2, 300000);
        mix->tap = std::make_shared<audio::Tap>(d.bus);
        mixer.addTap(mix->tap);
        std::lock_guard<std::mutex> pl(path->m);
        path->mixes[d.bus] = mix;
    }
    if (fresh) {
        Path* raw = path;
        path->thread = std::thread([raw] { raw->run(); });
        engine.setFrameSink(path->sink, [raw](ID3D11Texture2D* canvas, LONGLONG qpc) { raw->frame(canvas, qpc); });
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
        if (!path->avcC.empty()) dest->configure(path->avcC, mix->audio->config());
        mix->dests.push_back(dest);
    }
    dests_[d.id] = {d.videoKbps, d.bus, dest};
    return true;
}

void Streamer::dropMixIfUnused(int kbps, int bus) {   // m_ held
    for (auto& [_, e] : dests_)
        if (e.kbps == kbps && e.bus == bus) return;
    auto p = paths_.find(kbps);
    if (p == paths_.end()) return;
    std::shared_ptr<Mix> mix;
    {
        std::lock_guard<std::mutex> pl(p->second->m);
        auto it = p->second->mixes.find(bus);
        if (it == p->second->mixes.end()) return;
        mix = it->second;
        p->second->mixes.erase(it);
    }
    if (mixer_ && mix->tap) mixer_->removeTap(mix->tap);
}

void Streamer::stop(const std::string& id) {
    Entry e;
    {
        std::lock_guard<std::mutex> l(m_);
        auto it = dests_.find(id);
        if (it == dests_.end()) return;
        e = it->second;
        dests_.erase(it);
        if (auto p = paths_.find(e.kbps); p != paths_.end()) {
            std::lock_guard<std::mutex> pl(p->second->m);
            if (auto m = p->second->mixes.find(e.bus); m != p->second->mixes.end()) {
                auto& v = m->second->dests;
                v.erase(std::remove(v.begin(), v.end(), e.dest), v.end());
            }
        }
    }
    e.dest->stop();
    std::lock_guard<std::mutex> l(m_);
    bool used = false;
    for (auto& [_, o] : dests_)
        if (o.kbps == e.kbps) used = true;
    if (!used) stopPath(e.kbps);
    else dropMixIfUnused(e.kbps, e.bus);
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
    for (auto& [_, mix] : p.mixes)
        if (mixer_ && mix->tap) mixer_->removeTap(mix->tap);
    p.video->close();
    paths_.erase(it);
}

bool Streamer::active() const {
    std::lock_guard<std::mutex> l(m_);
    for (auto& [_, e] : dests_)
        if (e.dest->active()) return true;
    return false;
}

bool Streamer::active(const std::string& id) const {
    std::lock_guard<std::mutex> l(m_);
    auto it = dests_.find(id);
    return it != dests_.end() && it->second.dest->active();
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
    return it == dests_.end() ? DestinationStatus{} : it->second.dest->status();
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
            if (d.kbps == kbps && d.dest->active()) ++e.destinations;
        {
            std::lock_guard<std::mutex> pl(p->m);
            for (auto& [bus, _] : p->mixes) e.buses.push_back(bus);
        }
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
            if (d.kbps == kbps && d.dest->active()) used = true;
        if (!used) idle.push_back(kbps);
    }
    for (int k : idle) {
        for (auto& [_, d] : dests_)
            if (d.kbps == k) d.dest->stop();   // joins the finished thread
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
    std::vector<float> block((size_t)1024 * audio::kChannels);
    while (!stopping) {
        Sleep(5);
        std::vector<std::shared_ptr<Mix>> now;
        {
            std::lock_guard<std::mutex> l(m);
            for (auto& [_, mix] : mixes) now.push_back(mix);
        }
        // Each mix's sound, encoded on its own.
        media::Packet p;
        for (auto& mix : now) {
            while (mix->tap->ring.available() >= 1024) {
                const std::size_t n = mix->tap->ring.read(block.data(), 1024);
                if (!mix->anchored) {
                    mix->anchored = true;
                    mix->audio->encode(block.data(), n,
                                       std::max<std::int64_t>(0, (std::int64_t)((double)(mix->tap->firstQpc.load() - t0) * 1e6 / (double)freq)));
                } else {
                    mix->audio->encode(block.data(), n, 0);
                }
            }
            while (mix->audio->take(p, 1)) mix->il->push(std::move(p));
        }
        // The one video, to every mix.
        while (video->take(p)) {
            if (p.keyframe) {
                std::lock_guard<std::mutex> l(m);
                if (avcC.empty()) {
                    media::Bytes sps, pps;
                    if (media::findParameterSets(p.data.data(), p.data.size(), sps, pps)) {
                        avcC = media::avcC(sps, pps);
                        for (auto& [_, mix] : mixes)
                            for (auto& d : mix->dests) d->configure(avcC, mix->audio->config());
                    }
                }
            }
            p.data = media::annexBToLengthPrefixed(p.data.data(), p.data.size(), true);
            for (auto& mix : now) mix->il->push(p);
        }
        for (auto& mix : now) {
            std::vector<std::shared_ptr<Destination>> to;
            {
                std::lock_guard<std::mutex> l(m);
                to = mix->dests;
            }
            while (mix->il->pop(p))
                for (auto& d : to)
                    if (d->active()) d->push(p);
        }
    }
    CoUninitialize();
}

}  // namespace ncap::output
