// NorthRay Capture's window: the stream's extras (0.11).
//
// - The plan (Settings › Streaming › Your stream): when you mean to go live, how long for, and the next stream.
//   Design timers count down to it ("Starting soon"), show the time left, and show the next stream on an end
//   screen. Optionally Start streaming happens by itself at that time (only within two minutes of it, so opening
//   Capture hours later never starts a stream).
// - Recording alongside: a recording runs whenever you stream, stopped with it (not one you started yourself).
// - Chapters: each scene change is a chapter in the recording (in the MP4 copy).
// - Muting by scene: a scene can mute chosen sound sources while it's live (music under a talking scene). The
//   source's own mute setting is untouched; the mix just leaves it out while that scene shows.
// - Be right back: switches to the scene chosen for it and mutes your microphones; pressed again, it goes back.
// - The stream report: collected while streaming, saved as JSON in the Reports folder (no keys, no addresses),
//   shown when the stream ends, and the earlier ones from Settings › Streaming.
#include "capture_window.hpp"

#include "log.hpp"
#include "video/design_render.hpp"

#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace ncap {

using cjui::Align;
using cjui::Color;
using cjui::Font;
using cjui::MenuItem;
using cjui::narrow;
using cjui::Rect;
using cjui::widen;

std::wstring streamDestName(const Settings& s, std::size_t i);   // capture_stream.cpp

namespace {
constexpr std::size_t kKeepReports = 100;

live::Stamp nowStamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return {st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute};
}
std::int64_t nowCivilMs() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return live::civilMs(st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}
// The next whole hour, as a starting point for typing a time.
std::string suggestedStamp() {
    live::Stamp s = live::addMinutes(nowStamp(), 60);
    s.minute = 0;
    std::string t = live::stampText(s);
    t[10] = ' ';
    return t;
}
std::wstring clockText(double seconds) {
    const long long s = std::max(0LL, (long long)seconds);
    wchar_t b[32];
    swprintf(b, 32, L"%lld:%02lld:%02lld", s / 3600, s / 60 % 60, s % 60);
    return b;
}
}  // namespace

std::string CaptureWindow::liveSceneName() {
    const Scene* sc = scenes_.scene(programScene());
    return sc ? sc->name : std::string();
}

void CaptureWindow::applyStreamPlan() {
    live::Stamp at, next;
    const bool hasAt = live::parseStamp(settings_.goLiveAt, at), hasNext = live::parseStamp(settings_.nextStreamAt, next);
    video::setStreamPlan(hasAt ? live::stampMs(at) : -1, settings_.plannedMinutes > 0 ? settings_.plannedMinutes * 60000LL : -1,
                         hasNext ? live::stampWords(next) : std::string());
}

// --- the live scene: chapters, the report, mutes ---

void CaptureWindow::liveSceneCheck() {
    const Id live = programScene();
    std::set<Id> muted;
    if (const Scene* sc = scenes_.scene(live)) muted.insert(sc->mutes.begin(), sc->mutes.end());
    if (brbOn())
        for (auto& s : scenes_.sources())
            if (s.kind == SourceKind::InputAudio) muted.insert(s.id);
    if (muted != autoMuted_) {
        autoMuted_ = std::move(muted);
        syncAudio();
        invalidate();
    }
    if (live == liveSceneSeen_) return;
    const bool first = liveSceneSeen_.empty();
    liveSceneSeen_ = live;
    if (first) return;
    const std::string name = liveSceneName();
    if (report_.running()) report_.scene((double)(GetTickCount64() - streamStartTick_) / 1000.0, name);
    if (settings_.sceneChapters && recorder_.active() && !name.empty() && markers_.size() < 2000) {
        const double s = recorder_.status().seconds;
        markers_.push_back({(std::int64_t)(s * 1e6), name});
    }
    // Leaving the Be right back scene some other way ends it.
    if (brbOn() && scenes_.scene(live) && scenes_.scene(live)->name != settings_.brbScene) {
        brbFrom_.clear();
        liveSceneCheck();
    }
}

// --- starting and stopping ---

