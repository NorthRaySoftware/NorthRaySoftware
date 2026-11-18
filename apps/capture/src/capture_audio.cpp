// NorthRay Capture's window: sound sources and the audio mixer panel.
#include "capture_window.hpp"

#include "log.hpp"

#include "cjui/dialog.hpp"

#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <ctime>

namespace ncap {

using cjui::Align;
using cjui::Color;
using cjui::Font;
using cjui::MenuItem;
using cjui::narrow;
using cjui::Rect;
using cjui::widen;
using nlohmann::json;

namespace {

constexpr UINT_PTR kMeterTimer = 3;
enum : UINT {
    CmdInDefault = 5000, CmdIn0 = 5001, CmdOutDefault = 6000, CmdOut0 = 6001, CmdApp0 = 7000,
    CmdARename = 100, CmdAMute, CmdAClip, CmdARemove, CmdAFilters, CmdADevice0 = 200,
    CmdBus0 = 300, CmdPreset0 = 310, CmdBusClip0 = 320, CmdLeaveOutNone = 340, CmdLeaveOut0 = 341,
    CmdDuckOff = 700, CmdDuck0 = 701, CmdDuckAmt0 = 800,
};

double seconds() {
    static LARGE_INTEGER f = [] {
        LARGE_INTEGER x;
        QueryPerformanceFrequency(&x);
        return x;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

json settingsOf(const Source& s) {
    json j = json::parse(s.settings, nullptr, false);
    return j.is_object() ? j : json::object();
}

// What a capture is started with: changing anything else (the volume) doesn't restart it.
std::string captureKey(const Source& s) {
    const json j = settingsOf(s);
    auto get = [&](const char* k) { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(); };
    return get("device") + "|" + get("exe") + "|" + get("exclude");
}

}  // namespace

// ---------------------------------------------------------------- captures and the mixer

void CaptureWindow::startAudio() {
    mixer_.start();
    syncAudio();
    analyseRouting();
}

float CaptureWindow::audioGain(const Source& s) const {
    const json j = settingsOf(s);
    return j.contains("volume") && j["volume"].is_number() ? std::clamp(j["volume"].get<float>(), 0.0f, 1.0f) : 1.0f;
}

bool CaptureWindow::audioMuted(const Source& s) const {
    const json j = settingsOf(s);
    return j.contains("muted") && j["muted"].is_boolean() && j["muted"].get<bool>();
}

std::uint32_t CaptureWindow::audioBuses(const Source& s) const {
    const json j = settingsOf(s);
    if (j.contains("buses") && j["buses"].is_number_unsigned()) return j["buses"].get<std::uint32_t>() & 0xFFu;
    const routing::Kind k = s.kind == SourceKind::InputAudio || s.kind == SourceKind::Media ? routing::Kind::Input
                          : s.kind == SourceKind::DesktopAudio ? routing::Kind::Desktop : routing::Kind::Application;
    // A music or video file is also heard in your headphones (the monitor): it's a file, so playing it back can't
    // feed back into anything being captured.
    return routing::defaultBuses(k) | (s.kind == SourceKind::Media ? (std::uint32_t)routing::Monitor : 0u);
}

routing::AudioSource CaptureWindow::routingOf(const Source& s) const {
    const json j = settingsOf(s);
    auto get = [&](const char* k) { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(); };
    routing::AudioSource r;
    r.id = s.id;
    r.name = s.name;
    r.kind = s.kind == SourceKind::InputAudio || s.kind == SourceKind::Media ? routing::Kind::Input : s.kind == SourceKind::DesktopAudio ? routing::Kind::Desktop : routing::Kind::Application;
    r.device = get("device");
    if (s.kind == SourceKind::Media) {   // a file's sound: named by its files (its own, never a device's)
        std::vector<std::string> files;
        if (j.contains("files") && j["files"].is_array())
            for (auto& f : j["files"])
                if (f.is_string()) files.push_back(f.get<std::string>());
        std::string f = files.empty() ? std::string() : files[0];
        if (const auto cut = f.find_last_of("\\/"); cut != std::string::npos) f = f.substr(cut + 1);
        r.device = files.empty() ? std::string("No file chosen")
                   : files.size() == 1 ? "The file " + f
                                       : "Files: " + f + " and " + std::to_string(files.size() - 1) + " more";
    }
    if (r.kind == routing::Kind::Desktop && (r.device.empty() || r.device == "default")) r.device = narrow(audio::defaultDevice(false));
    r.app = routing::exeKey(get("exe"));
    if (!get("exclude").empty()) r.excluded = {routing::exeKey(get("exclude"))};
    r.buses = audioBuses(s);
    r.muted = audioMuted(s);
    r.volume = audioGain(s);
    return r;
}

void CaptureWindow::analyseRouting() {
    std::vector<routing::AudioSource> list;
    for (auto& s : scenes_.sources())
        if (hasSound(s.kind)) list.push_back(routingOf(s));
    warnings_.clear();
    carried_.clear();
    if (list.empty()) return;
    // Every app with a sound session, on each device it has one: playing now or able to start.
    std::vector<routing::PlayingApp> playing;
    for (auto& a : audio::listAudioApps())
        for (auto& d : a.devices) playing.push_back({routing::exeKey(narrow(a.exe)), narrow(d)});
    std::string monitor = narrow(settings_.monitorDevice);
    if (monitor == "default") monitor = narrow(audio::defaultDevice(false));
    if (settings_.monitorDevice == L"off")
        for (auto& r : list) r.buses &= ~(std::uint32_t)routing::Monitor;   // nothing is played back
    std::string second;
    std::uint32_t secondBus = 0;
    if (settings_.secondDevice != L"off") {
        second = narrow(settings_.secondDevice);
        if (second == "default") second = narrow(audio::defaultDevice(false));
        secondBus = audio::busBit(settings_.secondBus);
    }
    warnings_ = routing::analyse(list, playing, monitor, settings_.monitorHeadphones, second, secondBus);
    for (auto& r : list)
        if (r.kind == routing::Kind::Desktop) carried_[r.id] = routing::carriedApps(r, playing);
}

void CaptureWindow::applyMonitor() {
    // Played only when something is sent to the monitor.
    bool any = false;
    for (auto& s : scenes_.sources())
        if (hasSound(s.kind) && (audioBuses(s) & routing::Monitor) && !audioMuted(s)) any = true;
    mixer_.setMonitorDevice(any ? settings_.monitorDevice : L"off");
    mixer_.setSecondOutput(settings_.secondDevice, settings_.secondBus);
}

void CaptureWindow::secondOutputMenu(POINT screen) {
    const auto outs = audio::listOutputs();
    std::vector<MenuItem> items = {MenuItem::item(L"Off", 2, {}, true, settings_.secondDevice == L"off"),
                                   MenuItem::item(L"Whichever is Windows\u2019 default", 1, {}, true, settings_.secondDevice == L"default"), MenuItem::sep()};
    for (size_t i = 0; i < outs.size() && i < 90; ++i)
        items.push_back(MenuItem::item(outs[i].name, 10 + (UINT)i, {}, true, outs[i].id == settings_.secondDevice));
    const UINT cmd = cjui::showMenu(hwnd(), items, screen, pal_);
    if (!cmd) return;
    settings_.secondDevice = cmd == 1 ? L"default" : cmd == 2 ? L"off" : outs[cmd - 10].id;
    settings_.save(settingsPath_);
    mixer_.setSecondOutput(L"off", 0);   // restart on the new device
    applyMonitor();
    analyseRouting();
    invalidate();
}

void CaptureWindow::monitorMenu(POINT screen) {
    const auto outs = audio::listOutputs();
    std::vector<MenuItem> items = {MenuItem::item(L"Whichever is Windows\u2019 default", 1, {}, true, settings_.monitorDevice == L"default"),
                                   MenuItem::item(L"Off (hear nothing)", 2, {}, true, settings_.monitorDevice == L"off"), MenuItem::sep()};
    for (size_t i = 0; i < outs.size() && i < 90; ++i) items.push_back(MenuItem::item(outs[i].name, 10 + (UINT)i, {}, true, outs[i].id == settings_.monitorDevice));
    const UINT cmd = cjui::showMenu(hwnd(), items, screen, pal_);
    if (!cmd) return;
    settings_.monitorDevice = cmd == 1 ? L"default" : cmd == 2 ? L"off" : outs[cmd - 10].id;
    settings_.save(settingsPath_);
    mixer_.setMonitorDevice(L"off");   // restart on the new device
    applyMonitor();
    analyseRouting();
    invalidate();
}

void CaptureWindow::saveMixClip(int bus) {
    std::wstring folder = settings_.recordingsFolder;
    if (folder.empty()) {
        PWSTR videos = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &videos))) folder = std::wstring(videos) + L"\\NorthRay Capture";
        CoTaskMemFree(videos);
    }
    SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    wchar_t stamp[32];
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    wcsftime(stamp, 32, L"%Y-%m-%d %H.%M.%S", &tmv);
    const std::wstring name = widen(routing::busLabel(audio::busBit(bus)));
    const std::wstring path = folder + L"\\Test clip - " + name + L" mix - " + stamp + L".wav";
    if (!mixer_.startClip(audio::Mixer::busKey(bus), path, 10)) {
        flash(L"A test clip is already being saved, or the folder can\u2019t be written to.", 5);
        return;
    }
    flash(L"Saving 10 seconds of the " + name + L" mix: exactly what it carries\u2026", 10);
}

