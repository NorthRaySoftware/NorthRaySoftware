// NorthRay Capture: the RTMP client for streaming (system 8). Portable: it talks through a
// Transport (Winsock or TLS over it on Windows; plain sockets in the tests, against a real server),
// so the protocol is tested end to end without Windows.
//
//   AMF0       the encoding RTMP's commands use (numbers, strings, objects).
//   Chunking   messages split into chunks; incoming chunks put back together.
//   Session    handshake, connect, createStream, publish, then FLV-style audio and video messages.
//   FLV        the audio and video message bodies: H.264 (AVC) and AAC, with their configuration.
#pragma once

#include "ncap/media.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ncap::rtmp {

using media::Bytes;

// ---------------------------------------------------------------- URLs

struct Url {
    bool tls = false;       // rtmps
    std::string host;
    int port = 1935;        // 443 for rtmps when not given
    std::string app;        // "app", "live2"
    std::string tcUrl;      // "rtmp://host:port/app" as the server is told it
};
// "rtmp://live.example.net/app", "rtmps://a.rtmps.youtube.com:443/live2". A key pasted on the end of
// the URL (".../app/live_123") is not part of the app: `keyFromUrl` gets it, if any.
bool parseUrl(const std::string& text, Url& out, std::string* keyFromUrl = nullptr);

// ---------------------------------------------------------------- AMF0

struct Amf {
    enum Type { Number, Boolean, String, Object, Null, Undefined, EcmaArray } type = Null;
    double number = 0;
    bool boolean = false;
    std::string string;
    std::vector<std::pair<std::string, Amf>> props;   // Object and EcmaArray, in order
    static Amf num(double v);
    static Amf str(const std::string& v);
    static Amf boolean_(bool v);
    static Amf null();
    static Amf object(std::vector<std::pair<std::string, Amf>> p);
    static Amf array(std::vector<std::pair<std::string, Amf>> p);
    const Amf* get(const std::string& key) const;   // a property, or null
};
void amfWrite(Bytes& out, const Amf& v);
// Reads one value at `pos`; false if the data is cut short or unknown.
bool amfRead(const Bytes& in, std::size_t& pos, Amf& out);

// ---------------------------------------------------------------- the connection

class Transport {
public:
    virtual ~Transport() = default;
    virtual bool send(const std::uint8_t* data, std::size_t size) = 0;
    // Waits up to `timeoutMs`: bytes read, 0 on timeout, -1 if the connection closed or failed.
    virtual int receive(std::uint8_t* data, std::size_t size, int timeoutMs) = 0;
};

struct Message {
    std::uint8_t type = 0;
    std::uint32_t timestamp = 0, streamId = 0;
    Bytes body;
};

class Client {
public:
    explicit Client(Transport& t) : t_(t) {}
    // Handshake, connect, publish `key`. False with `error` in plain words (the server's own
    // words where it gave any: "Publishing failed: NetStream.Publish.BadName").
    bool publish(const Url& url, const std::string& key, std::string& error, int timeoutMs = 10000);
    // @setDataFrame onMetaData: what the stream is.
    bool metadata(int width, int height, double fps, int videoKbps, int audioKbps);
    // The decoder configuration: before the first frame, and again after reconnecting.
    bool videoConfig(const Bytes& avcC);
    bool audioConfig(const Bytes& audioSpecificConfig);
    // `data`: length-prefixed NAL units / one raw AAC frame. Times in ms from the stream's start.
    bool video(std::uint32_t ms, bool keyframe, const Bytes& data);
    bool audio(std::uint32_t ms, const Bytes& data);
    // Reads what the server has sent (acknowledgements, pings, status) without waiting long.
    // False if the server ended the stream or the connection broke (`error` says why).
    bool service(std::string& error, int timeoutMs = 0);
    // Ends the stream politely (FCUnpublish, deleteStream) before the connection is closed.
    void unpublish(const std::string& key);
    std::uint64_t bytesSent() const { return sent_; }

private:
    bool handshake(int timeoutMs, std::string& error);
    bool sendMessage(int csid, std::uint8_t type, std::uint32_t ts, std::uint32_t streamId, const Bytes& body);
    bool command(int csid, std::uint32_t streamId, const std::vector<Amf>& args);
    // Waits for a command reply (or onStatus) and handles protocol messages on the way.
    bool waitFor(const std::string& name, double txn, std::vector<Amf>& reply, int timeoutMs, std::string& error);
    bool readMessage(Message& m, int timeoutMs, std::string& error);   // false on timeout (error empty) or failure
    void handleControl(const Message& m);

    Transport& t_;
    std::uint32_t outChunk_ = 128, inChunk_ = 128;
    std::uint32_t streamId_ = 0;
    std::uint64_t sent_ = 0, received_ = 0, ackedAt_ = 0;
    std::uint32_t windowAck_ = 2500000;
    struct InChunk {
        std::uint32_t timestamp = 0, delta = 0, length = 0, streamId = 0;
        std::uint8_t type = 0;
        Bytes body;
        bool extended = false;
    };
    std::map<std::uint32_t, InChunk> in_;
    Bytes buf_;   // bytes received but not yet parsed
};

// FLV message bodies (also what an FLV file's tags hold).
Bytes flvVideo(bool keyframe, bool config, const Bytes& payload);
Bytes flvAudio(bool config, const Bytes& payload);

}  // namespace ncap::rtmp
