// NorthRay Capture: Twitch and YouTube accounts (the flows themselves are described in ncap/oauth.hpp).
//
// What's kept: per service, one Windows Credential Manager entry ("NorthRayCapture/account/<service>")
// holding the refresh token and the account's name. Access tokens are only ever in memory. Signing out
// removes the entry at once and revokes the tokens with the service. Nothing here writes to a log.
//
// Everything that touches the network runs on a worker thread; the window hears about changes through
// a posted message (wParam: the service) and asks for the state.
#pragma once

#include "ncap/oauth.hpp"

#include <windows.h>

#include <atomic>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace ncap::accounts {

enum class Service { Twitch = 0, YouTube = 1 };
const wchar_t* label(Service s);

struct State {
    enum Kind { SignedOut, SigningIn, SignedIn } kind = SignedOut;
    std::wstring name;       // the Twitch login or the YouTube channel
    std::wstring message;    // what's happening, or the last problem, in plain words
    std::wstring userCode;   // Twitch: the code shown on twitch.tv/activate, to check it matches
};

// Where to stream and the key, from the account.
struct Target {
    std::string url, key;
    std::wstring note;       // "YouTube stream: Default stream key"
};

class Accounts {
public:
    Accounts();
    ~Accounts();
    // `notify`/`message`: posted on every change (may be null in tests). `openUrl`: shows a page in the
    // person's browser (ShellExecute by default).
    void init(HWND notify, UINT message, oauth::Clients clients, oauth::Endpoints endpoints = {},
              std::function<void(const std::string&)> openUrl = {});
    bool available(Service s) const;   // this build (or the local override) has an app registration for it
    State state(Service s) const;

    // Reads the stored sign-ins; Twitch's is validated in the background (Twitch asks apps to, at start).
    void restore();
    void signIn(Service s);
    void cancel(Service s);
    void signOut(Service s);
    // Call now and then (the window's timer): Twitch's sign-in is validated every hour while kept.
    void tick();

    // Blocking (run it on a worker): a fresh key and server from the account.
    bool target(Service s, Target& out, std::wstring& error);
    // The same on a worker; `done` runs on that worker.
    void targetAsync(Service s, std::function<void(bool ok, const Target&, const std::wstring& error)> done);

private:
    struct Slot {
        mutable std::mutex m;      // state
        std::mutex tokenM;         // one refresh at a time (Twitch's refresh tokens are single-use)
        State st;
        std::string refresh, access, userId;
        ULONGLONG accessUntil = 0, validatedAt = 0;
        std::atomic<bool> cancel{false};
        std::atomic<int> generation{0};   // bumped by sign-in/out: stale workers drop their results
    };
    Slot& slot(Service s) { return slots_[(int)s]; }
    const Slot& slot(Service s) const { return slots_[(int)s]; }
    void set(Service s, const std::function<void(State&)>& f);
    void run(std::function<void()> f);
    void save(Service s);
    void forget(Service s, const std::wstring& why);

    void twitchSignIn(int gen);
    void youtubeSignIn(int gen);
    // A usable access token ("" with `error` if not); `force` refreshes even if the cached one looks fresh.
    std::string accessToken(Service s, bool force, std::wstring& error, bool& signedOut);
    bool twitchValidate(std::wstring& error);

    HWND notify_ = nullptr;
    UINT message_ = 0;
    oauth::Clients clients_;
    oauth::Endpoints ep_;
    std::function<void(const std::string&)> openUrl_;
    Slot slots_[2];
    std::mutex workersM_;
    std::list<std::pair<std::thread, std::shared_ptr<std::atomic<bool>>>> workers_;
    std::atomic<bool> quitting_{false};
};

}  // namespace ncap::accounts
