// NorthRay Capture's window: Test my setup (Settings › Test my setup…, 0.10.32). One page that checks what
// viewers will see and hear, and what you'll hear: each source in the scene with its picture and status, each
// sound source with a live meter (it's ticked once it's had sound in the last 10 seconds), a test chime played
// to the monitor only (never to the stream or recordings), and what recording and streaming will use.
#include "capture_window.hpp"

#include "log.hpp"
#include "audio/devices.hpp"
#include "output/credentials.hpp"

#include <algorithm>

namespace ncap {

using cjui::Align;
using cjui::Color;
using cjui::Font;
using cjui::narrow;
using cjui::Rect;
using cjui::widen;
using nlohmann::json;

namespace {
double nowSeconds() {   // the same clock as the meters' heardAt (capture_audio.cpp)
    static LARGE_INTEGER f = [] {
        LARGE_INTEGER x;
        QueryPerformanceFrequency(&x);
        return x;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
}  // namespace

void CaptureWindow::openSetupTest() {
    openSettings(true);
    setupTestOpen_ = true;
    log::info("Test my setup opened");
    invalidate();
}

// The test chime: its own player, heard on the monitor only (syncAudio adds it as "#testsound").
void CaptureWindow::playTestSound() {
    if (!testSound_) {
        testSound_ = std::make_unique<audio::SoundboardPlayer>();
        testSound_->setSounds({{"Test chime", "chime", ""}});
        syncAudio();
    }
    testSound_->play(0);
    testPlayedAt_ = nowSeconds();
}

void CaptureWindow::drawSetupTest(const Rect& area) {
    gfx_.fill(area, pal_.paper);
    const float colW = std::min(760.0f, area.w - 48);
    const float x = area.x + (area.w - colW) / 2;
    const float top = beginScroll(area);
    float y = top + 24;
    gfx_.text(L"Test my setup", {x, y, colW - 130, 34}, Font::Title, pal_.ink, {Align::Left, false});
    button("st:close", {x + colW - 110, y + 2, 110, 34}, L"Done", [this] { openSettings(false); }, true);
    y += 44;
    y += gfx_.text(L"Everything here is checked live. Nothing goes on stream or into a recording.", {x, y, colW, 40}, Font::Body, pal_.inkMuted) + 18;
    const Color good = cjui::hexColor("#3FCF8E"), warn = cjui::hexColor("#E8C547"), bad = cjui::hexColor("#E0566A");
    auto mark = [&](float mx, float my, int state) {   // 0 good, 1 waiting, 2 problem
        const Color c = state == 0 ? good : state == 1 ? warn : bad;
        gfx_.circle(mx + 9, my + 9, 9, c.withAlpha(0.2f), true);
        if (state == 0) {
            gfx_.line(mx + 4.5f, my + 9, mx + 8, my + 12.5f, c, 1.8f);
            gfx_.line(mx + 8, my + 12.5f, mx + 14, my + 5.5f, c, 1.8f);
        } else {
            gfx_.text(state == 1 ? L"…" : L"!", {mx, my, 18, 18}, Font::SmallStrong, c, {Align::Center, false, false, true});
        }
    };

    // What viewers see: the scene's sources.
    y += sectionLabel(x, y, colW, L"WHAT YOUR VIEWERS SEE") + 10;
    const Scene* sc = scenes_.scene(scenes_.current());
    engine_.wantThumbs();
    const auto thumbs = engine_.thumbs();
    if (!sc || sc->items.empty()) {
        y += gfx_.text(L"The scene “" + (sc ? widen(sc->name) : std::wstring()) + L"” has nothing in it yet. Add sources with + in the Sources panel.",
                       {x, y, colW, 40}, Font::Body, pal_.inkMuted) + 14;
    } else {
        for (auto it = sc->items.rbegin(); it != sc->items.rend(); ++it) {
            const Source* src = scenes_.source(it->source);
            if (!src || isAudioOnly(src->kind)) continue;
            bool problem = false;
            std::wstring st = sourceStatus(src->id, &problem);
            const Rect pic{x, y, 96, 54};
            gfx_.fillRounded(pic, 6, pal_.ink.withAlpha(0.07f));
            auto t = thumbs.find(it->id);
            if (t != thumbs.end() && !t->second.rgba.empty()) {
                const std::wstring key = L"thumb:" + widen(it->id) + L":" + std::to_wstring(t->second.gen);
                auto& old = thumbKeys_[it->id];
                if (old != key) {
                    if (!old.empty()) gfx_.forgetImage(old);
                    old = key;
                }
                gfx_.drawPixels(key, t->second.w, t->second.h, t->second.rgba.data(), pic);
            }
            gfx_.strokeRounded(pic, 6, pal_.rule, 1);
            const int state = problem ? 2 : !it->visible ? 1 : 0;
            mark(x + 108, y + 4, state);
            gfx_.text(widen(src->name), {x + 136, y + 2, colW - 136, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
            if (!it->visible) st = L"Hidden in this scene, so viewers won't see it." + (st.empty() ? L"" : L" " + st);
            gfx_.text(st, {x + 136, y + 26, colW - 136, 22}, Font::Small, problem ? bad : pal_.inkMuted, {Align::Left, false, true});
            y += 66;
        }
    }
    y += 10;

    // What they hear: each sound source, live.
    y += sectionLabel(x, y, colW, L"WHAT THEY HEAR") + 10;
    const double now = nowSeconds();
    bool anySound = false;
    for (const Source& src : scenes_.sources()) {
        if (!hasSound(src.kind)) continue;
        auto a = audio_.find(src.id);
        if (a == audio_.end()) continue;
        {   // Capture's own test tone isn't something to check.
            const json j = json::parse(src.settings, nullptr, false);
            if (j.is_object() && j.contains("device") && j["device"].is_string() && j["device"].get<std::string>().rfind("#", 0) == 0) continue;
        }
        anySound = true;
        const bool muted = audioMuted(src);
        const bool toStream = (audioBuses(src) & routing::Stream) != 0;
        const bool heard = a->second.heardAt >= 0 && now - a->second.heardAt < 10;
        std::wstring status = a->second.capture ? a->second.capture->status() : std::wstring();
        const bool unavailable = status.find(L"Unavailable") != std::wstring::npos;
        int state = 0;
        std::wstring say;
        if (unavailable) state = 2, say = status;
        else if (muted) state = 1, say = L"Muted, so viewers won't hear it. Unmute it in the mixer.";
        else if (!toStream) state = 1, say = L"Not sent to the stream (its Stream switch in the mixer is off).";
        else if (heard) state = 0, say = L"Heard in the last few seconds.";
        else state = 1, say = src.kind == SourceKind::InputAudio ? L"Say something: the bar should move." : L"Play something: the bar should move.";
        mark(x, y + 4, state);
        gfx_.text(widen(src.name), {x + 28, y + 2, colW * 0.45f, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
        drawMeter({x + 28 + colW * 0.45f, y + 8, colW * 0.55f - 28, 10}, a->second.left, a->second.right, muted);
        gfx_.text(say, {x + 28, y + 26, colW - 28, 22}, Font::Small, state == 2 ? bad : pal_.inkMuted, {Align::Left, false, true});
        y += 56;
    }
    if (!anySound)
        y += gfx_.text(L"No sound sources yet. Add your microphone with + in the Audio mixer.", {x, y, colW, 40}, Font::Body, pal_.inkMuted) + 14;
    y += 10;

    // What you hear: a chime in the monitor.
    y += sectionLabel(x, y, colW, L"WHAT YOU HEAR") + 10;
    {
        const std::wstring dev = settings_.monitorDevice.empty() || settings_.monitorDevice == L"default" ? L"Windows' default sound output"
                                 : settings_.monitorDevice == L"off" ? L"nothing (the monitor is off)"
                                                                     : audio::deviceName(settings_.monitorDevice);
        y += gfx_.text(L"Your headphones get the monitor: " + dev + L". Play the test sound and you should hear a chime there. It isn't streamed or recorded.",
                       {x, y, colW, 44}, Font::Body, pal_.inkMuted) + 10;
        button("st:chime", {x, y, 190, 34}, L"Play a test sound", [this] { playTestSound(); }, true);
        const bool recent = testPlayedAt_ > 0 && now - testPlayedAt_ < 6;
        const std::wstring ms = mixer_.monitorStatus();
        gfx_.text(recent ? L"Playing… " + ms : ms, {x + 204, y, colW - 204, 34}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
        y += 52;
    }

    // Recording and streaming: what they'll use.
    y += sectionLabel(x, y, colW, L"RECORDING AND STREAMING") + 10;
    {
        std::wstring enc = encodersKnown_ ? L"" : L"Still checking the encoders…";
        int encState = 1;
        if (encodersKnown_) {
            for (auto& e : encoders_)
                if (e.hardware && e.codec == "H.264") {
                    enc = L"Hardware encoding: " + e.name + L".";
                    encState = 0;
                    break;
                }
            if (enc.empty()) enc = encoders_.empty() ? L"No H.264 encoder was found, so Capture can't record or stream on this PC." : L"Software encoding (no hardware encoder found): it works, but uses more of the processor.";
            if (encoders_.empty()) encState = 2;
        }
        mark(x, y + 2, encState);
        y += gfx_.text(enc, {x + 28, y, colW - 28, 44}, Font::Body, pal_.ink) + 10;
        int on = 0, keyed = 0;
        for (auto& d : settings_.destinations)
            if (d.enabled) {
                ++on;
                keyed += secrets::exists("stream/" + d.id) ? 1 : 0;
            }
        const std::wstring dest = on == 0 ? L"No streaming destination is switched on (Settings › Streaming)."
                                  : keyed == on ? std::to_wstring(on) + (on == 1 ? L" destination is" : L" destinations are") + L" switched on, each with its stream key."
                                                : std::to_wstring(on - keyed) + L" of the destinations switched on " + (on - keyed == 1 ? L"has" : L"have") + L" no stream key yet (Settings › Streaming).";
        mark(x, y + 2, on == 0 ? 1 : keyed == on ? 0 : 2);
        y += gfx_.text(dest, {x + 28, y, colW - 28, 44}, Font::Body, pal_.ink) + 10;
        std::wstring why;
        const std::wstring advice = keepUpAdvice(why);
        mark(x, y + 2, advice.empty() ? 0 : 1);
        y += gfx_.text(advice.empty() ? std::wstring(L"The video is keeping up.") : advice, {x + 28, y, colW - 28, 200}, Font::Body, pal_.ink) + 10;
    }
    endScroll(y - top + 24);
}

}  // namespace ncap
