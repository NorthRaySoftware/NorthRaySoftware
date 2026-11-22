// Minimal HTTPS client on WinHTTP (part of Windows, no extra DLLs).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace cjui {

struct HttpResult {
    int status = 0;             // 0 = no response
    std::string body;
    std::string contentType;
    std::wstring error;         // human-readable reason when status != 200
    bool ok() const { return status == 200; }
};

// GET a small document (JSON). Bodies over maxBytes are refused.
HttpResult httpGet(const std::string& url, int timeoutMs = 10000, size_t maxBytes = 2 * 1024 * 1024);

// Any request with its own headers and body (web APIs such as GitHub's). The reply's body comes back whatever the
// status, up to maxBytes. Headers and body are never written anywhere else.
HttpResult httpRequest(const std::string& method, const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
                       const std::string& body, int timeoutMs = 60000, size_t maxBytes = 160u * 1024 * 1024);

// GET an endless stream (Icecast). Calls onData for each chunk until it
// returns false, `cancel` is set, or the connection drops. Returns the HTTP
// status (0 if no response) and fills error on failure.
int httpStream(const std::string& url, const std::function<bool(const uint8_t*, size_t)>& onData,
               const std::atomic<bool>& cancel, std::wstring& error, std::string* contentType = nullptr);

// Opens an https:// link in the user's browser. Refuses anything else.
bool openInBrowser(const std::string& url);

}  // namespace cjui
