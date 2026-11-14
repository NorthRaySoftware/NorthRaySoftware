// Several destinations fed from one encoded stream at once (Capture's Destination class), in Wine:
// one stopped and another started part-way, one whose server only appears later (reconnects). tools/check-destinations.sh.
#include "output/destination.hpp"
#include "output/net.hpp"
#include "ncap/media.hpp"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <thread>
using namespace ncap;
static media::Bytes slurp(const char* p) { std::ifstream in(p, std::ios::binary); return media::Bytes(std::istreambuf_iterator<char>(in), {}); }
static const char* st(output::DestinationStatus::State s) {
    const char* n[] = {"off", "waiting", "connecting", "live", "reconnecting", "failed"}; return n[(int)s]; }
static std::string nar(const std::wstring& w) { std::string s; for (wchar_t c : w) s += c < 128 ? (char)c : '?'; return s; }
int main(int argc, char** argv) {
    net::startNetworking();
    const media::Bytes h264 = slurp(argv[1]), adts = slurp(argv[2]);
    const double fps = 30;
    std::vector<media::Bytes> aus;
    for (auto& [p, n] : media::nalUnits(h264.data(), h264.size())) {
        if (media::nalType(p) == 9 || aus.empty()) aus.emplace_back();
        const std::uint8_t sc[4] = {0, 0, 0, 1};
        aus.back().insert(aus.back().end(), sc, sc + 4);
        aus.back().insert(aus.back().end(), p, p + n);
    }
    media::Bytes sps, pps;
    media::findParameterSets(h264.data(), h264.size(), sps, pps);
    std::vector<media::Bytes> frames;
    for (std::size_t i = 0; i + 7 <= adts.size();) {
        const std::size_t len = ((adts[i + 3] & 3) << 11) | (adts[i + 4] << 3) | (adts[i + 5] >> 5);
        const std::size_t hdr = (adts[i + 1] & 1) ? 7 : 9;
        if (len < hdr || i + len > adts.size()) break;
        frames.emplace_back(adts.begin() + (long)(i + hdr), adts.begin() + (long)(i + len));
        i += len;
    }
    output::Destination::Settings base;
    base.key = "k"; base.width = 1280; base.height = 720; base.fps = 30; base.videoKbps = 2500; base.longestWaitSeconds = 2;
    output::Destination a, b, c, d;
    auto S = [&](const char* name, int port, const char* key = "k") { auto s = base; s.name = name; s.key = key; s.url = "rtmp://127.0.0.1:" + std::to_string(port) + "/live"; return s; };
    a.start(S("A", 19371)); b.start(S("B", 19372)); d.start(S("D (late server)", 19379));

    for (auto* x : {&a, &b, &d}) x->configure(media::avcC(sps, pps), media::aacConfig(48000, 2));
    std::size_t vi = 0, ai = 0;
    const auto start = std::chrono::steady_clock::now();
    bool bStopped = false, cStarted = false;
    std::uint64_t sentVideo = 0;
    while (vi < aus.size() || ai < frames.size()) {
        const double vt = vi < aus.size() ? vi * 1000.0 / fps : 1e18;
        const double at = ai < frames.size() ? ai * 1024 * 1000.0 / 48000 : 1e18;
        std::this_thread::sleep_until(start + std::chrono::microseconds((long long)(std::min(vt, at) * 500)));   // 2x real time
        media::Packet p;
        if (vt <= at) {
            p.stream = 0; p.keyframe = media::containsKeyframe(aus[vi].data(), aus[vi].size());
            p.data = media::annexBToLengthPrefixed(aus[vi].data(), aus[vi].size(), true);
            p.dts = p.pts = (std::int64_t)(vt * 1000); ++vi; ++sentVideo;
        } else {
            p.stream = 1; p.keyframe = true; p.data = frames[ai]; p.dts = p.pts = (std::int64_t)(at * 1000); ++ai;
        }
        for (auto* x : {&a, &b, &c, &d}) if (x->active()) x->push(p);
        if (!bStopped && vi == aus.size() * 4 / 10) { b.stop(); bStopped = true; std::printf("B stopped at frame %zu (A still %s)\n", vi, st(a.status().state)); }
        if (!cStarted && vi == aus.size() * 6 / 10) {
            c.start(S("C (added later)", 19373)); c.configure(media::avcC(sps, pps), media::aacConfig(48000, 2)); cStarted = true;
            std::printf("C started at frame %zu\n", vi);
        }
        if (vi % 150 == 0 && vt <= at) {
            for (auto* x : {&a, &b, &c, &d}) {
                auto s = x->status();
                std::printf("  t=%4.1fs %-16s %-12s %6.0f kbps live %4.1fs dropped %llu attempts %d reconnects %d  %s\n", vt / 1000, x->settings().name.c_str(), st(s.state), s.kbps, s.seconds,
                            (unsigned long long)s.droppedFrames, s.attempts, s.reconnects, nar(s.message).c_str());
            }
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    for (auto* x : {&a, &b, &c, &d}) {
        auto s = x->status();
        std::printf("end %-16s %-12s sent %llu bytes, connected in %d ms, dropped %llu, reconnects %d: %s\n", x->settings().name.c_str(), st(s.state),
                    (unsigned long long)s.bytesSent, s.connectMs, (unsigned long long)s.droppedFrames, s.reconnects, nar(s.message).c_str());
        x->stop();
    }
    std::printf("video frames fed: %llu\n", (unsigned long long)sentVideo);
}