void CaptureWindow::liveStateCheck() {
    const bool on = streamer_.active();
    if (on == wasStreaming_) return;
    wasStreaming_ = on;
    if (on) {
        streamStartTick_ = GetTickCount64();
        report_.begin(nowStamp(), liveSceneName());
        if (settings_.recordWhileStreaming && !recorder_.active()) {
            startRecording();
            autoRecording_ = recorder_.active();
            if (autoRecording_) log::info("Recording alongside the stream");
        }
        if (recorder_.active()) report_.setRecorded();
        return;
    }
    // Stopped: the report, and the recording that came with it.
    if (autoRecording_ && recorder_.active()) stopRecording();
    autoRecording_ = false;
    if (brbOn()) {   // nobody's watching any more: back to normal
        brbFrom_.clear();
        liveSceneCheck();
    }
    if (!report_.running()) return;
    const live::Report r = report_.finish((double)(GetTickCount64() - streamStartTick_) / 1000.0);
    if (r.seconds < 10) return;   // a false start isn't worth a report
    std::error_code ec;
    std::filesystem::create_directories(reportsFolder(), ec);
    live::Stamp st;
    live::parseStamp(r.started, st);
    wchar_t name[64];
    swprintf(name, 64, L"%04d-%02d-%02d %02d.%02d.json", st.year, st.month, st.day, st.hour, st.minute);
    std::wstring path = reportsFolder() + L"\\" + name;
    for (int n = 2; std::filesystem::exists(path, ec) && n < 100; ++n) {
        swprintf(name, 64, L"%04d-%02d-%02d %02d.%02d (%d).json", st.year, st.month, st.day, st.hour, st.minute, n);
        path = reportsFolder() + L"\\" + name;
    }
    {
        std::ofstream f(std::filesystem::path(path), std::ios::binary);
        f << live::toJson(r);
        if (!f) {
            log::warn("The stream report couldn't be saved");
            return;
        }
    }
    log::info("Stream report saved: " + live::durationWords(r.seconds) + ", " + std::to_string(r.dropped) + " frames dropped");
    // Only the newest are kept.
    const auto files = reportFiles();
    for (std::size_t i = kKeepReports; i < files.size(); ++i) std::filesystem::remove(files[i], ec);
    openReport(0);
}

void CaptureWindow::liveTick() {
    applyStreamPlan();
    liveStateCheck();
    liveSceneCheck();
    if (report_.running()) {
        double kbps = 0;
        std::uint64_t dropped = 0;
        for (auto& id : streamer_.ids()) {
            if (!streamer_.active(id)) continue;
            const output::DestinationStatus st = streamer_.status(id);
            kbps += st.kbps;
            dropped += st.droppedFrames;
        }
        for (auto& e : streamer_.encodes()) dropped += e.encoderDropped;
        report_.sample((double)(GetTickCount64() - streamStartTick_) / 1000.0, kbps, dropped);
        if (recorder_.active()) report_.setRecorded();
    }
    // Going live by itself at the time set.
    live::Stamp at;
    if (settings_.goLiveStarts && live::parseStamp(settings_.goLiveAt, at) && settings_.goLiveDone != settings_.goLiveAt) {
        const std::int64_t late = nowCivilMs() - live::stampMs(at);
        if (late >= 0) {
            settings_.goLiveDone = settings_.goLiveAt;
            settings_.save(settingsPath_);
            if (late <= 120000 && !streamer_.active()) {
                if (!settings_.goLiveScene.empty())
                    for (auto& sc : scenes_.scenes())
                        if (sc.name == settings_.goLiveScene) {
                            if (settings_.studioMode) programScene_ = sc.id;
                            else scenes_.setCurrent(sc.id);
                            scenesChanged();
                        }
                log::info("Going live at the time set");
                flash(L"It's " + widen(live::stampWords(at)) + L": going live.", 6);
                startStreaming();
                liveStateCheck();
            }
        }
    }
}

// --- Be right back ---

