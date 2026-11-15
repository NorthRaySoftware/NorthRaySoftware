#include "ncap/routing.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace ncap::routing {

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool sameDevice(const std::string& a, const std::string& b) { return lower(a) == lower(b); }

std::string appLabel(const std::string& exe) {
    std::string n = exe;
    if (n.size() > 4 && n.compare(n.size() - 4, 4, ".exe") == 0) n.resize(n.size() - 4);
    if (!n.empty()) n[0] = (char)std::toupper((unsigned char)n[0]);
    return n;
}

const char* busName(std::uint32_t b) {
    switch (b) {
        case Stream: return "the stream";
        case Track1: return "recording track 1";
        case Track2: return "recording track 2";
        case Track3: return "recording track 3";
        case Track4: return "recording track 4";
        case Track5: return "recording track 5";
        case Track6: return "recording track 6";
        case Monitor: return "the monitor";
        default: return "a mix";
    }
}

}  // namespace

std::string exeKey(const std::string& pathOrName) {
    const size_t slash = pathOrName.find_last_of("\\/");
    return lower(slash == std::string::npos ? pathOrName : pathOrName.substr(slash + 1));
}

std::vector<Warning> analyse(const std::vector<AudioSource>& sources, const std::vector<PlayingApp>& playing, const std::string& monitorDevice,
                             bool monitorIsHeadphones, const std::string& secondDevice, std::uint32_t secondBus) {
    std::vector<Warning> out;

    // 1. Leaks through desktop audio. An app that has its own source kept off the stream (or that's
    //    listed as excluded somewhere) but plays on a device whose desktop capture goes to the stream.
    std::set<std::string> keptOffStream;
    for (const auto& s : sources)
        if (s.kind == Kind::Application && !s.reaches(Stream)) keptOffStream.insert(exeKey(s.app));
    for (const auto& d : sources) {
        if (d.kind != Kind::Desktop || !d.reaches(Stream)) continue;
        std::set<std::string> excluded;
        for (const auto& e : d.excluded) excluded.insert(exeKey(e));
        for (const auto& p : playing) {
            const std::string app = exeKey(p.app);
            if (!sameDevice(p.device, d.device) || excluded.count(app)) continue;
            if (keptOffStream.count(app)) {
                std::vector<std::string> ids = {d.id};
                for (const auto& s : sources)
                    if (s.kind == Kind::Application && exeKey(s.app) == app) ids.push_back(s.id);
                out.push_back({Warning::LeakThroughDesktop, ids, app,
                               appLabel(app) + " is kept off the stream, but \"" + d.name +
                                   "\" captures everything that device plays, so viewers will still hear it. Leave " + appLabel(app) +
                                   " out of \"" + d.name + "\", or replace it with sources for just the apps you want."});
            }
        }
    }

    // 2. The same sound reaching one bus twice: two app sources for one app, an app source alongside a
    //    desktop capture that includes that app, or two desktop captures of one device.
    const std::uint32_t buses[] = {Stream, Track1, Track2, Track3, Track4, Track5, Track6};
    for (std::uint32_t bus : buses) {
        std::map<std::string, std::vector<std::string>> byApp;      // app -> sources carrying it to this bus
        std::map<std::string, std::vector<std::string>> byDevice;   // desktop device -> sources
        for (const auto& s : sources) {
            if (!s.reaches(bus)) continue;
            if (s.kind == Kind::Application) byApp[exeKey(s.app)].push_back(s.id);
            if (s.kind == Kind::Desktop) byDevice[lower(s.device)].push_back(s.id);
        }
        for (const auto& s : sources) {
            if (!s.reaches(bus) || s.kind != Kind::Desktop) continue;
            std::set<std::string> excluded;
            for (const auto& e : s.excluded) excluded.insert(exeKey(e));
            for (auto& [app, ids] : byApp) {
                if (excluded.count(app)) continue;
                const bool plays = std::any_of(playing.begin(), playing.end(),
                                               [&](const PlayingApp& p) { return exeKey(p.app) == app && sameDevice(p.device, s.device); });
                if (plays) ids.push_back(s.id);
            }
        }
        for (auto& [app, ids] : byApp)
            if (ids.size() > 1) {
                bool dup = false;
                for (const auto& w : out)
                    if (w.type == Warning::CapturedTwice && w.app == app) dup = true;
                if (!dup)
                    out.push_back({Warning::CapturedTwice, ids, app,
                                   appLabel(app) + " reaches " + busName(bus) + " from " + std::to_string(ids.size()) +
                                       " sources, so it will sound doubled. Keep one, or leave it out of the desktop capture."});
            }
        for (auto& [dev, ids] : byDevice)
            if (ids.size() > 1)
                out.push_back({Warning::CapturedTwice, ids, "",
                               "Two desktop audio sources capture the same device for " + std::string(busName(bus)) + ", so everything is doubled."});
    }

    // 3. Monitoring desktop audio on the device it's captured from plays it back into itself.
    for (const auto& s : sources)
        if (s.kind == Kind::Desktop && s.reaches(Monitor) && sameDevice(s.device, monitorDevice))
            out.push_back({Warning::MonitorFeedback, {s.id}, "",
                           "\"" + s.name + "\" is monitored on the device it captures, so it would feed back into itself. Turn off its monitoring."});

    // 3b. The second output does the same if it plays desktop audio onto the device it's captured from.
    if (secondBus) {
        for (const auto& s : sources)
            if (s.kind == Kind::Desktop && s.reaches(secondBus) && sameDevice(s.device, secondDevice))
                out.push_back({Warning::OutputFeedback, {s.id}, "",
                               "The second output plays \"" + s.name + "\" onto the device it captures, so it would feed back into itself. "
                               "Choose another device for the second output, or keep this source off " + busLabel(secondBus) + "."});
        bool monitored = false;
        for (const auto& s : sources) monitored |= s.reaches(Monitor);
        if (monitored && sameDevice(secondDevice, monitorDevice))
            out.push_back({Warning::OutputsShareDevice, {}, "",
                           "The monitor and the second output play to the same device, so you'd hear both mixed together. Give the second "
                           "output a device of its own."});
    }

    // 4. Monitoring through speakers while a microphone goes out: the microphone hears the monitor.
    if (!monitorIsHeadphones) {
        std::vector<std::string> monitored, mics;
        for (const auto& s : sources) {
            if (s.reaches(Monitor) && s.kind != Kind::Input) monitored.push_back(s.id);
            if (s.kind == Kind::Input && (s.reaches(Stream) || (s.buses & kAllTracks && !s.muted))) mics.push_back(s.id);
        }
        if (!monitored.empty() && !mics.empty()) {
            std::vector<std::string> ids = mics;
            ids.insert(ids.end(), monitored.begin(), monitored.end());
            out.push_back({Warning::EchoOnMicrophone, ids, "",
                           "Monitored sound plays through speakers while a microphone is live, so the microphone may pick it up as an echo. Use headphones for monitoring."});
        }
    }
    return out;
}

