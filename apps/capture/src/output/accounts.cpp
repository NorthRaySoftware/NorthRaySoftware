#include "output/accounts.hpp"

#include "output/credentials.hpp"
#include "output/http.hpp"
#include "output/net.hpp"

#include <bcrypt.h>
#include <shellapi.h>

#include <nlohmann/json.hpp>

namespace ncap::accounts {

namespace {
const char* kTwitchIngest = "rtmps://ingest.global-contribute.live-video.net/app";

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
const char* key(Service s) { return s == Service::Twitch ? "account/twitch" : "account/youtube"; }

bool randomBytes(std::uint8_t* p, std::size_t n) { return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, p, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG)); }

std::wstring errorOf(const http::Response& r) {
    if (!r.status) return wide(r.error);
    return wide(oauth::describeError(r.status, r.body));
}

void sendAll(SOCKET s, const std::string& data) {
    std::size_t at = 0;
    while (at < data.size()) {
        const int n = ::send(s, data.data() + at, (int)(data.size() - at), 0);
        if (n <= 0) return;
        at += (std::size_t)n;
    }
}

void answer(SOCKET c, int status, const char* title, const char* text) {
    const std::string page =
        std::string("<!doctype html><meta charset=utf-8><title>NorthRay Capture</title>"
                    "<body style=\"font:16px Segoe UI,sans-serif;background:#0d1220;color:#e8ecf5;display:grid;place-items:center;height:90vh\">"
                    "<div style=\"max-width:30em\"><h2>") +
        title + "</h2><p>" + text + "</p></div>";
    const std::string head = "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK" : " Not Found") +
                             "\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: " +
                             std::to_string(page.size()) + "\r\n\r\n";
    sendAll(c, head + page);
}
}  // namespace

const wchar_t* label(Service s) { return s == Service::Twitch ? L"Twitch" : L"YouTube"; }

Accounts::Accounts() = default;

Accounts::~Accounts() {
    quitting_ = true;
    for (auto& sl : slots_) sl.cancel = true;
    std::lock_guard<std::mutex> l(workersM_);
    for (auto& w : workers_)
        if (w.first.joinable()) w.first.join();
}