void CaptureWindow::syncAudio() {
    for (auto it = audio_.begin(); it != audio_.end();) {
        const Source* s = scenes_.source(it->first);
        if (!s || !hasSound(s->kind)) it = audio_.erase(it);   // stops its thread
        else ++it;
    }
    for (auto it = boards_.begin(); it != boards_.end();) {
        const Source* s = scenes_.source(it->first);
        if (!s || s->kind != SourceKind::Soundboard) it = boards_.erase(it);
        else ++it;
    }
    std::vector<audio::MixSource> mix;
    // Auto-duck: {"duck": {"under": <source id>, "amount": -12}} in a source's settings.
    auto duckOf = [this](const Source& s, audio::MixSource& m) {
        const json j = settingsOf(s);
        if (!j.contains("duck") || !j["duck"].is_object()) return;
        const json& d = j["duck"];
        if (d.contains("under") && d["under"].is_string() && scenes_.source(d["under"].get<std::string>())) m.duckUnder = d["under"].get<std::string>();
        if (d.contains("amount") && d["amount"].is_number()) m.duck.amountDb = std::clamp(d["amount"].get<float>(), -40.0f, 0.0f);
    };
    for (const Source& s : scenes_.sources()) {
        if (!hasSound(s.kind)) continue;
        AudioLive& a = audio_[s.id];
        if (s.kind == SourceKind::Media) {   // the file's own sound, from its picture source
            auto it = live_.find(s.id);
            std::shared_ptr<audio::Ring> feed = it != live_.end() && it->second.capture ? it->second.capture->sound() : nullptr;
            audio::MixSource m{s.id, nullptr, audioGain(s), audioMuted(s), audioBuses(s)};
            m.feed = feed;
            m.filters = filtersFor(s);
            duckOf(s, m);
            mix.push_back(m);
            continue;
        }
        if (s.kind == SourceKind::Soundboard) {   // its buttons' sounds, played by its player
            auto& p = boards_[s.id];
            if (!p) p = std::make_unique<audio::SoundboardPlayer>();
            p->setSounds(soundboardSounds(s));
            audio::MixSource m{s.id, nullptr, audioGain(s), audioMuted(s), audioBuses(s)};
            m.feed = p->ring();
            m.filters = filtersFor(s);
            duckOf(s, m);
            mix.push_back(m);
            continue;
        }
        const std::string key = captureKey(s);
        if (key.rfind("#synctone", 0) == 0) {   // generated by the mixer, nothing to capture
            mix.push_back({s.id, nullptr, audioGain(s), audioMuted(s), audioBuses(s), nullptr, nullptr, true});
            continue;
        }
        if (!a.capture) a.capture = std::make_shared<audio::Capture>(s.kind);
        if (a.key != key || a.key.empty()) {
            a.key = key.empty() ? "|" : key;
            a.capture->setSettings(s.settings);
        }
        audio::MixSource m{s.id, a.capture, audioGain(s), audioMuted(s), audioBuses(s)};
        m.filters = filtersFor(s);
        duckOf(s, m);
        mix.push_back(m);
    }
    if (stinger_) {   // a stinger's sound goes out with it
        audio::MixSource m{"#stinger", nullptr, 1.0f, false, routing::Stream | routing::Track1};
        m.feed = stinger_->sound();
        mix.push_back(m);
    }
    if (testSound_) {   // Test my setup's chime: heard in the monitor, never streamed or recorded
        audio::MixSource m{"#testsound", nullptr, 1.0f, false, routing::Monitor};
        m.feed = testSound_->ring();
        mix.push_back(m);
    }
    mixer_.setSources(std::move(mix));
    applyMonitor();
    // Meters run while there's something to measure (slower in low-resource mode).
    const bool want = !audio_.empty();
    if (want != metersRunning_) {
        metersRunning_ = want;
        if (want) SetTimer(hwnd(), kMeterTimer, settings_.lowResource ? 100 : 33, nullptr);
        else KillTimer(hwnd(), kMeterTimer);
    }
}

