#include "ncap/oauth.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>

namespace ncap::oauth {

using nlohmann::json;

// --- SHA-256 (FIPS 180-4) ---
namespace {
constexpr std::uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
inline std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void block(std::uint32_t (&h)[8], const std::uint8_t* p) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = (std::uint32_t)p[i * 4] << 24 | (std::uint32_t)p[i * 4 + 1] << 16 | (std::uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        const std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

std::string str(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<long long>());
    return {};
}
int num(const json& j, const char* key, int fallback = 0) {
    auto it = j.find(key);
    if (it != j.end() && it->is_number()) return it->get<int>();
    return fallback;
}
json parse(const std::string& body) { return json::parse(body, nullptr, false); }
}  // namespace

std::array<std::uint8_t, 32> sha256(const void* data, std::size_t size) {
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::size_t n = size;
    while (n >= 64) {
        block(h, p);
        p += 64;
        n -= 64;
    }
    std::uint8_t tail[128] = {};
    std::memcpy(tail, p, n);
    tail[n] = 0x80;
    const std::size_t total = n + 9 <= 64 ? 64 : 128;
    const std::uint64_t bits = (std::uint64_t)size * 8;
    for (int i = 0; i < 8; ++i) tail[total - 1 - i] = (std::uint8_t)(bits >> (8 * i));
    block(h, tail);
    if (total == 128) block(h, tail + 64);
    std::array<std::uint8_t, 32> out{};
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j) out[(std::size_t)(i * 4 + j)] = (std::uint8_t)(h[i] >> (24 - 8 * j));
    return out;
}

std::string base64url(const std::uint8_t* data, std::size_t size) {
    static const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < size; i += 3) {
        const std::uint32_t v = (std::uint32_t)data[i] << 16 | (std::uint32_t)data[i + 1] << 8 | data[i + 2];
        out += abc[v >> 18];
        out += abc[(v >> 12) & 63];
        out += abc[(v >> 6) & 63];
        out += abc[v & 63];
    }
    if (size - i == 1) {
        const std::uint32_t v = (std::uint32_t)data[i] << 16;
        out += abc[v >> 18];
        out += abc[(v >> 12) & 63];
    } else if (size - i == 2) {
        const std::uint32_t v = (std::uint32_t)data[i] << 16 | (std::uint32_t)data[i + 1] << 8;
        out += abc[v >> 18];
        out += abc[(v >> 12) & 63];
        out += abc[(v >> 6) & 63];
    }
    return out;
}

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string urlDecode(const std::string& s) {
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') out += ' ';
        else if (s[i] == '%' && i + 2 < s.size() && val(s[i + 1]) >= 0 && val(s[i + 2]) >= 0) {
            out += (char)(val(s[i + 1]) * 16 + val(s[i + 2]));
            i += 2;
        } else out += s[i];
    }
    return out;
}

std::string form(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string out;
    for (auto& [k, v] : fields) {
        if (!out.empty()) out += '&';
        out += urlEncode(k) + '=' + urlEncode(v);
    }
    return out;
}

std::map<std::string, std::string> parseQuery(const std::string& query) {
    std::map<std::string, std::string> out;
    std::size_t i = 0;
    while (i <= query.size()) {
        std::size_t amp = query.find('&', i);
        if (amp == std::string::npos) amp = query.size();
        const std::string part = query.substr(i, amp - i);
        if (!part.empty()) {
            const std::size_t eq = part.find('=');
            if (eq == std::string::npos) out[urlDecode(part)] = "";
            else out[urlDecode(part.substr(0, eq))] = urlDecode(part.substr(eq + 1));
        }
        i = amp + 1;
    }
    return out;
}

std::string pkceVerifier(const std::uint8_t (&random)[32]) { return base64url(random, 32); }

std::string pkceChallenge(const std::string& verifier) {
    const auto d = sha256(verifier.data(), verifier.size());
    return base64url(d.data(), d.size());
}

