// NorthRay Capture: the setup wizard's sums (portable, unit-tested): from what you'll do (stream,
// record or both), this PC (a hardware H.264 encoder or not, its processor threads) and your
// connection (a Twitch bandwidth test, or a speed you type) to a starting point: canvas size, frame
// rate, the stream's video bitrate and the recording's. Written as reasons in plain words too, so the
// wizard can say why. It's a starting point; everything stays changeable in Settings.
#pragma once

#include <string>
#include <vector>

namespace ncap::wizard {

enum class Goal { Stream, Record, Both };

struct Machine {
    bool hardwareEncoder = false;   // NVENC, AMF, Quick Sync (found by the encoder probe)
    int threads = 4;                // logical processors
};

// A bandwidth test: the send rate each second while streaming at `targetKbps` to Twitch with
// ?bandwidthtest=true (Twitch takes it but doesn't show it), and frames dropped because the
// connection couldn't keep up.
struct TestResult {
    int targetKbps = 0;
    std::vector<double> kbpsPerSecond;
    unsigned long long droppedFrames = 0;
    bool connected = false;
};
// What the connection can keep up with: the steady send rate (the first seconds are skipped while it
// settles), lower if frames were dropped. 0 if the test didn't connect or ran too briefly.
int sustainedKbps(const TestResult& t);

struct Plan {
    int width = 1920, height = 1080, fps = 60;
    int streamKbps = 0;     // 0: not streaming
    int recordKbps = 0;     // 0: not recording
    std::vector<std::string> reasons;
};
// `uploadKbps`: what the connection keeps up with (sustainedKbps, or typed in), 0 if unknown.
// `platformMaxKbps`: the platform's ceiling for video (Twitch: 6000).
Plan recommend(Goal goal, const Machine& m, int uploadKbps, int platformMaxKbps = 6000);

}  // namespace ncap::wizard