void CaptureWindow::meterTick() {
    visualiserTick();
    const double now = seconds();
    for (auto& [id, lv] : mixer_.takeLevels()) {
        if (id == audio::Mixer::busKey(0)) {
            mixLeft_.feed(lv.left, now);
            mixRight_.feed(lv.right, now);
            continue;
        }
        if (id == audio::Mixer::busKey(7)) {
            monLeft_.feed(lv.left, now);
            monRight_.feed(lv.right, now);
            continue;
        }
        auto it = audio_.find(id);
        if (it == audio_.end()) continue;
        it->second.left.feed(lv.left, now);
        it->second.right.feed(lv.right, now);
        it->second.duckDb = lv.duckDb;
        if (std::max(lv.left, lv.right) > 0.003f) it->second.heardAt = now;   // about -50 dB
    }
    bool ok = false;
    const std::wstring clip = mixer_.takeFinishedClip(&ok);
    if (!clip.empty()) {
        if (ok) {
            log::info("Test clip saved: " + narrow(clip));
            flash(L"Saved the test clip: " + clip, 8);
        } else {
            log::error("Test clip couldn't be written: " + narrow(clip));
            flash(L"The test clip couldn't be written (is the disk full?)", 6);
        }
    }
    // Only the meters change: repaint (the whole window is cheap to draw, and nothing else moves).
    if (!settingsOpen_ || checklistOpen_) invalidate();
}

void CaptureWindow::setAudioSetting(const Id& source, const char* key, const json& value) {
    const Source* s = scenes_.source(source);
    if (!s) return;
    json j = settingsOf(*s);
    j[key] = value;
    if (std::string(key) == "buses") j["monitorChosen"] = true;   // their choice: kept (see loadScenes)
    scenes_.setSourceSettings(source, j.dump());
    syncAudio();
    scenesChanged();
}

// ---------------------------------------------------------------- adding

CaptureWindow::AudioChoices CaptureWindow::audioChoices() const {
    AudioChoices c;
    c.inputs = audio::listInputs();
    c.outputs = audio::listOutputs();
    c.appsSupported = audio::applicationAudioSupported();
    c.apps = audio::listAudioApps();
    return c;
}

std::vector<MenuItem> CaptureWindow::audioAddItems(const AudioChoices& c) const {
    std::vector<MenuItem> in, out, apps;
    in.push_back(MenuItem::item(L"Whichever is Windows’ default", CmdInDefault));
    for (size_t i = 0; i < c.inputs.size() && i < 900; ++i)
        in.push_back(MenuItem::item(c.inputs[i].name, CmdIn0 + (UINT)i, c.inputs[i].isDefault ? L"default" : L""));
    out.push_back(MenuItem::item(L"Whichever is Windows’ default", CmdOutDefault));
    for (size_t i = 0; i < c.outputs.size() && i < 900; ++i)
        out.push_back(MenuItem::item(c.outputs[i].name, CmdOut0 + (UINT)i, c.outputs[i].isDefault ? L"default" : L""));
    if (!c.appsSupported) apps.push_back(MenuItem::item(L"Needs Windows 11", 1, {}, false));
    else if (c.apps.empty()) apps.push_back(MenuItem::item(L"No programs have opened sound yet", 1, {}, false));
    else
        for (size_t i = 0; i < c.apps.size() && i < 900; ++i)
            apps.push_back(MenuItem::item(c.apps[i].name, CmdApp0 + (UINT)i, c.apps[i].playing ? L"playing" : L""));
    return {
        MenuItem::sub(L"Microphone or input", in),
        MenuItem::sub(L"Desktop audio (everything you hear)", out),
        MenuItem::sub(L"Application audio (one program)", apps),
    };
}

