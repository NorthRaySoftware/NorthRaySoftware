#include "output/twitch_chat.hpp"

#include "log.hpp"
#include "video/design_render.hpp"

#include <windows.h>
#include <winhttp.h>

#include <chrono>
#include <cstdlib>
#include <vector>

namespace ncap::output {

namespace {

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string errorText(DWORD e) {
    switch (e) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "can't find Twitch's chat server (no internet?)";
    case ERROR_WINHTTP_CANNOT_CONNECT: return "can't connect to Twitch's chat server";
    case ERROR_WINHTTP_TIMEOUT: return "Twitch's chat server didn't answer in time";
    case ERROR_WINHTTP_SECURE_FAILURE: return "the chat server's certificate couldn't be checked";
    case ERROR_WINHTTP_CONNECTION_ERROR: return "the connection to chat was lost";
    case ERROR_WINHTTP_OPERATION_CANCELLED: return "stopped";
    case ERROR_WINHTTP_INVALID_SERVER_RESPONSE: return "the chat server sent something unexpected";
    default: return "chat connection error " + std::to_string(e);
    }
}

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
};

// Where to connect. Twitch's chat WebSocket, or (for NorthRay's own tests only) the address in
// NORTHRAY_CHAT_TEST_URL, e.g. ws://127.0.0.1:8765/.
std::wstring chatUrl() {
    if (const char* t = std::getenv("NORTHRAY_CHAT_TEST_URL"); t && *t) return wide(t);
    return L"wss://irc-ws.chat.twitch.tv:443/";
}

}  // namespace

TwitchChat::~TwitchChat() { stopThread(); }

std::string TwitchChat::channel() const {
    std::lock_guard<std::mutex> l(m_);
    return channel_;
}

TwitchChat::Status TwitchChat::status() const {
    std::lock_guard<std::mutex> l(m_);
    return status_;
}

void TwitchChat::setState(State s, const std::string& error, int retry) {
    std::lock_guard<std::mutex> l(m_);
    status_.state = s;
    status_.error = error;
    status_.retryInS = retry;
}

void TwitchChat::watch(const std::string& channel) {
    if (channel == this->channel() && (thread_.joinable() || channel.empty())) return;
    stopThread();
    {
        std::lock_guard<std::mutex> l(m_);
        channel_ = channel;
        status_ = Status{};
        status_.channel = channel;
        log_.clear();
    }
    video::setDesignChat({});
    if (channel.empty()) return;
    stop_ = false;
    thread_ = std::thread([this, channel] { run(channel); });
}