void Accounts::init(HWND notify, UINT message, oauth::Clients clients, oauth::Endpoints endpoints,
                    std::function<void(const std::string&)> openUrl) {
    notify_ = notify;
    message_ = message;
    clients_ = std::move(clients);
    ep_ = std::move(endpoints);
    openUrl_ = openUrl ? std::move(openUrl) : [](const std::string& url) {
        ShellExecuteW(nullptr, L"open", wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    };
}

bool Accounts::available(Service s) const { return s == Service::Twitch ? clients_.twitch() : clients_.google(); }

State Accounts::state(Service s) const {
    std::lock_guard<std::mutex> l(slot(s).m);
    return slot(s).st;
}

void Accounts::set(Service s, const std::function<void(State&)>& f) {
    {
        std::lock_guard<std::mutex> l(slot(s).m);
        f(slot(s).st);
    }
    if (notify_ && !quitting_) PostMessageW(notify_, message_, (WPARAM)s, 0);
}

void Accounts::run(std::function<void()> f) {
    std::lock_guard<std::mutex> l(workersM_);
    // Tidy finished workers.
    for (auto it = workers_.begin(); it != workers_.end();) {
        if (it->second->load()) {
            it->first.join();
            it = workers_.erase(it);
        } else ++it;
    }
    if (quitting_) return;
    auto done = std::make_shared<std::atomic<bool>>(false);
    workers_.emplace_back(std::thread([f = std::move(f), done] {
                              f();
                              *done = true;
                          }),
                          done);
}

void Accounts::save(Service s) {
    Slot& sl = slot(s);
    nlohmann::json j;
    {
        std::lock_guard<std::mutex> l(sl.m);
        j = {{"refresh", sl.refresh}, {"userId", sl.userId}, {"name", narrow(sl.st.name)}};
    }
    secrets::save(key(s), j.dump());
}

void Accounts::forget(Service s, const std::wstring& why) {
    Slot& sl = slot(s);
    secrets::remove(key(s));
    {
        std::lock_guard<std::mutex> l(sl.m);
        sl.refresh.clear();
        sl.access.clear();
        sl.userId.clear();
        sl.accessUntil = 0;
    }
    set(s, [&](State& st) {
        st = State{};
        st.message = why;
    });
}

void Accounts::restore() {
    for (Service s : {Service::Twitch, Service::YouTube}) {
        const std::string text = secrets::load(key(s));
        if (text.empty()) continue;
        const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        if (!j.is_object() || !j.contains("refresh") || !j["refresh"].is_string() || j["refresh"].get<std::string>().empty()) {
            secrets::remove(key(s));
            continue;
        }
        Slot& sl = slot(s);
        {
            std::lock_guard<std::mutex> l(sl.m);
            sl.refresh = j["refresh"].get<std::string>();
            sl.userId = j.value("userId", std::string());
            sl.st.kind = State::SignedIn;
            sl.st.name = wide(j.value("name", std::string()));
        }
        set(s, [](State&) {});
    }
    if (available(Service::Twitch) && state(Service::Twitch).kind == State::SignedIn) {
        slot(Service::Twitch).validatedAt = GetTickCount64();
        const int gen = slot(Service::Twitch).generation;
        run([this, gen] {
            std::wstring e;
            if (!twitchValidate(e) && gen == slot(Service::Twitch).generation && state(Service::Twitch).kind == State::SignedIn)
                set(Service::Twitch, [&](State& st) { st.message = e; });
        });
    }
}

void Accounts::signIn(Service s) {
    if (!available(s)) return;
    Slot& sl = slot(s);
    sl.cancel = true;   // a sign-in already waiting gives way
    const int gen = ++sl.generation;
    sl.cancel = false;
    set(s, [](State& st) {
        st.kind = State::SigningIn;
        st.message = L"Starting…";
        st.userCode.clear();
    });
    if (s == Service::Twitch) run([this, gen] { twitchSignIn(gen); });
    else run([this, gen] { youtubeSignIn(gen); });
}

void Accounts::cancel(Service s) {
    Slot& sl = slot(s);
    if (state(s).kind != State::SigningIn) return;
    sl.cancel = true;
    ++sl.generation;
    // Back to whatever was kept before (a sign-in can replace an existing one).
    const bool had = !secrets::load(key(s)).empty();
    set(s, [&](State& st) {
        st.kind = had ? State::SignedIn : State::SignedOut;
        st.message = L"Sign-in cancelled.";
        st.userCode.clear();
    });
}

void Accounts::signOut(Service s) {
    Slot& sl = slot(s);
    sl.cancel = true;
    ++sl.generation;
    std::string refresh, access;
    {
        std::lock_guard<std::mutex> l(sl.m);
        refresh = sl.refresh;
        access = sl.access;
    }
    forget(s, L"Signed out.");
    // Revoke with the service too (best effort: the local sign-in is gone either way).
    if (!available(s)) return;
    run([this, s, refresh, access] {
        if (s == Service::Twitch) {
            std::string a = access;
            if (a.empty() && !refresh.empty()) {   // revoke takes an access token: trade the refresh token for one
                auto r = http::postForm(ep_.twitchToken, oauth::form({{"client_id", clients_.twitchClientId}, {"grant_type", "refresh_token"},
                                                                      {"refresh_token", refresh}}));
                a = oauth::parseTokens(r.status, r.body).access;
            }
            if (!a.empty()) http::postForm(ep_.twitchRevoke, oauth::form({{"client_id", clients_.twitchClientId}, {"token", a}}));
        } else if (!refresh.empty()) {
            http::postForm(ep_.googleRevoke, oauth::form({{"token", refresh}}));
        }
    });
}

void Accounts::tick() {
    if (!available(Service::Twitch) || state(Service::Twitch).kind != State::SignedIn) return;
    Slot& sl = slot(Service::Twitch);
    const ULONGLONG now = GetTickCount64();
    if (sl.validatedAt && now - sl.validatedAt < 3600 * 1000ULL) return;
    sl.validatedAt = now;   // one attempt per hour, even if it fails for lack of a connection
    const int gen = sl.generation;
    run([this, gen] {
        std::wstring e;
        if (!twitchValidate(e) && gen == slot(Service::Twitch).generation && state(Service::Twitch).kind == State::SignedIn)
            set(Service::Twitch, [&](State& st) { st.message = e; });
    });
}

// --- Twitch ---

void Accounts::twitchSignIn(int gen) {
    Slot& sl = slot(Service::Twitch);
    auto stale = [&] { return quitting_ || sl.cancel || gen != sl.generation; };
    auto fail = [&](const std::wstring& why) {
        if (stale()) return;
        const bool had = !secrets::load(key(Service::Twitch)).empty();
        set(Service::Twitch, [&](State& st) {
            st.kind = had ? State::SignedIn : State::SignedOut;
            st.message = why;
            st.userCode.clear();
        });
    };
    auto r = http::postForm(ep_.twitchDevice, oauth::form({{"client_id", clients_.twitchClientId}, {"scopes", oauth::kTwitchScopes}}));
    oauth::DeviceCode dc = oauth::parseDeviceCode(r.status, r.body);
    if (!r.status) return fail(wide(r.error));
    if (!dc.error.empty()) return fail(wide(dc.error));
    if (stale()) return;
    set(Service::Twitch, [&](State& st) {
        st.userCode = wide(dc.userCode);
        st.message = L"Approve NorthRay Capture on the Twitch page that opened in your browser. Check it shows the code " + st.userCode + L".";
    });
    openUrl_(dc.verificationUri);
    int interval = dc.interval;
    const ULONGLONG until = GetTickCount64() + (ULONGLONG)dc.expiresIn * 1000;
    oauth::Tokens tok;
    for (;;) {
        for (int i = 0; i < interval * 10; ++i) {
            if (stale()) return;
            Sleep(100);
        }
        if (GetTickCount64() > until) return fail(L"The Twitch code expired before it was approved. Try again.");
        r = http::postForm(ep_.twitchToken, oauth::form({{"client_id", clients_.twitchClientId}, {"scopes", oauth::kTwitchScopes},
                                                         {"device_code", dc.deviceCode},
                                                         {"grant_type", "urn:ietf:params:oauth:grant-type:device_code"}}));
        tok = oauth::parseTokens(r.status, r.body);
        const oauth::Poll p = oauth::classifyPoll(tok);
        if (p == oauth::Poll::Done) break;
        if (p == oauth::Poll::Failed) {
            if (tok.error == "invalid device code") return fail(L"The Twitch code expired or was already used. Try again.");
            if (tok.error == "access_denied" || tok.error == "authorization_denied") return fail(L"The sign-in wasn't approved on Twitch.");
            return fail(L"Twitch didn't complete the sign-in (" + wide(tok.error) + L").");
        }
        if (tok.error == "slow_down") interval += 5;
    }
    if (stale()) return;
    // Who is it?
    r = http::request("GET", ep_.twitchValidate, {{"Authorization", "OAuth " + tok.access}});
    const oauth::TwitchUser u = oauth::parseValidate(r.status, r.body);
    if (!u.valid) return fail(L"Twitch signed in but didn't say which account: " + errorOf(r));
    {
        std::lock_guard<std::mutex> l(sl.m);
        sl.refresh = tok.refresh;
        sl.access = tok.access;
        sl.accessUntil = GetTickCount64() + (ULONGLONG)std::max(0, tok.expiresIn - 120) * 1000;
        sl.userId = u.userId;
        sl.validatedAt = GetTickCount64();
        sl.st.name = wide(u.login);
    }
    save(Service::Twitch);
    set(Service::Twitch, [&](State& st) {
        st.kind = State::SignedIn;
        st.userCode.clear();
        st.message = L"Signed in.";
    });
}

bool Accounts::twitchValidate(std::wstring& error) {
    bool signedOut = false;
    std::string a = accessToken(Service::Twitch, false, error, signedOut);
    if (a.empty()) return false;
    auto r = http::request("GET", ep_.twitchValidate, {{"Authorization", "OAuth " + a}});
    if (r.status == 401) {   // revoked or expired early: try once with a fresh one
        a = accessToken(Service::Twitch, true, error, signedOut);
        if (a.empty()) return false;
        r = http::request("GET", ep_.twitchValidate, {{"Authorization", "OAuth " + a}});
        if (r.status == 401) {
            forget(Service::Twitch, L"Twitch no longer accepts this sign-in (it was disconnected on Twitch?). Sign in again.");
            error = state(Service::Twitch).message;
            return false;
        }
    }
    if (r.status != 200) {
        error = errorOf(r);
        return false;
    }
    slot(Service::Twitch).validatedAt = GetTickCount64();
    return true;
}

// --- YouTube ---

void Accounts::youtubeSignIn(int gen) {
    Slot& sl = slot(Service::YouTube);
    auto stale = [&] { return quitting_ || sl.cancel || gen != sl.generation; };
    auto fail = [&](const std::wstring& why) {
        if (stale()) return;
        const bool had = !secrets::load(key(Service::YouTube)).empty();
        set(Service::YouTube, [&](State& st) {
            st.kind = had ? State::SignedIn : State::SignedOut;
            st.message = why;
        });
    };
    std::uint8_t rnd[32], st8[16];
    if (!randomBytes(rnd, sizeof rnd) || !randomBytes(st8, sizeof st8)) return fail(L"Windows couldn't make a random sign-in code.");
    const std::string verifier = oauth::pkceVerifier(rnd), state = oauth::base64url(st8, sizeof st8);
    SecureZeroMemory(rnd, sizeof rnd);

    // A one-off listener on this PC only, on a port Windows picks.
    net::startNetworking();
    SOCKET ls = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) return fail(L"Couldn't start the sign-in listener.");
    struct Closer {
        SOCKET s;
        ~Closer() { closesocket(s); }
    } closeLs{ls};
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    int alen = sizeof a;
    if (::bind(ls, (sockaddr*)&a, sizeof a) != 0 || ::listen(ls, 4) != 0 || ::getsockname(ls, (sockaddr*)&a, &alen) != 0)
        return fail(L"Couldn't start the sign-in listener.");
    const std::string redirect = "http://127.0.0.1:" + std::to_string(ntohs(a.sin_port)) + "/";
    if (stale()) return;
    set(Service::YouTube, [](State& s) { s.message = L"Finish signing in with Google in your browser."; });
    openUrl_(oauth::googleAuthUrl(ep_, clients_, redirect, state, oauth::pkceChallenge(verifier)));

    std::string code;
    const ULONGLONG until = GetTickCount64() + 10 * 60 * 1000;
    while (code.empty()) {
        if (stale()) return;
        if (GetTickCount64() > until) return fail(L"The Google sign-in wasn't finished within 10 minutes. Try again.");
        fd_set fs;
        FD_ZERO(&fs);
        FD_SET(ls, &fs);
        timeval tv{0, 200000};
        if (::select(0, &fs, nullptr, nullptr, &tv) <= 0) continue;
        SOCKET c = ::accept(ls, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        Closer closeC{c};
        DWORD to = 5000;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof to);
        std::string head;
        char buf[2048];
        while (head.find("\r\n") == std::string::npos && head.size() < 8192) {
            const int n = ::recv(c, buf, sizeof buf, 0);
            if (n <= 0) break;
            head.append(buf, (std::size_t)n);
        }
        const oauth::Redirect rd = oauth::parseRedirect(head.substr(0, head.find("\r\n")), state);
        if (!rd.isCallback) {
            answer(c, 404, "Not here", "This address is only for finishing a NorthRay Capture sign-in.");
            continue;
        }
        if (!rd.stateOk) {   // not from this sign-in: ignore it and keep waiting
            answer(c, 200, "This sign-in didn't come from NorthRay Capture", "Nothing was changed. You can close this tab.");
            continue;
        }
        if (!rd.error.empty()) {
            answer(c, 200, "Sign-in cancelled", "NorthRay Capture wasn't given access. You can close this tab.");
            return fail(rd.error == "access_denied" ? L"The Google sign-in was cancelled." : L"Google didn't complete the sign-in (" + wide(rd.error) + L").");
        }
        answer(c, 200, "Signed in", "You can close this tab and go back to NorthRay Capture.");
        code = rd.code;
    }
    std::vector<std::pair<std::string, std::string>> f = {{"client_id", clients_.googleClientId}, {"code", code}, {"code_verifier", verifier},
                                                          {"grant_type", "authorization_code"}, {"redirect_uri", redirect}};
    if (!clients_.googleClientSecret.empty()) f.push_back({"client_secret", clients_.googleClientSecret});
    auto r = http::postForm(ep_.googleToken, oauth::form(f));
    const oauth::Tokens tok = oauth::parseTokens(r.status, r.body);
    if (!tok.ok()) return fail(L"Google didn't complete the sign-in: " + (r.status ? wide(tok.error) : wide(r.error)));
    if (tok.refresh.empty()) return fail(L"Google didn't allow Capture to stay signed in. Try again.");
    if (stale()) return;
    r = http::request("GET", ep_.youtubeChannels + "?part=snippet&mine=true", {{"Authorization", "Bearer " + tok.access}});
    std::string name = oauth::parseYouTubeChannel(r.status, r.body);
    {
        std::lock_guard<std::mutex> l(sl.m);
        sl.refresh = tok.refresh;
        sl.access = tok.access;
        sl.accessUntil = GetTickCount64() + (ULONGLONG)std::max(0, tok.expiresIn - 120) * 1000;
        sl.st.name = name.empty() ? L"your Google account" : wide(name);
    }
    save(Service::YouTube);
    set(Service::YouTube, [&](State& st) {
        st.kind = State::SignedIn;
        st.message = name.empty() ? L"Signed in, but this Google account has no YouTube channel yet." : L"Signed in.";
    });
}