bool CaptureWindow::audioAddCommand(UINT cmd, const AudioChoices& c) {
    auto add = [&](SourceKind kind, const std::string& name, const json& settings) {
        json j = settings;
        j["volume"] = 1.0;
        j["muted"] = false;
        const Id id = scenes_.addSource(kind, name, j.dump());
        log::info(std::string("Sound source added: ") + kindKey(kind) + " \"" + scenes_.source(id)->name + "\"");
        syncAudio();
        analyseRouting();
        scenesChanged();
    };
    if (cmd == CmdInDefault) add(SourceKind::InputAudio, "Microphone", {{"device", "default"}});
    else if (cmd >= CmdIn0 && cmd < CmdIn0 + c.inputs.size()) add(SourceKind::InputAudio, narrow(c.inputs[cmd - CmdIn0].name), {{"device", narrow(c.inputs[cmd - CmdIn0].id)}});
    else if (cmd == CmdOutDefault) add(SourceKind::DesktopAudio, "Desktop audio", {{"device", "default"}});
    else if (cmd >= CmdOut0 && cmd < CmdOut0 + c.outputs.size())
        add(SourceKind::DesktopAudio, "Desktop audio: " + narrow(c.outputs[cmd - CmdOut0].name), {{"device", narrow(c.outputs[cmd - CmdOut0].id)}});
    else if (cmd >= CmdApp0 && cmd < CmdApp0 + c.apps.size()) add(SourceKind::ApplicationAudio, narrow(c.apps[cmd - CmdApp0].name), {{"exe", narrow(c.apps[cmd - CmdApp0].exe)}});
    else return false;
    return true;
}

void CaptureWindow::addAudioMenu(POINT screen) {
    const AudioChoices c = audioChoices();
    const UINT cmd = cjui::showMenu(hwnd(), audioAddItems(c), screen, pal_);
    audioAddCommand(cmd, c);
}

// ---------------------------------------------------------------- a sound source's menu

void CaptureWindow::removeAudio(const Id& source) {
    const Source* s = scenes_.source(source);
    if (!s) return;
    if (cjui::messageBox(hwnd(), L"Remove “" + widen(s->name) + L"”? It stops being captured.", L"Remove sound source",
                         MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
    scenes_.removeSource(source);
    syncAudio();
    analyseRouting();
    scenesChanged();
}

void CaptureWindow::saveTestClip(const Id& source) {
    const Source* s = scenes_.source(source);
    if (!s) return;
    std::wstring folder = settings_.recordingsFolder;
    if (folder.empty()) {
        PWSTR videos = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &videos))) folder = std::wstring(videos) + L"\\NorthRay Capture";
        CoTaskMemFree(videos);
    }
    SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    std::wstring name = widen(s->name);
    for (auto& ch : name)
        if (wcschr(L"\\/:*?\"<>|", ch)) ch = L'-';
    wchar_t stamp[32];
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    wcsftime(stamp, 32, L"%Y-%m-%d %H.%M.%S", &tmv);
    const std::wstring path = folder + L"\\Test clip - " + name + L" - " + stamp + L".wav";
    if (!mixer_.startClip(source, path, 10)) {
        flash(L"A test clip is already being saved, or the folder can’t be written to.", 5);
        return;
    }
    flash(L"Saving 10 seconds of “" + widen(s->name) + L"” on its own (before its volume and mute)…", 10);
}