void TwitchChat::stopThread() {
    stop_ = true;
    {
        std::lock_guard<std::mutex> l(m_);
        if (socket_) WinHttpCloseHandle((HINTERNET)socket_);   // ends a waiting read
        socket_ = nullptr;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> l(m_);
    status_.state = State::Off;
}

void TwitchChat::run(std::string channel) {
    int backoff = 2;
    const std::wstring url = chatUrl();
    while (!stop_) {
        setState(State::Connecting);
        std::string failure;
        bool everJoined = false;
        [&] {
            URL_COMPONENTS uc{};
            uc.dwStructSize = sizeof uc;
            wchar_t host[256] = {}, path[1024] = {};
            uc.lpszHostName = host;
            uc.dwHostNameLength = 255;
            uc.lpszUrlPath = path;
            uc.dwUrlPathLength = 1023;
            // ws:// and wss:// aren't schemes WinHTTP knows: crack them as http(s).
            std::wstring u = url;
            bool secure = true;
            if (u.rfind(L"wss://", 0) == 0) u = L"https://" + u.substr(6);
            else if (u.rfind(L"ws://", 0) == 0) u = L"http://" + u.substr(5), secure = false;
            if (!WinHttpCrackUrl(u.c_str(), 0, 0, &uc)) {
                failure = "the chat address isn't valid";
                return;
            }
            Handle session, connect, req;
#ifdef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
            session.h = WinHttpOpen(L"NorthRayCapture", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
#endif
            if (!session.h) session.h = WinHttpOpen(L"NorthRayCapture", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (!session.h) {
                failure = errorText(GetLastError());
                return;
            }
            WinHttpSetTimeouts(session.h, 10000, 10000, 10000, 0);   // reads wait as long as chat is quiet
            connect.h = WinHttpConnect(session.h, host, uc.nPort, 0);
            if (connect.h)
                req.h = WinHttpOpenRequest(connect.h, L"GET", path[0] ? path : L"/", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           secure ? WINHTTP_FLAG_SECURE : 0);
            if (!req.h || !WinHttpSetOption(req.h, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) ||
                !WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) || !WinHttpReceiveResponse(req.h, nullptr)) {
                failure = errorText(GetLastError());
                return;
            }
            DWORD code = 0, len = sizeof code;
            WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &len, nullptr);
            if (code != 101) {
                failure = "Twitch's chat server answered " + std::to_string(code);
                return;
            }
            HINTERNET ws = WinHttpWebSocketCompleteUpgrade(req.h, 0);
            if (!ws) {
                failure = errorText(GetLastError());
                return;
            }
            {
                std::lock_guard<std::mutex> l(m_);
                if (stop_) {
                    WinHttpCloseHandle(ws);
                    return;
                }
                socket_ = ws;
            }
            auto send = [&](const std::string& line) {
                const std::string s = line + "\r\n";
                return WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)s.data(), (DWORD)s.size()) == NO_ERROR;
            };
            bool ok = true;
            for (const auto& l : chat::loginLines(channel, (unsigned)GetTickCount64())) ok = ok && send(l);
            std::string pending;
            std::vector<char> buf(16384);
            while (ok && !stop_) {
                DWORD got = 0;
                WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
                const DWORD r = WinHttpWebSocketReceive(ws, buf.data(), (DWORD)buf.size(), &got, &type);
                if (r != NO_ERROR) {
                    failure = errorText(r);
                    break;
                }
                if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                    failure = "Twitch closed the chat connection";
                    break;
                }
                pending.append(buf.data(), got);
                if (pending.size() > 1 << 20) pending.clear();   // a broken stream: start again
                std::size_t nl;
                while ((nl = pending.find("\r\n")) != std::string::npos) {
                    const std::string raw = pending.substr(0, nl);
                    pending.erase(0, nl + 2);
                    chat::IrcLine line;
                    if (!chat::parseLine(raw, line)) continue;
                    const chat::Event e = chat::interpret(line, (std::int64_t)GetTickCount64());
                    if (e.type == chat::Event::Ping) ok = send("PONG :" + e.arg);
                    else if (e.type == chat::Event::Reconnect) {
                        failure = "Twitch asked to reconnect";
                        ok = false;
                    } else if (e.type == chat::Event::Joined && !everJoined) {
                        everJoined = true;
                        backoff = 2;
                        setState(State::Connected);
                        log::info("chat: joined a channel");
                    } else if (e.type == chat::Event::Notice && !everJoined) {
                        failure = e.arg.empty() ? "Twitch refused" : e.arg;
                        ok = false;
                    } else {
                        std::vector<chat::Message> copy;
                        bool changed = false;
                        {
                            std::lock_guard<std::mutex> l(m_);
                            changed = log_.apply(e);
                            if (changed) {
                                copy = log_.messages();
                                status_.messages = copy.size();
                            }
                        }
                        if (changed) video::setDesignChat(std::move(copy));
                    }
                }
            }
            std::lock_guard<std::mutex> l(m_);
            if (socket_) {
                WinHttpWebSocketClose((HINTERNET)socket_, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
                WinHttpCloseHandle((HINTERNET)socket_);
                socket_ = nullptr;
            }
        }();
        if (stop_) break;
        if (failure.empty()) failure = "the connection ended";
        log::info("chat: " + failure + "; retrying in " + std::to_string(backoff) + " s");
        for (int left = backoff; left > 0 && !stop_; --left) {
            setState(State::Waiting, failure, left);
            std::unique_lock<std::mutex> l(m_);
            cv_.wait_for(l, std::chrono::seconds(1), [this] { return stop_.load(); });
        }
        backoff = std::min(60, backoff * 2);
    }
}

}  // namespace ncap::output
