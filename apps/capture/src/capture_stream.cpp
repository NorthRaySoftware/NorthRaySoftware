// NorthRay Capture's window: streaming.
#include "capture_window.hpp"

#include "log.hpp"
#include "output/credentials.hpp"
#include "oauth_clients.hpp"

#include "ncap/rtmp.hpp"

#include "cjui/dialog.hpp"

#include <mfapi.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

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

// The services you can sign in to: -1 for the others.
int accountOf(const std::string& id) {
    if (id == "twitch") return (int)accounts::Service::Twitch;
    if (id == "youtube") return (int)accounts::Service::YouTube;
    return -1;
}

struct TargetResult {
    bool ok = false;
    accounts::Target target;
    std::wstring error;
    int service = 0;
    ~TargetResult() { SecureZeroMemory(target.key.data(), target.key.size()); }
};

const Service& serviceOf(const std::string& id) {
    for (auto& s : kServices)
        if (id == s.id) return s;
    return kServices[0];
}

}  // namespace

void CaptureWindow::initAccounts() {
    oauth::Clients c = oauth::parseClients(kBuiltInOAuthClients);
    // A local copy overrides the built-in registrations (for trying one out without a rebuild).
    std::ifstream f(std::filesystem::path(stateFolder_) / L"oauth-clients.json", std::ios::binary);
    if (f) {
        const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        const oauth::Clients o = oauth::parseClients(text);
        if (o.twitch()) c.twitchClientId = o.twitchClientId;
        if (o.google()) {
            c.googleClientId = o.googleClientId;
            c.googleClientSecret = o.googleClientSecret;
        }
        if (o.twitch() || o.google()) log::info("Using the app registrations in oauth-clients.json (a local override)");
    }
    accounts_.init(hwnd(), WM_APP_ACCOUNTS, c);
    log::info(std::string("Signing in: Twitch ") + (c.twitch() ? "available" : "not set up in this build") + ", YouTube " +
              (c.google() ? "available" : "not set up in this build"));
    accounts_.restore();
}

void CaptureWindow::onAccountChange(int service) {
    const auto s = (accounts::Service)service;
    const accounts::State st = accounts_.state(s);
    if ((int)st.kind != accountKinds_[service]) {
        accountKinds_[service] = (int)st.kind;
        const std::string who = narrow(accounts::label(s));
        if (st.kind == accounts::State::SignedIn) log::info(who + ": signed in as " + narrow(st.name));
        else if (st.kind == accounts::State::SigningIn) log::info(who + ": signing in");
        else log::info(who + ": signed out" + (st.message.empty() ? std::string() : " (" + narrow(st.message) + ")"));
    }
    invalidate();
}

void CaptureWindow::startStreaming() {
    if (streamer_.active() || streamPending_) return;
    const int acc = accountOf(settings_.streamService);
    if (acc >= 0 && accounts_.available((accounts::Service)acc) && accounts_.state((accounts::Service)acc).kind == accounts::State::SignedIn) {
        // The key (and YouTube's server) come fresh from the account: they may have been reset since.
        streamPending_ = true;
        invalidate();
        const HWND h = hwnd();
        accounts_.targetAsync((accounts::Service)acc, [h, acc](bool ok, const accounts::Target& t, const std::wstring& e) {
            auto* r = new TargetResult;
            r->ok = ok;
            r->target = t;
            r->error = e;
            r->service = acc;
            if (!PostMessageW(h, WM_APP_TARGET, 0, (LPARAM)r)) delete r;
        });
        return;
    }
    std::string key = secrets::load(kKeyName);
    startStreamingTo(settings_.streamUrl, key, L"");
    SecureZeroMemory(key.data(), key.size());
}

void CaptureWindow::onStreamTarget(LPARAM lp) {
    std::unique_ptr<TargetResult> r(reinterpret_cast<TargetResult*>(lp));
    streamPending_ = false;
    invalidate();
    const std::wstring who = accounts::label((accounts::Service)r->service);
    if (!r->ok) {
        log::error("Streaming couldn't start: no key from " + narrow(who) + ": " + narrow(r->error));
        cjui::messageBox(hwnd(), L"Capture couldn't get your stream key from " + who + L".\n\n" + r->error, L"Streaming couldn’t start",
                         MB_OK | MB_ICONWARNING);
        return;
    }
    // Twitch: the chosen server (Twitch's automatic one unless changed); YouTube: the account's own.
    const bool twitch = r->service == (int)accounts::Service::Twitch;
    const std::string url = twitch && !settings_.streamUrl.empty() ? settings_.streamUrl : r->target.url;
    startStreamingTo(url, r->target.key, r->target.note);
}

