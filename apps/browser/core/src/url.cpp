#include "nrb/url.hpp"

#include <cctype>

namespace nrb {

std::string toLower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return out;
}

static bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

std::string_view trimSpaces(std::string_view s) {
    // Also trims UTF-8 no-break spaces (C2 A0), which arrive with pasted text.
    for (;;) {
        if (!s.empty() && isSpace(s.front())) { s.remove_prefix(1); continue; }
        if (s.size() >= 2 && (unsigned char)s[0] == 0xC2 && (unsigned char)s[1] == 0xA0) { s.remove_prefix(2); continue; }
        break;
    }
    for (;;) {
        if (!s.empty() && isSpace(s.back())) { s.remove_suffix(1); continue; }
        if (s.size() >= 2 && (unsigned char)s[s.size() - 2] == 0xC2 && (unsigned char)s.back() == 0xA0) { s.remove_suffix(2); continue; }
        break;
    }
    return s;
}

static bool isSchemeChar(char c, bool first) {
    if (std::isalpha((unsigned char)c)) return true;
    return !first && (std::isdigit((unsigned char)c) || c == '+' || c == '-' || c == '.');
}

bool splitUrl(std::string_view url, UrlParts& out) {
    out = {};
    size_t colon = url.find(':');
    if (colon == std::string_view::npos || colon == 0) return false;
    for (size_t i = 0; i < colon; ++i)
        if (!isSchemeChar(url[i], i == 0)) return false;
    out.scheme = toLower(url.substr(0, colon));
    std::string_view rest = url.substr(colon + 1);
    if (rest.size() >= 2 && rest[0] == '/' && rest[1] == '/') {
        out.hasAuthority = true;
        rest.remove_prefix(2);
        size_t end = rest.find_first_of("/?#");
        std::string_view authority = rest.substr(0, end);
        out.rest = end == std::string_view::npos ? std::string() : std::string(rest.substr(end));
        size_t at = authority.rfind('@');
        if (at != std::string_view::npos) {
            out.hasUserInfo = true;
            authority.remove_prefix(at + 1);
        }
        if (!authority.empty() && authority.front() == '[') {
            size_t close = authority.find(']');
            if (close == std::string_view::npos) {
                out.host = toLower(authority);
            } else {
                out.host = toLower(authority.substr(0, close + 1));
                if (close + 1 < authority.size() && authority[close + 1] == ':') out.port = std::string(authority.substr(close + 2));
            }
        } else {
            size_t pc = authority.rfind(':');
            if (pc != std::string_view::npos) {
                out.host = toLower(authority.substr(0, pc));
                out.port = std::string(authority.substr(pc + 1));
            } else {
                out.host = toLower(authority);
            }
        }
    } else {
        out.rest = std::string(rest);
    }
    return true;
}

std::string hostOf(std::string_view url) {
    UrlParts p;
    if (!splitUrl(url, p) || !p.hasAuthority) return {};
    return p.host;
}

std::string displayUrl(std::string_view url) {
    UrlParts p;
    if (!splitUrl(url, p)) return std::string(url);
    if (p.scheme == "about" && toLower(p.rest) == "blank") return {};
    if (p.scheme == "https" && p.hasAuthority && !p.hasUserInfo) {
        std::string s = p.host;
        if (!p.port.empty() && p.port != "443") s += ":" + p.port;
        if (p.rest != "/") s += p.rest;
        return s;
    }
    if (p.scheme == "http" && p.hasAuthority && !p.hasUserInfo && p.rest == "/") {
        std::string s = "http://" + p.host;
        if (!p.port.empty() && p.port != "80") s += ":" + p.port;
        return s;
    }
    return std::string(url);
}

SiteKind classifySite(std::string_view url, const SecurityInfo& sec) {
    UrlParts p;
    if (url.empty() || !splitUrl(url, p)) return SiteKind::Blank;
    if (p.scheme == "about") return SiteKind::Blank;
    if (p.scheme == "https" || p.scheme == "wss") {
        if (sec.known && sec.certificateError) return SiteKind::CertProblem;
        if (sec.known && sec.mixedContent) return SiteKind::MixedContent;
        if (sec.known && !sec.secureConnection) return SiteKind::Insecure;
        return SiteKind::Secure;
    }
    if (p.scheme == "http" || p.scheme == "ws") return SiteKind::Insecure;
    if (p.scheme == "file") return SiteKind::LocalFile;
    if (p.scheme == "chrome" || p.scheme == "edge" || p.scheme == "view-source" || p.scheme == "devtools" || p.scheme == "chrome-extension")
        return SiteKind::Internal;
    return SiteKind::Other;
}

const char* siteLabel(SiteKind k) {
    switch (k) {
        case SiteKind::Insecure: return "Not secure";
        case SiteKind::CertProblem: return "Certificate problem";
        case SiteKind::MixedContent: return "Partly secure";
        case SiteKind::LocalFile: return "File";
        case SiteKind::Internal: return "NorthRay";
        case SiteKind::Blank: case SiteKind::Secure: case SiteKind::Other: break;
    }
    return "";
}

const char* siteExplanation(SiteKind k) {
    switch (k) {
        case SiteKind::Secure:
            return "The connection to this site is encrypted and its certificate is valid. That keeps what you send private on the way, "
                   "but it doesn't mean the site itself is trustworthy.";
        case SiteKind::MixedContent:
            return "The page is encrypted, but some of its pictures or other parts came over an unencrypted connection, "
                   "so they could have been changed on the way.";
        case SiteKind::CertProblem:
            return "This site's certificate couldn't be checked, so the connection may not be private.";
        case SiteKind::Insecure:
            return "The connection to this site isn't encrypted. Don't enter passwords or payment details here.";
        case SiteKind::LocalFile:
            return "This is a file on this computer.";
        case SiteKind::Internal:
            return "This is a page built into the browser.";
        case SiteKind::Other:
            return "This page was made in the browser rather than loaded from a website.";
        case SiteKind::Blank:
            break;
    }
    return "";
}

}  // namespace nrb
