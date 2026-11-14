#include "nrb/omnibox.hpp"

#include "nrb/url.hpp"

#include <array>
#include <cctype>
#include <cstdlib>

namespace nrb {

const std::vector<SearchEngine>& builtInSearchEngines() {
    // Query URLs as published by each engine. DuckDuckGo is the default because
    // it needs no account and doesn't build a profile; the choice is the
    // person's, in Settings.
    static const std::vector<SearchEngine> engines = {
        {"duckduckgo", "DuckDuckGo", "https://duckduckgo.com/?q={q}"},
        {"google", "Google", "https://www.google.com/search?q={q}"},
        {"bing", "Bing", "https://www.bing.com/search?q={q}"},
        {"brave", "Brave Search", "https://search.brave.com/search?q={q}"},
        {"ecosia", "Ecosia", "https://www.ecosia.org/search?q={q}"},
        {"startpage", "Startpage", "https://www.startpage.com/do/search?q={q}"},
    };
    return engines;
}

const SearchEngine* findSearchEngine(std::string_view id) {
    for (const SearchEngine& e : builtInSearchEngines())
        if (e.id == id) return &e;
    return nullptr;
}

const SearchEngine& defaultSearchEngine() { return builtInSearchEngines().front(); }

static const char* kHex = "0123456789ABCDEF";

std::string encodeQueryComponent(std::string_view s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
        else if (c == ' ') out += '+';
        else { out += '%'; out += kHex[c >> 4]; out += kHex[c & 15]; }
    }
    return out;
}

std::string searchUrl(std::string_view query, const SearchEngine& engine) {
    std::string url = engine.queryUrl;
    const std::string q = encodeQueryComponent(trimSpaces(query));
    const size_t at = url.find("{q}");
    if (at == std::string::npos) return url + q;
    url.replace(at, 3, q);
    return url;
}

std::string fileUrlFromWindowsPath(std::string_view path) {
    std::string out = "file:///";
    for (unsigned char c : path) {
        if (c == '\\') out += '/';
        else if (std::isalnum(c) || c == '/' || c == ':' || c == '-' || c == '_' || c == '.' || c == '~' || c == '!' ||
                 c == '$' || c == '&' || c == '\'' || c == '(' || c == ')' || c == '*' || c == '+' || c == ',' ||
                 c == ';' || c == '=' || c == '@')
            out += char(c);
        else { out += '%'; out += kHex[c >> 4]; out += kHex[c & 15]; }
    }
    return out;
}

// ------------------------------------------------------------------ hosts

static bool parseIpv4(std::string_view h, std::array<int, 4>& o) {
    int part = 0;
    size_t i = 0;
    while (part < 4) {
        if (i >= h.size() || !std::isdigit((unsigned char)h[i])) return false;
        int v = 0, digits = 0;
        while (i < h.size() && std::isdigit((unsigned char)h[i])) {
            v = v * 10 + (h[i] - '0');
            if (++digits > 3 || v > 255) return false;
            ++i;
        }
        o[part++] = v;
        if (part < 4) {
            if (i >= h.size() || h[i] != '.') return false;
            ++i;
        }
    }
    return i == h.size();
}

static bool endsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

bool isLocalOrPrivateHost(std::string_view hostIn) {
    std::string host = toLower(hostIn);
    if (!host.empty() && host.back() == '.') host.pop_back();
    if (host == "localhost" || endsWith(host, ".localhost")) return true;
    std::array<int, 4> ip{};
    if (parseIpv4(host, ip)) {
        return ip[0] == 127 || ip[0] == 10 || ip[0] == 0 || (ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31) ||
               (ip[0] == 192 && ip[1] == 168) || (ip[0] == 169 && ip[1] == 254) ||
               (ip[0] == 100 && ip[1] >= 64 && ip[1] <= 127);  // carrier-grade NAT
    }
    if (host.size() > 2 && host.front() == '[') {
        const std::string v6 = host.substr(1, host.size() - 2);
        return v6 == "::1" || v6.rfind("fe8", 0) == 0 || v6.rfind("fe9", 0) == 0 || v6.rfind("fea", 0) == 0 ||
               v6.rfind("feb", 0) == 0 || v6.rfind("fc", 0) == 0 || v6.rfind("fd", 0) == 0;
    }
    static const char* privateSuffixes[] = {".local", ".lan", ".internal", ".intranet", ".home.arpa", ".localdomain",
                                            ".home", ".corp", ".private"};
    for (const char* s : privateSuffixes)
        if (endsWith(host, s)) return true;
    return false;
}

