#include "ncap/mp4.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ncap::mp4 {

bool MemoryInput::read(std::uint64_t offset, std::uint8_t* out, std::size_t n) {
    if (offset > b_.size() || n > b_.size() - offset) return false;
    std::memcpy(out, b_.data() + offset, n);
    return true;
}

std::string mp4NameFor(const std::string& p) {
    const std::size_t dot = p.find_last_of('.');
    const std::size_t slash = p.find_last_of("\\/");
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        std::string ext = p.substr(dot);
        for (char& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext == ".mkv") return p.substr(0, dot) + ".mp4";
    }
    return p + ".mp4";
}

// ---------------------------------------------------------------- reading Matroska

namespace {

constexpr std::uint32_t kEbml = 0x1A45DFA3, kSegment = 0x18538067, kInfo = 0x1549A966, kTimecodeScale = 0x2AD7B1;
constexpr std::uint32_t kTracks = 0x1654AE6B, kTrackEntry = 0xAE, kTrackNumber = 0xD7, kTrackType = 0x83, kCodecId = 0x86,
                        kCodecPrivate = 0x63A2, kName = 0x536E, kVideo = 0xE0, kPixelWidth = 0xB0, kPixelHeight = 0xBA, kAudio = 0xE1,
                        kSamplingFrequency = 0xB5, kChannels = 0x9F;
constexpr std::uint32_t kCluster = 0x1F43B675, kTimecode = 0xE7, kSimpleBlock = 0xA3, kBlockGroup = 0xA0, kBlock = 0xA1,
                        kReferenceBlock = 0xFB;
constexpr std::uint32_t kCues = 0x1C53BB6B, kSeekHead = 0x114D9B74, kTags = 0x1254C367, kChapters = 0x1043A770, kAttachments = 0x1941A469;
constexpr std::uint64_t kUnknownSize = ~0ull;

bool topLevel(std::uint32_t id) {
    return id == kCluster || id == kCues || id == kSeekHead || id == kTags || id == kChapters || id == kAttachments || id == kInfo || id == kTracks;
}

struct Reader {
    Input& in;
    std::uint64_t size;
    // An element header at `at`: its ID (with the length marker, as IDs are written) and its size.
    bool header(std::uint64_t at, std::uint32_t& id, std::uint64_t& len, std::uint64_t& dataAt) {
        std::uint8_t b[12];
        const std::size_t n = (std::size_t)std::min<std::uint64_t>(12, size > at ? size - at : 0);
        if (n < 2 || !in.read(at, b, n)) return false;
        int idLen = 1;
        while (idLen <= 4 && !(b[0] & (0x80 >> (idLen - 1)))) ++idLen;
        if (idLen > 4 || (std::size_t)idLen >= n) return false;
        id = 0;
        for (int i = 0; i < idLen; ++i) id = (id << 8) | b[i];
        const std::uint8_t first = b[idLen];
        int sLen = 1;
        while (sLen <= 8 && !(first & (0x80 >> (sLen - 1)))) ++sLen;
        if (sLen > 8 || (std::size_t)(idLen + sLen) > n) return false;
        std::uint64_t v = first & (0xFF >> sLen);
        bool allOnes = v == (0xFFu >> sLen);
        for (int i = 1; i < sLen; ++i) {
            v = (v << 8) | b[idLen + i];
            allOnes = allOnes && b[idLen + i] == 0xFF;
        }
        len = allOnes ? kUnknownSize : v;
        dataAt = at + (std::uint64_t)idLen + (std::uint64_t)sLen;
        return true;
    }
    bool bytes(std::uint64_t at, std::uint64_t n, Bytes& out) {
        if (n > (64u << 20) || at > size || n > size - at) return false;
        out.resize((std::size_t)n);
        return n == 0 || in.read(at, out.data(), (std::size_t)n);
    }
    std::uint64_t uint(std::uint64_t at, std::uint64_t n) {
        Bytes b;
        if (n > 8 || !bytes(at, n, b)) return 0;
        std::uint64_t v = 0;
        for (auto c : b) v = (v << 8) | c;
        return v;
    }
    double flt(std::uint64_t at, std::uint64_t n) {
        const std::uint64_t v = uint(at, n);
        if (n == 4) {
            float f;
            const std::uint32_t u = (std::uint32_t)v;
            std::memcpy(&f, &u, 4);
            return f;
        }
        double d;
        std::memcpy(&d, &v, 8);
        return n == 8 ? d : 0;
    }
};

}  // namespace