void CaptureWindow::toggleBrb() {
    if (brbOn()) {
        const Id back = brbFrom_;
        brbFrom_.clear();
        if (scenes_.scene(back)) {
            if (settings_.studioMode) programScene_ = back;
            else scenes_.setCurrent(back);
        }
        scenesChanged();
        flash(L"Welcome back: your mic is on again.", 3);
        log::info("Be right back: ended");
        return;
    }
    const Scene* target = nullptr;
    for (auto& sc : scenes_.scenes())
        if (sc.name == settings_.brbScene) target = &sc;
    if (!target) {
        // No scene chosen yet (or it was renamed): pick one now.
        std::vector<MenuItem> items;
        const auto& all = scenes_.scenes();
        for (std::size_t i = 0; i < all.size() && i < 200; ++i) items.push_back(MenuItem::item(widen(all[i].name), 100 + (UINT)i));
        if (items.empty()) return;
        POINT pt;
        GetCursorPos(&pt);
        std::vector<MenuItem> menu = {MenuItem::item(L"Which scene is your Be right back scene?", 0, {}, false), MenuItem::sep()};
        menu.insert(menu.end(), items.begin(), items.end());
        const UINT cmd = cjui::showMenu(hwnd(), menu, pt, pal_);
        if (cmd < 100 || cmd >= 100 + all.size()) return;
        settings_.brbScene = all[cmd - 100].name;
        settings_.save(settingsPath_);
        target = &scenes_.scenes()[cmd - 100];
    }
    const Id from = programScene();
    if (from == target->id) {
        flash(L"That scene is already live. Be right back switches to it and mutes your mic, then switches back.", 5);
        return;
    }
    brbFrom_ = from;
    const Id to = target->id;
    if (settings_.studioMode) programScene_ = to;
    else scenes_.setCurrent(to);
    scenesChanged();
    flash(L"Be right back: " + widen(settings_.brbScene) + L" is live and your mic is muted. Press it again to come back.", 4);
    log::info("Be right back: on");
}

float CaptureWindow::drawBrbButton(float x, float y, float w) {
    const bool on = brbOn();
    const Rect r{x, y, w, 32};
    addHit("ctl:brb", r, [this] { toggleBrb(); });
    tooltip("ctl:brb", withHotkey(on ? L"Back: return to the scene you were on and unmute your mic."
                                     : L"Switch to your Be right back scene and mute your mic (Settings › Streaming › Your stream).",
                                  hotkeys::Action::Brb));
    const float hv = hoverAmount("ctl:brb");
    const Color amber = cjui::hexColor("#E8C547");
    gfx_.fillRounded(r, 8, on ? amber.withAlpha(0.22f + 0.1f * hv) : pal_.ink.withAlpha((pal_.dark ? 0.07f : 0.05f) + 0.05f * hv));
    gfx_.strokeRounded(r, 8, on ? amber : pal_.rule.mix(pal_.ink, 0.3f * hv), 1);
    // A coffee cup.
    const float cx = r.x + 20, cy = r.y + r.h / 2;
    const Color ic = on ? amber : pal_.inkMuted.mix(pal_.ink, hv);
    gfx_.strokeRounded({cx - 6, cy - 4, 10, 9}, 2, ic, 1.4f);
    gfx_.circle(cx + 5.5f, cy, 2.5f, ic, false, 1.2f);
    gfx_.line(cx - 3, cy - 9, cx - 3, cy - 6, ic, 1.2f);
    gfx_.line(cx + 1, cy - 9, cx + 1, cy - 6, ic, 1.2f);
    gfx_.text(on ? L"Back from the break" : L"Be right back", {r.x + 36, r.y, r.w - 44, r.h}, Font::Body, pal_.ink, {Align::Left, false, true, true});
    if (focused("ctl:brb")) focusRing(r);
    return y + r.h + 6;
}

// --- Settings › Streaming › Your stream ---

