#include "ncap/media.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>

namespace ncap::media {

std::vector<std::pair<const std::uint8_t*, std::size_t>> nalUnits(const std::uint8_t* d, std::size_t n) {
    std::vector<std::pair<const std::uint8_t*, std::size_t>> out;
    // Start codes are 00 00 01 or 00 00 00 01.
    auto startAt = [&](std::size_t i, std::size_t& len) {
        if (i + 3 <= n && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            len = 3;
            return true;
        }
        if (i + 4 <= n && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 && d[i + 3] == 1) {
            len = 4;
            return true;
        }
        return false;
    };
    std::size_t i = 0, len = 0;
    while (i < n && !startAt(i, len)) ++i;
    while (i < n) {
        const std::size_t begin = i + len;
        std::size_t j = begin, next = 0;
        while (j < n && !startAt(j, next)) ++j;
        std::size_t end = j;
        // Trailing zero bytes before the next start code belong to neither NAL unit.
        while (end > begin && d[end - 1] == 0 && j < n) --end;
        if (end > begin) out.push_back({d + begin, end - begin});
        i = j;
        len = next;
    }
    return out;
}

Bytes annexBToLengthPrefixed(const std::uint8_t* d, std::size_t n, bool dropParameterSets) {
    Bytes out;
    out.reserve(n + 16);
    for (auto& [p, len] : nalUnits(d, n)) {
        const int t = nalType(p);
        if (dropParameterSets && (t == 7 || t == 8 || t == 9)) continue;
        out.push_back((std::uint8_t)(len >> 24));
        out.push_back((std::uint8_t)(len >> 16));
        out.push_back((std::uint8_t)(len >> 8));
        out.push_back((std::uint8_t)len);
        out.insert(out.end(), p, p + len);
    }
    return out;
}

bool containsKeyframe(const std::uint8_t* d, std::size_t n) {
    for (auto& [p, len] : nalUnits(d, n))
        if (len && nalType(p) == 5) return true;
    return false;
}

bool findParameterSets(const std::uint8_t* d, std::size_t n, Bytes& sps, Bytes& pps) {
    for (auto& [p, len] : nalUnits(d, n)) {
        if (!len) continue;
        if (nalType(p) == 7 && sps.empty()) sps.assign(p, p + len);
        if (nalType(p) == 8 && pps.empty()) pps.assign(p, p + len);
    }
    return !sps.empty() && !pps.empty();
}

Bytes avcC(const Bytes& sps, const Bytes& pps) {
    if (sps.size() < 4 || pps.empty()) return {};
    Bytes b = {1, sps[1], sps[2], sps[3], 0xFF /* 4-byte lengths */, 0xE1 /* one SPS */};
    b.push_back((std::uint8_t)(sps.size() >> 8));
    b.push_back((std::uint8_t)sps.size());
    b.insert(b.end(), sps.begin(), sps.end());
    b.push_back(1);   // one PPS
    b.push_back((std::uint8_t)(pps.size() >> 8));
    b.push_back((std::uint8_t)pps.size());
    b.insert(b.end(), pps.begin(), pps.end());
    return b;
}

Bytes aacConfig(int rate, int channels) {
    static const int rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
    int index = -1;
    for (int i = 0; i < 13; ++i)
        if (rates[i] == rate) index = i;
    if (index < 0 || channels < 1 || channels > 7) return {};
    const unsigned v = (2u << 11) | ((unsigned)index << 7) | ((unsigned)channels << 3);   // AAC-LC, rate, channels, 3 zero bits
    return {(std::uint8_t)(v >> 8), (std::uint8_t)v};
}

// ---------------------------------------------------------------- interleaving

Interleaver::Interleaver(int streams, std::int64_t maxWaitUs)
    : q_((std::size_t)streams), last_((std::size_t)streams, std::numeric_limits<std::int64_t>::min()), maxWait_(maxWaitUs) {}

void Interleaver::push(Packet p) {
    if (p.stream < 0 || (std::size_t)p.stream >= q_.size()) return;
    last_[(std::size_t)p.stream] = std::max(last_[(std::size_t)p.stream], p.dts);
    q_[(std::size_t)p.stream].push_back(std::move(p));
}

bool Interleaver::pop(Packet& out) {
    // The earliest queued packet...
    int best = -1;
    for (std::size_t s = 0; s < q_.size(); ++s)
        if (!q_[s].empty() && (best < 0 || q_[s].front().dts < q_[(std::size_t)best].front().dts)) best = (int)s;
    if (best < 0) return false;
    const std::int64_t t = q_[(std::size_t)best].front().dts;
    // ...is safe once every other stream has reached it (nothing earlier can still come), or has
    // gone quiet for longer than we wait.
    const std::int64_t unseen = std::numeric_limits<std::int64_t>::min();
    std::int64_t newest = unseen, first = std::numeric_limits<std::int64_t>::max();
    for (auto v : last_)
        if (v != unseen) newest = std::max(newest, v);
    for (std::size_t s = 0; s < q_.size(); ++s)
        if (!q_[s].empty()) first = std::min(first, q_[s].front().dts);
    for (std::size_t s = 0; s < q_.size(); ++s) {
        if ((int)s == best) continue;
        // A stream that hasn't sent anything yet counts as being at the earliest time queued.
        const std::int64_t at = last_[s] == unseen ? first : last_[s];
        if (last_[s] != unseen && at >= t) continue;
        if (newest - at > maxWait_) continue;
        return false;
    }
    out = std::move(q_[(std::size_t)best].front());
    q_[(std::size_t)best].pop_front();
    return true;
}

bool Interleaver::drain(Packet& out) {
    int best = -1;
    for (std::size_t s = 0; s < q_.size(); ++s)
        if (!q_[s].empty() && (best < 0 || q_[s].front().dts < q_[(std::size_t)best].front().dts)) best = (int)s;
    if (best < 0) return false;
    out = std::move(q_[(std::size_t)best].front());
    q_[(std::size_t)best].pop_front();
    return true;
}

std::size_t Interleaver::queued() const {
    std::size_t n = 0;
    for (auto& q : q_) n += q.size();
    return n;
}

std::string recordingName(int y, int mo, int d, int h, int mi, int s) {
    char b[48];
    std::snprintf(b, sizeof b, "%04d-%02d-%02d %02d-%02d-%02d", y, mo, d, h, mi, s);
    return b;
}

}  // namespace ncap::media
