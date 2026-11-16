// NorthRay Capture's window: streaming to one or more destinations (system 8).
#include "capture_window.hpp"
#include "video/design_render.hpp"

#include "log.hpp"
#include "output/credentials.hpp"

#include "ncap/rtmp.hpp"

#include "cjui/dialog.hpp"
#include "cjui/menu.hpp"

#include <mfapi.h>

#include <algorithm>
#include <set>

namespace ncap {

using cjui::Align;
using cjui::Color;
using cjui::Font;
using cjui::MenuItem;
using cjui::narrow;
using cjui::Rect;
using cjui::widen;

namespace {

constexpr std::size_t kMaxDestinations = 6;
const int kBitrates[] = {2500, 4500, 6000, 8000, 12000};

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
    {"youtube", L"YouTube", "rtmps://a.rtmps.youtube.com:443/live2",
     L"Key: YouTube Studio › Create › Go live › Stream. 1080p60: up to about 12 Mbit/s."},
    {"kick", L"Kick", "", L"Paste the Stream URL and Stream Key from Kick's dashboard (Settings › Stream URL & Key)."},
    {"custom", L"Custom", "", L"Any RTMP or RTMPS server: its address (rtmp:// or rtmps://) and stream key."},
};

const Service& serviceOf(const std::string& id) {
    for (auto& s : kServices)
        if (id == s.id) return s;
    return kServices[3];
}

std::string keyName(const std::string& id) { return "stream/" + id; }

const Color kLive = cjui::hexColor("#3FCF8E"), kWait = cjui::hexColor("#E8C547"), kBad = cjui::hexColor("#E0566A");

std::wstring clock(double seconds) {
    const int sec = (int)seconds;
    wchar_t t[32];
    swprintf(t, 32, L"%d:%02d:%02d", sec / 3600, sec / 60 % 60, sec % 60);
    return t;
}

std::wstring destName(const Settings& s, std::size_t i) {
    const std::wstring base = serviceOf(s.destinations[i].service).label;
    int same = 0, nth = 0;
    for (std::size_t k = 0; k < s.destinations.size(); ++k)
        if (s.destinations[k].service == s.destinations[i].service) {
            ++same;
            if (k <= i) ++nth;
        }
    return same > 1 ? base + L" " + std::to_wstring(nth) : base;
}

}  // namespace

// A destination's name as shown everywhere ("Twitch", "YouTube 2"), for other parts of the window.
std::wstring streamDestName(const Settings& s, std::size_t i) { return destName(s, i); }

// --- starting and stopping ---

bool CaptureWindow::startDestination(const std::string& id, bool quiet) {
    std::size_t i = 0;
    while (i < settings_.destinations.size() && settings_.destinations[i].id != id) ++i;
    if (i == settings_.destinations.size() || streamer_.active(id)) return false;
    const auto& d = settings_.destinations[i];
    if (!mfForStream_) {
        MFStartup(MF_VERSION, MFSTARTUP_LITE);
        mfForStream_ = true;
    }
    output::StreamCommon c;
    c.width = scenes_.canvasWidth;
    c.height = scenes_.canvasHeight;
    c.fps = settings_.outputFps;
    c.encoder = settings_.recordEncoder;
    c.audioKbps = 160;
    c.longestWaitSeconds = settings_.streamRetryMax;
    output::DestinationStart ds;
    ds.id = d.id;
    ds.name = narrow(destName(settings_, i));
    ds.url = d.url;
    ds.key = secrets::load(keyName(d.id));
    if (!ds.key.empty()) log::addSecret(ds.key);   // never written to a log, whatever happens
    ds.videoKbps = d.kbps;
    ds.bus = d.bus;
    std::wstring error;
    const bool ok = streamer_.start(engine_, mixer_, c, ds, error);
    SecureZeroMemory(ds.key.data(), ds.key.size());
    if (!ok) {
        log::error("Streaming to " + ds.name + " couldn't start: " + narrow(error));
        if (!quiet) cjui::messageBox(hwnd(), error, L"Streaming to " + destName(settings_, i) + L" couldn’t start", MB_OK | MB_ICONWARNING);
        else startErrors_.push_back(destName(settings_, i) + L": " + error);
        return false;
    }
    rtmp::Url u;
    rtmp::parseUrl(ds.url, u);
    log::info("Streaming to " + ds.name + " at " + u.tcUrl + " (" + std::to_string(c.width) + "x" + std::to_string(c.height) + "@" +
              std::to_string(c.fps) + ", " + std::to_string(ds.videoKbps) + " kbps CBR; " + std::to_string(streamer_.encodes().size()) +
              " encode(s) running)");
    destStates_[id] = -1;
    invalidate();
    return true;
}

