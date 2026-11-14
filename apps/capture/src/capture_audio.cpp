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
    CmdARename = 100, CmdAMute, CmdAClip, CmdARemove, CmdADevice0 = 200,
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
    return get("device") + "|" + get("exe");
}

}  // namespace

// ---------------------------------------------------------------- captures and the mixer

void CaptureWindow::startAudio() {
    mixer_.start();
    syncAudio();
}

float CaptureWindow::audioGain(const Source& s) const {
    const json j = settingsOf(s);
    return j.contains("volume") && j["volume"].is_number() ? std::clamp(j["volume"].get<float>(), 0.0f, 1.0f) : 1.0f;
}

bool CaptureWindow::audioMuted(const Source& s) const {
    const json j = settingsOf(s);
    return j.contains("muted") && j["muted"].is_boolean() && j["muted"].get<bool>();
}

void CaptureWindow::syncAudio() {
    for (auto it = audio_.begin(); it != audio_.end();) {
        const Source* s = scenes_.source(it->first);
        if (!s || !isAudioOnly(s->kind)) it = audio_.erase(it);   // stops its thread
        else ++it;
    }
    std::vector<audio::MixSource> mix;
    for (const Source& s : scenes_.sources()) {
        if (!isAudioOnly(s.kind)) continue;
        AudioLive& a = audio_[s.id];
        const std::string key = captureKey(s);
        if (!a.capture) a.capture = std::make_shared<audio::Capture>(s.kind);
        if (a.key != key || a.key.empty()) {
            a.key = key.empty() ? "|" : key;
            a.capture->setSettings(s.settings);
        }
        mix.push_back({s.id, a.capture, audioGain(s), audioMuted(s)});
    }
    mixer_.setSources(std::move(mix));
    // Meters run while there's something to measure (slower in low-resource mode).
    const bool want = !audio_.empty();
    if (want != metersRunning_) {
        metersRunning_ = want;
        if (want) SetTimer(hwnd(), kMeterTimer, settings_.lowResource ? 100 : 33, nullptr);
        else KillTimer(hwnd(), kMeterTimer);
    }
}

void CaptureWindow::meterTick() {
    const double now = seconds();
    for (auto& [id, lv] : mixer_.takeLevels()) {
        if (id.empty()) {
            mixLeft_.feed(lv.left, now);
            mixRight_.feed(lv.right, now);
            continue;
        }
        auto it = audio_.find(id);
        if (it == audio_.end()) continue;
        it->second.left.feed(lv.left, now);
        it->second.right.feed(lv.right, now);
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
    if (!settingsOpen_) invalidate();
}

void CaptureWindow::setAudioSetting(const Id& source, const char* key, const json& value) {
    const Source* s = scenes_.source(source);
    if (!s) return;
    json j = settingsOf(*s);
    j[key] = value;
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
    std::vector<MenuItem> menu = {
        MenuItem::item(L"Rename…", CmdARename),
        MenuItem::item(muted ? L"Unmute" : L"Mute", CmdAMute),
    };
    // Which device or program it takes its sound from.
    std::vector<audio::Device> devices;
    std::vector<audio::App> apps;
    const json j = settingsOf(*s);
    const std::string cur = j.contains("device") && j["device"].is_string() ? j["device"].get<std::string>()
                            : j.contains("exe") && j["exe"].is_string() ? j["exe"].get<std::string>() : std::string();
    std::vector<MenuItem> from;
    if (s->kind == SourceKind::ApplicationAudio) {
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
    menu.push_back(MenuItem::sep());
    menu.push_back(MenuItem::item(L"Save a 10-second test clip", CmdAClip));
    menu.push_back(MenuItem::sep());
    menu.push_back(MenuItem::item(L"Remove", CmdARemove));
    const UINT cmd = cjui::showMenu(hwnd(), menu, screen, pal_);
    if (cmd == CmdARename) renameSource(source);
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
        if (isAudioOnly(s.kind)) list.push_back(&s);
    if (list.empty()) {
        note(b, L"No sound sources yet",
             L"Use + above to add your microphone, desktop audio (everything you hear), or one program’s sound on its own "
             L"(Windows 11): for example a game, while music and voice chat stay out. Sound sources are heard in every scene.");
        return;
    }
    float y = b.y + 8;
    // The mix: everything added together, as it will be streamed (routing arrives in Milestone 4).
    gfx_.text(L"Mix", {b.x + 14, y, 60, 16}, Font::SmallStrong, pal_.inkMuted, {Align::Left, false});
    drawMeter({b.x + 52, y + 3, b.w - 66, 10}, mixLeft_, mixRight_, false);
    y += 26;
    gfx_.line(b.x + 10, y - 4, b.right() - 10, y - 4, pal_.rule);
    for (const Source* s : list) {
        const Rect row{b.x + 6, y, b.w - 12, 74};
        if (row.y > b.bottom()) break;
        audioRows_.push_back({s->id, row});
        AudioLive& a = audio_[s->id];
        const bool muted = audioMuted(*s);
        const float gain = audioGain(*s);
        const std::string id = "aud:" + s->id;
        gfx_.text(widen(s->name), {row.x + 8, row.y + 2, row.w - 90, 20}, Font::BodyStrong, muted ? pal_.inkFaint : pal_.ink, {Align::Left, false, true, true});
        gfx_.text(audio::dbText(audio::gainToDb(gain)), {row.x, row.y + 2, row.w - 8, 20}, Font::Small, pal_.inkMuted, {Align::Right, false, false, true});
        const bool problem = a.capture && a.capture->problem();
        const std::wstring st = a.capture ? a.capture->status() : L"";
        gfx_.text(st, {row.x + 8, row.y + 21, row.w - 16, 16}, Font::Small, problem ? cjui::hexColor("#E0566A") : pal_.inkMuted, {Align::Left, false, true, true});
        drawMeter({row.x + 8, row.y + 41, row.w - 16, 10}, a.left, a.right, muted);
        // Mute, then the fader.
        const Rect mute{row.x + 4, row.y + 52, 26, 22};
        const Id sid = s->id;
        addHit(id + ":mute", mute, [this, sid, muted] { setAudioSetting(sid, "muted", !muted); });
        const float hm = hoverAmount(id + ":mute");
        if (hm > 0.01f) gfx_.fillRounded(mute, 5, pal_.ink.withAlpha(0.08f * hm));
        const Color ic = muted ? cjui::hexColor("#E0566A") : pal_.inkMuted.mix(pal_.ink, hm);
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
        y += 80;
        gfx_.line(b.x + 10, y - 4, b.right() - 10, y - 4, pal_.rule.withAlpha(0.6f));
    }
}

}  // namespace ncap