void CaptureWindow::startStreamingTo(const std::string& url, const std::string& key, const std::wstring& note) {
    if (streamer_.active()) return;
    output::StreamSettings ss;
    ss.name = narrow(serviceOf(settings_.streamService).label);
    ss.url = url;
    ss.key = key;
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
              std::to_string(ss.video.fps) + ", " + std::to_string(ss.video.bitrateKbps) + " kbps CBR)" +
              (note.empty() ? std::string() : "; " + narrow(note)));
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

float CaptureWindow::drawAccountRow(int service, float x, float y0, float colW) {
    const auto s = (accounts::Service)service;
    const std::wstring who = accounts::label(s);
    const bool busy = streamer_.active() || streamPending_;
    float y = y0;
    auto row = [&](const std::wstring& help, float controlW) {
        gfx_.text(L"Account", {x, y, colW - controlW - 20, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
        float h = 24;
        if (!help.empty()) h += gfx_.text(help, {x, y + 24, colW - controlW - 20, 80}, Font::Small, pal_.inkMuted);
        return h;
    };
    if (!accounts_.available(s)) {
        const float h = row(L"Signing in to " + who + L" isn't set up in this version of Capture yet (NorthRay's app registration with " + who +
                                L" is still to come). Enter the stream key below instead.",
                            0);
        return std::max(h, 34.0f) + 14;
    }
    const accounts::State st = accounts_.state(s);
    float h = 0;
    if (st.kind == accounts::State::SignedIn) {
        std::wstring help = L"Signed in as " + st.name + L". Capture can only read your stream key" +
                            (s == accounts::Service::YouTube ? L" and server" : L"") + L": it can't post, change your channel or read your messages.";
        if (!st.message.empty() && st.message != L"Signed in.") help += L"\n" + st.message;
        h = row(help, 150);
        button("acc:out", {x + colW - 150, y - 2, 150, 32}, L"Sign out", [this, s, busy] {
            if (busy) {
                flash(L"Stop streaming to sign out.", 4);
                return;
            }
            accounts_.signOut(s);
        });
    } else if (st.kind == accounts::State::SigningIn) {
        std::wstring help = st.message;
        if (!st.userCode.empty()) help += L"\nIf the page didn't open: twitch.tv/activate, code " + st.userCode + L".";
        h = row(help, st.userCode.empty() ? 150 : 340);
        button("acc:cancel", {x + colW - 150, y - 2, 150, 32}, L"Cancel", [this, s] { accounts_.cancel(s); });
        if (!st.userCode.empty())
            gfx_.text(st.userCode, {x + colW - 330, y - 2, 170, 32}, Font::Title, pal_.ink, {Align::Right, false, true});
    } else {
        std::wstring help = L"Sign in and Capture gets your stream key itself" +
                            std::wstring(s == accounts::Service::YouTube ? L" (and YouTube's server)" : L"") +
                            L". It can only read the key: it can't post, change your channel or read your messages.";
        if (!st.message.empty()) help += L"\n" + st.message;
        h = row(help, 190);
        button("acc:in", {x + colW - 190, y - 2, 190, 32}, L"Sign in with " + who + L"…", [this, s, busy] {
            if (busy) return;
            accounts_.signIn(s);
        });
    }
    return std::max(h, 34.0f) + 14;
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
    const int acc = accountOf(settings_.streamService);
    const bool fromAccount = acc >= 0 && accounts_.available((accounts::Service)acc) &&
                             accounts_.state((accounts::Service)acc).kind == accounts::State::SignedIn;
    if (acc >= 0) y += drawAccountRow(acc, x, y, colW);
    if (fromAccount && acc == (int)accounts::Service::YouTube) {
        const float h = row(L"Server", L"From your YouTube account (its secure RTMPS server) each time you go live.", 0);
        y += std::max(h, 34.0f) + 14;
    } else {
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
    if (fromAccount) {
        const float h = row(L"Stream key",
                            std::wstring(L"Comes from your ") + accounts::label((accounts::Service)acc) +
                                L" account each time you go live (so a key reset on the website is picked up). It's never stored.",
                            0);
        y += std::max(h, 34.0f) + 14;
    } else {
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
