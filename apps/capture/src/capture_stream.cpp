// NorthRay Capture's window: streaming.
#include "capture_window.hpp"

#include "log.hpp"
#include "output/credentials.hpp"

#include "ncap/rtmp.hpp"

#include "cjui/dialog.hpp"

#include <mfapi.h>

#include <algorithm>

namespace ncap {

using cjui::Align;
using cjui::Font;
using cjui::narrow;
using cjui::widen;

namespace {

constexpr const char* kKeyName = "stream/main";

struct Service {
    const char* id;
    const wchar_t* label;
    const char* url;           // "" = the person pastes it from the platform's dashboard
    const wchar_t* hint;
};
// Checked 14 Nov 2026: Twitch's "Default" ingest (ingest.twitch.tv/ingests, url_template_secure) and
// YouTube's RTMPS ingestion (port 443). Kick gives each channel its own address.
const Service kServices[] = {
    {"twitch", L"Twitch", "rtmps://ingest.global-contribute.live-video.net/app",
     L"Twitch's automatic server (it picks the nearest). Key: Creator Dashboard › Settings › Stream. Up to 6 Mbit/s."},
    {"youtube", L"YouTube", "rtmps://a.rtmps.youtube.com:443/live2", L"Key: YouTube Studio › Go live › Stream. 1080p60: about 9 Mbit/s or less."},
    {"kick", L"Kick", "", L"Paste the Stream URL and Stream Key from Kick's dashboard (Settings › Stream URL & Key)."},
    {"custom", L"Custom", "", L"Any RTMP or RTMPS server: its address (rtmp:// or rtmps://) and stream key."},
};

const Service& serviceOf(const std::string& id) {
    for (auto& s : kServices)
        if (id == s.id) return s;
    return kServices[0];
}

}  // namespace

void CaptureWindow::startStreaming() {
    if (streamer_.active()) return;
    output::StreamSettings ss;
    ss.name = narrow(serviceOf(settings_.streamService).label);
    ss.url = settings_.streamUrl;
    ss.key = secrets::load(kKeyName);
    if (!ss.key.empty()) log::addSecret(ss.key);   // never written to a log, whatever happens
    ss.video.width = scenes_.canvasWidth;
    ss.video.height = scenes_.canvasHeight;
    ss.video.fps = settings_.outputFps;
    ss.video.bitrateKbps = settings_.streamKbps;
    ss.video.encoder = settings_.recordEncoder;
    ss.audioKbps = 160;
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    std::wstring error;
    if (!streamer_.start(engine_, mixer_, ss, error)) {
        MFShutdown();
        log::error("Streaming couldn't start: " + narrow(error));
        cjui::messageBox(hwnd(), error, L"Streaming couldn’t start", MB_OK | MB_ICONWARNING);
        return;
    }
    rtmp::Url u;
    rtmp::parseUrl(ss.url, u);
    log::info("Streaming to " + ss.name + " at " + u.tcUrl + " (" + std::to_string(ss.video.width) + "x" + std::to_string(ss.video.height) + "@" +
              std::to_string(ss.video.fps) + ", " + std::to_string(ss.video.bitrateKbps) + " kbps CBR)");
    invalidate();
}

void CaptureWindow::stopStreaming() {
    if (!streamer_.active()) return;
    const output::StreamStatus st = streamer_.status();
    streamer_.stop();
    MFShutdown();
    log::info("Streaming stopped after " + std::to_string((int)st.seconds) + " s live, " + std::to_string(st.droppedFrames) + " frames dropped");
    invalidate();
}

void CaptureWindow::streamTick() {
    if (!streamer_.active()) return;
    const output::StreamStatus st = streamer_.status();
    static output::StreamStatus::State last = output::StreamStatus::Off;
    if (st.state != last) {
        log::info("Stream: " + narrow(st.message));
        last = st.state;
    }
    if (st.state == output::StreamStatus::Failed) {
        const std::wstring why = st.message;
        streamer_.stop();
        MFShutdown();
        cjui::messageBox(hwnd(), why, L"Streaming stopped", MB_OK | MB_ICONWARNING);
    }
}

float CaptureWindow::drawStreamingSettings(float x, float y0, float colW) {
    float y = y0;
    const bool busy = streamer_.active();
    auto row = [&](const std::wstring& label, const std::wstring& help, float controlW) {
        gfx_.text(label, {x, y, colW - controlW - 20, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
        float h = 24;
        if (!help.empty()) h += gfx_.text(help, {x, y + 24, colW - controlW - 20, 80}, Font::Small, pal_.inkMuted);
        return h;
    };
    const Service& svc = serviceOf(settings_.streamService);
    {
        const float h = row(L"Service", svc.hint, 330);
        int sel = 0;
        for (int i = 0; i < 4; ++i)
            if (settings_.streamService == kServices[i].id) sel = i;
        segmented("str:svc", {x + colW - 330, y - 2, 330, 32}, {L"Twitch", L"YouTube", L"Kick", L"Custom"}, sel, [this, busy](int i) {
            if (busy) {
                flash(L"Stop streaming to change where it goes.", 4);
                return;
            }
            if (settings_.streamService == kServices[i].id) return;
            settings_.streamService = kServices[i].id;
            if (*kServices[i].url) settings_.streamUrl = kServices[i].url;
            else settings_.streamUrl.clear();
            settings_.save(settingsPath_);
        });
        y += std::max(h, 34.0f) + 14;
    }
    {
        const std::wstring url = settings_.streamUrl.empty() ? L"Not set yet" : widen(settings_.streamUrl);
        const float h = row(L"Server", url, 150);
        button("str:url", {x + colW - 150, y - 2, 150, 32}, L"Change…", [this, busy] {
            if (busy) return;
            askText(L"The server address (rtmp:// or rtmps://)", widen(settings_.streamUrl), [this](const std::wstring& v) {
                std::string text = narrow(v), keyInUrl;
                rtmp::Url u;
                if (!rtmp::parseUrl(text, u, &keyInUrl)) {
                    flash(L"That isn't an RTMP address: it should start with rtmp:// or rtmps://.", 6);
                    return;
                }
                // A key pasted on the end of the address goes to Credential Manager, not into settings.
                if (!keyInUrl.empty()) {
                    secrets::save(kKeyName, keyInUrl);
                    text = u.tcUrl;
                    flash(L"The stream key at the end of the address was moved to Windows Credential Manager.", 6);
                }
                settings_.streamUrl = text;
                // An address that isn't the chosen service's own is a custom server (Kick's are always pasted).
                const Service& cur = serviceOf(settings_.streamService);
                if (*cur.url && text != cur.url) settings_.streamService = "custom";
                settings_.save(settingsPath_);
            });
        });
        y += std::max(h, 34.0f) + 14;
    }
    {
        const bool have = secrets::exists(kKeyName);
        const float h = row(L"Stream key",
                            have ? L"Saved in Windows Credential Manager. It's never shown, and never written to settings or logs."
                                 : L"Not set yet. It's kept in Windows Credential Manager, never in settings or logs.",
                            have ? 270 : 150);
        button("str:key", {x + colW - 150, y - 2, 150, 32}, have ? L"Replace…" : L"Enter…", [this, busy] {
            if (busy) return;
            askText(
                L"Paste the stream key", L"",
                [this](const std::wstring& v) {
                    std::string key = narrow(v);
                    while (!key.empty() && (key.back() == ' ' || key.back() == '\r' || key.back() == '\n')) key.pop_back();
                    while (!key.empty() && key.front() == ' ') key.erase(0, 1);
                    if (key.empty()) return;
                    if (!secrets::save(kKeyName, key)) flash(L"Windows Credential Manager didn't save the key.", 6);
                    else flash(L"Saved the stream key.", 4);
                    std::fill(key.begin(), key.end(), '\0');
                },
                true);
        });
        if (have)
            button("str:keydel", {x + colW - 270, y - 2, 110, 32}, L"Remove", [this, busy] {
                if (busy) return;
                secrets::remove(kKeyName);
                flash(L"Removed the stream key.", 4);
            });
        y += std::max(h, 34.0f) + 14;
    }
    {
        const float h = row(L"Video bitrate", L"Constant, as platforms ask. Leave room: under about two thirds of your upload speed.", 330);
        const int v[] = {2500, 4500, 6000, 8000};
        int sel = -1;
        for (int i = 0; i < 4; ++i)
            if (settings_.streamKbps == v[i]) sel = i;
        segmented("str:kbps", {x + colW - 330, y - 2, 330, 32}, {L"2.5 Mbit/s", L"4.5", L"6", L"8"}, sel, [this, busy](int i) {
            if (busy) return;
            const int vv[] = {2500, 4500, 6000, 8000};
            settings_.streamKbps = vv[i];
            settings_.save(settingsPath_);
        });
        y += std::max(h, 34.0f) + 14;
    }
    y += gfx_.text(L"The stream uses the encoder chosen under Recording, the canvas size and frame rate, and the Stream mix from the Audio mixer "
                   L"(AAC, 160 kbit/s). If the connection drops, Capture reconnects by itself.",
                   {x, y, colW, 60}, Font::Small, pal_.inkMuted) + 8;
    if (busy) y += gfx_.text(L"These can't change while streaming.", {x, y, colW, 20}, Font::Small, cjui::hexColor("#E0566A")) + 6;
    return y - y0;
}

}  // namespace ncap