// --- tokens and targets ---

std::string Accounts::accessToken(Service s, bool force, std::wstring& error, bool& signedOut) {
    Slot& sl = slot(s);
    std::lock_guard<std::mutex> one(sl.tokenM);
    std::string refresh;
    {
        std::lock_guard<std::mutex> l(sl.m);
        if (sl.st.kind != State::SignedIn) {
            error = std::wstring(L"Not signed in to ") + label(s) + L".";
            signedOut = true;
            return {};
        }
        if (!force && !sl.access.empty() && GetTickCount64() < sl.accessUntil) return sl.access;
        refresh = sl.refresh;
    }
    std::vector<std::pair<std::string, std::string>> f = {{"grant_type", "refresh_token"}, {"refresh_token", refresh}};
    if (s == Service::Twitch) f.push_back({"client_id", clients_.twitchClientId});
    else {
        f.push_back({"client_id", clients_.googleClientId});
        if (!clients_.googleClientSecret.empty()) f.push_back({"client_secret", clients_.googleClientSecret});
    }
    const int gen = sl.generation;
    auto r = http::postForm(s == Service::Twitch ? ep_.twitchToken : ep_.googleToken, oauth::form(f));
    const oauth::Tokens t = oauth::parseTokens(r.status, r.body);
    if (!t.ok()) {
        if (!r.status || r.status >= 500) {   // no connection or the service's own trouble: keep the sign-in
            error = errorOf(r);
            return {};
        }
        if (gen == sl.generation)
            forget(s, std::wstring(label(s)) + L" no longer accepts this sign-in (it expires after a month unused, or was disconnected). Sign in again.");
        error = state(s).message;
        signedOut = true;
        return {};
    }
    {
        std::lock_guard<std::mutex> l(sl.m);
        if (gen != sl.generation) {
            error = L"Signed out meanwhile.";
            signedOut = true;
            return {};
        }
        if (!t.refresh.empty()) sl.refresh = t.refresh;   // Twitch's are single-use: keep the new one at once
        sl.access = t.access;
        sl.accessUntil = GetTickCount64() + (ULONGLONG)std::max(0, t.expiresIn - 120) * 1000;
    }
    if (!t.refresh.empty()) save(s);
    return t.access;
}