float CaptureWindow::drawLiveSettings(float x, float y0, float colW) {
    float y = y0;
    y += sectionLabel(x, y, colW, L"Your stream");
    auto row = [&](const std::string& id, const std::wstring& title, const std::wstring& note, bool on, std::function<void()> flip) {
        gfx_.text(title, {x, y, colW - 80, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
        const float h = 24 + gfx_.text(note, {x, y + 24, colW - 80, 80}, Font::Small, pal_.inkMuted);
        toggle(id, x + colW - 52, y + 4, on, std::move(flip));
        y += std::max(h, 34.0f) + 12;
    };
    auto sceneMenu = [this](std::string& target, bool allowNone) {
        std::vector<MenuItem> items;
        if (allowNone) items.push_back(MenuItem::item(L"Stay on the scene that's live", 99, {}, true, target.empty()));
        const auto& all = scenes_.scenes();
        for (std::size_t i = 0; i < all.size() && i < 200; ++i) items.push_back(MenuItem::item(widen(all[i].name), 100 + (UINT)i, {}, true, all[i].name == target));
        POINT pt;
        GetCursorPos(&pt);
        const UINT cmd = cjui::showMenu(hwnd(), items, pt, pal_);
        if (cmd == 99) target.clear();
        else if (cmd >= 100 && cmd < 100 + all.size()) target = all[cmd - 100].name;
        else return;
        settings_.save(settingsPath_);
    };
    auto askStamp = [this](const std::wstring& title, std::string& target) {
        std::string initial = target.empty() ? suggestedStamp() : target;
        if (initial.size() > 10 && initial[10] == 'T') initial[10] = ' ';
        askText(title, widen(initial), [this, &target](const std::wstring& text) {
            live::Stamp s;
            if (text.find_first_not_of(L' ') == std::wstring::npos) {
                target.clear();
            } else if (!live::parseStamp(narrow(text), s)) {
                flash(L"That isn't a date and time Capture understands. Type it like 2026-11-22 19:00.", 6);
                return;
            } else {
                target = live::stampText(s);
            }
            settings_.save(settingsPath_);
            applyStreamPlan();
            invalidate();
        });
    };

    // Going live at a set time.
    {
        live::Stamp at;
        const bool has = live::parseStamp(settings_.goLiveAt, at);
        gfx_.text(L"Going live at", {x, y, colW - 300, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
        std::wstring note;
        if (has) {
            const std::int64_t in = live::stampMs(at) - nowCivilMs();
            note = widen(live::stampWords(at)) + (in > 0 ? L", in " + widen(live::durationWords((double)in / 1000.0)) : L", which has passed") +
                   L". Timers set to “Until going live” count down to it: put one on your Starting soon scene.";
        } else {
            note = L"Not set. Set a time and timers set to “Until going live” count down to it, for a Starting soon scene.";
        }
        const float h = 24 + gfx_.text(note, {x, y + 24, colW - 300, 80}, Font::Small, pal_.inkMuted);
        button("live:at", {x + colW - 190, y, 110, 30}, has ? L"Change…" : L"Set a time…",
               [this, askStamp] { askStamp(L"Going live at (year-month-day hour:minute)", settings_.goLiveAt); });
        if (has)
            button("live:atclear", {x + colW - 72, y, 72, 30}, L"Clear", [this] {
                settings_.goLiveAt.clear();
                settings_.save(settingsPath_);
                applyStreamPlan();
            });
        y += std::max(h, 34.0f) + 12;
    }
    if (live::Stamp at; live::parseStamp(settings_.goLiveAt, at)) {
        row("live:autostart", L"Start streaming at that time",
            L"Capture goes live by itself, if it's open then (and only within two minutes of it). It switches to the scene below first.",
            settings_.goLiveStarts, [this] {
                settings_.goLiveStarts = !settings_.goLiveStarts;
                settings_.save(settingsPath_);
            });
        if (settings_.goLiveStarts) {
            gfx_.text(L"Scene to go live with", {x + 16, y + 4, colW - 300, 22}, Font::Body, pal_.ink, {Align::Left, false, true});
            button("live:atscene", {x + colW - 260, y, 260, 30}, settings_.goLiveScene.empty() ? L"The scene that's live" : widen(settings_.goLiveScene),
                   [this, sceneMenu] { sceneMenu(settings_.goLiveScene, true); });
            y += 44;
        }
    }
    // How long it's meant to be.
    {
        gfx_.text(L"How long the stream is", {x, y, colW - 350, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
        const float h = 24 + gfx_.text(L"For timers set to “Time left”: they count down from this while you're live.",
                                       {x, y + 24, colW - 350, 80}, Font::Small, pal_.inkMuted);
        const int v[] = {0, 60, 120, 180, 240};
        int sel = -1;
        for (int i = 0; i < 5; ++i)
            if (settings_.plannedMinutes == v[i]) sel = i;
        segmented("live:planned", {x + colW - 330, y - 2, 330, 32}, {L"Not set", L"1 h", L"2 h", L"3 h", L"4 h"}, sel, [this](int i) {
            const int vv[] = {0, 60, 120, 180, 240};
            settings_.plannedMinutes = vv[i];
            settings_.save(settingsPath_);
            applyStreamPlan();
        });
        y += std::max(h, 34.0f) + 12;
    }
    // The next stream, for end screens.
    {
        live::Stamp next;
        const bool has = live::parseStamp(settings_.nextStreamAt, next);
        gfx_.text(L"Next stream", {x, y, colW - 300, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
        const float h = 24 + gfx_.text(has ? widen(live::stampWords(next)) + L". Timers set to “Next stream” show it: put one on your ending scene."
                                           : L"Not set. Timers set to “Next stream” show “To be announced” until it is.",
                                       {x, y + 24, colW - 300, 80}, Font::Small, pal_.inkMuted);
        button("live:next", {x + colW - 190, y, 110, 30}, has ? L"Change…" : L"Set a time…",
               [this, askStamp] { askStamp(L"Next stream (year-month-day hour:minute)", settings_.nextStreamAt); });
        if (has)
            button("live:nextclear", {x + colW - 72, y, 72, 30}, L"Clear", [this] {
                settings_.nextStreamAt.clear();
                settings_.save(settingsPath_);
                applyStreamPlan();
            });
        y += std::max(h, 34.0f) + 12;
    }
    // Be right back.
    {
        gfx_.text(L"Be right back scene", {x, y, colW - 300, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
        const float h = 24 + gfx_.text(L"The Be right back button (under Start streaming, or a hotkey) switches to this scene and mutes your "
                                       L"microphones; pressed again, it goes back to where you were.",
                                       {x, y + 24, colW - 300, 80}, Font::Small, pal_.inkMuted);
        button("live:brb", {x + colW - 260, y, 260, 30}, settings_.brbScene.empty() ? L"Choose a scene…" : widen(settings_.brbScene),
               [this, sceneMenu] { sceneMenu(settings_.brbScene, false); });
        y += std::max(h, 34.0f) + 12;
    }
    row("live:record", L"Record while streaming",
        L"A recording starts with the stream and stops with it (a recording you started yourself keeps going).", settings_.recordWhileStreaming, [this] {
            settings_.recordWhileStreaming = !settings_.recordWhileStreaming;
            settings_.save(settingsPath_);
        });
    row("live:chapters", L"Chapters at scene changes",
        L"Each time the live scene changes, the recording gets a chapter named after it (in the MP4 copy, so editors and players can jump "
        L"to it).",
        settings_.sceneChapters, [this] {
            settings_.sceneChapters = !settings_.sceneChapters;
            settings_.save(settingsPath_);
        });
    // Reports.
    {
        const auto files = reportFiles();
        gfx_.text(L"Stream reports", {x, y, colW - 200, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
        const float h = 24 + gfx_.text(files.empty() ? std::wstring(L"After each stream: how long it was, the upload rate, dropped frames and how long each "
                                                                    L"scene was live. None yet.")
                                                     : L"After each stream: how long it was, the upload rate, dropped frames and how long each scene was "
                                                       L"live. " + std::to_wstring(files.size()) + (files.size() == 1 ? L" kept." : L" kept (the newest 100)."),
                                       {x, y + 24, colW - 200, 80}, Font::Small, pal_.inkMuted);
        if (!files.empty()) button("live:reports", {x + colW - 170, y, 170, 30}, L"See the last one", [this] { openReport(0); });
        y += std::max(h, 34.0f) + 12;
    }
    return y - y0;
}

// --- the report page ---

std::wstring CaptureWindow::reportsFolder() const { return stateFolder_ + L"\\Reports"; }

std::vector<std::wstring> CaptureWindow::reportFiles() const {
    std::vector<std::wstring> out;
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(reportsFolder(), ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec) && it->path().extension() == L".json") out.push_back(it->path().wstring());
    std::sort(out.begin(), out.end(), std::greater<>());   // the names start with the date and time
    return out;
}

void CaptureWindow::openReport(int index) {
    const auto files = reportFiles();
    if (files.empty()) return;
    index = std::clamp(index, 0, (int)files.size() - 1);
    std::ifstream f(std::filesystem::path(files[(std::size_t)index]), std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    live::Report r;
    if (!live::fromJson(ss.str(), r)) {
        flash(L"That stream report couldn't be read.", 4);
        return;
    }
    shownReport_ = std::move(r);
    shownReportIndex_ = index;
    openSettings(true);
    reportOpen_ = true;
    invalidate();
}

void CaptureWindow::drawReport(const Rect& area) {
    gfx_.fill(area, pal_.paper);
    const live::Report& r = shownReport_;
    const float colW = std::min(760.0f, area.w - 48);
    const float x = area.x + (area.w - colW) / 2;
    const float top = beginScroll(area);
    float y = top + 24;
    gfx_.text(L"Stream report", {x, y, colW - 330, 34}, Font::Title, pal_.ink, {Align::Left, false});
    const int count = (int)reportFiles().size();
    if (shownReportIndex_ + 1 < count) button("rep:older", {x + colW - 320, y + 2, 100, 34}, L"Older", [this] { openReport(shownReportIndex_ + 1); });
    if (shownReportIndex_ > 0) button("rep:newer", {x + colW - 212, y + 2, 94, 34}, L"Newer", [this] { openReport(shownReportIndex_ - 1); });
    button("rep:close", {x + colW - 110, y + 2, 110, 34}, L"Done", [this] { openSettings(false); });
    y += 44;
    live::Stamp st;
    live::parseStamp(r.started, st);
    y += gfx_.text(L"Started " + widen(live::stampWords(st)) + (r.recorded ? L" · recorded" : L""), {x, y, colW, 22}, Font::Body, pal_.inkMuted) + 18;

    // The headline figures.
    double avg = 0;
    for (float v : r.kbps) avg += v;
    if (!r.kbps.empty()) avg /= (double)r.kbps.size();
    const double frames = r.seconds * settings_.outputFps;
    wchar_t pct[48] = L"";
    if (frames > 0 && r.dropped) swprintf(pct, 48, L" (%.2g%%)", 100.0 * (double)r.dropped / frames);
    wchar_t rate[48];
    swprintf(rate, 48, L"%.1f Mbit/s", avg / 1000.0);
    const std::pair<std::wstring, std::wstring> tiles[] = {
        {L"Length", widen(live::durationWords(r.seconds))},
        {L"Average upload", r.kbps.empty() ? std::wstring(L"–") : std::wstring(rate)},
        {L"Frames dropped", std::to_wstring(r.dropped) + pct},
    };
    const float tw = (colW - 24) / 3;
    for (int i = 0; i < 3; ++i) {
        const Rect t{x + i * (tw + 12), y, tw, 76};
        gfx_.fillRounded(t, 10, pal_.ink.withAlpha(pal_.dark ? 0.06f : 0.04f));
        gfx_.text(tiles[i].first, {t.x + 14, t.y + 10, t.w - 28, 20}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
        gfx_.text(tiles[i].second, {t.x + 14, t.y + 32, t.w - 28, 34}, Font::Title, i == 2 && r.dropped ? cjui::hexColor("#E8C547") : pal_.ink,
                  {Align::Left, false, true});
    }
    y += 76 + 24;

    // The upload rate over the stream, with the scene changes marked along the bottom.
    y += sectionLabel(x, y, colW, L"Upload rate");
    {
        const Rect g{x, y, colW, 170};
        gfx_.fillRounded(g, 10, pal_.ink.withAlpha(pal_.dark ? 0.05f : 0.03f));
        const Rect plot{g.x + 56, g.y + 14, g.w - 72, g.h - 40};
        float top = 0;
        for (float v : r.kbps) top = std::max(top, v);
        top = std::max(1000.0f, std::ceil(top / 1000.0f) * 1000.0f);
        for (int i = 0; i <= 2; ++i) {
            const float yy = plot.bottom() - plot.h * (float)i / 2;
            gfx_.line(plot.x, yy + 0.5f, plot.right(), yy + 0.5f, pal_.rule.withAlpha(0.5f));
            wchar_t lab[24];
            swprintf(lab, 24, L"%.3g M", top / 1000.0f * (float)i / 2);
            gfx_.text(lab, {g.x + 6, yy - 9, 46, 18}, Font::Small, pal_.inkMuted, {Align::Right, false, false, true});
        }
        const double total = std::max(1.0, r.seconds);
        const Color purple = cjui::hexColor("#7A5CFF");
        for (auto& [t, name] : r.changes) {
            if (t <= 0) continue;
            const float xx = plot.x + plot.w * (float)(t / total);
            gfx_.line(xx, plot.y, xx, plot.bottom(), pal_.inkFaint.withAlpha(0.5f));
        }
        if (r.kbps.size() >= 2) {
            float px = 0, py = 0;
            for (std::size_t i = 0; i < r.kbps.size(); ++i) {
                const float xx = plot.x + plot.w * (float)i / (float)(r.kbps.size() - 1);
                const float yy = plot.bottom() - plot.h * std::clamp(r.kbps[i] / top, 0.0f, 1.0f);
                if (i) gfx_.line(px, py, xx, yy, purple, 1.8f);
                px = xx;
                py = yy;
            }
        } else {
            gfx_.text(L"Too short for a graph.", plot, Font::Small, pal_.inkMuted, {Align::Center, false, false, true});
        }
        gfx_.text(L"0:00", {plot.x, plot.bottom() + 4, 80, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
        gfx_.text(clockText(r.seconds), {plot.right() - 80, plot.bottom() + 4, 80, 18}, Font::Small, pal_.inkMuted, {Align::Right, false});
        y += g.h + 24;
    }

    // Scenes.
    if (!r.scenes.empty()) {
        y += sectionLabel(x, y, colW, L"Scenes");
        auto scenes = r.scenes;
        std::stable_sort(scenes.begin(), scenes.end(), [](auto& a, auto& b) { return a.second > b.second; });
        const double total = std::max(1.0, r.seconds);
        for (std::size_t i = 0; i < scenes.size() && i < 40; ++i) {
            const auto& [name, secs] = scenes[i];
            gfx_.text(widen(name), {x, y, colW * 0.4f, 24}, Font::Body, pal_.ink, {Align::Left, false, true, true});
            const Rect bar{x + colW * 0.4f, y + 8, colW * 0.4f, 8};
            gfx_.fillRounded(bar, 4, pal_.ink.withAlpha(0.07f));
            gfx_.fillRounded({bar.x, bar.y, std::max(4.0f, bar.w * (float)std::min(1.0, secs / total)), bar.h}, 4, cjui::hexColor("#7A5CFF"));
            gfx_.text(widen(live::durationWords(secs)), {x + colW * 0.8f, y, colW * 0.2f, 24}, Font::Small, pal_.inkMuted, {Align::Right, false, false, true});
            y += 28;
        }
        y += 16;
    }
    // Destinations.
    if (!r.destinations.empty()) {
        y += sectionLabel(x, y, colW, L"Destinations");
        for (auto& d : r.destinations) {
            wchar_t t[160];
            swprintf(t, 160, L"%ls live · %.1f Mbit/s on average · %llu dropped · %d reconnection%ls", widen(live::durationWords(d.seconds)).c_str(),
                     d.averageKbps / 1000.0, (unsigned long long)d.dropped, d.reconnects, d.reconnects == 1 ? L"" : L"s");
            gfx_.text(widen(d.name), {x, y, colW * 0.3f, 24}, Font::BodyStrong, pal_.ink, {Align::Left, false, true, true});
            gfx_.text(t, {x + colW * 0.3f, y, colW * 0.7f, 24}, Font::Small, pal_.inkMuted, {Align::Right, false, true, true});
            y += 30;
        }
        y += 16;
    }
    y += gfx_.text(L"Reports are kept in " + reportsFolder() + L". They hold no stream keys or addresses.", {x, y, colW, 40}, Font::Small, pal_.inkFaint) + 24;
    endScroll(y - top);
}

}  // namespace ncap
