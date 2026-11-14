// Small, forgiving URL helpers for the interface: splitting, display text and
// what kind of site a page is. Not a validator; Chromium canonicalises every
// URL before it's loaded. Portable and unit-tested.
#pragma once

#include <string>
#include <string_view>

namespace nrb {

std::string toLower(std::string_view s);
std::string_view trimSpaces(std::string_view s);

struct UrlParts {
    std::string scheme;    // lower case, without ':'
    std::string host;      // lower case; IPv6 keeps its brackets
    std::string port;      // digits, or empty
    std::string rest;      // path, query and fragment, from the first '/', '?' or '#'
    bool hasAuthority = false;  // "scheme://..."
    bool hasUserInfo = false;   // "user@" before the host
};

// False if there's no scheme.
bool splitUrl(std::string_view url, UrlParts& out);

// The address as shown when the address bar isn't being edited:
// "https://example.com/" -> "example.com", "https://example.com/a" -> "example.com/a".
// http:// stays visible, so an unencrypted page never looks like a secure one.
std::string displayUrl(std::string_view url);

// The host for a tab tooltip or a status line ("example.com"), or "" if none.
std::string hostOf(std::string_view url);

enum class SiteKind {
    Blank,        // nothing loaded (about:blank, new tab)
    Secure,       // https with a valid certificate and no mixed content
    MixedContent, // https, but some of the page came over plain http
    CertProblem,  // https with a certificate error
    Insecure,     // http
    LocalFile,    // file://
    Internal,     // chrome://, view-source:, devtools
    Other,        // data:, blob: and similar
};

struct SecurityInfo {
    bool known = false;          // the engine has reported on this page
    bool secureConnection = false;
    bool certificateError = false;
    bool mixedContent = false;
};

SiteKind classifySite(std::string_view url, const SecurityInfo& sec);

// The words shown beside the site indicator, e.g. "Not secure". Empty for
// ordinary secure pages (the padlock is enough).
const char* siteLabel(SiteKind k);
// One or two sentences for the site information panel.
const char* siteExplanation(SiteKind k);

}  // namespace nrb
