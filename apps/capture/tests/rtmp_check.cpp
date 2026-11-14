// Streams an H.264 stream (with access unit delimiters) and an AAC stream to an RTMP server through
// NorthRay Capture's own RTMP client, in real time, for checking against a real server
// (tools/check-rtmp.sh runs ffmpeg as one).
//   capture_rtmp_check rtmp://127.0.0.1:19350/live key video.h264 fps audio.aac
#include "ncap/media.hpp"
#include "ncap/rtmp.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <thread>

using namespace ncap;

class Socket : public rtmp::Transport {
public:
    bool open(const std::string& host, int port) {
        addrinfo hints{}, *res = nullptr;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) return false;
        fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        const bool ok = fd >= 0 && ::connect(fd, res->ai_addr, res->ai_addrlen) == 0;
        freeaddrinfo(res);
        return ok;
    }
    ~Socket() override { if (fd >= 0) close(fd); }
    bool send(const std::uint8_t* d, std::size_t n) override {
        while (n) {
            const ssize_t w = ::send(fd, d, n, MSG_NOSIGNAL);
            if (w <= 0) return false;
            d += w;
            n -= (std::size_t)w;
        }
        return true;
    }
    int receive(std::uint8_t* d, std::size_t n, int timeoutMs) override {
        pollfd p{fd, POLLIN, 0};
        const int r = poll(&p, 1, timeoutMs);
        if (r == 0) return 0;
        if (r < 0) return -1;
        const ssize_t got = recv(fd, d, n, 0);
        return got > 0 ? (int)got : -1;
    }
    int fd = -1;
};

static media::Bytes slurp(const char* p) {
    std::ifstream in(p, std::ios::binary);
    return media::Bytes(std::istreambuf_iterator<char>(in), {});
}

int main(int argc, char** argv) {
    if (argc < 6) return 2;
    rtmp::Url url;
    if (!rtmp::parseUrl(argv[1], url)) return 3;
    Socket s;
    if (!s.open(url.host, url.port)) {
        std::printf("connect failed\n");
        return 4;
    }
    rtmp::Client c(s);
    std::string err;
    if (!c.publish(url, argv[2], err)) {
        std::printf("publish failed: %s\n", err.c_str());
        return 5;
    }
    const media::Bytes h264 = slurp(argv[3]);
    const double fps = std::atof(argv[4]);
    std::vector<media::Bytes> aus;
    for (auto& [p, n] : media::nalUnits(h264.data(), h264.size())) {
        if (media::nalType(p) == 9 || aus.empty()) aus.emplace_back();
        const std::uint8_t sc[4] = {0, 0, 0, 1};
        aus.back().insert(aus.back().end(), sc, sc + 4);
        aus.back().insert(aus.back().end(), p, p + n);
    }
    media::Bytes sps, pps;
    media::findParameterSets(h264.data(), h264.size(), sps, pps);
    const media::Bytes adts = slurp(argv[5]);
    std::vector<media::Bytes> frames;
    for (std::size_t i = 0; i + 7 <= adts.size();) {
        const std::size_t len = ((adts[i + 3] & 3) << 11) | (adts[i + 4] << 3) | (adts[i + 5] >> 5);
        const std::size_t hdr = (adts[i + 1] & 1) ? 7 : 9;
        if (len < hdr || i + len > adts.size()) break;
        frames.emplace_back(adts.begin() + (long)(i + hdr), adts.begin() + (long)(i + len));
        i += len;
    }
    c.metadata(1280, 720, fps, 2500, 160);
    c.videoConfig(media::avcC(sps, pps));
    c.audioConfig(media::aacConfig(48000, 2));
    // Interleave by time, paced roughly in real time (4x faster to keep the check short).
    std::size_t vi = 0, ai = 0;
    const auto start = std::chrono::steady_clock::now();
    while (vi < aus.size() || ai < frames.size()) {
        const double vt = vi < aus.size() ? vi * 1000.0 / fps : 1e18;
        const double at = ai < frames.size() ? ai * 1024 * 1000.0 / 48000 : 1e18;
        const double t = std::min(vt, at);
        std::this_thread::sleep_until(start + std::chrono::microseconds((long long)(t * 250)));
        bool ok;
        if (vt <= at) {
            ok = c.video((std::uint32_t)vt, media::containsKeyframe(aus[vi].data(), aus[vi].size()),
                         media::annexBToLengthPrefixed(aus[vi].data(), aus[vi].size(), true));
            ++vi;
        } else {
            ok = c.audio((std::uint32_t)at, frames[ai]);
            ++ai;
        }
        if (!ok || !c.service(err, 0)) {
            std::printf("stream failed: %s\n", err.c_str());
            return 6;
        }
    }
    c.unpublish(argv[2]);
    std::printf("sent %zu video and %zu audio frames, %llu bytes\n", vi, ai, (unsigned long long)c.bytesSent());
    return 0;
}