std::uint32_t applyPreset(Preset p, Kind k, std::uint32_t current) {
    const std::uint32_t own = defaultBuses(k) & ~(std::uint32_t)(Stream | Track1);   // its kind's track
    const std::uint32_t mon = current & Monitor;
    switch (p) {
        case Preset::StreamAndRecord: return Stream | Track1 | own | mon;
        case Preset::RecordOnly: return Track1 | own | mon;
        case Preset::StreamOnly: return Stream | mon;
        case Preset::MonitorOnly: return Monitor;
    }
    return current;
}

bool matchPreset(std::uint32_t buses, Kind k, Preset& out) {
    for (Preset p : {Preset::StreamAndRecord, Preset::RecordOnly, Preset::StreamOnly, Preset::MonitorOnly})
        if (applyPreset(p, k, buses) == buses) {
            out = p;
            return true;
        }
    return false;
}

std::uint32_t defaultBuses(Kind k) {
    switch (k) {
        case Kind::Input: return Stream | Track1 | Track2;
        case Kind::Desktop: return Stream | Track1 | Track3;
        case Kind::Application: return Stream | Track1 | Track4;
    }
    return Stream | Track1;
}

std::string busLabel(std::uint32_t bus) {
    if (bus == Stream) return "Stream";
    if (bus == Monitor) return "Monitor";
    for (int n = 1; n <= 6; ++n)
        if (bus == track(n)) return "Track " + std::to_string(n);
    return "Mix";
}

std::vector<std::string> carriedApps(const AudioSource& d, const std::vector<PlayingApp>& playing) {
    std::set<std::string> excluded, out;
    for (const auto& e : d.excluded) excluded.insert(exeKey(e));
    for (const auto& p : playing)
        if (sameDevice(p.device, d.device) && !excluded.count(exeKey(p.app))) out.insert(exeKey(p.app));
    return {out.begin(), out.end()};
}

std::string describeRoute(const AudioSource& s) {
    std::string from = s.kind == Kind::Application ? appLabel(exeKey(s.app)) + " (application)"
                     : s.kind == Kind::Desktop     ? "Everything playing on " + (s.device.empty() ? std::string("the default output") : s.device)
                                                   : (s.device.empty() ? std::string("The default microphone") : s.device);
    if (s.kind == Kind::Desktop && !s.excluded.empty()) {
        from += ", except ";
        for (size_t i = 0; i < s.excluded.size(); ++i) from += (i ? ", " : "") + appLabel(exeKey(s.excluded[i]));
    }
    if (s.muted) return from + " → muted";
    std::vector<std::string> to;
    if (s.buses & Stream) to.push_back("Stream");
    for (int n = 1; n <= 6; ++n)
        if (s.buses & track(n)) to.push_back("Track " + std::to_string(n));
    if (s.buses & Monitor) to.push_back("Monitor");
    if (to.empty()) return from + " → nowhere";
    std::string r = from + " → ";
    for (size_t i = 0; i < to.size(); ++i) r += (i ? " · " : "") + to[i];
    return r;
}

}  // namespace ncap::routing