void CaptureWindow::audioItemMenu(const Id& source, POINT screen) {
    const Source* s = scenes_.source(source);
    if (!s) return;
    const bool muted = audioMuted(*s);
    const std::size_t nFilters = dsp::fromJson(settingsOf(*s).contains("filters") ? settingsOf(*s)["filters"].dump() : "[]").size();
    std::vector<MenuItem> menu = {
        MenuItem::item(L"Rename…", CmdARename),
        MenuItem::item(muted ? L"Unmute" : L"Mute", CmdAMute),
        MenuItem::item(L"Filters…", CmdAFilters, nFilters ? std::to_wstring(nFilters) + L" on it" : L"gate, compressor, EQ…"),
    };
    // Which device or program it takes its sound from.
    std::vector<audio::Device> devices;
    std::vector<audio::App> apps;
    const json j = settingsOf(*s);
    const std::string cur = j.contains("device") && j["device"].is_string() ? j["device"].get<std::string>()
                            : j.contains("exe") && j["exe"].is_string() ? j["exe"].get<std::string>() : std::string();
    std::vector<MenuItem> from;
    if (s->kind == SourceKind::Media) {
        // A file's sound: where it comes from is the file (its menu in Sources).
    } else if (s->kind == SourceKind::ApplicationAudio) {
        apps = audio::listAudioApps();
        for (size_t i = 0; i < apps.size() && i < 90; ++i)
            from.push_back(MenuItem::item(apps[i].name, CmdADevice0 + (UINT)i, apps[i].playing ? L"playing" : L"", true, narrow(apps[i].exe) == cur));
        if (from.empty()) from.push_back(MenuItem::item(L"No programs have opened sound yet", 1, {}, false));
        menu.push_back(MenuItem::sub(L"Program", from));
    } else {
        devices = s->kind == SourceKind::InputAudio ? audio::listInputs() : audio::listOutputs();
        from.push_back(MenuItem::item(L"Whichever is Windows’ default", CmdADevice0 + 99, {}, true, cur.empty() || cur == "default"));
        for (size_t i = 0; i < devices.size() && i < 90; ++i)
            from.push_back(MenuItem::item(devices[i].name, CmdADevice0 + (UINT)i, {}, true, narrow(devices[i].id) == cur));
        menu.push_back(MenuItem::sub(L"Device", from));
    }
    // Where it goes.
    const std::uint32_t buses = audioBuses(*s);
    const routing::Kind rk = routingOf(*s).kind;
    routing::Preset now{};
    const bool matched = routing::matchPreset(buses, rk, now);
    const wchar_t* presetNames[] = {L"Stream and recordings", L"Recordings only (never the stream)", L"Stream only (not recorded)", L"Only me (monitor)"};
    std::vector<MenuItem> to;
    for (int pr = 0; pr < 4; ++pr)
        to.push_back(MenuItem::item(presetNames[pr], CmdPreset0 + (UINT)pr, {}, true, matched && (int)now == pr));
    to.push_back(MenuItem::sep());
    for (int b = 0; b < audio::kBuses; ++b) {
        if (b == 1 || b == 7) to.push_back(MenuItem::sep());
        if (b == 0) to.push_back(MenuItem::item(L"Or each mix on its own:", 1, {}, false));
        std::wstring label = widen(routing::busLabel(audio::busBit(b)));
        if (b == 0) label = L"Stream (what viewers hear)";
        if (b == 7) label = L"Monitor (your headphones)";
        to.push_back(MenuItem::item(label, CmdBus0 + (UINT)b, {}, true, (buses & audio::busBit(b)) != 0));
    }
    menu.push_back(MenuItem::sub(L"Send to", to));
    // Auto-duck: quieter while another source (usually the microphone) has sound.
    std::vector<const Source*> triggers;
    std::string duckUnder;
    float duckAmount = -12;
    if (j.contains("duck") && j["duck"].is_object()) {
        if (j["duck"].contains("under") && j["duck"]["under"].is_string()) duckUnder = j["duck"]["under"].get<std::string>();
        if (j["duck"].contains("amount") && j["duck"]["amount"].is_number()) duckAmount = j["duck"]["amount"].get<float>();
    }
    {
        std::vector<MenuItem> dm = {MenuItem::item(L"Off", CmdDuckOff, {}, true, duckUnder.empty())};
        for (int pass = 0; pass < 2; ++pass)   // microphones and inputs first
            for (const Source& o : scenes_.sources())
                if (o.id != source && hasSound(o.kind) && ((o.kind == SourceKind::InputAudio) == (pass == 0)) && triggers.size() < 60) {
                    dm.push_back(MenuItem::item(L"While " + widen(o.name) + L" has sound", CmdDuck0 + (UINT)triggers.size(),
                                                o.kind == SourceKind::InputAudio ? L"microphone or input" : L"", true, o.id == duckUnder));
                    triggers.push_back(&o);
                }
        dm.push_back(MenuItem::sep());
        const int amounts[] = {-6, -12, -18, -24};
        for (int i = 0; i < 4; ++i)
            dm.push_back(MenuItem::item(std::to_wstring(-amounts[i]) + L" dB quieter", CmdDuckAmt0 + (UINT)i, {}, !duckUnder.empty(),
                                        !duckUnder.empty() && std::lround(duckAmount) == amounts[i]));
        menu.push_back(MenuItem::sub(L"Auto-duck (quieter while you talk)", dm));
    }
    // Desktop audio: leave one program out (Windows takes one per source).
    std::vector<audio::App> leaveOut;
    if (s->kind == SourceKind::DesktopAudio) {
        const std::string ex = j.contains("exclude") && j["exclude"].is_string() ? j["exclude"].get<std::string>() : std::string();
        std::vector<MenuItem> lo = {MenuItem::item(L"Nothing: everything it plays", CmdLeaveOutNone, {}, true, ex.empty())};
        if (!audio::applicationAudioSupported()) lo.push_back(MenuItem::item(L"Needs Windows 11", 1, {}, false));
        else {
            leaveOut = audio::listAudioApps();
            for (size_t i = 0; i < leaveOut.size() && i < 90; ++i)
                lo.push_back(MenuItem::item(leaveOut[i].name, CmdLeaveOut0 + (UINT)i, {}, true, routing::exeKey(narrow(leaveOut[i].exe)) == routing::exeKey(ex)));
        }
        menu.push_back(MenuItem::sub(L"Leave a program out", lo));
    }
    std::vector<MenuItem> clips;
    for (int b = 0; b < audio::kBuses; ++b) clips.push_back(MenuItem::item(widen(routing::busLabel(audio::busBit(b))) + L" mix", CmdBusClip0 + (UINT)b));
    menu.push_back(MenuItem::sep());
    menu.push_back(MenuItem::item(L"Save a 10-second test clip", CmdAClip));
    menu.push_back(MenuItem::sub(L"Save a test clip of a mix", clips));
    menu.push_back(MenuItem::sep());
    menu.push_back(MenuItem::item(L"Remove", CmdARemove));
    const UINT cmd = cjui::showMenu(hwnd(), menu, screen, pal_);
    if (cmd >= CmdBus0 && cmd < CmdBus0 + audio::kBuses) {
        setAudioSetting(source, "buses", buses ^ audio::busBit((int)(cmd - CmdBus0)));
        analyseRouting();
        return;
    }
    if (cmd >= CmdPreset0 && cmd < CmdPreset0 + 4) {
        setAudioSetting(source, "buses", routing::applyPreset((routing::Preset)(cmd - CmdPreset0), rk, buses));
        analyseRouting();
        return;
    }
    if (cmd >= CmdBusClip0 && cmd < CmdBusClip0 + audio::kBuses) {
        saveMixClip((int)(cmd - CmdBusClip0));
        return;
    }
    if (cmd == CmdLeaveOutNone || (cmd >= CmdLeaveOut0 && cmd < CmdLeaveOut0 + leaveOut.size())) {
        setAudioSetting(source, "exclude", cmd == CmdLeaveOutNone ? std::string() : narrow(leaveOut[cmd - CmdLeaveOut0].exe));
        analyseRouting();
        return;
    }
    if (cmd == CmdDuckOff) {
        json jj = settingsOf(*s);
        jj.erase("duck");
        scenes_.setSourceSettings(source, jj.dump());
        syncAudio();
        scenesChanged();
        return;
    }
    if (cmd >= CmdDuck0 && cmd < CmdDuck0 + triggers.size()) {
        setAudioSetting(source, "duck", json{{"under", triggers[cmd - CmdDuck0]->id}, {"amount", duckAmount}});
        flash(L"Auto-duck on: " + widen(s->name) + L" gets " + std::to_wstring(-std::lround(duckAmount)) + L" dB quieter while " +
                  widen(triggers[cmd - CmdDuck0]->name) + L" has sound.", 6);
        return;
    }
    if (cmd >= CmdDuckAmt0 && cmd < CmdDuckAmt0 + 4 && !duckUnder.empty()) {
        const int amounts[] = {-6, -12, -18, -24};
        setAudioSetting(source, "duck", json{{"under", duckUnder}, {"amount", amounts[cmd - CmdDuckAmt0]}});
        return;
    }
    if (cmd == CmdAFilters) openFilters(source);
    else if (cmd == CmdARename) renameSource(source);
    else if (cmd == CmdAMute) setAudioSetting(source, "muted", !muted);
    else if (cmd == CmdAClip) saveTestClip(source);
    else if (cmd == CmdARemove) removeAudio(source);
    else if (cmd == CmdADevice0 + 99) setAudioSetting(source, "device", "default");
    else if (cmd >= CmdADevice0 && cmd < CmdADevice0 + 90) {
        const size_t i = cmd - CmdADevice0;
        if (s->kind == SourceKind::ApplicationAudio && i < apps.size()) setAudioSetting(source, "exe", narrow(apps[i].exe));
        else if (i < devices.size()) setAudioSetting(source, "device", narrow(devices[i].id));
    }
}