bool isAllowedInternalPage(std::string_view hostIn) {
    static const char* allowed[] = {"version", "gpu", "sandbox", "credits", "license", "accessibility",
                                    "process-internals", "media-internals", "webrtc-internals", "net-internals"};
    const std::string host = toLower(hostIn);
    for (const char* a : allowed)
        if (host == a) return true;
    return false;
}

// A DNS-style name: labels of letters, digits and hyphens (or any non-ASCII,
// for internationalised names), no empty labels except a trailing dot, the
// last label not all digits.
static bool looksLikeHostName(std::string_view h, bool& hasDot) {
    hasDot = false;
    if (h.empty() || h.size() > 253) return false;
    if (h.back() == '.') h.remove_suffix(1);
    size_t start = 0;
    std::string_view last;
    for (size_t i = 0; i <= h.size(); ++i) {
        if (i == h.size() || h[i] == '.') {
            std::string_view label = h.substr(start, i - start);
            if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') return false;
            if (i < h.size()) hasDot = true;
            last = label;
            start = i + 1;
            continue;
        }
        const unsigned char c = (unsigned char)h[i];
        if (!(std::isalnum(c) || c == '-' || c == '_' || c >= 0x80)) return false;
    }
    bool allDigits = true;
    for (char c : last)
        if (!std::isdigit((unsigned char)c)) allDigits = false;
    return !allDigits;
}

static bool hasWhitespace(std::string_view s) {
    for (char c : s)
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') return true;
    return false;
}

static std::string encodeSpaces(std::string_view s) {
    std::string out;
    for (char c : s) {
        if (c == ' ') out += "%20";
        else if (c == '\t' || c == '\r' || c == '\n') continue;
        else out += c;
    }
    return out;
}

static InputResult refuse(const char* why) { return {InputKind::Refused, {}, why}; }

static InputResult withScheme(std::string_view text, const UrlParts& p) {
    const std::string& s = p.scheme;
    if (s == "http" || s == "https") {
        if (!p.hasAuthority || p.host.empty()) return refuse("That address is missing the site's name.");
        return {InputKind::Url, encodeSpaces(text), {}};
    }
    if (s == "file") {
        // file://server/share would make Windows connect to that server and
        // offer it your sign-in, so only local files open.
        if (p.hasAuthority && !p.host.empty() && p.host != "localhost")
            return refuse("Network file locations (\\\\server\\share) can't be opened in NorthRay Browser yet.");
        return {InputKind::Url, encodeSpaces(text), {}};
    }
    if (s == "about") {
        if (toLower(p.rest) == "blank") return {InputKind::Url, "about:blank", {}};
        return refuse("That about: page doesn't exist in NorthRay Browser.");
    }
    if (s == "chrome" || s == "edge") {   // the engine's own diagnostic pages
        if (p.hasAuthority && isAllowedInternalPage(p.host)) return {InputKind::Url, encodeSpaces(text), {}};
        return refuse("That internal page isn't available in NorthRay Browser.");
    }
    if (s == "view-source") {
        UrlParts inner;
        const std::string_view target = text.substr(text.find(':') + 1);
        if (splitUrl(target, inner) && (inner.scheme == "http" || inner.scheme == "https"))
            return {InputKind::Url, encodeSpaces(text), {}};
        return refuse("View source works for web addresses (http and https) only.");
    }
    if (s == "javascript")
        return refuse("Scripts can't be run from the address bar. Pasted scripts are a common way to take over accounts.");
    if (s == "data" || s == "blob" || s == "filesystem")
        return refuse("Addresses that contain a whole page (data:) can't be opened from the address bar, because they can pretend to be any site.");
    return refuse("Opening that kind of link in another app isn't built yet.");
}