void CaptureWindow::startStreaming() {
    std::vector<std::string> ids;
    for (auto& d : settings_.destinations)
        if (d.enabled && !streamer_.active(d.id)) ids.push_back(d.id);
    if (ids.empty()) {
        flash(L"No destination is switched on: choose one in Settings › Streaming.", 6);
        return;
    }
    // One destination: a problem gets a proper message. Several: each problem is shown briefly, and
    // the others go ahead.
    int started = 0;
    startErrors_.clear();
    for (auto& id : ids) started += startDestination(id, ids.size() > 1) ? 1 : 0;
    if (!startErrors_.empty()) {
        std::wstring all;
        for (auto& e : startErrors_) all += L"\u2022 " + e + L"\n";
        if (started) all += L"\nThe other destinations have started.";
        cjui::messageBox(hwnd(), all, started ? L"Some destinations couldn’t start" : L"Streaming couldn’t start", MB_OK | MB_ICONWARNING);
        startErrors_.clear();
    }
    if (!streamer_.active() && mfForStream_) {
        MFShutdown();
        mfForStream_ = false;
    }
}

void CaptureWindow::stopDestination(const std::string& id) {
    const output::DestinationStatus st = streamer_.status(id);
    streamer_.stop(id);
    destStates_.erase(id);
    log::info("Streaming to a destination stopped after " + std::to_string((int)st.seconds) + " s live, " + std::to_string(st.droppedFrames) +
              " frames dropped, " + std::to_string(st.reconnects) + " reconnections");
    if (!streamer_.active() && mfForStream_) {
        MFShutdown();
        mfForStream_ = false;
    }
    invalidate();
}

void CaptureWindow::stopStreaming() {
    for (auto& id : streamer_.ids()) stopDestination(id);
    streamer_.stop();
    if (mfForStream_) {
        MFShutdown();
        mfForStream_ = false;
    }
    invalidate();
}

void CaptureWindow::streamTick() {
    {   // How long it's been live, for timers in designs.
        double longest = -1;
        for (auto& id : streamer_.ids())
            if (streamer_.active(id)) longest = std::max(longest, streamer_.status(id).seconds);
        video::setLiveTime(longest < 0 ? -1 : (std::int64_t)(longest * 1000));
    }
    if (streamer_.ids().empty()) return;
    streamer_.tick();
    int running = 0;
    for (auto& id : streamer_.ids()) {
        const output::DestinationStatus st = streamer_.status(id);
        if (streamer_.active(id)) ++running;
        std::wstring name = widen(id);
        for (std::size_t i = 0; i < settings_.destinations.size(); ++i)
            if (settings_.destinations[i].id == id) name = destName(settings_, i);
        auto it = destStates_.find(id);
        if (it != destStates_.end() && it->second == (int)st.state) continue;
        destStates_[id] = (int)st.state;
        log::info("Stream to " + narrow(name) + ": " + narrow(st.message));
        if (st.state == output::DestinationStatus::Failed) {
            const bool others = streamer_.active();
            if (others) flash(name + L" stopped: " + st.message + L" The others carry on.", 10);
            else cjui::messageBox(hwnd(), st.message, L"Streaming to " + name + L" stopped", MB_OK | MB_ICONWARNING);
        }
    }
    if (!running && mfForStream_) {
        MFShutdown();
        mfForStream_ = false;
    }
}

// --- the controls panel and the status bar ---

std::wstring CaptureWindow::streamStatusText(bool& live) const {
    live = false;
    int on = 0, liveN = 0;
    double kbps = 0;
    std::uint64_t dropped = 0;
    for (auto& id : streamer_.ids()) {
        if (!streamer_.active(id)) continue;
        const output::DestinationStatus st = streamer_.status(id);
        ++on;
        if (st.state == output::DestinationStatus::Live) ++liveN;
        kbps += st.kbps;
        dropped += st.droppedFrames;
    }
    for (auto& e : streamer_.encodes()) dropped += e.encoderDropped;
    if (!on) return L"off";
    live = liveN > 0;
    wchar_t t[96];
    if (on == 1) swprintf(t, 96, liveN ? L"live · %.1f Mbit/s up" : L"connecting…", kbps / 1000.0);
    else swprintf(t, 96, L"live on %d of %d · %.1f Mbit/s up", liveN, on, kbps / 1000.0);
    std::wstring v = t;
    if (dropped) v += L" · " + std::to_wstring(dropped) + L" dropped";
    return v;
}