Clients parseClients(const std::string& text) {
    Clients c;
    const json j = parse(text);
    if (!j.is_object()) return c;
    if (auto t = j.find("twitch"); t != j.end() && t->is_object()) c.twitchClientId = str(*t, "clientId");
    if (auto g = j.find("google"); g != j.end() && g->is_object()) {
        c.googleClientId = str(*g, "clientId");
        c.googleClientSecret = str(*g, "clientSecret");
    }
    return c;
}

Tokens parseTokens(int status, const std::string& body) {
    Tokens t;
    const json j = parse(body);
    if (!j.is_object()) {
        t.error = status ? "unreadable_response" : "no_connection";
        return t;
    }
    if (status == 200) {
        t.access = str(j, "access_token");
        t.refresh = str(j, "refresh_token");
        t.expiresIn = num(j, "expires_in");
        if (t.access.empty()) t.error = "no_access_token";
        return t;
    }
    // Google: {"error":"invalid_grant",...}; Twitch: {"status":400,"message":"authorization_pending"}.
    t.error = str(j, "error");
    if (t.error.empty() || t.error == "Bad Request" || t.error == "Unauthorized") {
        const std::string m = str(j, "message");
        if (!m.empty()) t.error = m;
    }
    if (t.error.empty()) t.error = "http_" + std::to_string(status);
    return t;
}

DeviceCode parseDeviceCode(int status, const std::string& body) {
    DeviceCode d;
    const json j = parse(body);
    if (status != 200 || !j.is_object()) {
        d.error = describeError(status, body);
        return d;
    }
    d.deviceCode = str(j, "device_code");
    d.userCode = str(j, "user_code");
    d.verificationUri = str(j, "verification_uri");
    d.expiresIn = num(j, "expires_in", 1800);
    d.interval = std::max(1, num(j, "interval", 5));
    if (d.deviceCode.empty() || d.verificationUri.empty()) d.error = "Twitch's reply was missing the sign-in code.";
    // Only ever send the person to Twitch itself.
    else if (d.verificationUri.rfind("https://www.twitch.tv/", 0) != 0 && d.verificationUri.rfind("https://twitch.tv/", 0) != 0)
        d.error = "Twitch's reply pointed somewhere other than twitch.tv, so it wasn't opened.";
    return d;
}

Poll classifyPoll(const Tokens& t) {
    if (t.ok()) return Poll::Done;
    if (t.error == "authorization_pending" || t.error == "slow_down" || t.error == "no_connection") return Poll::Pending;
    return Poll::Failed;
}

TwitchUser parseValidate(int status, const std::string& body) {
    TwitchUser u;
    const json j = parse(body);
    if (status != 200 || !j.is_object()) return u;
    u.login = str(j, "login");
    u.userId = str(j, "user_id");
    u.expiresIn = num(j, "expires_in");
    u.valid = !u.userId.empty();
    return u;
}

std::string parseTwitchKey(int status, const std::string& body, std::string& message) {
    const json j = parse(body);
    if (status == 200 && j.is_object()) {
        auto d = j.find("data");
        if (d != j.end() && d->is_array() && !d->empty() && (*d)[0].is_object()) {
            const std::string k = str((*d)[0], "stream_key");
            if (!k.empty()) return k;
        }
        message = "Twitch didn't send a stream key.";
        return {};
    }
    if (status == 403 && j.is_object() && !str(j, "message").empty()) message = "Twitch: " + str(j, "message");
    else message = describeError(status, body);
    return {};
}

