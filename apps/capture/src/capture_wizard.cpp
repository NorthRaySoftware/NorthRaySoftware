// NorthRay Capture's window: the setup wizard (Settings > Setup wizard...). Three steps: what you'll
// do; how fast your connection is (a Twitch bandwidth test, a speed you type, or skip); and a starting
// point with the reasons for it (core/wizard), applied with one button and changeable in Settings
// afterwards.
//
// The bandwidth test streams the program to your Twitch destination for 20 seconds with
// "?bandwidthtest=true" after the key, which Twitch documents for exactly this: it takes the stream but
// never shows it to anyone. It runs at 8,000 kbps and measures what actually got through.
#include "capture_window.hpp"

#include "log.hpp"
#include "output/credentials.hpp"

#include "ncap/wizard.hpp"

#include <mfapi.h>

#include <algorithm>
#include <thread>

namespace ncap {

using cjui::Align;
using cjui::Font;
using cjui::narrow;
using cjui::Rect;
using cjui::widen;

namespace {
constexpr const char* kTestId = "__bandwidth-test";
constexpr int kTestKbps = 8000, kTestSeconds = 20;
}  // namespace

void CaptureWindow::openWizard() {
    openSettings(true);
    wizardOpen_ = true;
    wizStep_ = 0;
    wizUpload_ = 0;
    wizTest_ = {};
    invalidate();
}

// The Twitch destination with a stream key (the test needs one), or -1.
int CaptureWindow::wizardTwitch() const {
    for (std::size_t i = 0; i < settings_.destinations.size(); ++i)
        if (settings_.destinations[i].service == "twitch" && secrets::exists("stream/" + settings_.destinations[i].id)) return (int)i;
    return -1;
}

void CaptureWindow::startBandwidthTest() {
    const int i = wizardTwitch();
    if (i < 0 || streamer_.active() || recorder_.active()) return;
    const auto& d = settings_.destinations[(size_t)i];
    if (!mfForStream_) {
        MFStartup(MF_VERSION, MFSTARTUP_LITE);
        mfForStream_ = true;
    }
    output::StreamCommon c;
    c.width = scenes_.canvasWidth;
    c.height = scenes_.canvasHeight;
    c.fps = settings_.outputFps;
    c.encoder = settings_.recordEncoder;
    c.longestWaitSeconds = 5;
    output::DestinationStart ds;
    ds.id = kTestId;
    ds.name = "Twitch (bandwidth test)";
    ds.url = d.url;
    const std::string key = secrets::load("stream/" + d.id);
    ds.key = key + (key.find('?') == std::string::npos ? "?" : "&") + "bandwidthtest=true";
    log::addSecret(key);
    log::addSecret(ds.key);
    ds.videoKbps = kTestKbps;
    std::wstring error;
    wizTest_ = {};
    wizTest_.targetKbps = kTestKbps;
    if (!streamer_.start(engine_, mixer_, c, ds, error)) {
        flash(L"The test couldn't start: " + error, 8);
        return;
    }
    wizTesting_ = true;
    wizTestStart_ = GetTickCount64();
    log::info("Bandwidth test started (Twitch, bandwidthtest=true, 8000 kbps)");
}

// Each second while testing (from the stats timer).
void CaptureWindow::wizardTick() {
    if (!wizTesting_) return;
    const output::DestinationStatus st = streamer_.status(kTestId);
    if (st.state == output::DestinationStatus::Live) {
        wizTest_.connected = true;
        wizTest_.kbpsPerSecond.push_back(st.kbps);
        wizTest_.droppedFrames = st.droppedFrames;
    }
    const double secs = (double)(GetTickCount64() - wizTestStart_) / 1000.0;
    const bool failed = st.state == output::DestinationStatus::Failed || (!wizTest_.connected && secs > 15);
    if (wizTest_.kbpsPerSecond.size() >= (size_t)kTestSeconds || failed) {
        streamer_.stop(kTestId);
        streamer_.clear(kTestId);
        wizTesting_ = false;
        wizUpload_ = wizard::sustainedKbps(wizTest_);
        if (failed && !wizTest_.connected) wizTestError_ = st.message.empty() ? L"It couldn't connect to Twitch." : st.message;
        else wizTestError_.clear();
        log::info("Bandwidth test finished: " + std::to_string(wizUpload_) + " kbps sustained, " + std::to_string(wizTest_.droppedFrames) + " dropped");
    }
    invalidate();
}

void CaptureWindow::drawWizard(const Rect& area) {
    gfx_.fill(area, pal_.paper);
    const float colW = std::min(720.0f, area.w - 48);
    const float x = area.x + (area.w - colW) / 2;
    const float top = beginScroll(area);
    float y = top + 24;
    gfx_.text(L"Setup wizard", {x, y, colW - 130, 34}, Font::Title, pal_.ink, {Align::Left, false});
    button("wiz:close", {x + colW - 110, y + 2, 110, 34}, L"Close", [this] {
        if (wizTesting_) {
            streamer_.stop(kTestId);
            streamer_.clear(kTestId);
            wizTesting_ = false;
        }
        wizardOpen_ = false;
        invalidate();
    });
    y += 44;
    const wchar_t* steps[] = {L"1  What for", L"2  Your connection", L"3  Your settings"};
    for (int i = 0; i < 3; ++i)
        gfx_.text(steps[i], {x + i * colW / 3, y, colW / 3, 20}, i == wizStep_ ? Font::BodyStrong : Font::Small, i == wizStep_ ? pal_.ribbon : pal_.inkFaint,
                  {Align::Left, false, true});
    y += 36;
    auto para = [&](const std::wstring& t, cjui::Color c) { y += gfx_.text(t, {x, y, colW, 400}, Font::Body, c, {Align::Left, true}) + 12; };
    const bool streams = wizGoal_ != (int)wizard::Goal::Record;

    if (wizStep_ == 0) {
        para(L"What will you use Capture for? This decides what the wizard checks and suggests. You can change everything later in Settings.", pal_.inkMuted);
        segmented("wiz:goal", {x, y, colW, 36}, {L"Streaming", L"Recording", L"Both"}, wizGoal_, [this](int i) { wizGoal_ = i; });
        y += 56;
        int hw = 0;
        for (auto& e : encoders_) hw += e.hardware && e.codec == "H.264";
        const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
        para(encodersKnown_ ? (hw ? L"This PC has a hardware video encoder, so encoding barely touches the processor."
                                  : L"No hardware video encoder was found: the processor will encode, so the suggestions stay lighter.")
                            : L"Still checking this PC's video encoders…",
             pal_.ink);
        para(L"Processor: " + std::to_wstring(threads) + L" threads.", pal_.inkMuted);
        button("wiz:next0", {x + colW - 160, y + 8, 160, 36}, L"Next", [this] {
            wizStep_ = wizGoal_ == (int)wizard::Goal::Record ? 2 : 1;
            invalidate();
        }, true);
        y += 60;
    } else if (wizStep_ == 1) {
        para(L"How much can your connection send? Streams need a steady upload; too high a bitrate and viewers see buffering.", pal_.inkMuted);
        const int tw = wizardTwitch();
        if (wizTesting_) {
            const double secs = (double)(GetTickCount64() - wizTestStart_) / 1000.0;
            const auto& k = wizTest_.kbpsPerSecond;
            wchar_t t[160];
            swprintf(t, 160, L"Testing with Twitch… %d of %d seconds%ls", (int)k.size(), kTestSeconds,
                     !wizTest_.connected ? (secs < 3 ? L" (connecting)" : L" (still connecting)") : L"");
            para(t, pal_.ink);
            if (!k.empty()) {
                swprintf(t, 160, L"Sending %.0f kbps now · %llu frames dropped", k.back(), (unsigned long long)wizTest_.droppedFrames);
                para(t, pal_.inkMuted);
            }
            const Rect bar{x, y, colW, 8};
            gfx_.fillRounded(bar, 4, pal_.ink.withAlpha(0.08f));
            gfx_.fillRounded({x, y, colW * std::min(1.0f, (float)k.size() / kTestSeconds), 8}, 4, pal_.ribbon);
            y += 26;
        } else {
            if (wizUpload_ > 0)
                para(L"Your connection keeps up with about " + std::to_wstring(wizUpload_) + L" kbps" +
                         (wizTest_.droppedFrames ? L" (frames were dropped at " + std::to_wstring(kTestKbps) + L", so it's lower than the test rate)." : L"."),
                     pal_.ink);
            if (!wizTestError_.empty()) para(L"The test didn't finish: " + wizTestError_, pal_.live);
            if (tw >= 0) {
                para(L"Test with Twitch: 20 seconds streaming your program to Twitch's bandwidth test. Twitch receives it but doesn't show it "
                     L"to anyone and your channel doesn't go live.",
                     pal_.inkMuted);
                button("wiz:test", {x, y, 240, 36}, L"Test my connection", [this] { startBandwidthTest(); }, true);
                y += 48;
            } else {
                para(L"To test with Twitch, add Twitch and its stream key under Settings › Streaming first. Or type your upload speed from any "
                     L"speed test below.",
                     pal_.inkMuted);
            }
            button("wiz:type", {x, y, 240, 34}, L"Type my upload speed…", [this] {
                askText(L"Upload speed in megabits per second (Mbps), from a speed test", L"", [this](const std::wstring& v) {
                    try {
                        const double mbps = std::stod(v);
                        if (mbps > 0.2 && mbps < 10000) {
                            wizUpload_ = (int)(mbps * 1000 * 0.8);   // speed tests are bursts: a steady stream gets less
                            wizTest_ = {};
                            invalidate();
                            return;
                        }
                    } catch (...) {
                    }
                    flash(L"Type a number of megabits per second, like 12.5.", 5);
                });
            });
            y += 50;
            button("wiz:back1", {x, y + 8, 120, 36}, L"Back", [this] { wizStep_ = 0; invalidate(); });
            button("wiz:next1", {x + colW - 160, y + 8, 160, 36}, wizUpload_ > 0 ? L"Next" : L"Skip", [this] { wizStep_ = 2; invalidate(); }, true);
            y += 60;
        }
    } else {
        int hw = 0;
        for (auto& e : encoders_) hw += e.hardware && e.codec == "H.264";
        const wizard::Plan p = wizard::recommend((wizard::Goal)wizGoal_, {hw > 0, (int)std::max(1u, std::thread::hardware_concurrency())},
                                                 streams ? wizUpload_ : 0);
        wchar_t t[200];
        swprintf(t, 200, L"Canvas %d × %d · %d fps", p.width, p.height, p.fps);
        para(t, pal_.ink);
        if (p.streamKbps) para(L"Streaming: " + std::to_wstring(p.streamKbps) + L" kbps video", pal_.ink);
        if (p.recordKbps) para(L"Recording: " + std::to_wstring(p.recordKbps) + L" kbps video", pal_.ink);
        y += 6;
        for (auto& r : p.reasons) para(L"• " + widen(r), pal_.inkMuted);
        const bool canvasDiffers = p.width != scenes_.canvasWidth || p.height != scenes_.canvasHeight;
        bool emptyCollection = true;
        for (auto& s : scenes_.scenes()) emptyCollection = emptyCollection && s.items.empty();
        if (canvasDiffers && !emptyCollection)
            para(L"Your scenes are laid out on " + std::to_wstring(scenes_.canvasWidth) + L" × " + std::to_wstring(scenes_.canvasHeight) +
                     L", so the canvas is left as it is (changing it doesn't move what's in your scenes). Change it under Settings › Video if you like.",
                 pal_.inkMuted);
        button("wiz:back2", {x, y + 8, 120, 36}, L"Back", [this] { wizStep_ = wizGoal_ == (int)wizard::Goal::Record ? 0 : 1; invalidate(); });
        button("wiz:apply", {x + colW - 220, y + 8, 220, 36}, L"Use these settings", [this, p, canvasDiffers, emptyCollection] {
            if (recorder_.active() || streamer_.active()) {
                flash(L"Stop recording and streaming first.", 4);
                return;
            }
            settings_.outputFps = p.fps;
            engine_.setOutputFps(p.fps);
            if (p.streamKbps)
                for (auto& d : settings_.destinations) d.kbps = p.streamKbps;
            if (p.recordKbps) settings_.recordKbps = p.recordKbps;
            settings_.save(settingsPath_);
            if (canvasDiffers && emptyCollection) {
                scenes_.canvasWidth = p.width;
                scenes_.canvasHeight = p.height;
                scenesChanged();
                saveScenes();
            }
            log::info("Setup wizard applied: " + std::to_string(p.fps) + " fps, stream " + std::to_string(p.streamKbps) + " kbps, record " +
                      std::to_string(p.recordKbps) + " kbps");
            flash(L"Done: your settings are set. Everything can be changed in Settings.", 6);
            wizardOpen_ = false;
            invalidate();
        }, true);
        y += 60;
    }
    endScroll(y - top + 24);
}

}  // namespace ncap
