// NorthRay Capture: signing in to Twitch and YouTube (the portable part: no network here).
//
// Twitch: the device code flow for public clients (no client secret): Capture asks Twitch for a code,
// the person approves it on twitch.tv/activate in their browser, Capture polls for the tokens. Refresh
// tokens are single-use; access tokens last about 4 hours and are validated at start and hourly.
// Scope: channel:read:stream_key, nothing else.
//
// YouTube: Google's installed-app flow: the browser opens Google's sign-in, which redirects to a
// one-off listener on 127.0.0.1 with the code; PKCE (S256) and a random state guard it. Scope:
// youtube.readonly (enough to read the channel's stream key and ingest addresses).
//
// Nothing here logs or returns tokens in messages; callers keep tokens in Windows Credential Manager.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ncap::oauth {

// --- encoding ---
std::array<std::uint8_t, 32> sha256(const void* data, std::size_t size);
std::string base64url(const std::uint8_t* data, std::size_t size);   // no padding
std::string urlEncode(const std::string& s);                          // RFC 3986 unreserved kept
std::string urlDecode(const std::string& s);                          // '+' as space
std::string form(const std::vector<std::pair<std::string, std::string>>& fields);
std::map<std::string, std::string> parseQuery(const std::string& query);

// PKCE: a verifier from 32 random bytes (43 characters) and its S256 challenge.
std::string pkceVerifier(const std::uint8_t (&random)[32]);
std::string pkceChallenge(const std::string& verifier);

// The app registrations (client IDs). Public values: a desktop app can't keep a secret, and Google's
// "client secret" for desktop clients isn't confidential either (Google's own wording).
struct Clients {
    std::string twitchClientId;
    std::string googleClientId, googleClientSecret;
    bool twitch() const { return !twitchClientId.empty(); }
    bool google() const { return !googleClientId.empty(); }
};
// {"twitch":{"clientId":"…"},"google":{"clientId":"…","clientSecret":"…"}}; missing parts stay empty.
Clients parseClients(const std::string& json);

// Where requests go. The real services by default; tests point these at a local stand-in.
struct Endpoints {
    std::string twitchDevice = "https://id.twitch.tv/oauth2/device";
    std::string twitchToken = "https://id.twitch.tv/oauth2/token";
    std::string twitchValidate = "https://id.twitch.tv/oauth2/validate";
    std::string twitchRevoke = "https://id.twitch.tv/oauth2/revoke";
    std::string twitchStreamKey = "https://api.twitch.tv/helix/streams/key";
    std::string googleAuth = "https://accounts.google.com/o/oauth2/v2/auth";
    std::string googleToken = "https://oauth2.googleapis.com/token";
    std::string googleRevoke = "https://oauth2.googleapis.com/revoke";
    std::string youtubeStreams = "https://www.googleapis.com/youtube/v3/liveStreams";
    std::string youtubeChannels = "https://www.googleapis.com/youtube/v3/channels";
};

constexpr const char* kTwitchScopes = "channel:read:stream_key";
constexpr const char* kGoogleScopes = "https://www.googleapis.com/auth/youtube.readonly";

// --- responses ---
struct Tokens {
    std::string access, refresh;
    int expiresIn = 0;
    std::string error;   // the service's error code ("authorization_pending", "invalid_grant", ...)
    bool ok() const { return !access.empty(); }
};
Tokens parseTokens(int status, const std::string& body);

struct DeviceCode {
    std::string deviceCode, userCode, verificationUri;
    int expiresIn = 0, interval = 5;
    std::string error;
};
DeviceCode parseDeviceCode(int status, const std::string& body);

// Twitch's device-flow poll: keep waiting, done, or a final failure (expired, denied, ...).
enum class Poll { Pending, Done, Failed };
Poll classifyPoll(const Tokens& t);

struct TwitchUser {
    std::string login, userId;
    int expiresIn = 0;
    bool valid = false;
};
TwitchUser parseValidate(int status, const std::string& body);

// The stream key, or a message in plain words (Twitch's own, for 403s: it asks apps to show it).
std::string parseTwitchKey(int status, const std::string& body, std::string& message);

struct YouTubeStream {
    std::string title, key, rtmps, rtmp;
    bool isDefault = false, reusable = false;
};
// The streams on the channel that take RTMP, the default (YouTube Studio's) first.
std::vector<YouTubeStream> parseYouTubeStreams(int status, const std::string& body, std::string& message);

// The signed-in channel's name (channels?part=snippet&mine=true), "" if there's no channel.
std::string parseYouTubeChannel(int status, const std::string& body);

// A service's error body in plain words (never including tokens).
std::string describeError(int status, const std::string& body);

// --- the loopback listener (YouTube) ---
std::string googleAuthUrl(const Endpoints& e, const Clients& c, const std::string& redirectUri, const std::string& state,
                          const std::string& challenge);
// From the browser's first request line ("GET /?code=…&state=… HTTP/1.1"): the code, if the state matches.
struct Redirect {
    std::string code, error;
    bool stateOk = false;
    bool isCallback = false;   // false: some other request (a favicon) to answer with 404
};
Redirect parseRedirect(const std::string& requestHead, const std::string& expectedState);

}  // namespace ncap::oauth
