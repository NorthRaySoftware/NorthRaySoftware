// NorthRay Capture: small HTTPS requests through WinHTTP (sign-in and account APIs). Windows checks the
// server's certificate; nothing is skipped. Request and response bodies may hold tokens, so nothing
// here logs them.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace ncap::http {

struct Response {
    int status = 0;          // 0: no answer (see error)
    std::string body;
    std::string error;       // in plain words, when status is 0
};

using Headers = std::vector<std::pair<std::string, std::string>>;

Response request(const std::string& method, const std::string& url, const Headers& headers = {}, const std::string& body = {},
                 int timeoutMs = 15000);

// POST application/x-www-form-urlencoded.
inline Response postForm(const std::string& url, const std::string& form, const Headers& extra = {}) {
    Headers h = extra;
    h.push_back({"Content-Type", "application/x-www-form-urlencoded"});
    return request("POST", url, h, form);
}

}  // namespace ncap::http