bool scan(Input& in, Contents& out, std::string& error) {
    out = Contents{};
    Reader r{in, in.size()};
    std::uint32_t id;
    std::uint64_t len, data;
    std::uint64_t pos = 0;
    if (!r.header(0, id, len, data) || id != kEbml) {
        error = "it isn't an MKV file";
        return false;
    }
    pos = data + len;
    if (!r.header(pos, id, len, data) || id != kSegment) {
        error = "the MKV file has no recording in it";
        return false;
    }
    const std::uint64_t segEnd = len == kUnknownSize || data + len > r.size ? r.size : data + len;
    if (len == kUnknownSize || data + len > r.size) out.cutShort = true;
    std::uint64_t scale = 1000000;   // nanoseconds per timecode tick
    bool sawCues = false;
    std::vector<int> trackIndex(128, -1);   // Matroska track number -> index
    pos = data;
    while (pos < segEnd) {
        if (!r.header(pos, id, len, data)) break;
        const bool unknown = len == kUnknownSize;
        const std::uint64_t end = unknown ? segEnd : std::min(segEnd, data + len);
        if (!unknown && data + len > segEnd) out.cutShort = true;
        if (id == kInfo) {
            for (std::uint64_t p = data; p < end;) {
                std::uint32_t cid;
                std::uint64_t clen, cdata;
                if (!r.header(p, cid, clen, cdata) || clen == kUnknownSize) break;
                if (cid == kTimecodeScale) scale = std::max<std::uint64_t>(1, r.uint(cdata, clen));
                p = cdata + clen;
            }
        } else if (id == kTracks) {
            for (std::uint64_t p = data; p < end;) {
                std::uint32_t tid;
                std::uint64_t tlen, tdata;
                if (!r.header(p, tid, tlen, tdata) || tlen == kUnknownSize) break;
                if (tid == kTrackEntry) {
                    Track t;
                    int type = 0;
                    for (std::uint64_t q = tdata; q < tdata + tlen;) {
                        std::uint32_t eid;
                        std::uint64_t elen, edata;
                        if (!r.header(q, eid, elen, edata) || elen == kUnknownSize) break;
                        Bytes b;
                        if (eid == kTrackNumber) t.number = (int)r.uint(edata, elen);
                        else if (eid == kTrackType) type = (int)r.uint(edata, elen);
                        else if (eid == kCodecId && r.bytes(edata, elen, b)) t.codec.assign(b.begin(), b.end());
                        else if (eid == kCodecPrivate) r.bytes(edata, elen, t.priv);
                        else if (eid == kName && r.bytes(edata, elen, b)) t.name.assign(b.begin(), b.end());
                        else if (eid == kVideo || eid == kAudio) {
                            for (std::uint64_t s = edata; s < edata + elen;) {
                                std::uint32_t sid;
                                std::uint64_t slen, sdata;
                                if (!r.header(s, sid, slen, sdata) || slen == kUnknownSize) break;
                                if (sid == kPixelWidth) t.width = (int)r.uint(sdata, slen);
                                if (sid == kPixelHeight) t.height = (int)r.uint(sdata, slen);
                                if (sid == kSamplingFrequency) t.rate = (int)std::lround(r.flt(sdata, slen));
                                if (sid == kChannels) t.channels = (int)r.uint(sdata, slen);
                                s = sdata + slen;
                            }
                        }
                        q = edata + elen;
                    }
                    t.video = type == 1;
                    if ((type == 1 || type == 2) && t.number > 0 && t.number < 128) {
                        trackIndex[(size_t)t.number] = (int)out.tracks.size();
                        out.tracks.push_back(t);
                    }
                }
                p = tdata + tlen;
            }
        } else if (id == kCluster) {
            std::int64_t clusterTc = 0;
            std::uint64_t p = data;
            while (p < end) {
                std::uint32_t cid;
                std::uint64_t clen, cdata;
                if (!r.header(p, cid, clen, cdata)) {
                    p = end;
                    break;
                }
                if (unknown && topLevel(cid)) break;   // the next cluster (or the index) starts here
                if (clen == kUnknownSize || cdata + clen > r.size) {   // cut off mid-frame
                    out.cutShort = true;
                    p = r.size;
                    break;
                }
                if (cid == kTimecode) clusterTc = (std::int64_t)r.uint(cdata, clen);
                std::uint64_t blockAt = 0, blockLen = 0;
                bool key = false;
                if (cid == kSimpleBlock) {
                    blockAt = cdata, blockLen = clen;
                } else if (cid == kBlockGroup) {
                    bool referenced = false;
                    for (std::uint64_t q = cdata; q < cdata + clen;) {
                        std::uint32_t bid;
                        std::uint64_t blen, bdata;
                        if (!r.header(q, bid, blen, bdata) || blen == kUnknownSize) break;
                        if (bid == kBlock) blockAt = bdata, blockLen = blen;
                        if (bid == kReferenceBlock) referenced = true;
                        q = bdata + blen;
                    }
                    key = !referenced;
                }
                if (blockLen >= 4) {
                    std::uint8_t h[12];
                    const std::size_t hn = (std::size_t)std::min<std::uint64_t>(12, blockLen);
                    if (in.read(blockAt, h, hn)) {
                        int tl = 1;
                        while (tl <= 8 && !(h[0] & (0x80 >> (tl - 1)))) ++tl;
                        if (tl <= 4 && (std::size_t)(tl + 3) <= hn) {
                            std::uint64_t num = h[0] & (0xFF >> tl);
                            for (int i = 1; i < tl; ++i) num = (num << 8) | h[i];
                            const std::int16_t rel = (std::int16_t)((h[tl] << 8) | h[tl + 1]);
                            const std::uint8_t flags = h[tl + 2];
                            if (flags & 0x06) {
                                error = "the MKV file uses laced frames, which this copy can't convert";
                                return false;
                            }
                            if (cid == kSimpleBlock) key = (flags & 0x80) != 0;
                            if (num < 128 && trackIndex[(size_t)num] >= 0) {
                                Frame f;
                                f.track = trackIndex[(size_t)num];
                                f.offset = blockAt + (std::uint64_t)tl + 3;
                                f.size = (std::uint32_t)(blockLen - (std::uint64_t)tl - 3);
                                f.us = (std::int64_t)((double)(clusterTc + rel) * (double)scale / 1000.0);
                                f.key = key;
                                out.frames.push_back(f);
                            }
                        }
                    }
                }
                p = cdata + clen;
            }
            pos = p;
            continue;
        } else if (id == kCues) {
            sawCues = true;
        }
        if (unknown) break;   // an unknown-size element that isn't a cluster: nothing more to read
        pos = data + len;
        if (data + len > r.size) {
            out.cutShort = true;
            break;
        }
    }
    if (!sawCues) out.cutShort = true;
    if (out.tracks.empty()) {
        error = "the MKV file has no tracks";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- writing MP4

namespace {

void u8(Bytes& b, std::uint32_t v) { b.push_back((std::uint8_t)v); }
void u16(Bytes& b, std::uint32_t v) {
    b.push_back((std::uint8_t)(v >> 8));
    b.push_back((std::uint8_t)v);
}
void u32(Bytes& b, std::uint32_t v) {
    for (int i = 3; i >= 0; --i) b.push_back((std::uint8_t)(v >> (8 * i)));
}
void u64(Bytes& b, std::uint64_t v) {
    for (int i = 7; i >= 0; --i) b.push_back((std::uint8_t)(v >> (8 * i)));
}
void str4(Bytes& b, const char* s) { b.insert(b.end(), s, s + 4); }
void zeros(Bytes& b, std::size_t n) { b.insert(b.end(), n, 0); }
void add(Bytes& b, const Bytes& x) { b.insert(b.end(), x.begin(), x.end()); }

Bytes box(const char* type, const Bytes& payload) {
    Bytes b;
    u32(b, (std::uint32_t)(payload.size() + 8));
    str4(b, type);
    add(b, payload);
    return b;
}
Bytes full(const char* type, int version, std::uint32_t flags, const Bytes& payload) {
    Bytes p;
    u32(p, ((std::uint32_t)version << 24) | (flags & 0xFFFFFF));
    add(p, payload);
    return box(type, p);
}
void matrix(Bytes& b) {
    const std::uint32_t m[9] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};
    for (auto v : m) u32(b, v);
}
// An MPEG-4 descriptor with a one-byte length (ours are always short).
Bytes desc(int tag, const Bytes& body) {
    Bytes b;
    u8(b, (std::uint32_t)tag);
    u8(b, (std::uint32_t)body.size());
    add(b, body);
    return b;
}

struct Built {
    std::vector<std::size_t> frames;   // indices into Contents::frames
    std::uint32_t timescale = 1000;
    std::vector<std::uint32_t> durations;   // per sample, in the track's timescale
    std::uint64_t mediaDuration = 0;        // in the track's timescale
    std::int64_t startUs = 0;
};

}  // namespace

Result remux(Input& in, mkv::Sink& out, const std::vector<Chapter>& chapters, const std::function<bool(double)>& progress) {
    Result res;
    Contents c;
    if (!scan(in, c, res.error)) return res;
    // The tracks to copy: the first H.264 video, every AAC audio (in order).
    int videoT = -1;
    std::vector<int> audioT;
    for (std::size_t i = 0; i < c.tracks.size(); ++i) {
        const Track& t = c.tracks[i];
        if (t.video && videoT < 0 && t.codec == "V_MPEG4/ISO/AVC" && !t.priv.empty()) videoT = (int)i;
        if (!t.video && t.codec.rfind("A_AAC", 0) == 0 && t.priv.size() >= 2) audioT.push_back((int)i);
    }
    if (videoT < 0 && audioT.empty()) {
        res.error = "the MKV file has no H.264 video or AAC sound to copy";
        return res;
    }
    std::vector<int> order;
    if (videoT >= 0) order.push_back(videoT);
    for (int a : audioT) order.push_back(a);
    std::vector<int> slot(c.tracks.size(), -1);
    for (std::size_t i = 0; i < order.size(); ++i) slot[(size_t)order[i]] = (int)i;
    std::vector<Built> built(order.size());
    for (std::size_t i = 0; i < c.frames.size(); ++i)
        if (slot[(size_t)c.frames[i].track] >= 0) built[(size_t)slot[(size_t)c.frames[i].track]].frames.push_back(i);
    std::int64_t movieEndUs = 0;
    for (std::size_t k = 0; k < order.size(); ++k) {
        Built& b = built[k];
        const Track& t = c.tracks[(size_t)order[k]];
        if (b.frames.empty()) continue;
        b.timescale = t.video ? 90000u : (std::uint32_t)std::clamp(t.rate, 8000, 192000);
        b.startUs = c.frames[b.frames[0]].us;
        // Durations from the times between frames, rounded on the running total so nothing drifts.
        std::int64_t prevTick = 0;
        for (std::size_t i = 0; i < b.frames.size(); ++i) {
            const std::int64_t us = c.frames[b.frames[i]].us - b.startUs;
            if (us < 0 || (i > 0 && c.frames[b.frames[i]].us < c.frames[b.frames[i - 1]].us)) {
                res.error = "the frames in the MKV file are out of order";
                return res;
            }
            const std::int64_t tick = (std::int64_t)std::llround((double)us * b.timescale / 1e6);
            if (i > 0) b.durations.push_back((std::uint32_t)std::max<std::int64_t>(0, tick - prevTick));
            prevTick = tick;
        }
        // The last frame lasts as long as the one before it (or a frame's usual length).
        const std::uint32_t last = !b.durations.empty() ? b.durations.back() : t.video ? b.timescale / 60 : 1024;
        b.durations.push_back(last);
        b.mediaDuration = (std::uint64_t)prevTick + last;
        movieEndUs = std::max(movieEndUs, b.startUs + (std::int64_t)((double)b.mediaDuration * 1e6 / b.timescale));
        if (t.video) res.videoFrames = b.frames.size();
        else ++res.audioTracks;
    }
    res.durationUs = movieEndUs;
    std::uint64_t mdatBytes = 0;
    for (auto& f : c.frames)
        if (slot[(size_t)f.track] >= 0) mdatBytes += f.size;
    const bool large = mdatBytes + (64ull << 20) > 0xFFFFFFFFull;   // 64-bit offsets and mdat size

    // The movie box, for a given start of the frame data.
    auto buildMoov = [&](std::uint64_t dataStart) {
        // Each frame's offset in the MP4: in the MKV's order, packed.
        std::vector<std::uint64_t> at(c.frames.size(), 0);
        std::uint64_t o = dataStart;
        for (std::size_t i = 0; i < c.frames.size(); ++i)
            if (slot[(size_t)c.frames[i].track] >= 0) {
                at[i] = o;
                o += c.frames[i].size;
            }
        Bytes moov;
        {
            Bytes p;
            u32(p, 0), u32(p, 0), u32(p, 1000), u32(p, (std::uint32_t)std::llround(movieEndUs / 1000.0));
            u32(p, 0x00010000), u16(p, 0x0100), zeros(p, 10);
            matrix(p);
            zeros(p, 24);
            u32(p, (std::uint32_t)order.size() + 1);
            add(moov, full("mvhd", 0, 0, p));
        }
        bool firstAudio = true;
        for (std::size_t k = 0; k < order.size(); ++k) {
            const Built& b = built[k];
            if (b.frames.empty()) continue;
            const Track& t = c.tracks[(size_t)order[k]];
            const std::uint64_t mediaMs = (std::uint64_t)std::llround((double)b.mediaDuration * 1000.0 / b.timescale);
            const std::uint64_t startMs = (std::uint64_t)std::llround(b.startUs / 1000.0);
            Bytes trak;
            {   // tkhd: video and the first audio track enabled; other audio tracks are alternatives
                const bool enabled = t.video || firstAudio;
                Bytes p;
                u32(p, 0), u32(p, 0), u32(p, (std::uint32_t)k + 1), u32(p, 0), u32(p, (std::uint32_t)(startMs + mediaMs));
                zeros(p, 8);
                u16(p, 0), u16(p, t.video ? 0 : 1), u16(p, t.video ? 0 : 0x0100), u16(p, 0);
                matrix(p);
                u32(p, t.video ? (std::uint32_t)t.width << 16 : 0), u32(p, t.video ? (std::uint32_t)t.height << 16 : 0);
                add(trak, full("tkhd", 0, enabled ? 0x3 : 0x2, p));
            }
            if (startMs > 0) {   // starts later than the movie: an empty edit keeps it in step
                Bytes p;
                u32(p, 2);
                u32(p, (std::uint32_t)startMs), u32(p, 0xFFFFFFFFu), u32(p, 0x00010000);
                u32(p, (std::uint32_t)mediaMs), u32(p, 0), u32(p, 0x00010000);
                add(trak, box("edts", full("elst", 0, 0, p)));
            }
            Bytes mdia;
            {
                Bytes p;
                u32(p, 0), u32(p, 0), u32(p, b.timescale), u32(p, (std::uint32_t)std::min<std::uint64_t>(b.mediaDuration, 0xFFFFFFFFu));
                u16(p, 0x55C4), u16(p, 0);   // "und"
                add(mdia, full("mdhd", 0, 0, p));
            }
            {
                Bytes p;
                u32(p, 0);
                str4(p, t.video ? "vide" : "soun");
                zeros(p, 12);
                const std::string name = !t.name.empty() ? t.name : t.video ? "Video" : "Sound";
                p.insert(p.end(), name.begin(), name.end());
                p.push_back(0);
                add(mdia, full("hdlr", 0, 0, p));
            }
            Bytes minf;
            if (t.video) {
                Bytes p;
                zeros(p, 8);
                add(minf, full("vmhd", 0, 1, p));
            } else {
                Bytes p;
                zeros(p, 4);
                add(minf, full("smhd", 0, 0, p));
            }
            {
                Bytes url = full("url ", 0, 1, {});
                Bytes p;
                u32(p, 1);
                add(p, url);
                add(minf, box("dinf", full("dref", 0, 0, p)));
            }
            Bytes stbl;
            {   // stsd
                Bytes entry;
                zeros(entry, 6);
                u16(entry, 1);
                if (t.video) {
                    zeros(entry, 16);
                    u16(entry, (std::uint32_t)t.width), u16(entry, (std::uint32_t)t.height);
                    u32(entry, 0x00480000), u32(entry, 0x00480000), u32(entry, 0), u16(entry, 1);
                    zeros(entry, 32);
                    u16(entry, 0x0018), u16(entry, 0xFFFF);
                    add(entry, box("avcC", t.priv));
                    Bytes p;
                    u32(p, 1);
                    add(p, box("avc1", entry));
                    add(stbl, full("stsd", 0, 0, p));
                } else {
                    zeros(entry, 8);
                    u16(entry, (std::uint32_t)t.channels), u16(entry, 16), u16(entry, 0), u16(entry, 0);
                    u32(entry, (std::uint32_t)std::min(t.rate, 65535) << 16);
                    Bytes dec;
                    u8(dec, 0x40), u8(dec, 0x15), zeros(dec, 3), u32(dec, 0), u32(dec, 0);
                    add(dec, desc(5, t.priv));
                    Bytes es;
                    u16(es, 0), u8(es, 0);
                    add(es, desc(4, dec));
                    add(es, desc(6, Bytes{0x02}));
                    add(entry, full("esds", 0, 0, desc(3, es)));
                    Bytes p;
                    u32(p, 1);
                    add(p, box("mp4a", entry));
                    add(stbl, full("stsd", 0, 0, p));
                }
            }
            {   // stts: runs of equal durations
                Bytes runs;
                std::uint32_t n = 0;
                for (std::size_t i = 0; i < b.durations.size();) {
                    std::size_t j = i;
                    while (j < b.durations.size() && b.durations[j] == b.durations[i]) ++j;
                    u32(runs, (std::uint32_t)(j - i)), u32(runs, b.durations[i]);
                    ++n;
                    i = j;
                }
                Bytes p;
                u32(p, n);
                add(p, runs);
                add(stbl, full("stts", 0, 0, p));
            }
            if (t.video) {   // the keyframes
                Bytes keys;
                std::uint32_t n = 0;
                for (std::size_t i = 0; i < b.frames.size(); ++i)
                    if (c.frames[b.frames[i]].key) {
                        u32(keys, (std::uint32_t)i + 1);
                        ++n;
                    }
                if (n != b.frames.size()) {
                    Bytes p;
                    u32(p, n);
                    add(p, keys);
                    add(stbl, full("stss", 0, 0, p));
                }
            }
            {   // one frame per chunk
                Bytes p;
                u32(p, 1), u32(p, 1), u32(p, 1), u32(p, 1);
                add(stbl, full("stsc", 0, 0, p));
            }
            {
                Bytes p;
                u32(p, 0), u32(p, (std::uint32_t)b.frames.size());
                for (auto i : b.frames) u32(p, c.frames[i].size);
                add(stbl, full("stsz", 0, 0, p));
            }
            {
                Bytes p;
                u32(p, (std::uint32_t)b.frames.size());
                for (auto i : b.frames) {
                    if (large) u64(p, at[i]);
                    else u32(p, (std::uint32_t)at[i]);
                }
                add(stbl, full(large ? "co64" : "stco", 0, 0, p));
            }
            add(minf, box("stbl", stbl));
            add(mdia, box("minf", minf));
            add(trak, box("mdia", mdia));
            add(moov, box("trak", trak));
            if (!t.video) firstAudio = false;
        }
        if (!chapters.empty()) {   // Nero chapters (chpl), read by most players and editors
            Bytes p;
            zeros(p, 4);
            u8(p, (std::uint32_t)std::min<std::size_t>(chapters.size(), 255));
            for (std::size_t i = 0; i < chapters.size() && i < 255; ++i) {
                u64(p, (std::uint64_t)std::max<std::int64_t>(0, chapters[i].us) * 10);   // 100 ns units
                const std::string title = chapters[i].title.substr(0, 255);
                u8(p, (std::uint32_t)title.size());
                p.insert(p.end(), title.begin(), title.end());
            }
            add(moov, box("udta", full("chpl", 1, 0, p)));
        }
        return box("moov", moov);
    };

    Bytes ftyp;
    str4(ftyp, "isom"), u32(ftyp, 512);
    str4(ftyp, "isom"), str4(ftyp, "iso2"), str4(ftyp, "avc1"), str4(ftyp, "mp41");
    ftyp = box("ftyp", ftyp);
    // The movie box's size doesn't depend on the offsets (they're fixed width): build once to measure.
    const std::size_t moovSize = buildMoov(0).size();
    const std::uint64_t mdatHeader = large ? 16 : 8;
    const std::uint64_t dataStart = ftyp.size() + moovSize + mdatHeader;
    const Bytes moov = buildMoov(dataStart);
    if (!out.write(ftyp.data(), ftyp.size()) || !out.write(moov.data(), moov.size())) {
        res.error = "the MP4 file couldn't be written (is the disk full?)";
        return res;
    }
    Bytes mh;
    if (large) {
        u32(mh, 1);
        str4(mh, "mdat");
        u64(mh, mdatBytes + 16);
    } else {
        u32(mh, (std::uint32_t)(mdatBytes + 8));
        str4(mh, "mdat");
    }
    if (!out.write(mh.data(), mh.size())) {
        res.error = "the MP4 file couldn't be written (is the disk full?)";
        return res;
    }
    // The frames, copied in the MKV's order.
    Bytes buf;
    std::uint64_t copied = 0;
    for (std::size_t i = 0; i < c.frames.size(); ++i) {
        const Frame& f = c.frames[i];
        if (slot[(size_t)f.track] < 0) continue;
        buf.resize(f.size);
        if (f.size && !in.read(f.offset, buf.data(), f.size)) {
            res.error = "the MKV file couldn't be read";
            return res;
        }
        if (f.size && !out.write(buf.data(), f.size)) {
            res.error = "the MP4 file couldn't be written (is the disk full?)";
            return res;
        }
        copied += f.size;
        if (progress && (i % 512) == 0 && !progress(mdatBytes ? (double)copied / (double)mdatBytes : 1.0)) {
            res.error = "stopped";
            return res;
        }
    }
    if (progress) progress(1.0);
    res.ok = true;
    return res;
}

}  // namespace ncap::mp4
