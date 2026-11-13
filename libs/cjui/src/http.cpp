#include "cjui/http.hpp"

#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>

#include <vector>

namespace cjui {

namespace {

std::wstring widenUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

struct Handle {
    HINTERNET h = nullptr;
    Handle() = default;
    explicit Handle(HINTERNET x) : h(x) {}
    ~Handle() { if (h) WinHttpCloseHandle(h); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    operator HINTERNET() const { return h; }
};

std::wstring describe(DWORD err) {
    switch (err) {
        case ERROR_WINHTTP_TIMEOUT: return L"The server didn't answer in time.";
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: return L"The server's address couldn't be found.";
        case ERROR_WINHTTP_CANNOT_CONNECT: return L"Couldn't connect to the server.";
        case ERROR_WINHTTP_CONNECTION_ERROR: return L"The connection was dropped.";
        case ERROR_WINHTTP_SECURE_FAILURE: return L"The server's security certificate wasn't accepted.";
        case ERROR_WINHTTP_UNRECOGNIZED_SCHEME: case ERROR_WINHTTP_INVALID_URL: return L"The address isn't valid.";
        default: return L"Network error " + std::to_wstring(err) + L".";
    }
}

// Opens a GET request and waits for the response headers.
bool openRequest(const std::string& url, int timeoutMs, Handle& session, Handle& connect, Handle& request,
                 int& status, std::string& contentType, std::wstring& error) {
    const std::wstring wurl = widenUtf8(url);
    URL_COMPONENTS uc{sizeof uc};
    wchar_t host[256], path[2048];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    uc.dwSchemeLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) { error = L"The address isn't valid."; return false; }
    std::wstring fullPath = std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength) fullPath.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (fullPath.empty()) fullPath = L"/";

    session.h = WinHttpOpen(L"CJCommunity/0.2", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        // Older Windows versions don't know the automatic-proxy flag.
        session.h = WinHttpOpen(L"CJCommunity/0.2", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session) { error = describe(GetLastError()); return false; }
    WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    connect.h = WinHttpConnect(session, std::wstring(uc.lpszHostName, uc.dwHostNameLength).c_str(), uc.nPort, 0);
    if (!connect) { error = describe(GetLastError()); return false; }
    const DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    request.h = WinHttpOpenRequest(connect, L"GET", fullPath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) { error = describe(GetLastError()); return false; }
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        error = describe(GetLastError());
        return false;
    }
    DWORD code = 0, size = sizeof code;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &code, &size, WINHTTP_NO_HEADER_INDEX);
    status = (int)code;
    wchar_t ct[256];
    DWORD ctSize = sizeof ct;
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, ct, &ctSize,
                            WINHTTP_NO_HEADER_INDEX)) {
        std::wstring w(ct, ctSize / sizeof(wchar_t));
        contentType.assign(w.begin(), w.end());
    }
    if (status != 200) error = L"The server answered with status " + std::to_wstring(status) + L".";
    return true;
}

}  // namespace

HttpResult httpGet(const std::string& url, int timeoutMs, size_t maxBytes) {
    HttpResult r;
    Handle session, connect, request;
    if (!openRequest(url, timeoutMs, session, connect, request, r.status, r.contentType, r.error)) return r;
    if (r.status != 200) return r;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail)) { r.status = 0; r.error = describe(GetLastError()); break; }
        if (avail == 0) break;
        if (r.body.size() + avail > maxBytes) { r.status = 0; r.error = L"The server's reply was too large."; break; }
        std::vector<char> buf(avail);
        DWORD read = 0;
        if (!WinHttpReadData(request, buf.data(), avail, &read)) { r.status = 0; r.error = describe(GetLastError()); break; }
        r.body.append(buf.data(), read);
    }
    return r;
}

int httpStream(const std::string& url, const std::function<bool(const uint8_t*, size_t)>& onData,
               const std::atomic<bool>& cancel, std::wstring& error, std::string* contentType) {
    Handle session, connect, request;
    int status = 0;
    std::string ct;
    if (!openRequest(url, 15000, session, connect, request, status, ct, error)) return 0;
    if (contentType) *contentType = ct;
    if (status != 200) return status;
    std::vector<uint8_t> buf(16 * 1024);
    while (!cancel.load()) {
        DWORD read = 0;
        if (!WinHttpReadData(request, buf.data(), (DWORD)buf.size(), &read)) { error = describe(GetLastError()); break; }
        if (read == 0) { error = L"The stream ended."; break; }
        if (!onData(buf.data(), read)) break;
    }
    return status;
}

bool openInBrowser(const std::string& url) {
    if (url.rfind("https://", 0) != 0) return false;
    return (INT_PTR)ShellExecuteW(nullptr, L"open", widenUtf8(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL) > 32;
}

}  // namespace cjui
