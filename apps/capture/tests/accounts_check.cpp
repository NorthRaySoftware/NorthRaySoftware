// Runs Capture's account code (Windows) against a local stand-in for Twitch and Google (tools/accounts-mock.py): tools/check-accounts.sh.
#include "output/accounts.hpp"
#include "output/credentials.hpp"
#include "output/http.hpp"
#include <cstdio>
#include <thread>
using namespace ncap;
using accounts::Service;
using accounts::State;
static const std::string M = "http://127.0.0.1:18088";
static int fails = 0;
#define EXPECT(c) do { if (c) std::printf("  ok   %s\n", #c); else { ++fails; std::printf("  FAIL %s (line %d)\n", #c, __LINE__); } } while (0)
static std::string mode = "approve";   // what the "browser" does
static std::string narrowW(const std::wstring& w) { std::string s; for (wchar_t c : w) s += c < 128 ? (char)c : '?'; return s; }

static void browser(const std::string& url) {
    std::thread([url] {
        if (url.rfind("https://www.twitch.tv/activate", 0) == 0) {
            if (mode == "approve") { Sleep(1500); http::request("GET", M + "/approve"); }
            return;
        }
        // Google: read redirect_uri, state, code_challenge from the URL, as Google would.
        auto q = oauth::parseQuery(url.substr(url.find('?') + 1));
        http::postForm(M + "/expect", oauth::form({{"challenge", q["code_challenge"]}}));
        if (q["code_challenge_method"] != "S256" || q["scope"] != oauth::kGoogleScopes) { std::printf("  FAIL bad auth url\n"); ++fails; }
        Sleep(500);
        const std::string r = q["redirect_uri"];
        if (mode == "deny") { http::request("GET", r + "?error=access_denied&state=" + oauth::urlEncode(q["state"])); return; }
        auto fav = http::request("GET", r + "favicon.ico");
        auto forged = http::request("GET", r + "?code=evil&state=forged");
        auto ok = http::request("GET", r + "?state=" + oauth::urlEncode(q["state"]) + "&code=gcode1&scope=x");
        std::printf("  browser: favicon %d, forged %d (%s), real %d (%s)\n", fav.status, forged.status,
                    forged.body.find("didn't come from") != std::string::npos ? "ignored" : "?", ok.status,
                    ok.body.find("Signed in") != std::string::npos ? "signed in page" : "?");
    }).detach();
}

static bool waitFor(accounts::Accounts& a, Service s, State::Kind k, int ms) {
    for (int i = 0; i < ms / 50; ++i) { if (a.state(s).kind == k) return true; Sleep(50); }
    return false;
}

