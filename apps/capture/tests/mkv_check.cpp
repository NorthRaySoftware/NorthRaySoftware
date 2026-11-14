// Builds an MKV from an H.264 Annex B stream (with access unit delimiters) and AAC ADTS streams,
// through NorthRay Capture's own writer, so ffprobe and ffmpeg can check the result.
//   capture_mkv_check out.mkv video.h264 fps audio1.aac [audio2.aac ...]
#include "ncap/media.hpp"
#include "ncap/mkv.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>

using namespace ncap;

class FileSink : public mkv::Sink {
public:
    explicit FileSink(const char* path) : f(std::fopen(path, "wb+")) {}
    ~FileSink() override { if (f) std::fclose(f); }
    bool write(const std::uint8_t* d, std::size_t n) override { pos_ += n; return std::fwrite(d, 1, n, f) == n; }
    bool writeAt(std::uint64_t at, const std::uint8_t* d, std::size_t n) override {
        std::fseek(f, (long)at, SEEK_SET);
        const bool ok = std::fwrite(d, 1, n, f) == n;
        std::fseek(f, 0, SEEK_END);
        return ok;
    }
    std::uint64_t position() const override { return pos_; }
    std::FILE* f;
    std::uint64_t pos_ = 0;
};

static media::Bytes slurp(const char* p) {
    std::ifstream in(p, std::ios::binary);
    return media::Bytes(std::istreambuf_iterator<char>(in), {});
}

int main(int argc, char** argv) {
    if (argc < 5) return 2;
    const media::Bytes h264 = slurp(argv[2]);
    const double fps = std::atof(argv[3]);
    // Access units: split at each AUD (NAL type 9).
    std::vector<media::Bytes> aus;
    const auto nals = media::nalUnits(h264.data(), h264.size());
    for (auto& [p, n] : nals) {
        if (media::nalType(p) == 9 || aus.empty()) aus.emplace_back();
        const std::uint8_t sc[4] = {0, 0, 0, 1};
        aus.back().insert(aus.back().end(), sc, sc + 4);
        aus.back().insert(aus.back().end(), p, p + n);
    }
    media::Bytes sps, pps;
    if (!media::findParameterSets(h264.data(), h264.size(), sps, pps)) return 3;
    mkv::VideoTrack v;
    v.width = 1280;
    v.height = 720;
    v.fps = fps;
    v.avcC = media::avcC(sps, pps);
    // AAC: strip the ADTS headers.
    std::vector<std::vector<media::Bytes>> tracks;
    std::vector<mkv::AudioTrack> at;
    for (int a = 4; a < argc; ++a) {
        const media::Bytes adts = slurp(argv[a]);
        std::vector<media::Bytes> frames;
        for (std::size_t i = 0; i + 7 <= adts.size();) {
            const std::size_t len = ((adts[i + 3] & 3) << 11) | (adts[i + 4] << 3) | (adts[i + 5] >> 5);
            const std::size_t hdr = (adts[i + 1] & 1) ? 7 : 9;
            if (len < hdr || i + len > adts.size()) break;
            frames.emplace_back(adts.begin() + (long)(i + hdr), adts.begin() + (long)(i + len));
            i += len;
        }
        tracks.push_back(frames);
        at.push_back({"Track " + std::to_string(a - 3), 48000, 2, media::aacConfig(48000, 2)});
    }
    FileSink sink(argv[1]);
    mkv::Writer w;
    if (!w.begin(&sink, v, at, "NorthRay Capture (check)")) return 4;
    media::Interleaver il(1 + (int)tracks.size());
    for (std::size_t i = 0; i < aus.size(); ++i) {
        media::Packet p;
        p.stream = 0;
        p.pts = p.dts = (std::int64_t)(i * 1e6 / fps);
        p.keyframe = media::containsKeyframe(aus[i].data(), aus[i].size());
        p.data = media::annexBToLengthPrefixed(aus[i].data(), aus[i].size(), true);
        il.push(std::move(p));
    }
    for (std::size_t t = 0; t < tracks.size(); ++t)
        for (std::size_t i = 0; i < tracks[t].size(); ++i) {
            media::Packet p;
            p.stream = 1 + (int)t;
            p.pts = p.dts = (std::int64_t)(i * 1024 * 1e6 / 48000);
            p.data = tracks[t][i];
            il.push(std::move(p));
        }
    media::Packet p;
    int written = 0;
    while (il.drain(p)) {
        const bool ok = p.stream == 0 ? w.video(p.pts, p.keyframe, p.data) : w.audio(p.stream - 1, p.pts, p.data);
        if (!ok) return 5;
        ++written;
    }
    if (!w.finish()) return 6;
    std::printf("%d packets, %zu access units\n", written, aus.size());
    return 0;
}