// ---------------------------------------------------------------- the panel

void CaptureWindow::drawMeter(const Rect& r, audio::Meter& left, audio::Meter& right, bool muted) {
    const double now = seconds();
    // -60 dB to 0 dB across; green to -20, yellow to -9, red above.
    auto xOf = [&](float db) { return r.x + r.w * std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f); };
    const Color green = cjui::hexColor("#3FCF8E"), yellow = cjui::hexColor("#E8C547"), red = cjui::hexColor("#E0566A");
    const float barH = std::floor((r.h - 2) / 2);
    audio::Meter* m[2] = {&left, &right};
    for (int ch = 0; ch < 2; ++ch) {
        const Rect bar{r.x, r.y + ch * (barH + 2), r.w, barH};
        const float dim = pal_.dark ? 0.16f : 0.22f;
        gfx_.fill({bar.x, bar.y, xOf(-20) - bar.x, bar.h}, green.withAlpha(dim));
        gfx_.fill({xOf(-20), bar.y, xOf(-9) - xOf(-20), bar.h}, yellow.withAlpha(dim));
        gfx_.fill({xOf(-9), bar.y, bar.right() - xOf(-9), bar.h}, red.withAlpha(dim));
        const float level = m[ch]->level(now);
        if (level > -60) {
            const float a = muted ? 0.35f : 1.0f;
            const float x = xOf(level);
            gfx_.fill({bar.x, bar.y, std::min(x, xOf(-20)) - bar.x, bar.h}, green.withAlpha(a));
            if (x > xOf(-20)) gfx_.fill({xOf(-20), bar.y, std::min(x, xOf(-9)) - xOf(-20), bar.h}, yellow.withAlpha(a));
            if (x > xOf(-9)) gfx_.fill({xOf(-9), bar.y, x - xOf(-9), bar.h}, red.withAlpha(a));
        }
        const float hold = m[ch]->hold(now);
        if (hold > -60) gfx_.fill({xOf(hold) - 1, bar.y, 2, bar.h}, m[ch]->clipped(now) ? red : pal_.ink.withAlpha(0.7f));
    }
}