float CaptureWindow::drawStreamControls(float x, float y, float w, float bottom) {
    const bool on = streamer_.active();
    const Color purple = cjui::hexColor("#7A5CFF");
    {
        const Rect r{x, y, w, 40};
        addHit("ctl:stream", r, [this, on] {
            if (on) stopStreaming();
            else if (settings_.preLiveChecklist) openChecklist();
            else startStreaming();
        });
        const float hv = hoverAmount("ctl:stream");
        bool live = false;
        streamStatusText(live);
        gfx_.fillRounded(r, 8, on ? purple.withAlpha(0.22f + 0.1f * hv) : pal_.ink.withAlpha((pal_.dark ? 0.07f : 0.05f) + 0.05f * hv));
        gfx_.strokeRounded(r, 8, on ? purple : pal_.rule.mix(pal_.ink, 0.3f * hv), 1);
        gfx_.circle(r.x + 18, r.y + r.h / 2, 5, live ? kLive : on ? kWait : purple.withAlpha(0.8f), true);
        gfx_.text(on ? L"Stop streaming" : L"Start streaming", {r.x + 32, r.y, r.w - 40, r.h}, Font::BodyStrong, pal_.ink,
                  {Align::Left, false, true, true});
        if (on) {
            int n = 0, l = 0;
            double longest = 0;
            for (auto& id : streamer_.ids())
                if (streamer_.active(id)) {
                    const auto st = streamer_.status(id);
                    ++n;
                    if (st.state == output::DestinationStatus::Live) ++l;
                    longest = std::max(longest, st.seconds);
                }
            const std::wstring right = n > 1 ? std::to_wstring(l) + L" of " + std::to_wstring(n) + L" live" : l ? clock(longest) : L"connecting";
            gfx_.text(right, {r.x, r.y, r.w - 12, r.h}, Font::Body, pal_.ink, {Align::Right, false, false, true});
        }
        if (focused("ctl:stream")) focusRing(r);
        y += 46;
    }
    // The destinations, each with its own state; click one to start or stop just that one.
    const bool show = settings_.destinations.size() > 1 || on;
    if (!show) return y;
    for (std::size_t i = 0; i < settings_.destinations.size(); ++i) {
        if (y + 30 > bottom) break;
        const auto& d = settings_.destinations[i];
        const bool known = streamer_.known(d.id), running = streamer_.active(d.id);
        if (!d.enabled && !known) continue;
        const output::DestinationStatus st = known ? streamer_.status(d.id) : output::DestinationStatus{};
        const Rect r{x + 8, y, w - 8, 28};
        const std::string hid = "ctl:dest:" + d.id;
        addHit(hid, r, [this, id = d.id, running] {
            if (running) stopDestination(id);
            else {
                if (streamer_.known(id)) streamer_.clear(id);
                startDestination(id, false);
            }
        });
        const float hv = hoverAmount(hid);
        if (hv > 0) gfx_.fillRounded(r, 6, pal_.ink.withAlpha(0.05f * hv));
        Color dot = pal_.inkFaint;
        std::wstring right;
        switch (known ? st.state : output::DestinationStatus::Off) {
        case output::DestinationStatus::Live: {
            dot = kLive;
            wchar_t t[64];
            swprintf(t, 64, L"%ls · %.1f Mbit/s", clock(st.seconds).c_str(), st.kbps / 1000.0);
            right = t;
            if (st.droppedFrames) right += L" · " + std::to_wstring(st.droppedFrames) + L" dropped";
            if (st.reconnects) right += L" · " + std::to_wstring(st.reconnects) + L" reconnects";
            break;
        }
        case output::DestinationStatus::Reconnecting:
            dot = kWait;
            right = st.retryIn > 0 ? L"retrying in " + std::to_wstring(st.retryIn) + L" s (try " + std::to_wstring(st.attempts + 1) + L")"
                                   : L"reconnecting…";
            break;
        case output::DestinationStatus::Connecting:
        case output::DestinationStatus::Waiting:
            dot = kWait;
            right = L"connecting…";
            break;
        case output::DestinationStatus::Failed:
            dot = kBad;
            right = L"stopped: " + st.message;
            break;
        default: right = hv > 0 ? L"click to start" : L"off"; break;
        }
        if (running && hv > 0) right = L"click to stop";
        gfx_.circle(r.x + 10, r.y + r.h / 2, 3.5f, dot, true);
        const std::wstring name = destName(settings_, i);
        const float nw = std::min(gfx_.measureWidth(name, Font::Body) + 4, r.w * 0.45f);
        gfx_.text(name, {r.x + 22, r.y, nw, r.h}, Font::Body, pal_.ink, {Align::Left, false, true, true});
        gfx_.text(right, {r.x + 22 + nw + 8, r.y, r.w - nw - 38, r.h}, Font::Small,
                  known && st.state == output::DestinationStatus::Failed ? kBad : pal_.inkMuted, {Align::Right, false, true, true});
        if (focused(hid)) focusRing(r);
        y += 30;
    }
    return y + 8;
}

