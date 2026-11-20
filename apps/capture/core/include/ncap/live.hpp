// NorthRay Capture core: the stream's plan and its report (0.11).
//
// The plan: when you mean to go live ("2026-11-22T19:00", local time), how long for, and when the next stream is,
// for the Starting soon countdown, "time left" and end-screen timers, and for going live by itself at that time.
//
// The report: what happened in a stream, collected while it runs and kept afterwards: how long it was, the upload
// rate every few seconds (for a graph), frames dropped, each destination's own figures, and how long each scene
// was live. Saved as JSON in the reports folder; nothing in it is secret (no stream keys, no addresses).
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ncap::live {

// A local date and time to the minute.
struct Stamp {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0;
    bool operator==(const Stamp&) const = default;
};
bool parseStamp(const std::string& s, Stamp& out);   // "2026-11-22T19:00" (also with a space); false if not a real time
std::string stampText(const Stamp& s);               // "2026-11-22T19:00"
std::string stampWords(const Stamp& s);              // "Sun 22 Nov, 19:00"
// Milliseconds from 1970-01-01 00:00 as if it were UTC: only for differences between local times.
std::int64_t civilMs(int year, int month, int day, int hour, int minute, int second = 0, int ms = 0);
inline std::int64_t stampMs(const Stamp& s) { return civilMs(s.year, s.month, s.day, s.hour, s.minute); }
int weekday(int year, int month, int day);   // 0 = Sunday
Stamp addMinutes(const Stamp& s, int minutes);

struct Report {
    std::string started;                      // stampText of when it began
    double seconds = 0;                       // how long it was live (the longest destination)
    int sampleSeconds = 5;                    // the spacing of `kbps`
    std::vector<float> kbps;                  // the upload rate, all destinations together
    std::uint64_t dropped = 0;                // frames dropped: by the network and by the encoder
    struct Destination {
        std::string name;
        double seconds = 0, averageKbps = 0;
        std::uint64_t dropped = 0;
        int reconnects = 0;
    };
    std::vector<Destination> destinations;
    std::vector<std::pair<std::string, double>> scenes;   // each scene's live time, in the order first shown
    std::vector<std::pair<double, std::string>> changes;  // when each scene went live (seconds in)
    bool recorded = false;                    // a recording ran alongside
    // Writing in NorthRay Writing Studio during the stream (0.13): words written, the chapters worked on (in the order
    // first written in), and sprints finished with their words.
    struct Writing {
        bool any = false;
        std::int64_t words = 0;
        std::vector<std::string> chapters;
        int sprints = 0;
        std::int64_t sprintWords = 0;
        bool operator==(const Writing&) const = default;
    } writing;
};

// Collects a report while the stream runs. Times are seconds since the stream started.
class Collector {
public:
    static constexpr std::size_t kMaxSamples = 8640;   // 12 hours at 5 s
    static constexpr std::size_t kMaxChanges = 2000;
    void begin(const Stamp& started, const std::string& scene);
    bool running() const { return running_; }
    // Called about once a second with the total upload rate now and the frames dropped so far.
    void sample(double t, double kbps, std::uint64_t droppedSoFar);
    void scene(double t, const std::string& name);   // the live scene changed
    void destination(const Report::Destination& d);  // a destination's final figures (replaces one of the same name)
    void setRecorded() { r_.recorded = true; }
    // Writing Studio (0.13): today's words now (the first call is the starting point; a smaller number, a new day, starts
    // counting again from it) and the chapter open; a sprint that ended with its words.
    void writing(std::int64_t wordsToday, const std::string& chapter);
    void sprint(std::int64_t words);
    Report finish(double t);
    const Report& sofar() const { return r_; }

private:
    void closeScene(double t);
    Report r_;
    bool running_ = false;
    std::string scene_;
    double sceneSince_ = 0;
    double bucketSum_ = 0;
    int bucketN_ = 0;
    double bucketStart_ = 0;
    std::int64_t wordsBase_ = -1, wordsLast_ = 0;
};

std::string toJson(const Report& r);
bool fromJson(const std::string& text, Report& out);
std::string durationWords(double seconds);   // "1 h 24 min", "12 min 5 s", "40 s"
// Time-lapse (0.12): one frame every `interval` seconds, played at `fps`. How long an hour of real time becomes,
// in words ("An hour becomes 1 min 12 s"), and which frame a moment `elapsed` seconds in belongs to.
std::string timelapseWords(double interval, int fps);
inline long long timelapseFrame(double elapsed, double interval) { return interval > 0 && elapsed >= 0 ? (long long)(elapsed / interval) : -1; }

}  // namespace ncap::live