std::vector<YouTubeStream> parseYouTubeStreams(int status, const std::string& body, std::string& message) {
    std::vector<YouTubeStream> out;
    const json j = parse(body);
    if (status != 200 || !j.is_object()) {
        message = describeError(status, body);
        return out;
    }
    auto items = j.find("items");
    if (items == j.end() || !items->is_array()) return out;
    for (auto& it : *items) {
        if (!it.is_object()) continue;
        const json& cdn = it.value("cdn", json::object());
        if (str(cdn, "ingestionType") != "rtmp") continue;
        const json& info = cdn.value("ingestionInfo", json::object());
        YouTubeStream s;
        s.key = str(info, "streamName");
        s.rtmps = str(info, "rtmpsIngestionAddress");
        s.rtmp = str(info, "ingestionAddress");
        if (s.key.empty() || (s.rtmps.empty() && s.rtmp.empty())) continue;
        const json& sn = it.value("snippet", json::object());
        s.title = str(sn, "title");
        s.isDefault = sn.value("isDefaultStream", false);
        s.reusable = it.value("contentDetails", json::object()).value("isReusable", false);
        out.push_back(std::move(s));
    }
    std::stable_sort(out.begin(), out.end(), [](const YouTubeStream& a, const YouTubeStream& b) {
        return (a.isDefault ? 0 : a.reusable ? 1 : 2) < (b.isDefault ? 0 : b.reusable ? 1 : 2);
    });
    return out;
}

std::string parseYouTubeChannel(int status, const std::string& body) {
    const json j = parse(body);
    if (status != 200 || !j.is_object()) return {};
    auto items = j.find("items");
    if (items == j.end() || !items->is_array() || items->empty() || !(*items)[0].is_object()) return {};
    return str((*items)[0].value("snippet", json::object()), "title");
}

std::string describeError(int status, const std::string& body) {
    if (status == 0) return "Couldn't reach the service (no connection?).";
    const json j = parse(body);
    std::string detail;
    if (j.is_object()) {
        // Google API errors: {"error":{"code":403,"message":"…","errors":[{"reason":"…"}]}}
        if (auto e = j.find("error"); e != j.end() && e->is_object()) detail = str(*e, "message");
        if (detail.empty()) detail = str(j, "error_description");
        if (detail.empty()) detail = str(j, "message");
        if (detail.empty()) detail = str(j, "error");
    }
    if (detail.size() > 200) detail = detail.substr(0, 200) + "…";
    if (status == 401) return "The sign-in is no longer accepted" + (detail.empty() ? std::string(".") : " (" + detail + ").") + " Sign in again.";
    std::string out = "The service said no (HTTP " + std::to_string(status) + ")";
    if (!detail.empty()) out += ": " + detail;
    return out + ".";
}

std::string googleAuthUrl(const Endpoints& e, const Clients& c, const std::string& redirectUri, const std::string& state,
                          const std::string& challenge) {
    return e.googleAuth + "?" +
           form({{"client_id", c.googleClientId},
                 {"redirect_uri", redirectUri},
                 {"response_type", "code"},
                 {"scope", kGoogleScopes},
                 {"code_challenge", challenge},
                 {"code_challenge_method", "S256"},
                 {"state", state},
                 {"access_type", "offline"},
                 {"prompt", "consent"}});
}

Redirect parseRedirect(const std::string& head, const std::string& expectedState) {
    Redirect r;
    // "GET /?code=…&state=… HTTP/1.1"
    if (head.rfind("GET ", 0) != 0) return r;
    const std::size_t sp = head.find(' ', 4);
    if (sp == std::string::npos) return r;
    const std::string target = head.substr(4, sp - 4);
    const std::size_t q = target.find('?');
    const std::string path = target.substr(0, q);
    if (path != "/") return r;
    r.isCallback = true;
    const auto params = q == std::string::npos ? std::map<std::string, std::string>{} : parseQuery(target.substr(q + 1));
    auto get = [&](const char* k) {
        auto it = params.find(k);
        return it == params.end() ? std::string() : it->second;
    };
    r.stateOk = !expectedState.empty() && get("state") == expectedState;
    r.error = get("error");
    if (r.stateOk && r.error.empty()) r.code = get("code");
    if (r.code.empty() && r.error.empty()) r.error = r.stateOk ? "no_code" : "state_mismatch";
    return r;
}

}  // namespace ncap::oauth