InputResult interpretInput(std::string_view raw, const SearchEngine& engine) {
    std::string_view text = trimSpaces(raw);
    if (text.empty()) return {};
    if (text.size() > 32 * 1024) return refuse("That's too long to be an address or a search.");

    // "?anything" is always a search.
    if (text.front() == '?') {
        std::string_view q = trimSpaces(text.substr(1));
        if (q.empty()) return {};
        return {InputKind::Search, searchUrl(q, engine), {}};
    }

    // Windows paths are opened as files, never searched for.
    if (text.size() >= 3 && std::isalpha((unsigned char)text[0]) && text[1] == ':' && (text[2] == '\\' || text[2] == '/'))
        return {InputKind::Url, fileUrlFromWindowsPath(text), {}};
    if (text.size() >= 2 && text[0] == '\\' && text[1] == '\\')
        return refuse("Network file locations (\\\\server\\share) can't be opened in NorthRay Browser yet.");
    if (text.rfind(".\\", 0) == 0 || text.rfind("..\\", 0) == 0 || text.rfind("./", 0) == 0 || text.rfind("../", 0) == 0)
        return refuse("Type the whole path of the file, starting with the drive, like C:\\Users\\...");

    // An explicit scheme. "localhost:3000" and "example.com:8080/x" look like
    // schemes but are a host and a port.
    UrlParts p;
    if (splitUrl(text, p)) {
        const std::string_view after = text.substr(text.find(':') + 1);
        size_t digits = 0;
        while (digits < after.size() && std::isdigit((unsigned char)after[digits])) ++digits;
        const bool isPort = digits > 0 && (digits == after.size() || after[digits] == '/' || after[digits] == '?' || after[digits] == '#');
        const bool knownWithSlashes = p.hasAuthority && (p.scheme == "http" || p.scheme == "https" || p.scheme == "file");
        if (!isPort && (knownWithSlashes || !hasWhitespace(text))) return withScheme(text, p);
        if (!isPort) return {InputKind::Search, searchUrl(text, engine), {}};  // "note: buy milk"
    }

    if (hasWhitespace(text)) return {InputKind::Search, searchUrl(text, engine), {}};

    // No scheme, no spaces: a host (with optional port and path), or a word.
    const size_t end = text.find_first_of("/?#");
    std::string_view authority = text.substr(0, end);
    const bool hasPathMark = end != std::string_view::npos;
    if (authority.find('@') != std::string_view::npos) return {InputKind::Search, searchUrl(text, engine), {}};  // an e-mail address

    std::string_view host = authority, port;
    if (!authority.empty() && authority.front() == '[') {
        const size_t close = authority.find(']');
        if (close == std::string_view::npos) return {InputKind::Search, searchUrl(text, engine), {}};
        host = authority.substr(0, close + 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') return {InputKind::Search, searchUrl(text, engine), {}};
            port = authority.substr(close + 2);
        }
    } else if (const size_t colon = authority.rfind(':'); colon != std::string_view::npos) {
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
    }
    for (char c : port)
        if (!std::isdigit((unsigned char)c)) return {InputKind::Search, searchUrl(text, engine), {}};
    if (port.size() > 5 || (!port.empty() && std::atoi(std::string(port).c_str()) > 65535))
        return {InputKind::Search, searchUrl(text, engine), {}};

    std::array<int, 4> ip{};
    const bool isIp = parseIpv4(host, ip) || (host.size() > 2 && host.front() == '[');
    bool hasDot = false;
    const bool hostName = !isIp && looksLikeHostName(host, hasDot);
    const bool local = isLocalOrPrivateHost(host);

    bool isAddress = isIp || local || (hostName && hasDot);
    // A single word becomes an address only with an explicit port or path:
    // "intranet:8080", "printer/".
    if (!isAddress && hostName && (!port.empty() || hasPathMark)) isAddress = true;
    if (!isAddress) return {InputKind::Search, searchUrl(text, engine), {}};
    // Local and private hosts (and single-word intranet names) usually have no
    // public certificate; everything else is tried over https first.
    const bool plainHttp = local || (hostName && !hasDot);
    return {InputKind::Url, std::string(plainHttp ? "http://" : "https://") + std::string(text), {}};
}

}  // namespace nrb