// --- Settings › Streaming ---

float CaptureWindow::drawDestinationSettings(int index, float x, float y0, float colW) {
    float y = y0;
    auto& d = settings_.destinations[(std::size_t)index];
    const std::string id = d.id;
    const bool busy = streamer_.active(id);
    auto row = [&](const std::wstring& label, const std::wstring& help, float controlW) {
        gfx_.text(label, {x + 14, y, colW - controlW - 34, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
        float h = 24;
        if (!help.empty()) h += gfx_.text(help, {x + 14, y + 24, colW - controlW - 34, 80}, Font::Small, pal_.inkMuted);
        return h;
    };
    auto find = [this](const std::string& want) -> Settings::StreamDestination* {
        for (auto& e : settings_.destinations)
            if (e.id == want) return &e;
        return nullptr;
    };
    const Service& svc = serviceOf(d.service);
    // Header: the name, switched on or not, remove.
    {
        gfx_.text(destName(settings_, (std::size_t)index), {x, y, colW - 340, 26}, Font::Heading, pal_.ink, {Align::Left, false, true, true});
        gfx_.text(L"Starts with the others", {x + colW - 330, y, 150, 26}, Font::Small, pal_.inkMuted, {Align::Right, false, true});
        toggle("str:on:" + id, x + colW - 170, y + 1, d.enabled, [this, find, id] {
            if (auto* e = find(id)) {
                e->enabled = !e->enabled;
                settings_.save(settingsPath_);
            }
        });
        if (settings_.destinations.size() > 1)
            button("str:del:" + id, {x + colW - 110, y - 2, 110, 30}, L"Remove", [this, id, busy] {
                if (busy) {
                    flash(L"Stop streaming there first.", 4);
                    return;
                }
                if (streamer_.known(id)) streamer_.clear(id);
                secrets::remove(keyName(id));
                auto& v = settings_.destinations;
                v.erase(std::remove_if(v.begin(), v.end(), [&](const Settings::StreamDestination& e) { return e.id == id; }), v.end());
                settings_.save(settingsPath_);
            });
        y += 40;
    }
    {
        const float h = row(L"Service", svc.hint, 330);
        int sel = 3;
        for (int i = 0; i < 4; ++i)
            if (d.service == kServices[i].id) sel = i;
        segmented("str:svc:" + id, {x + colW - 330, y - 2, 330, 32}, {L"Twitch", L"YouTube", L"Kick", L"Custom"}, sel, [this, find, id, busy](int i) {
            auto* e = find(id);
            if (!e || e->service == kServices[i].id) return;
            if (busy) {
                flash(L"Stop streaming there to change where it goes.", 4);
                return;
            }
            e->service = kServices[i].id;
            e->url = kServices[i].url;
            settings_.save(settingsPath_);
        });
        y += std::max(h, 34.0f) + 12;
    }
    {
        const float h = row(L"Server", d.url.empty() ? L"Not set yet" : widen(d.url), 150);
        button("str:url:" + id, {x + colW - 150, y - 2, 150, 32}, L"Change…", [this, find, id, busy] {
            auto* e = find(id);
            if (!e || busy) return;
            askText(L"The server address (rtmp:// or rtmps://)", widen(e->url), [this, find, id](const std::wstring& v) {
                std::string text = narrow(v), keyInUrl;
                rtmp::Url u;
                if (!rtmp::parseUrl(text, u, &keyInUrl)) {
                    flash(L"That isn't an RTMP address: it should start with rtmp:// or rtmps://.", 6);
                    return;
                }
                auto* e2 = find(id);
                if (!e2) return;
                // A key pasted on the end of the address goes to Credential Manager, not into settings.
                if (!keyInUrl.empty()) {
                    secrets::save(keyName(id), keyInUrl);
                    text = u.tcUrl;
                    flash(L"The stream key at the end of the address was moved to Windows Credential Manager.", 6);
                }
                e2->url = text;
                // An address that isn't the chosen service's own is a custom server (Kick's are always pasted).
                const Service& cur = serviceOf(e2->service);
                if (*cur.url && text != cur.url) e2->service = "custom";
                settings_.save(settingsPath_);
            });
        });
        y += std::max(h, 34.0f) + 12;
    }
    {
        const bool have = secrets::exists(keyName(id));
        const float h = row(L"Stream key",
                            have ? L"Saved in Windows Credential Manager. It's never shown, and never written to settings or logs."
                                 : L"Not set yet. It's kept in Windows Credential Manager, never in settings or logs.",
                            have ? 270 : 150);
        button("str:key:" + id, {x + colW - 150, y - 2, 150, 32}, have ? L"Replace…" : L"Enter…", [this, id, busy] {
            if (busy) return;
            askText(
                L"Paste the stream key", L"",
                [this, id](const std::wstring& v) {
                    std::string key = narrow(v);
                    while (!key.empty() && (key.back() == ' ' || key.back() == '\r' || key.back() == '\n')) key.pop_back();
                    while (!key.empty() && key.front() == ' ') key.erase(0, 1);
                    if (key.empty()) return;
                    if (!secrets::save(keyName(id), key)) flash(L"Windows Credential Manager didn't save the key.", 6);
                    else flash(L"Saved the stream key.", 4);
                    SecureZeroMemory(key.data(), key.size());
                },
                true);
        });
        if (have)
            button("str:keydel:" + id, {x + colW - 270, y - 2, 110, 32}, L"Remove", [this, id, busy] {
                if (busy) return;
                secrets::remove(keyName(id));
                flash(L"Removed the stream key.", 4);
            });
        y += std::max(h, 34.0f) + 12;
    }
    {
        const float h = row(L"Video bitrate", d.service == "twitch" ? L"Twitch takes up to about 6 Mbit/s." : L"", 380);
        int sel = -1;
        for (int i = 0; i < 5; ++i)
            if (d.kbps == kBitrates[i]) sel = i;
        segmented("str:kbps:" + id, {x + colW - 380, y - 2, 380, 32}, {L"2.5 Mbit/s", L"4.5", L"6", L"8", L"12"}, sel, [this, find, id, busy](int i) {
            auto* e = find(id);
            if (!e || busy) return;
            e->kbps = kBitrates[i];
            settings_.save(settingsPath_);
        });
        y += std::max(h, 34.0f) + 12;
    }
    {
        const float h = row(L"Sound", L"What this destination hears: the Stream mix, or one of the recording tracks (a game-only mix for one "
                                      L"platform, say). Choose what goes on each in the Audio mixer.", 200);
        const std::wstring now = d.bus == 0 ? L"The Stream mix" : L"Track " + std::to_wstring(d.bus);
        button("str:bus:" + id, {x + colW - 200, y - 2, 200, 32}, now, [this, find, id, busy] {
            if (busy) return;
            std::vector<MenuItem> items = {MenuItem::item(L"The Stream mix", 100)};
            for (int t = 1; t <= 6; ++t) items.push_back(MenuItem::item(L"Track " + std::to_wstring(t), 100 + (UINT)t));
            POINT pt;
            GetCursorPos(&pt);
            const UINT cmd = cjui::showMenu(hwnd(), items, pt, pal_);
            auto* e = find(id);
            if (!e || cmd < 100 || cmd > 106) return;
            e->bus = (int)cmd - 100;
            settings_.save(settingsPath_);
        });
        y += std::max(h, 34.0f) + 12;
    }
    if (busy) y += gfx_.text(L"Streaming there now: stop it to change these.", {x + 14, y, colW, 20}, Font::Small, kBad) + 6;
    return y - y0;
}

float CaptureWindow::drawStreamingSettings(float x, float y0, float colW) {
    float y = y0;
    for (std::size_t i = 0; i < settings_.destinations.size(); ++i) {
        const float h = drawDestinationSettings((int)i, x, y, colW);
        gfx_.line(x, y + h + 4.5f, x + colW, y + h + 4.5f, pal_.rule.withAlpha(0.6f));
        y += h + 16;
    }
    if (settings_.destinations.size() < kMaxDestinations) {
        button("str:add", {x, y, 200, 32}, L"Add a destination", [this] {
            Settings::StreamDestination d;
            std::set<std::string> used;
            for (auto& e : settings_.destinations) used.insert(e.id);
            for (int n = 2;; ++n)
                if (!used.count("dest" + std::to_string(n))) {
                    d.id = "dest" + std::to_string(n);
                    break;
                }
            // Suggest a service not used yet.
            std::set<std::string> services;
            for (auto& e : settings_.destinations) services.insert(e.service);
            for (auto& s : kServices)
                if (!services.count(s.id)) {
                    d.service = s.id;
                    d.url = s.url;
                    break;
                }
            if (d.service == "youtube") d.kbps = 8000;
            settings_.destinations.push_back(d);
            settings_.save(settingsPath_);
        });
        y += 44;
    }
    // What it all adds up to.
    std::set<int> rates;
    std::set<std::pair<int, int>> mixes;
    double upload = 0;
    int on = 0;
    for (auto& d : settings_.destinations)
        if (d.enabled) {
            rates.insert(d.kbps);
            mixes.insert({d.kbps, d.bus});
            upload += (d.kbps + 160) * 1.05;   // video, audio, and about 5% for RTMP and TLS
            ++on;
        }
    std::wstring sum;
    if (on) {
        wchar_t t[240];
        swprintf(t, 240,
                 L"%d destination%ls switched on: about %.1f Mbit/s of upload in all (each gets its own copy). Keep that under about two "
                 L"thirds of your upload speed.",
                 on, on == 1 ? L"" : L"s", upload / 1000.0);
        sum = t;
        sum += rates.size() <= 1 ? std::wstring(L" One encode: every destination shares it.")
                                 : L" " + std::to_wstring(rates.size()) +
                                       L" encodes, one per bitrate (destinations at the same bitrate share one; each extra encode is more work "
                                       L"for the graphics card).";
        if (mixes.size() > rates.size())
            sum += L" Destinations with different sound share the video but each sound is encoded on its own (a small cost).";
    } else {
        sum = L"No destination is switched on, so Start streaming has nowhere to go.";
    }
    y += gfx_.text(sum, {x, y, colW, 80}, Font::Small, pal_.inkMuted) + 14;
    {
        gfx_.text(L"Reconnecting", {x, y, colW - 350, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
        const float h = 24 + gfx_.text(L"If a connection drops, Capture tries again after 2 s, then waits twice as long each time, up to this. "
                                       L"Each destination reconnects on its own; the others carry on.",
                                       {x, y + 24, colW - 350, 80}, Font::Small, pal_.inkMuted);
        const int v[] = {15, 30, 60, 120};
        int sel = -1;
        for (int i = 0; i < 4; ++i)
            if (settings_.streamRetryMax == v[i]) sel = i;
        segmented("str:retry", {x + colW - 330, y - 2, 330, 32}, {L"15 s", L"30 s", L"1 min", L"2 min"}, sel, [this](int i) {
            const int vv[] = {15, 30, 60, 120};
            settings_.streamRetryMax = vv[i];
            settings_.save(settingsPath_);
        });
        y += std::max(h, 34.0f) + 12;
    }
    {
        gfx_.text(L"Pre-live checklist", {x, y, colW - 80, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
        const float h = 24 + gfx_.text(L"Start streaming first shows what Capture can check (destinations, keys, your mic, the scene) and your "
                                       L"own list to tick. It never stops you going live.",
                                       {x, y + 24, colW - 80, 80}, Font::Small, pal_.inkMuted);
        toggle("str:checklist", x + colW - 52, y + 4, settings_.preLiveChecklist, [this] {
            settings_.preLiveChecklist = !settings_.preLiveChecklist;
            settings_.save(settingsPath_);
        });
        y += std::max(h, 34.0f) + 12;
    }
    y += gfx_.text(L"Every destination uses the encoder chosen under Recording, the canvas size and frame rate, and the Stream mix from the "
                   L"Audio mixer (AAC, 160 kbit/s).",
                   {x, y, colW, 60}, Font::Small, pal_.inkMuted) + 8;
    return y - y0;
}

}  // namespace ncap