int main() {
    oauth::Clients c = oauth::parseClients(R"({"twitch":{"clientId":"tw-client"},"google":{"clientId":"g-client","clientSecret":"g-secret"}})");
    oauth::Endpoints e;
    e.twitchDevice = M + "/twitch/device"; e.twitchToken = M + "/twitch/token"; e.twitchValidate = M + "/twitch/validate";
    e.twitchRevoke = M + "/twitch/revoke"; e.twitchStreamKey = M + "/twitch/key"; e.googleAuth = "https://accounts.example/auth";
    e.googleToken = M + "/google/token"; e.googleRevoke = M + "/google/revoke"; e.youtubeStreams = M + "/yt/streams"; e.youtubeChannels = M + "/yt/channels";
    secrets::remove("account/twitch"); secrets::remove("account/youtube");
    {
        std::printf("twitch sign-in\n");
        accounts::Accounts a; a.init(nullptr, 0, c, e, browser);
        EXPECT(a.available(Service::Twitch) && a.available(Service::YouTube));
        a.signIn(Service::Twitch);
        Sleep(800);
        EXPECT(a.state(Service::Twitch).userCode == L"WXYZ1234");
        EXPECT(waitFor(a, Service::Twitch, State::SignedIn, 8000));
        EXPECT(a.state(Service::Twitch).name == L"cjtest");
        accounts::Target t; std::wstring err;
        EXPECT(a.target(Service::Twitch, t, err) && t.key == "live_42_mockkey" && t.url == "rtmps://ingest.global-contribute.live-video.net/app");
        http::request("GET", M + "/expire");   // the access token stops working: Capture refreshes (single-use rotation)
        EXPECT(a.target(Service::Twitch, t, err) && t.key == "live_42_mockkey");
        const std::string stored = secrets::load("account/twitch");
        EXPECT(stored.find("trt2") != std::string::npos && stored.find("tat") == std::string::npos);   // new refresh kept, no access token stored
    }
    {
        std::printf("twitch restored from Credential Manager\n");
        accounts::Accounts a; a.init(nullptr, 0, c, e, browser);
        a.restore();
        EXPECT(a.state(Service::Twitch).kind == State::SignedIn && a.state(Service::Twitch).name == L"cjtest");
        accounts::Target t; std::wstring err;
        EXPECT(a.target(Service::Twitch, t, err) && t.key == "live_42_mockkey");
        a.signOut(Service::Twitch);
        EXPECT(a.state(Service::Twitch).kind == State::SignedOut && !secrets::exists("account/twitch"));
        Sleep(1500);
        EXPECT(!a.target(Service::Twitch, t, err));
        std::printf("  after sign-out: %s\n", narrowW(err).c_str());
    }
    {
        std::printf("twitch: code never approved\n");
        http::request("GET", M + "/setexpires?s=3");
        mode = "ignore";
        accounts::Accounts a; a.init(nullptr, 0, c, e, browser);
        a.signIn(Service::Twitch);
        EXPECT(waitFor(a, Service::Twitch, State::SignedOut, 10000));
        std::printf("  message: %s\n", narrowW(a.state(Service::Twitch).message).c_str());
        a.signIn(Service::Twitch); Sleep(500); a.cancel(Service::Twitch);
        EXPECT(a.state(Service::Twitch).kind == State::SignedOut && a.state(Service::Twitch).message == L"Sign-in cancelled.");
        http::request("GET", M + "/setexpires?s=600");
    }
    {
        std::printf("youtube sign-in\n");
        mode = "approve";
        accounts::Accounts a; a.init(nullptr, 0, c, e, browser);
        a.signIn(Service::YouTube);
        EXPECT(waitFor(a, Service::YouTube, State::SignedIn, 10000));
        EXPECT(a.state(Service::YouTube).name == L"CJ Test Channel");
        accounts::Target t; std::wstring err;
        EXPECT(a.target(Service::YouTube, t, err) && t.key == "yt-mock-key" && t.url == "rtmps://a.rtmps.youtube.com/live2");
        http::request("GET", M + "/expire");
        EXPECT(a.target(Service::YouTube, t, err) && t.key == "yt-mock-key");
        a.signOut(Service::YouTube);
        Sleep(1500);
        auto st = http::request("GET", M + "/state");
        EXPECT(st.body.find("google:grt1") != std::string::npos && !secrets::exists("account/youtube"));
        std::printf("youtube: cancelled in the browser\n");
        mode = "deny";
        a.signIn(Service::YouTube);
        EXPECT(waitFor(a, Service::YouTube, State::SignedOut, 10000) && a.state(Service::YouTube).message == L"The Google sign-in was cancelled.");
    }
    {
        std::printf("not set up in this build\n");
        accounts::Accounts a; a.init(nullptr, 0, oauth::Clients{}, e, browser);
        accounts::Target t; std::wstring err;
        EXPECT(!a.available(Service::Twitch) && !a.target(Service::Twitch, t, err));
        std::printf("  %s\n", narrowW(err).c_str());
    }
    std::printf("%s (%d failed)\n", fails ? "FAILED" : "all passed", fails);
    return fails ? 1 : 0;
}