void CaptureWindow::drawMixer(const Rect& b) {
    audioRows_.clear();
    std::vector<const Source*> list;
    for (auto& s : scenes_.sources())
        if (hasSound(s.kind)) list.push_back(&s);
    if (list.empty()) {
        note(b, L"No sound sources yet",
             L"Use + above to add your microphone, desktop audio (everything you hear), or one program’s sound on its own "
             L"(Windows 11): for example a game, while music and voice chat stay out. Sound sources are heard in every scene.");
        return;
    }
    gfx_.pushClip(b);
    mixerBody_ = b;
    mixerScroll_ = std::clamp(mixerScroll_, 0.0f, std::max(0.0f, mixerContentH_ - b.h));
    float y = b.y + 8 - mixerScroll_;
    // What viewers hear, and what you hear.
    gfx_.text(L"Stream", {b.x + 12, y, 70, 16}, Font::SmallStrong, pal_.inkMuted, {Align::Left, false});
    drawMeter({b.x + 70, y + 3, b.w - 82, 10}, mixLeft_, mixRight_, false);
    y += 20;
    gfx_.text(L"Monitor", {b.x + 12, y, 70, 16}, Font::SmallStrong, pal_.inkMuted, {Align::Left, false});
    drawMeter({b.x + 70, y + 3, b.w - 82, 10}, monLeft_, monRight_, settings_.monitorDevice == L"off");
    y += 24;
    // Warnings: sound that would reach viewers against the routing, feedback, echo.
    const Color warn = cjui::hexColor("#E0566A");
    for (size_t i = 0; i < warnings_.size() && i < 3; ++i) {
        const routing::Warning& w = warnings_[i];
        const std::wstring msg = widen(w.message);
        const float th = gfx_.measureHeight(msg, b.w - 40, Font::Small);
        bool canFix = false;
        Id desk;
        if (w.type == routing::Warning::LeakThroughDesktop && audio::applicationAudioSupported())
            for (auto& id : w.sources)
                if (const Source* src = scenes_.source(id); src && src->kind == SourceKind::DesktopAudio) {
                    const json j = settingsOf(*src);
                    if (!(j.contains("exclude") && j["exclude"].is_string() && !j["exclude"].get<std::string>().empty())) {
                        canFix = true;
                        desk = id;
                    }
                }
        const Rect box{b.x + 8, y, b.w - 16, th + (canFix ? 44 : 14)};
        gfx_.fillRounded(box, 8, warn.withAlpha(pal_.dark ? 0.16f : 0.10f));
        gfx_.strokeRounded(box, 8, warn.withAlpha(0.55f), 1);
        gfx_.text(msg, {box.x + 12, box.y + 7, box.w - 24, th + 4}, Font::Small, pal_.ink);
        if (canFix) {
            const std::string app = w.app;
            button("fix:" + std::to_string(i), {box.x + 12, box.bottom() - 34, std::min(box.w - 24, 230.0f), 28},
                   L"Leave " + audio::describeExe(widen(app)) + L" out of desktop audio", [this, desk, app] {
                       setAudioSetting(desk, "exclude", app);
                       analyseRouting();
                   });
        }
        y += box.h + 8;
    }
    gfx_.line(b.x + 10, y - 2, b.right() - 10, y - 2, pal_.rule);
    y += 4;
    for (const Source* s : list) {
        const Rect row{b.x + 6, y, b.w - 12, 100};
        if (row.y > b.bottom()) {
            y += 106;
            continue;
        }
        if (row.bottom() > b.y) audioRows_.push_back({s->id, row});
        AudioLive& a = audio_[s->id];
        const bool muted = audioMuted(*s);
        const float gain = audioGain(*s);
        const std::uint32_t buses = audioBuses(*s);
        const std::string id = "aud:" + s->id;
        bool flagged = false;
        for (auto& w : warnings_)
            for (auto& wid : w.sources) flagged = flagged || wid == s->id;
        gfx_.text(widen(s->name), {row.x + 8, row.y + 2, row.w - 90, 20}, Font::BodyStrong, muted ? pal_.inkFaint : pal_.ink, {Align::Left, false, true, true});
        gfx_.text(audio::dbText(audio::gainToDb(gain)), {row.x, row.y + 2, row.w - 8, 20}, Font::Small, pal_.inkMuted, {Align::Right, false, false, true});
        bool problem = a.capture && a.capture->problem();
        std::wstring st = a.capture ? a.capture->status() : L"";
        if (s->kind == SourceKind::Media) {   // the file's playing position
            st = sourceStatus(s->id, &problem);
        }
        if (captureKey(*s).rfind("#synctone", 0) == 0) st = L"Test tone: a beep at the start of each second, with the sync test pattern";
        if (s->kind == SourceKind::DesktopAudio && !problem) {
            auto c = carried_.find(s->id);
            if (c != carried_.end() && !c->second.empty()) {
                st += L" · hears ";
                for (size_t k = 0; k < c->second.size() && k < 4; ++k) st += (k ? L", " : L"") + audio::describeExe(widen(c->second[k]));
                if (c->second.size() > 4) st += L"…";
            }
        }
        if (a.duckDb < -0.5f) {
            wchar_t db[32];
            swprintf(db, 32, L"ducked %.0f dB \u00B7 ", a.duckDb);
            st = db + st;
        }
        gfx_.text(st, {row.x + 8, row.y + 21, row.w - 16, 16}, Font::Small, problem || flagged ? warn : pal_.inkMuted, {Align::Left, false, true, true});
        drawMeter({row.x + 8, row.y + 41, row.w - 16, 10}, a.left, a.right, muted);
        // Mute, then the fader.
        const Rect mute{row.x + 4, row.y + 52, 26, 22};
        const Id sid = s->id;
        addHit(id + ":mute", mute, [this, sid, muted] {
            setAudioSetting(sid, "muted", !muted);
            analyseRouting();
        });
        const float hm = hoverAmount(id + ":mute");
        if (hm > 0.01f) gfx_.fillRounded(mute, 5, pal_.ink.withAlpha(0.08f * hm));
        const Color ic = muted ? warn : pal_.inkMuted.mix(pal_.ink, hm);
        // A speaker: a box and a cone, crossed out when muted, with a wave when not.
        const float mx = mute.x + 6, my = mute.y + 11;
        gfx_.fill({mx, my - 3, 4, 6}, ic);
        gfx_.line(mx + 4, my - 3, mx + 9, my - 7, ic, 1.4f);
        gfx_.line(mx + 9, my - 7, mx + 9, my + 7, ic, 1.4f);
        gfx_.line(mx + 9, my + 7, mx + 4, my + 3, ic, 1.4f);
        if (muted) {
            gfx_.line(mx + 12, my - 4, mx + 17, my + 4, ic, 1.4f);
            gfx_.line(mx + 17, my - 4, mx + 12, my + 4, ic, 1.4f);
        } else {
            gfx_.line(mx + 12, my - 3, mx + 13, my, ic, 1.2f);
            gfx_.line(mx + 13, my, mx + 12, my + 3, ic, 1.2f);
        }
        if (focused(id + ":mute")) focusRing(mute);
        slider(id + ":vol", {row.x + 40, row.y + 53, row.w - 50, 20}, audio::gainToFader(gain), [this, sid](float v) {
            const float g = audio::faderToGain(v);
            setAudioSetting(sid, "volume", std::round(g * 10000) / 10000);
        });
        // Where it goes: Stream, tracks 1-6, Monitor. Each toggles.
        float cx = row.x + 8;
        const float cy = row.y + 77;
        for (int bi = 0; bi < audio::kBuses; ++bi) {
            const std::wstring label = bi == 0 ? L"Stream" : bi == 7 ? L"Mon" : std::to_wstring(bi);
            const float cw = bi == 0 || bi == 7 ? gfx_.measureWidth(label, Font::Small) + 12 : 16;
            if (cx + cw > row.right() - 4) break;
            const Rect chip{cx, cy, cw, 18};
            const bool on = (buses & audio::busBit(bi)) != 0;
            const std::string cid = id + ":bus" + std::to_string(bi);
            addHit(cid, chip, [this, sid, buses, bi] {
                setAudioSetting(sid, "buses", buses ^ audio::busBit(bi));
                analyseRouting();
            });
            const float hc = hoverAmount(cid);
            const Color accent = bi == 0 ? pal_.ribbon : bi == 7 ? cjui::hexColor("#7AA7FF") : pal_.inkMuted;
            if (on) gfx_.fillRounded(chip, 5, accent.withAlpha(pal_.dark ? 0.30f : 0.22f));
            gfx_.strokeRounded(chip, 5, on ? accent : pal_.rule.mix(pal_.ink, 0.25f * hc), 1);
            gfx_.text(label, chip, Font::Small, on ? pal_.ink : pal_.inkFaint.mix(pal_.ink, hc), {Align::Center, false, false, true});
            if (focused(cid)) focusRing(chip);
            cx += cw + (bi == 0 || bi == 6 ? 6 : 2);
        }
        y += 106;
        gfx_.line(b.x + 10, y - 4, b.right() - 10, y - 4, pal_.rule.withAlpha(0.6f));
    }
    mixerContentH_ = y + mixerScroll_ - b.y;
    // More below (or above): the edge fades out; the mouse wheel scrolls.
    if (mixerScroll_ < mixerContentH_ - b.h - 1)
        for (int i = 0; i < 8; ++i) gfx_.fill({b.x, b.bottom() - 24 + i * 3.0f, b.w, 3}, pal_.surface.withAlpha(0.12f * (float)(i + 1)));
    if (mixerScroll_ > 1)
        for (int i = 0; i < 8; ++i) gfx_.fill({b.x, b.y + 21 - i * 3.0f, b.w, 3}, pal_.surface.withAlpha(0.12f * (float)(i + 1)));
    gfx_.popClip();
}

