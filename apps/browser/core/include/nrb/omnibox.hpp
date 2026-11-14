// What the address bar does with what was typed.
//
// Portable (no Windows, no CEF), so it is unit-tested on every platform.
// The rules are deliberately predictable:
//   * Anything that looks like an address, a file path or an internal host is
//     opened, never sent to a search engine. A mistaken address gives an error
//     page with "Search instead", which is recoverable; a private host name or
//     path sent to a search engine is not.
//   * Text with spaces, or a single plain word, is a search.
//   * Schemes that are commonly abused from the address bar (javascript:,
//     data:) are refused with a reason, rather than silently ignored.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace nrb {

struct SearchEngine {
    std::string id;        // "duckduckgo"
    std::string name;      // "DuckDuckGo"
    std::string queryUrl;  // "https://duckduckgo.com/?q={q}"
};

// The engines offered in Settings. The first is the default.
const std::vector<SearchEngine>& builtInSearchEngines();
// nullptr if there's no engine with that id.
const SearchEngine* findSearchEngine(std::string_view id);
const SearchEngine& defaultSearchEngine();

enum class InputKind {
    Empty,    // nothing to do
    Url,      // open `url`
    Search,   // open `url`, the search engine's results for the text
    Refused,  // don't open anything; tell the person `reason`
};

struct InputResult {
    InputKind kind = InputKind::Empty;
    std::string url;     // for Url and Search
    std::string reason;  // for Refused: one plain sentence
};

InputResult interpretInput(std::string_view text, const SearchEngine& engine);

// The search URL for `query` (always a search, whatever the text looks like).
std::string searchUrl(std::string_view query, const SearchEngine& engine);

// Form-style query encoding: unreserved characters stay, space becomes '+',
// everything else (UTF-8 bytes) becomes %XX.
std::string encodeQueryComponent(std::string_view s);

// "C:\Users\Me\notes.txt" -> "file:///C:/Users/Me/notes.txt" (percent-encoded).
std::string fileUrlFromWindowsPath(std::string_view path);

// Hosts that are local or private: localhost, loopback and private IP
// addresses, and names under .local, .lan, .internal, .home.arpa and the like.
bool isLocalOrPrivateHost(std::string_view host);

// The engine's internal pages (chrome:// or edge://) people may open by typing:
// diagnostics and the licence pages. Everything else there is refused.
bool isAllowedInternalPage(std::string_view host);

}  // namespace nrb
