#include "ncap/rtmp.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <random>

namespace ncap::rtmp {

namespace {

std::uint64_t nowMs() {
    return (std::uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void put16(Bytes& b, std::uint32_t v) {
    b.push_back((std::uint8_t)(v >> 8));
    b.push_back((std::uint8_t)v);
}
void put24(Bytes& b, std::uint32_t v) {
    b.push_back((std::uint8_t)(v >> 16));
    b.push_back((std::uint8_t)(v >> 8));
    b.push_back((std::uint8_t)v);
}
void put32(Bytes& b, std::uint32_t v) {
    b.push_back((std::uint8_t)(v >> 24));
    put24(b, v);
}
std::uint32_t get24(const std::uint8_t* p) { return ((std::uint32_t)p[0] << 16) | ((std::uint32_t)p[1] << 8) | p[2]; }
std::uint32_t get32(const std::uint8_t* p) { return ((std::uint32_t)p[0] << 24) | get24(p + 1); }

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

}  // namespace

// ---------------------------------------------------------------- URLs

bool parseUrl(const std::string& text, Url& out, std::string* keyFromUrl) {
    std::string s = text;
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(0, 1);
    const std::string l = lower(s);
    std::size_t at;
    if (l.rfind("rtmps://", 0) == 0) {
        out.tls = true;
        at = 8;
    } else if (l.rfind("rtmp://", 0) == 0) {
        out.tls = false;
        at = 7;
    } else {
        return false;
    }
    const std::size_t slash = s.find('/', at);
    std::string hostPort = s.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
    std::string path = slash == std::string::npos ? "" : s.substr(slash + 1);
    out.port = out.tls ? 443 : 1935;
    const std::size_t colon = hostPort.rfind(':');
    if (colon != std::string::npos && hostPort.find(']') == std::string::npos) {
        const std::string p = hostPort.substr(colon + 1);
        if (p.empty() || p.find_first_not_of("0123456789") != std::string::npos) return false;
        out.port = std::stoi(p);
        if (out.port <= 0 || out.port > 65535) return false;
        hostPort.resize(colon);
    }
    if (hostPort.empty()) return false;
    out.host = hostPort;
    while (!path.empty() && path.back() == '/') path.pop_back();
    const std::size_t seg = path.find('/');
    out.app = seg == std::string::npos ? path : path.substr(0, seg);
    if (keyFromUrl) *keyFromUrl = seg == std::string::npos ? "" : path.substr(seg + 1);
    if (out.app.empty()) return false;
    const bool defaultPort = out.port == (out.tls ? 443 : 1935);
    out.tcUrl = std::string(out.tls ? "rtmps://" : "rtmp://") + out.host + (defaultPort ? "" : ":" + std::to_string(out.port)) + "/" + out.app;
    return true;
}

// ---------------------------------------------------------------- AMF0

Amf Amf::num(double v) {
    Amf a;
    a.type = Number;
    a.number = v;
    return a;
}
Amf Amf::str(const std::string& v) {
    Amf a;
    a.type = String;
    a.string = v;
    return a;
}
Amf Amf::boolean_(bool v) {
    Amf a;
    a.type = Boolean;
    a.boolean = v;
    return a;
}
Amf Amf::null() { return Amf(); }
Amf Amf::object(std::vector<std::pair<std::string, Amf>> p) {
    Amf a;
    a.type = Object;
    a.props = std::move(p);
    return a;
}
Amf Amf::array(std::vector<std::pair<std::string, Amf>> p) {
    Amf a;
    a.type = EcmaArray;
    a.props = std::move(p);
    return a;
}
const Amf* Amf::get(const std::string& key) const {
    for (auto& [k, v] : props)
        if (k == key) return &v;
    return nullptr;
}

namespace {
void amfKey(Bytes& out, const std::string& k) {
    put16(out, (std::uint32_t)k.size());
    out.insert(out.end(), k.begin(), k.end());
}
}  // namespace

void amfWrite(Bytes& out, const Amf& v) {
    switch (v.type) {
        case Amf::Number: {
            out.push_back(0x00);
            std::uint64_t u;
            std::memcpy(&u, &v.number, 8);
            for (int i = 7; i >= 0; --i) out.push_back((std::uint8_t)(u >> (8 * i)));
            break;
        }
        case Amf::Boolean:
            out.push_back(0x01);
            out.push_back(v.boolean ? 1 : 0);
            break;
        case Amf::String:
            out.push_back(0x02);
            amfKey(out, v.string);
            break;
        case Amf::Object:
        case Amf::EcmaArray:
            if (v.type == Amf::Object) out.push_back(0x03);
            else {
                out.push_back(0x08);
                put32(out, (std::uint32_t)v.props.size());
            }
            for (auto& [k, p] : v.props) {
                amfKey(out, k);
                amfWrite(out, p);
            }
            out.push_back(0);
            out.push_back(0);
            out.push_back(0x09);
            break;
        case Amf::Undefined: out.push_back(0x06); break;
        case Amf::Null: out.push_back(0x05); break;
    }
}

bool amfRead(const Bytes& in, std::size_t& pos, Amf& out) {
    if (pos >= in.size()) return false;
    const std::uint8_t marker = in[pos++];
    auto readStr = [&](std::string& s) {
        if (pos + 2 > in.size()) return false;
        const std::size_t n = ((std::size_t)in[pos] << 8) | in[pos + 1];
        pos += 2;
        if (pos + n > in.size()) return false;
        s.assign(in.begin() + (long)pos, in.begin() + (long)(pos + n));
        pos += n;
        return true;
    };
    auto readProps = [&](Amf& o) {
        for (int guard = 0; guard < 10000; ++guard) {
            if (pos + 3 <= in.size() && in[pos] == 0 && in[pos + 1] == 0 && in[pos + 2] == 0x09) {
                pos += 3;
                return true;
            }
            std::string k;
            Amf v;
            if (!readStr(k) || !amfRead(in, pos, v)) return false;
            o.props.push_back({k, v});
        }
        return false;
    };
    switch (marker) {
        case 0x00: {
            if (pos + 8 > in.size()) return false;
            std::uint64_t u = 0;
            for (int i = 0; i < 8; ++i) u = (u << 8) | in[pos + (std::size_t)i];
            pos += 8;
            out = Amf();
            out.type = Amf::Number;
            std::memcpy(&out.number, &u, 8);
            return true;
        }
        case 0x01:
            if (pos >= in.size()) return false;
            out = Amf::boolean_(in[pos++] != 0);
            return true;
        case 0x02:
            out = Amf::str("");
            return readStr(out.string);
        case 0x03:
            out = Amf::object({});
            return readProps(out);
        case 0x05: out = Amf::null(); return true;
        case 0x06:
            out = Amf();
            out.type = Amf::Undefined;
            return true;
        case 0x08:
            if (pos + 4 > in.size()) return false;
            pos += 4;
            out = Amf::array({});
            return readProps(out);
        default: return false;
    }
}

// ---------------------------------------------------------------- FLV bodies

Bytes flvVideo(bool keyframe, bool config, const Bytes& payload) {
    Bytes b = {(std::uint8_t)((keyframe ? 0x10 : 0x20) | 7), (std::uint8_t)(config ? 0 : 1), 0, 0, 0};
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}

Bytes flvAudio(bool config, const Bytes& payload) {
    Bytes b = {0xAF, (std::uint8_t)(config ? 0 : 1)};   // AAC, 44 kHz flag (as AAC requires), 16-bit, stereo
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}

// ---------------------------------------------------------------- the session

bool Client::handshake(int timeoutMs, std::string& error) {
    Bytes c01(1 + 1536);
    c01[0] = 3;
    std::mt19937 rng((unsigned)nowMs());
    for (std::size_t i = 9; i < c01.size(); ++i) c01[i] = (std::uint8_t)rng();
    if (!t_.send(c01.data(), c01.size())) {
        error = "The connection to the server failed.";
        return false;
    }
    // S0 + S1 + S2 = 3073 bytes.
    Bytes s(3073);
    std::size_t got = 0;
    const std::uint64_t until = nowMs() + (std::uint64_t)timeoutMs;
    bool sentC2 = false;
    while (got < s.size()) {
        const int left = (int)(until - std::min(until, nowMs()));
        if (left <= 0) {
            error = "The server didn't answer the RTMP handshake in time.";
            return false;
        }
        const int n = t_.receive(s.data() + got, s.size() - got, left);
        if (n < 0) {
            error = "The server closed the connection during the handshake.";
            return false;
        }
        got += (std::size_t)n;
        if (!sentC2 && got >= 1537) {
            if (s[0] != 3) {
                error = "The server isn't speaking RTMP (is the address right?).";
                return false;
            }
            // C2 echoes S1.
            if (!t_.send(s.data() + 1, 1536)) {
                error = "The connection to the server failed.";
                return false;
            }
            sentC2 = true;
        }
    }
    return true;
}

bool Client::sendMessage(int csid, std::uint8_t type, std::uint32_t ts, std::uint32_t streamId, const Bytes& body) {
    Bytes out;
    out.reserve(body.size() + 16 + body.size() / outChunk_ * 5);
    const bool ext = ts >= 0xFFFFFF;
    out.push_back((std::uint8_t)(csid & 0x3F));   // fmt 0
    put24(out, ext ? 0xFFFFFF : ts);
    put24(out, (std::uint32_t)body.size());
    out.push_back(type);
    for (int i = 0; i < 4; ++i) out.push_back((std::uint8_t)(streamId >> (8 * i)));   // little-endian
    if (ext) put32(out, ts);
    for (std::size_t off = 0; off < body.size(); off += outChunk_) {
        if (off) {
            out.push_back((std::uint8_t)(0xC0 | (csid & 0x3F)));   // fmt 3: continuation
            if (ext) put32(out, ts);
        }
        const std::size_t n = std::min<std::size_t>(outChunk_, body.size() - off);
        out.insert(out.end(), body.begin() + (long)off, body.begin() + (long)(off + n));
    }
    if (!t_.send(out.data(), out.size())) return false;
    sent_ += out.size();
    return true;
}

bool Client::command(int csid, std::uint32_t streamId, const std::vector<Amf>& args) {
    Bytes body;
    for (auto& a : args) amfWrite(body, a);
    return sendMessage(csid, 20, 0, streamId, body);
}

bool Client::readMessage(Message& m, int timeoutMs, std::string& error) {
    const std::uint64_t until = nowMs() + (std::uint64_t)std::max(0, timeoutMs);
    for (;;) {
        // Parse a whole chunk from what's buffered, if there is one.
        std::size_t p = 0;
        auto need = [&](std::size_t n) { return p + n <= buf_.size(); };
        bool complete = false;
        if (need(1)) {
            const int fmt = buf_[0] >> 6;
            std::uint32_t csid = buf_[0] & 0x3F;
            p = 1;
            bool ok = true;
            if (csid == 0) {
                ok = need(1);
                if (ok) csid = 64u + buf_[p++];
            } else if (csid == 1) {
                ok = need(2);
                if (ok) {
                    csid = 64u + buf_[p] + 256u * buf_[p + 1];
                    p += 2;
                }
            }
            const std::size_t hdr = fmt == 0 ? 11 : fmt == 1 ? 7 : fmt == 2 ? 3 : 0;
            ok = ok && need(hdr);
            if (ok) {
                InChunk& c = in_[csid];
                std::uint32_t ts = 0;
                if (hdr >= 3) ts = get24(&buf_[p]);
                if (fmt <= 1) {
                    c.length = get24(&buf_[p + 3]);
                    c.type = buf_[p + 6];
                }
                if (fmt == 0) c.streamId = buf_[p + 7] | (buf_[p + 8] << 8) | (buf_[p + 9] << 16) | ((std::uint32_t)buf_[p + 10] << 24);
                p += hdr;
                if (hdr >= 3) c.extended = ts == 0xFFFFFF;
                if (c.extended) {
                    ok = need(4);
                    if (ok) {
                        ts = get32(&buf_[p]);
                        p += 4;
                    }
                }
                if (ok) {
                    const std::size_t want = std::min<std::size_t>(inChunk_, c.length - c.body.size());
                    ok = need(want);
                    if (ok) {
                        if (c.body.empty()) {   // a new message: its time
                            if (fmt == 0) c.timestamp = ts;
                            else if (fmt == 1 || fmt == 2) {
                                c.delta = ts;
                                c.timestamp += ts;
                            } else
                                c.timestamp += c.delta;
                        }
                        c.body.insert(c.body.end(), buf_.begin() + (long)p, buf_.begin() + (long)(p + want));
                        p += want;
                        buf_.erase(buf_.begin(), buf_.begin() + (long)p);
                        if (c.body.size() >= c.length) {
                            m.type = c.type;
                            m.timestamp = c.timestamp;
                            m.streamId = c.streamId;
                            m.body.swap(c.body);
                            c.body.clear();
                            complete = true;
                        }
                        if (complete) return true;
                        continue;   // another chunk may already be buffered
                    }
                }
            }
        }
        // Not enough yet: read more.
        const int left = (int)(until - std::min(until, nowMs()));
        std::uint8_t tmp[16384];
        const int n = t_.receive(tmp, sizeof tmp, left);
        if (n < 0) {
            error = "The server closed the connection.";
            return false;
        }
        if (n == 0) return false;   // timed out (error stays empty)
        buf_.insert(buf_.end(), tmp, tmp + n);
        received_ += (std::uint64_t)n;
        if (received_ - ackedAt_ >= windowAck_ / 2) {   // acknowledge what's been received
            Bytes a;
            put32(a, (std::uint32_t)received_);
            sendMessage(2, 3, 0, 0, a);
            ackedAt_ = received_;
        }
    }
}

void Client::handleControl(const Message& m) {
    if (m.type == 1 && m.body.size() >= 4) inChunk_ = get32(m.body.data()) & 0x7FFFFFFF;
    if (m.type == 5 && m.body.size() >= 4) windowAck_ = std::max<std::uint32_t>(get32(m.body.data()), 4096);
    if (m.type == 4 && m.body.size() >= 6 && m.body[0] == 0 && m.body[1] == 6) {   // ping: answer it
        Bytes pong = {0, 7};
        pong.insert(pong.end(), m.body.begin() + 2, m.body.begin() + 6);
        sendMessage(2, 4, 0, 0, pong);
    }
}

bool Client::waitFor(const std::string& name, double txn, std::vector<Amf>& reply, int timeoutMs, std::string& error) {
    const std::uint64_t until = nowMs() + (std::uint64_t)timeoutMs;
    while (nowMs() < until) {
        Message m;
        if (!readMessage(m, (int)(until - nowMs()), error)) {
            if (!error.empty()) return false;
            continue;
        }
        handleControl(m);
        if (m.type != 20) continue;
        std::vector<Amf> args;
        std::size_t pos = 0;
        Amf a;
        while (pos < m.body.size() && amfRead(m.body, pos, a)) args.push_back(a);
        if (args.empty() || args[0].type != Amf::String) continue;
        const std::string& cmd = args[0].string;
        const double t = args.size() > 1 && args[1].type == Amf::Number ? args[1].number : -1;
        if (cmd == "_error" && t == txn) {
            std::string why;
            for (auto& x : args)
                if (const Amf* d = x.get("description")) why = d->string;
                else if (const Amf* c = x.get("code"); c && why.empty()) why = c->string;
            error = "The server refused: " + (why.empty() ? std::string("no reason given") : why);
            return false;
        }
        if (cmd == name && (name == "onStatus" || t == txn)) {
            reply = args;
            return true;
        }
    }
    if (error.empty()) error = "The server stopped answering (waiting for " + name + ").";
    return false;
}

bool Client::publish(const Url& url, const std::string& key, std::string& error, int timeoutMs) {
    if (!handshake(timeoutMs, error)) return false;
    // Bigger chunks: fewer headers for video.
    Bytes cs;
    put32(cs, 4096);
    if (!sendMessage(2, 1, 0, 0, cs)) {
        error = "The connection to the server failed.";
        return false;
    }
    outChunk_ = 4096;
    if (!command(3, 0, {Amf::str("connect"), Amf::num(1),
                        Amf::object({{"app", Amf::str(url.app)},
                                     {"type", Amf::str("nonprivate")},
                                     {"flashVer", Amf::str("FMLE/3.0 (compatible; NorthRay Capture)")},
                                     {"swfUrl", Amf::str(url.tcUrl)},
                                     {"tcUrl", Amf::str(url.tcUrl)}})})) {
        error = "The connection to the server failed.";
        return false;
    }
    std::vector<Amf> reply;
    if (!waitFor("_result", 1, reply, timeoutMs, error)) return false;
    command(3, 0, {Amf::str("releaseStream"), Amf::num(2), Amf::null(), Amf::str(key)});
    command(3, 0, {Amf::str("FCPublish"), Amf::num(3), Amf::null(), Amf::str(key)});
    if (!command(3, 0, {Amf::str("createStream"), Amf::num(4), Amf::null()}) || !waitFor("_result", 4, reply, timeoutMs, error)) {
        if (error.empty()) error = "The server didn't create a stream.";
        return false;
    }
    streamId_ = reply.size() > 3 && reply[3].type == Amf::Number ? (std::uint32_t)reply[3].number : 1;
    if (!command(8, streamId_, {Amf::str("publish"), Amf::num(5), Amf::null(), Amf::str(key), Amf::str("live")})) {
        error = "The connection to the server failed.";
        return false;
    }
    if (!waitFor("onStatus", 0, reply, timeoutMs, error)) return false;
    for (auto& a : reply)
        if (const Amf* code = a.get("code")) {
            if (code->string == "NetStream.Publish.Start") return true;
            const Amf* d = a.get("description");
            error = "Publishing failed: " + code->string + (d && !d->string.empty() ? " (" + d->string + ")" : "") +
                    (code->string.find("BadName") != std::string::npos ? ". Check the stream key." : "");
            return false;
        }
    error = "The server's answer to publishing wasn't understood.";
    return false;
}

bool Client::metadata(int width, int height, double fps, int videoKbps, int audioKbps) {
    Bytes body;
    amfWrite(body, Amf::str("@setDataFrame"));
    amfWrite(body, Amf::str("onMetaData"));
    amfWrite(body, Amf::array({{"duration", Amf::num(0)},
                               {"width", Amf::num(width)},
                               {"height", Amf::num(height)},
                               {"videodatarate", Amf::num(videoKbps)},
                               {"framerate", Amf::num(fps)},
                               {"videocodecid", Amf::num(7)},
                               {"audiodatarate", Amf::num(audioKbps)},
                               {"audiosamplerate", Amf::num(48000)},
                               {"audiosamplesize", Amf::num(16)},
                               {"stereo", Amf::boolean_(true)},
                               {"audiocodecid", Amf::num(10)},
                               {"encoder", Amf::str("NorthRay Capture")}}));
    return sendMessage(4, 18, 0, streamId_, body);
}

bool Client::videoConfig(const Bytes& avcC) { return sendMessage(6, 9, 0, streamId_, flvVideo(true, true, avcC)); }
bool Client::audioConfig(const Bytes& asc) { return sendMessage(5, 8, 0, streamId_, flvAudio(true, asc)); }
bool Client::video(std::uint32_t ms, bool keyframe, const Bytes& data) { return sendMessage(6, 9, ms, streamId_, flvVideo(keyframe, false, data)); }
bool Client::audio(std::uint32_t ms, const Bytes& data) { return sendMessage(5, 8, ms, streamId_, flvAudio(false, data)); }

void Client::unpublish(const std::string& key) {
    command(3, 0, {Amf::str("FCUnpublish"), Amf::num(6), Amf::null(), Amf::str(key)});
    command(3, 0, {Amf::str("deleteStream"), Amf::num(7), Amf::null(), Amf::num(streamId_)});
}

bool Client::service(std::string& error, int timeoutMs) {
    Message m;
    while (readMessage(m, timeoutMs, error)) {
        timeoutMs = 0;
        handleControl(m);
        if (m.type != 20) continue;
        std::vector<Amf> args;
        std::size_t pos = 0;
        Amf a;
        while (pos < m.body.size() && amfRead(m.body, pos, a)) args.push_back(a);
        if (args.empty() || args[0].type != Amf::String || args[0].string != "onStatus") continue;
        for (auto& x : args)
            if (const Amf* code = x.get("level"); code && code->string == "error") {
                const Amf* c = x.get("code");
                error = "The server ended the stream: " + (c ? c->string : std::string("an error"));
                return false;
            }
    }
    return error.empty();
}

}  // namespace ncap::rtmp
