#include "output/http.hpp"

#include <windows.h>
#include <winhttp.h>

namespace ncap::http {

namespace {
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
};

std::string describe(DWORD e) {
    switch (e) {
    case ERROR_WINHTTP_TIMEOUT: return "The service took too long to answer.";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "Couldn't find the service (no internet connection?).";
    case ERROR_WINHTTP_CANNOT_CONNECT: return "Couldn't connect to the service (no internet connection, or a firewall?).";
    case ERROR_WINHTTP_CONNECTION_ERROR: return "The connection to the service was lost.";
    case ERROR_WINHTTP_SECURE_FAILURE:
    case ERROR_WINHTTP_SECURE_INVALID_CA:
    case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
    case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
        return "The service's security certificate wasn't valid, so Capture didn't connect (check the PC's clock).";
    default: return "Couldn't reach the service (error " + std::to_string(e) + ").";
    }
}
}  // namespace

Response request(const std::string& method, const std::string& url, const Headers& headers, const std::string& body, int timeoutMs) {
    Response r;
    const std::wstring wurl = wide(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    wchar_t host[256] = {}, path[4096] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    wchar_t extra[4096] = {};
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
        r.error = "That isn't a web address.";
        return r;
    }
    const bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
    Handle session, connect, req;
#ifdef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
    session.h = WinHttpOpen(L"NorthRayCapture", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
#endif
    if (!session.h) session.h = WinHttpOpen(L"NorthRayCapture", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) {
        r.error = describe(GetLastError());
        return r;
    }
    WinHttpSetTimeouts(session.h, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    if (!WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;   // Windows without TLS 1.3 in WinHTTP
        WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
    }
    connect.h = WinHttpConnect(session.h, host, uc.nPort, 0);
    if (!connect.h) {
        r.error = describe(GetLastError());
        return r;
    }
    const std::wstring target = std::wstring(path) + extra;
    req.h = WinHttpOpenRequest(connect.h, wide(method).c_str(), target.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                               secure ? WINHTTP_FLAG_SECURE : 0);
    if (!req.h) {
        r.error = describe(GetLastError());
        return r;
    }
    // Sign-in answers are never followed elsewhere.
    DWORD noRedirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(req.h, WINHTTP_OPTION_REDIRECT_POLICY, &noRedirect, sizeof noRedirect);
    std::wstring hs;
    for (auto& [k, v] : headers) hs += wide(k) + L": " + wide(v) + L"\r\n";
    hs += L"Accept: application/json\r\n";
    if (!WinHttpSendRequest(req.h, hs.c_str(), (DWORD)-1L, body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(), (DWORD)body.size(),
                            (DWORD)body.size(), 0) ||
        !WinHttpReceiveResponse(req.h, nullptr)) {
        r.error = describe(GetLastError());
        return r;
    }
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                        WINHTTP_NO_HEADER_INDEX);
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail)) {
            r.error = describe(GetLastError());
            return r;
        }
        if (!avail) break;
        if (r.body.size() + avail > 4 * 1024 * 1024) break;   // nothing we ask for is this big
        const std::size_t at = r.body.size();
        r.body.resize(at + avail);
        DWORD got = 0;
        if (!WinHttpReadData(req.h, r.body.data() + at, avail, &got)) {
            r.error = describe(GetLastError());
            return r;
        }
        r.body.resize(at + got);
    }
    r.status = (int)status;
    return r;
}

}  // namespace ncap::http
