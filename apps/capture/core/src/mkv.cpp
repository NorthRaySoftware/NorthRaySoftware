#include "ncap/mkv.hpp"

#include <cstring>

namespace ncap::mkv {

namespace {

// Element IDs.
constexpr std::uint32_t kEbml = 0x1A45DFA3, kEbmlVersion = 0x4286, kEbmlReadVersion = 0x42F7, kMaxIdLength = 0x42F2, kMaxSizeLength = 0x42F3,
                        kDocType = 0x4282, kDocTypeVersion = 0x4287, kDocTypeReadVersion = 0x4285;
constexpr std::uint32_t kSegment = 0x18538067, kSeekHead = 0x114D9B74, kSeek = 0x4DBB, kSeekId = 0x53AB, kSeekPosition = 0x53AC, kVoid = 0xEC;
constexpr std::uint32_t kInfo = 0x1549A966, kTimecodeScale = 0x2AD7B1, kDuration = 0x4489, kMuxingApp = 0x4D80, kWritingApp = 0x5741;
constexpr std::uint32_t kTracks = 0x1654AE6B, kTrackEntry = 0xAE, kTrackNumber = 0xD7, kTrackUid = 0x73C5, kTrackType = 0x83, kFlagLacing = 0x9C,
                        kCodecId = 0x86, kCodecPrivate = 0x63A2, kName = 0x536E, kDefaultDuration = 0x23E383, kFlagDefault = 0x88, kLanguage = 0x22B59C,
                        kVideo = 0xE0, kPixelWidth = 0xB0, kPixelHeight = 0xBA, kAudio = 0xE1, kSamplingFrequency = 0xB5, kChannels = 0x9F;
constexpr std::uint32_t kCluster = 0x1F43B675, kTimecode = 0xE7, kSimpleBlock = 0xA3;
constexpr std::uint32_t kCues = 0x1C53BB6B, kCuePoint = 0xBB, kCueTime = 0xB3, kCueTrackPositions = 0xB7, kCueTrack = 0xF7, kCueClusterPosition = 0xF1;

void putId(Bytes& b, std::uint32_t id) {
    int n = id > 0xFFFFFF ? 4 : id > 0xFFFF ? 3 : id > 0xFF ? 2 : 1;
    for (int i = n - 1; i >= 0; --i) b.push_back((std::uint8_t)(id >> (8 * i)));
}

// A size as a variable-length integer, as short as it can be.
void putSize(Bytes& b, std::uint64_t v) {
    int n = 1;
    while (n < 8 && v >= (1ull << (7 * n)) - 1) ++n;
    const std::uint64_t marked = v | (1ull << (7 * n));
    for (int i = n - 1; i >= 0; --i) b.push_back((std::uint8_t)(marked >> (8 * i)));
}

// A size in exactly 8 bytes (so it can be patched later); all ones = "unknown".
void putSize8(Bytes& b, std::uint64_t v) {
    b.push_back(0x01);
    for (int i = 6; i >= 0; --i) b.push_back((std::uint8_t)(v >> (8 * i)));
}
const std::uint64_t kUnknown = 0x00FFFFFFFFFFFFFFull;

void element(Bytes& b, std::uint32_t id, const Bytes& payload) {
    putId(b, id);
    putSize(b, payload.size());
    b.insert(b.end(), payload.begin(), payload.end());
}

void uintElement(Bytes& b, std::uint32_t id, std::uint64_t v) {
    Bytes p;
    int n = 1;
    while (n < 8 && (v >> (8 * n)) != 0) ++n;
    for (int i = n - 1; i >= 0; --i) p.push_back((std::uint8_t)(v >> (8 * i)));
    element(b, id, p);
}

void uint8Element(Bytes& b, std::uint32_t id, std::uint64_t v) {   // fixed 8 bytes (patched later)
    Bytes p;
    for (int i = 7; i >= 0; --i) p.push_back((std::uint8_t)(v >> (8 * i)));
    element(b, id, p);
}

Bytes floatBytes(double v) {
    std::uint64_t u;
    std::memcpy(&u, &v, 8);
    Bytes p;
    for (int i = 7; i >= 0; --i) p.push_back((std::uint8_t)(u >> (8 * i)));
    return p;
}

void floatElement(Bytes& b, std::uint32_t id, double v) { element(b, id, floatBytes(v)); }

void stringElement(Bytes& b, std::uint32_t id, const std::string& s) { element(b, id, Bytes(s.begin(), s.end())); }

Bytes seekEntry(std::uint32_t id, std::uint64_t pos) {
    Bytes s, idBytes;
    putId(idBytes, id);
    element(s, kSeekId, idBytes);
    uint8Element(s, kSeekPosition, pos);
    Bytes out;
    element(out, kSeek, s);
    return out;
}

}  // namespace

bool MemorySink::write(const std::uint8_t* d, std::size_t n) {
    bytes.insert(bytes.end(), d, d + n);
    return true;
}

bool MemorySink::writeAt(std::uint64_t at, const std::uint8_t* d, std::size_t n) {
    if (at + n > bytes.size()) return false;
    std::memcpy(bytes.data() + at, d, n);
    return true;
}

bool Writer::begin(Sink* sink, const VideoTrack& video, const std::vector<AudioTrack>& audio, const std::string& app) {
    sink_ = sink;
    ok_ = true;
    cues_.clear();
    inCluster_ = false;
    lastUs_ = 0;
    Bytes h, p;
    uintElement(p, kEbmlVersion, 1);
    uintElement(p, kEbmlReadVersion, 1);
    uintElement(p, kMaxIdLength, 4);
    uintElement(p, kMaxSizeLength, 8);
    stringElement(p, kDocType, "matroska");
    uintElement(p, kDocTypeVersion, 4);
    uintElement(p, kDocTypeReadVersion, 2);
    element(h, kEbml, p);
    putId(h, kSegment);
    segmentSize_ = sink_->position() + h.size();
    putSize8(h, kUnknown);
    segmentData_ = sink_->position() + h.size();

    // The info and tracks, built first so the seek head can point at them.
    Bytes info, ip;
    uintElement(ip, kTimecodeScale, 1000000);   // 1 ms
    const std::size_t durationOffsetInIp = ip.size() + 3;   // after the 2-byte ID and 1-byte size
    floatElement(ip, kDuration, 0);
    stringElement(ip, kMuxingApp, app);
    stringElement(ip, kWritingApp, app);
    element(info, kInfo, ip);
    const std::size_t infoHeader = info.size() - ip.size();

    Bytes tracks, tp;
    {
        Bytes e, v;
        uintElement(e, kTrackNumber, 1);
        uintElement(e, kTrackUid, 1);
        uintElement(e, kTrackType, 1);
        uintElement(e, kFlagLacing, 0);
        stringElement(e, kCodecId, "V_MPEG4/ISO/AVC");
        element(e, kCodecPrivate, video.avcC);
        if (video.fps > 0) uintElement(e, kDefaultDuration, (std::uint64_t)(1e9 / video.fps + 0.5));
        uintElement(v, kPixelWidth, (std::uint64_t)video.width);
        uintElement(v, kPixelHeight, (std::uint64_t)video.height);
        element(e, kVideo, v);
        element(tp, kTrackEntry, e);
    }
    for (std::size_t i = 0; i < audio.size(); ++i) {
        Bytes e, a;
        uintElement(e, kTrackNumber, i + 2);
        uintElement(e, kTrackUid, i + 2);
        uintElement(e, kTrackType, 2);
        uintElement(e, kFlagLacing, 0);
        uintElement(e, kFlagDefault, i == 0 ? 1 : 0);
        stringElement(e, kLanguage, "und");
        stringElement(e, kName, audio[i].name);
        stringElement(e, kCodecId, "A_AAC");
        element(e, kCodecPrivate, audio[i].config);
        floatElement(a, kSamplingFrequency, audio[i].rate);
        uintElement(a, kChannels, (std::uint64_t)audio[i].channels);
        element(e, kAudio, a);
        element(tp, kTrackEntry, e);
    }
    element(tracks, kTracks, tp);

    // The seek head: info, tracks, and room for the cues (a void element, replaced at the end, so
    // a file that never got there doesn't point at nothing).
    Bytes sp;
    const Bytes probe = seekEntry(kInfo, 0);
    Bytes seekHead;
    {
        // Its size doesn't depend on the positions (8-byte position fields), so measure once.
        const std::size_t entries = probe.size() * 2 + 21;
        Bytes tmp;
        putId(tmp, kSeekHead);
        putSize(tmp, entries);
        const std::uint64_t infoPos = tmp.size() + entries;
        const std::uint64_t tracksPos = infoPos + info.size();
        Bytes a = seekEntry(kInfo, infoPos), b = seekEntry(kTracks, tracksPos);
        sp.insert(sp.end(), a.begin(), a.end());
        sp.insert(sp.end(), b.begin(), b.end());
        cuesSeekAt_ = segmentData_ + tmp.size() + sp.size();
        sp.push_back((std::uint8_t)kVoid);
        sp.push_back(0x80 | 19);
        sp.insert(sp.end(), 19, 0);
        element(seekHead, kSeekHead, sp);
    }
    durationAt_ = segmentData_ + seekHead.size() + infoHeader + durationOffsetInIp;
    h.insert(h.end(), seekHead.begin(), seekHead.end());
    h.insert(h.end(), info.begin(), info.end());
    h.insert(h.end(), tracks.begin(), tracks.end());
    ok_ = sink_->write(h.data(), h.size());
    return ok_;
}

bool Writer::openCluster(std::int64_t ms) {
    if (inCluster_ && !closeCluster()) return false;
    Bytes c;
    putId(c, kCluster);
    clusterStart_ = sink_->position();
    clusterSize_ = clusterStart_ + 4;
    putSize8(c, kUnknown);   // filled in when it closes; "unknown" keeps a crashed file readable
    uintElement(c, kTimecode, (std::uint64_t)ms);
    clusterMs_ = ms;
    inCluster_ = true;
    return ok_ = ok_ && sink_->write(c.data(), c.size());
}

bool Writer::closeCluster() {
    if (!inCluster_) return true;
    const std::uint64_t size = sink_->position() - (clusterSize_ + 8);
    Bytes s;
    putSize8(s, size);
    inCluster_ = false;
    return ok_ = ok_ && sink_->writeAt(clusterSize_, s.data(), s.size());
}

bool Writer::block(int track, std::int64_t us, bool keyframe, const Bytes& data) {
    if (!ok_ || !sink_) return false;
    if (us < 0) us = 0;
    const std::int64_t ms = us / 1000;
    // A new cluster at each video keyframe (so seeking lands on one), and before the 16-bit
    // relative time would run out.
    if (!inCluster_ || (track == 1 && keyframe) || ms - clusterMs_ > 30000) {
        if (!openCluster(ms)) return false;
        if (track == 1 && keyframe) cues_.push_back({ms, clusterStart_ - segmentData_});
    }
    Bytes b;
    putId(b, kSimpleBlock);
    putSize(b, 4 + data.size());
    b.push_back((std::uint8_t)(0x80 | track));   // track number as a 1-byte vint
    const std::int64_t rel = ms - clusterMs_;
    b.push_back((std::uint8_t)((rel >> 8) & 0xFF));
    b.push_back((std::uint8_t)(rel & 0xFF));
    b.push_back(keyframe ? 0x80 : 0x00);
    b.insert(b.end(), data.begin(), data.end());
    if (us > lastUs_) lastUs_ = us;
    return ok_ = ok_ && sink_->write(b.data(), b.size());
}

bool Writer::video(std::int64_t us, bool keyframe, const Bytes& data) { return block(1, us, keyframe, data); }

bool Writer::audio(int track, std::int64_t us, const Bytes& data) { return block(track + 2, us, true, data); }

bool Writer::finish() {
    if (!sink_) return false;
    closeCluster();
    // The cues.
    const std::uint64_t cuesPos = sink_->position() - segmentData_;
    Bytes cp;
    for (auto& c : cues_) {
        Bytes pt, tp;
        uintElement(pt, kCueTime, (std::uint64_t)c.ms);
        uintElement(tp, kCueTrack, 1);
        uintElement(tp, kCueClusterPosition, c.cluster);
        element(pt, kCueTrackPositions, tp);
        element(cp, kCuePoint, pt);
    }
    if (!cues_.empty()) {
        Bytes cues;
        element(cues, kCues, cp);
        ok_ = ok_ && sink_->write(cues.data(), cues.size());
        const Bytes seek = seekEntry(kCues, cuesPos);   // exactly the void's 21 bytes
        if (seek.size() == 21) ok_ = ok_ && sink_->writeAt(cuesSeekAt_, seek.data(), seek.size());
    }
    // The duration (in timecode units: ms) and the segment's size.
    const Bytes d = floatBytes((double)lastUs_ / 1000.0);
    ok_ = ok_ && sink_->writeAt(durationAt_, d.data(), d.size());
    Bytes s;
    putSize8(s, sink_->position() - segmentData_);
    ok_ = ok_ && sink_->writeAt(segmentSize_, s.data(), s.size());
    return ok_;
}

}  // namespace ncap::mkv
