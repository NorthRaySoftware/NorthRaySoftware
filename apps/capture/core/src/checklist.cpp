#include "ncap/checklist.hpp"

#include <algorithm>

namespace ncap::checklist {

std::vector<std::string> defaultItems() {
    return {"Phone on silent", "A drink within reach", "Stream title and category set", "Notifications turned off"};
}

std::vector<std::string> tidy(const std::vector<std::string>& items) {
    std::vector<std::string> out;
    for (std::string t : items) {
        const auto a = t.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) continue;
        t = t.substr(a, t.find_last_not_of(" \t\r\n") - a + 1);
        if (t.size() > kMaxChars) {
            std::size_t cut = kMaxChars;
            while (cut > 0 && (static_cast<unsigned char>(t[cut]) & 0xC0) == 0x80) --cut;   // not inside a character
            t = t.substr(0, cut);
        }
        if (std::find(out.begin(), out.end(), t) != out.end()) continue;
        out.push_back(t);
        if (out.size() == kMaxItems) break;
    }
    return out;
}

namespace {
std::string joinNames(const std::vector<std::string>& v) {
    std::string s;
    for (std::size_t i = 0; i < v.size(); ++i) s += (i ? (i + 1 == v.size() ? " and " : ", ") : "") + v[i];
    return s;
}
}  // namespace

std::vector<Check> run(const Snapshot& s) {
    std::vector<Check> c;
    // Where it's going.
    if (s.destinations.empty()) c.push_back({State::Problem, "No destination is switched on (Settings › Streaming)."});
    else {
        std::vector<std::string> names, noKey, noServer;
        for (auto& d : s.destinations) (!d.hasServer ? noServer : d.hasKey ? names : noKey).push_back(d.name);
        if (!noServer.empty())
            c.push_back({State::Problem, joinNames(noServer) + (noServer.size() > 1 ? " have no server address yet, so they won't start." : " has no server address yet, so it won't start.")});
        if (!names.empty()) c.push_back({State::Ok, "Going live on " + joinNames(names) + "."});
        if (!noKey.empty()) c.push_back({State::Problem, joinNames(noKey) + (noKey.size() > 1 ? " have no stream key, so they won't start." : " has no stream key, so it won't start.")});
    }
    // What viewers see first.
    if (s.sceneEmpty) c.push_back({State::Warn, "The scene showing, " + s.scene + ", is empty: viewers would see black."});
    else c.push_back({State::Ok, "Viewers will see " + s.scene + " first."});
    // The mic.
    if (s.mics.empty()) c.push_back({State::Warn, "There's no microphone in the mixer, so viewers won't hear you."});
    std::uint32_t carried = 0;
    for (auto& d : s.destinations) carried |= 1u << std::clamp(d.bus, 0, 6);
    for (auto& m : s.mics) {
        if (!m.working) c.push_back({State::Problem, m.name + " isn't working (unplugged, or another program has it)."});
        else if (m.muted) c.push_back({State::Warn, m.name + " is muted."});
        else if (!s.destinations.empty() && !(m.buses & carried)) c.push_back({State::Warn, m.name + " isn't in the sound your stream carries (its routing in the mixer)."});
        else if (m.silentFor < 0 || m.silentFor > kQuietSeconds) c.push_back({State::Waiting, "Say something to check " + m.name + "…"});
        else c.push_back({State::Ok, m.name + " is picking you up."});
    }
    // The encoder.
    if (!s.encodersKnown) c.push_back({State::Waiting, "Checking this PC's video encoders…"});
    else if (!s.hardwareEncoder)
        c.push_back({State::Warn, "No hardware video encoder: the processor will encode, which can slow games. Keep the bitrate and canvas modest."});
    if (s.recording) c.push_back({State::Ok, "Recording is already on and carries on."});
    return c;
}

bool anyProblem(const std::vector<Check>& checks) {
    return std::any_of(checks.begin(), checks.end(), [](const Check& c) { return c.state == State::Warn || c.state == State::Problem; });
}

}  // namespace ncap::checklist
