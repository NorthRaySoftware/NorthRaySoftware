// Live Twitch chat, read-only: an anonymous connection (no account, no sign-in) to Twitch's chat
// WebSocket that joins one channel and passes its messages to the designs (video::setDesignChat).
// Reconnects by itself (2 s doubling to a minute). Sends nothing but the anonymous login, the JOIN and
// answers to Twitch's PINGs. The IRC parts are in core/chat.
#pragma once

#include "ncap/chat.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace ncap::output {

class TwitchChat {
public:
    TwitchChat() = default;
    ~TwitchChat();
    TwitchChat(const TwitchChat&) = delete;
    TwitchChat& operator=(const TwitchChat&) = delete;

    // Joins a channel (leaving any other); "" stops.
    void watch(const std::string& channel);
    std::string channel() const;

    enum class State { Off, Connecting, Connected, Waiting };
    struct Status {
        State state = State::Off;
        std::string channel;
        std::string error;      // why the last try failed (Waiting)
        int retryInS = 0;       // Waiting
        std::size_t messages = 0;
    };
    Status status() const;

private:
    void run(std::string channel);
    void stopThread();
    void setState(State s, const std::string& error = {}, int retry = 0);

    std::thread thread_;
    std::atomic<bool> stop_{false};
    mutable std::mutex m_;
    std::condition_variable cv_;
    void* socket_ = nullptr;   // the WebSocket handle while connected (closed to stop a waiting read)
    std::string channel_;
    Status status_;
    chat::Log log_{100};
};

}  // namespace ncap::output
