// NorthRay Capture: one streaming destination (system 8: "Distribute → Multiple Destinations").
//
// A destination is fed encoded packets (from an encode it shares with other destinations at the same
// bitrate) and sends them to one server on its own thread, with its own buffer: a slow or failed
// destination never holds up the encode or the others. It connects once the stream's configuration
// (SPS/PPS, AAC) is known, starts every connection at a keyframe with timestamps from 0, reconnects
// after 2, 4, 8... up to the chosen longest wait, and keeps at most ~3 s waiting (older groups of frames
// are dropped from a keyframe on, and counted). A refused stream key on the first attempt isn't retried.
#pragma once

#include "ncap/media.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace ncap::output {

struct DestinationStatus {
    enum State { Off, Waiting, Connecting, Live, Reconnecting, Failed } state = Off;
    std::wstring message;          // in plain words: what's happening, why it's reconnecting, why it failed
    double seconds = 0;            // live time (this connection and earlier ones)
    double kbps = 0;               // sent in the last second or so
    std::uint64_t droppedFrames = 0;
    std::uint64_t bytesSent = 0;
    int attempts = 0;              // reconnection attempts in a row
    int reconnects = 0;            // successful reconnections since it started
    int retryIn = 0;               // seconds until the next attempt
    int connectMs = 0;             // how long the last connection took (TCP + TLS + RTMP), a rough diagnostic
};

class Destination {
public:
    struct Settings {
        std::string name;          // "Twitch"
        std::string url, key;      // the key is never logged or put in a message
        int width = 1920, height = 1080, fps = 60, videoKbps = 6000, audioKbps = 160;
        int longestWaitSeconds = 60;
    };
    ~Destination() { stop(); }
    void start(const Settings& s);
    void stop();                   // politely (FCUnpublish) if connected
    bool active() const { return running_.load() && !finished_.load(); }   // false once it has failed by itself
    DestinationStatus status() const;
    const Settings& settings() const { return s_; }

    // From the encode (any thread). The configuration arrives once, before or with the first keyframe.
    void configure(const media::Bytes& avcC, const media::Bytes& aacConfig);
    void push(const media::Packet& p);   // decode order; video packets length-prefixed

private:
    void run();
    Settings s_;
    std::thread thread_;
    std::atomic<bool> running_{false}, stopping_{false}, connected_{false}, finished_{false};   // drops count only while connected
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<media::Packet> queue_;
    media::Bytes avcC_, aac_;
    std::uint64_t dropped_ = 0;
    DestinationStatus st_;
};

}  // namespace ncap::output