bool Accounts::target(Service s, Target& out, std::wstring& error) {
    if (!available(s)) {
        error = std::wstring(L"Signing in to ") + label(s) + L" isn't set up in this version of Capture.";
        return false;
    }
    bool signedOut = false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const std::string a = accessToken(s, attempt > 0, error, signedOut);
        if (a.empty()) return false;
        http::Response r;
        if (s == Service::Twitch) {
            std::string userId;
            {
                std::lock_guard<std::mutex> l(slot(s).m);
                userId = slot(s).userId;
            }
            r = http::request("GET", ep_.twitchStreamKey + "?broadcaster_id=" + oauth::urlEncode(userId),
                              {{"Authorization", "Bearer " + a}, {"Client-Id", clients_.twitchClientId}});
            if (r.status == 401 && attempt == 0) continue;
            std::string msg;
            const std::string k = oauth::parseTwitchKey(r.status, r.body, msg);
            if (k.empty()) {
                error = wide(msg);
                return false;
            }
            out.url = kTwitchIngest;
            out.key = k;
            out.note = L"Twitch's automatic server";
            return true;
        }
        r = http::request("GET", ep_.youtubeStreams + "?part=snippet,cdn,contentDetails&mine=true&maxResults=50", {{"Authorization", "Bearer " + a}});
        if (r.status == 401 && attempt == 0) continue;
        std::string msg;
        const auto streams = oauth::parseYouTubeStreams(r.status, r.body, msg);
        if (!msg.empty()) {
            error = L"YouTube: " + wide(msg);
            return false;
        }
        if (streams.empty()) {
            error = L"Your YouTube channel has no stream key yet. Open YouTube Studio › Go live once (the first time, YouTube can take up to "
                    L"24 hours to turn live streaming on), then try again.";
            return false;
        }
        const oauth::YouTubeStream& y = streams.front();
        out.url = y.rtmps.empty() ? y.rtmp : y.rtmps;
        out.key = y.key;
        out.note = L"YouTube stream: " + (y.title.empty() ? std::wstring(L"untitled") : wide(y.title));
        return true;
    }
    return false;
}

void Accounts::targetAsync(Service s, std::function<void(bool, const Target&, const std::wstring&)> done) {
    run([this, s, done = std::move(done)] {
        Target t;
        std::wstring e;
        const bool ok = target(s, t, e);
        if (!quitting_) done(ok, t, e);
        SecureZeroMemory(t.key.data(), t.key.size());
    });
}

}  // namespace ncap::accounts