float CaptureWindow::drawRoutes(float x, float y0, float w) {
    float y = y0;
    std::vector<const Source*> list;
    for (auto& s : scenes_.sources())
        if (hasSound(s.kind)) list.push_back(&s);
    if (list.empty()) {
        return gfx_.text(L"No sound sources yet. Add them from + in the Audio mixer.", {x, y, w, 40}, Font::Small, pal_.inkMuted) + 4;
    }
    for (const Source* s : list) {
        routing::AudioSource r = routingOf(*s);
        if (r.device == "#synctone") r.device = "the sync test tone";
        else if (s->kind == SourceKind::Media) {}   // already the file
        else if (!r.device.empty()) {   // the device's name, not its id
            const std::wstring n = audio::deviceName(widen(r.device));
            r.device = n.empty() ? std::string("a device that isn't connected") : narrow(n);
        }
        if (s->kind == SourceKind::InputAudio && (r.device.empty() || r.device == "default")) r.device = "Windows' default microphone";
        const std::wstring route = widen(routing::describeRoute(r));
        gfx_.text(widen(s->name), {x, y, w, 20}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
        y += 22;
        y += gfx_.text(route, {x, y, w, 60}, Font::Small, pal_.inkMuted) + 10;
    }
    if (warnings_.empty()) {
        y += gfx_.text(L"No problems found: nothing kept off the stream reaches it, nothing is doubled, and nothing feeds back.", {x, y, w, 40},
                       Font::Small, cjui::hexColor("#3FCF8E")) + 6;
    } else {
        for (auto& wn : warnings_) y += gfx_.text(L"⚠ " + widen(wn.message), {x, y, w, 80}, Font::Small, cjui::hexColor("#E0566A")) + 8;
    }
    return y - y0;
}

}  // namespace ncap
