// NorthRay Capture: streaming to several destinations at once (system 8):
// Capture → Composite → Encode → Distribute → Multiple Destinations.
//
// Destinations that ask for the same video bitrate share one H.264 encode; a destination at another
// bitrate gets its own. Within an encode, each audio mix a destination asks for (the Stream bus, or a
// track: a game-only mix for one platform, say) has its own AAC encoder, shared by the destinations
// that want it, and its own interleaving with the video. Each destination sends on its own thread
// with its own buffer and reconnects by itself (output/destination.hpp), so one failing never ends the
// others. Destinations start and stop independently; an encode with no destinations left is stopped.
#pragma once

#include "audio/mixer.hpp"
#include "encode/audio_encoder.hpp"
#include "encode/video_encoder.hpp"
#include "output/destination.hpp"
#include "video/engine.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ncap::output {

struct StreamCommon {
    int width = 1920, height = 1080, fps = 60;
    std::wstring encoder;            // as for recording: "" = automatic
    int audioKbps = 160;
    int longestWaitSeconds = 60;     // between reconnection attempts
};

struct DestinationStart {
    std::string id;                  // the settings' destination id
    std::string name;                // "Twitch"
    std::string url, key;            // the key is never logged
    int videoKbps = 6000;
    int bus = 0;                     // the mixer bus it carries: 0 Stream, 1-6 Track 1-6
};

struct EncodeStatus {
    int videoKbps = 0;
    std::wstring encoder;
    bool hardware = false;
    std::uint64_t encoderDropped = 0;
    int destinations = 0;
    std::vector<int> buses;          // the audio mixes it carries
};

class Streamer {
public:
    ~Streamer() { stop(); }
    // False with a reason in plain words (no encoder, bad address...). Others already live carry on.
    bool start(video::Engine& engine, audio::Mixer& mixer, const StreamCommon& common, const DestinationStart& d, std::wstring& error);
    void stop(const std::string& id);
    void stop();                                   // everything
    bool active() const;                           // any destination running
    bool active(const std::string& id) const;
    bool known(const std::string& id) const;       // running, or failed and not yet cleared
    void clear(const std::string& id);             // forget a failed destination
    DestinationStatus status(const std::string& id) const;
    std::vector<std::string> ids() const;
    std::vector<EncodeStatus> encodes() const;
    // Stops encodes nobody is using any more (their destinations failed). Call now and then.
    void tick();

private:
    // One audio mix within an encode: a bus's tap, its AAC encoder, and the destinations that carry it.
    struct Mix {
        int bus = 0;
        std::unique_ptr<encode::AudioEncoder> audio;
        std::shared_ptr<audio::Tap> tap;
        std::unique_ptr<media::Interleaver> il;
        bool anchored = false;
        std::vector<std::shared_ptr<Destination>> dests;
    };
    struct Path {
        int kbps = 0;
        std::string sink;
        std::unique_ptr<encode::VideoEncoder> video;
        std::thread thread;
        std::atomic<bool> stopping{false};
        LONGLONG t0 = 0, freq = 1;
        std::int64_t lastFrame = -1;
        std::mutex frameM;                         // the engine's frame callback vs. stopping
        std::mutex m;                              // mixes, their destinations, configuration
        std::map<int, std::shared_ptr<Mix>> mixes; // by bus
        media::Bytes avcC;
        int fps = 60;
        void frame(ID3D11Texture2D* canvas, LONGLONG qpc);
        void run();
    };
    struct Entry {
        int kbps = 0, bus = 0;
        std::shared_ptr<Destination> dest;
    };
    void stopPath(int kbps);
    void dropMixIfUnused(int kbps, int bus);   // m_ held

    video::Engine* engine_ = nullptr;
    audio::Mixer* mixer_ = nullptr;
    std::map<int, std::unique_ptr<Path>> paths_;                    // by video bitrate
    std::map<std::string, Entry> dests_;                            // by destination id
    mutable std::mutex m_;
};

}  // namespace ncap::output
