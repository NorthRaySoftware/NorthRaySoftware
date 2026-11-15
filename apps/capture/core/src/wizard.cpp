#include "ncap/wizard.hpp"

#include <algorithm>
#include <cmath>

namespace ncap::wizard {

int sustainedKbps(const TestResult& t) {
    if (!t.connected || t.kbpsPerSecond.size() < 6) return 0;
    // Skip the first 3 seconds (connecting, the encoder settling); take the median of the rest, so one
    // slow or bursty second doesn't decide it.
    std::vector<double> v(t.kbpsPerSecond.begin() + 3, t.kbpsPerSecond.end());
    std::sort(v.begin(), v.end());
    const double median = v[v.size() / 2];
    double k = median;
    // Dropped frames mean it couldn't keep up with even that: take off a share for each one dropped
    // per second of test (at least 15% if any were dropped).
    if (t.droppedFrames > 0) {
        const double perSec = (double)t.droppedFrames / (double)t.kbpsPerSecond.size();
        k *= std::clamp(0.85 - 0.02 * perSec, 0.4, 0.85);
    }
    return (int)std::lround(k);
}

Plan recommend(Goal goal, const Machine& m, int upload, int platformMax) {
    Plan p;
    const bool streams = goal != Goal::Record, records = goal != Goal::Stream;
    // What the PC can encode comfortably: a hardware encoder does 1080p60 easily; without one the
    // processor does the work, so keep it lighter.
    const bool strongCpu = m.threads >= 12;
    int maxW = 1920, maxH = 1080, maxFps = 60;
    if (!m.hardwareEncoder) {
        if (strongCpu) {
            maxFps = 30;
            p.reasons.push_back("No hardware encoder was found, so your processor encodes the video: 1080p at 30 fps keeps it comfortable.");
        } else {
            maxW = 1280, maxH = 720, maxFps = 30;
            p.reasons.push_back("No hardware encoder was found and the processor has " + std::to_string(m.threads) +
                                " threads, so 720p at 30 fps keeps games and Capture running smoothly.");
        }
    } else {
        p.reasons.push_back("Your graphics card can encode video itself, so 1080p at 60 fps is easy for it.");
    }
    p.width = maxW, p.height = maxH, p.fps = maxFps;
    if (streams) {
        if (upload <= 0) {
            // Unknown connection: a safe middle that most home connections manage.
            p.streamKbps = std::min(platformMax, 4500);
            p.width = std::min(p.width, 1280), p.height = std::min(p.height, 720);
            p.reasons.push_back("Without a connection test, 720p at 4,500 kbps is a safe start. Run the test to aim higher.");
        } else {
            // Leave a quarter of the connection free for audio, chat, games and anything else at home.
            const int usable = (int)(upload * 0.75);
            p.streamKbps = std::max(500, std::min(platformMax, usable - 160));
            p.reasons.push_back("Your connection can send about " + std::to_string(upload) + " kbps; the stream uses " +
                                std::to_string(p.streamKbps) + " kbps of it, leaving room for the rest of your home.");
            // The picture that bitrate carries well (Twitch's own guidance, roughly).
            if (p.streamKbps < 2500) {
                p.width = std::min(p.width, 1280), p.height = std::min(p.height, 720), p.fps = std::min(p.fps, 30);
                p.reasons.push_back("That's a low bitrate: at 720p and 30 fps calm scenes look fine, but fast games will look blocky. A faster "
                                    "connection (or a cable instead of Wi-Fi) helps most.");
            } else if (p.streamKbps < 4500) {
                p.width = std::min(p.width, 1280), p.height = std::min(p.height, 720), p.fps = std::min(p.fps, 30);
                p.reasons.push_back("At that bitrate, 720p at 30 fps looks best.");
            } else if (p.streamKbps < 6000) {
                p.width = std::min(p.width, 1280), p.height = std::min(p.height, 720);
                p.reasons.push_back(p.fps == 60 ? "At that bitrate, 720p at 60 fps looks best." : "At that bitrate, 720p looks best.");
            }
        }
        if (p.streamKbps >= platformMax && upload > 0)
            p.reasons.push_back("Twitch takes up to " + std::to_string(platformMax) + " kbps of video, so it's capped there.");
    }
    if (records) {
        // Recordings aren't limited by the connection: plenty for editing later.
        const double pixelsPerSec = (double)p.width * p.height * p.fps;
        p.recordKbps = (int)std::lround(std::clamp(pixelsPerSec / (1920.0 * 1080.0 * 60.0) * 40000.0, 8000.0, 40000.0) / 1000.0) * 1000;
        if (goal == Goal::Record && m.hardwareEncoder) p.recordKbps = 50000;
        p.reasons.push_back("Recordings use " + std::to_string(p.recordKbps) + " kbps: high enough to edit without the picture breaking up.");
    }
    return p;
}

}  // namespace ncap::wizard
