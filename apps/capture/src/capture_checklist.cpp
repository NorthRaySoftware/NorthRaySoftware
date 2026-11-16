// NorthRay Capture's window: the pre-live checklist (Settings › Streaming › Pre-live checklist). When it's
// on, Start streaming opens this first: what Capture can check for itself (core/checklist, from the
// window's state, redrawn with the meters so the mic check turns green when you speak) and your own list
// to tick. "Go live" starts streaming whatever it says; it only names problems.
#include "capture_window.hpp"

#include "log.hpp"
#include "output/credentials.hpp"

#include "ncap/checklist.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace ncap {

using cjui::Align;
using cjui::Color;
using cjui::Font;
using cjui::narrow;
using cjui::Rect;
using cjui::widen;
using nlohmann::json;

std::wstring streamDestName(const Settings& s, std::size_t i);   // capture_stream.cpp

namespace {
double nowSeconds() {
    static LARGE_INTEGER f = [] {
        LARGE_INTEGER x;
        QueryPerformanceFrequency(&x);
        return x;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
}  // namespace

void CaptureWindow::openChecklist() {
    openSettings(true);
    checklistOpen_ = true;
    checkTicks_.assign(settings_.checklistItems.size(), false);
    log::info("Pre-live checklist opened");
    invalidate();
}

void CaptureWindow::drawChecklist(const Rect& area) {
    gfx_.fill(area, pal_.paper);
    const float colW = std::min(680.0f, area.w - 48);
    const float x = area.x + (area.w - colW) / 2;
    const float top = beginScroll(area);
    float y = top + 24;
    gfx_.text(L"Before you go live", {x, y, colW - 130, 34}, Font::Title, pal_.ink, {Align::Left, false});
    button("chk:close", {x + colW - 110, y + 2, 110, 34}, L"Not yet", [this] { openSettings(false); });
    y += 52;

    // What Capture can see.
    checklist::Snapshot s;
    for (std::size_t i = 0; i < settings_.destinations.size(); ++i) {
        const auto& d = settings_.destinations[i];
        if (!d.enabled || streamer_.active(d.id)) continue;
        checklist::Destination cd{narrow(streamDestName(settings_, i)), secrets::exists("stream/" + d.id), d.bus};
        cd.hasServer = !d.url.empty();
        s.destinations.push_back(cd);
    }
    if (const Scene* sc = scenes_.scene(scenes_.current())) {
        s.scene = sc->name;
        s.sceneEmpty = sc->items.empty();
    }
    const double now = nowSeconds();
    for (const Source& src : scenes_.sources()) {
        if (src.kind != SourceKind::InputAudio) continue;
        {   // Capture's own sounds (the sync test tone, "#synctone") aren't microphones.
            const json j = json::parse(src.settings, nullptr, false);
            if (j.is_object() && j.contains("device") && j["device"].is_string() && j["device"].get<std::string>().rfind("#", 0) == 0) continue;
        }
        checklist::Mic m;
        m.name = src.name;
        m.muted = audioMuted(src);
        m.buses = audioBuses(src);
        auto it = audio_.find(src.id);
        m.working = it != audio_.end() && it->second.capture && !it->second.capture->problem();
        if (it != audio_.end() && it->second.heardAt >= 0) m.silentFor = now - it->second.heardAt;
        s.mics.push_back(m);
    }
    s.encodersKnown = encodersKnown_;
    s.hardwareEncoder = std::any_of(encoders_.begin(), encoders_.end(), [](const sys::Encoder& e) { return e.hardware && e.codec == "H.264"; });
    s.recording = recorder_.active();
    const auto checks = checklist::run(s);

    gfx_.text(L"CAPTURE CHECKED", {x, y, colW, 18}, Font::Small, pal_.inkFaint, {Align::Left, false, true});
    y += 26;
    const Color ok = cjui::hexColor("#3FCF8E"), warn = cjui::hexColor("#E8C547"), bad = cjui::hexColor("#E0566A");
    for (auto& c : checks) {
        const Color dot = c.state == checklist::State::Ok ? ok : c.state == checklist::State::Warn ? warn : c.state == checklist::State::Problem ? bad
                                                                                                                                                : pal_.inkFaint;
        if (c.state == checklist::State::Waiting) gfx_.circle(x + 7, y + 11, 5, dot, false);
        else gfx_.circle(x + 7, y + 11, 5, dot, true);
        y += std::max(22.0f, gfx_.text(widen(c.text), {x + 24, y, colW - 24, 80}, Font::Body, pal_.ink, {Align::Left, true})) + 8;
    }
    y += 16;

    // Your own list.
    gfx_.text(L"YOUR LIST", {x, y, colW - 160, 18}, Font::Small, pal_.inkFaint, {Align::Left, false, true});
    button("chk:add", {x + colW - 150, y - 8, 150, 30}, L"Add an item…", [this] {
        askText(L"Something to check before you go live", L"", [this](const std::wstring& v) {
            auto items = settings_.checklistItems;
            items.push_back(narrow(v));
            items = checklist::tidy(items);
            if (items.size() == settings_.checklistItems.size()) {
                flash(v.empty() ? L"Type something to add." : L"That's already on the list (or the list is full at 12).", 4);
                return;
            }
            settings_.checklistItems = items;
            settings_.save(settingsPath_);
            checkTicks_.push_back(false);
            invalidate();
        });
    });
    y += 30;
    if (checkTicks_.size() != settings_.checklistItems.size()) checkTicks_.resize(settings_.checklistItems.size(), false);
    if (settings_.checklistItems.empty()) {
        y += gfx_.text(L"Nothing on your list. Add things you like to check, such as phone on silent.", {x, y, colW, 40}, Font::Small, pal_.inkMuted) + 8;
    }
    int ticked = 0;
    for (std::size_t i = 0; i < settings_.checklistItems.size(); ++i) {
        const bool on = checkTicks_[i];
        ticked += on;
        const Rect row{x, y, colW - 44, 34};
        const std::string id = "chk:item:" + std::to_string(i);
        addHit(id, row, [this, i] {
            if (i < checkTicks_.size()) checkTicks_[i] = !checkTicks_[i];
            invalidate();
        });
        const float hv = hoverAmount(id);
        gfx_.fillRounded(row, 6, pal_.ink.withAlpha(0.04f + 0.05f * hv));
        const Rect box{x + 10, y + 9, 16, 16};
        gfx_.strokeRounded(box, 4, on ? ok : pal_.inkMuted, 1.5f);
        if (on) {
            gfx_.fillRounded(box, 4, ok);
            gfx_.line(box.x + 4, box.y + 8.5f, box.x + 7, box.y + 11.5f, pal_.paper, 2);
            gfx_.line(box.x + 7, box.y + 11.5f, box.x + 12.5f, box.y + 5, pal_.paper, 2);
        }
        gfx_.text(widen(settings_.checklistItems[i]), {x + 36, y, row.w - 44, 34}, Font::Body, on ? pal_.inkMuted : pal_.ink, {Align::Left, false, true, true});
        button("chk:del:" + std::to_string(i), {x + colW - 36, y + 2, 36, 30}, L"×", [this, i] {
            auto& v = settings_.checklistItems;
            if (i >= v.size()) return;
            v.erase(v.begin() + (long)i);
            if (i < checkTicks_.size()) checkTicks_.erase(checkTicks_.begin() + (long)i);
            settings_.save(settingsPath_);
            invalidate();
        });
        y += 40;
    }
    y += 12;

    // Going live.
    const bool problems = checklist::anyProblem(checks);
    const bool allTicked = ticked == (int)settings_.checklistItems.size();
    std::wstring note;
    if (problems) note = L"Some checks aren't right. You can still go live.";
    else if (!allTicked) note = std::to_wstring((int)settings_.checklistItems.size() - ticked) + L" left on your list.";
    else note = L"All set.";
    gfx_.text(note, {x, y + 8, colW - 220, 36}, Font::Body, problems ? warn : allTicked ? ok : pal_.inkMuted, {Align::Left, false, true});
    button("chk:go", {x + colW - 200, y + 8, 200, 36}, problems || !allTicked ? L"Go live anyway" : L"Go live", [this, problems, allTicked] {
        log::info(std::string("Going live from the pre-live checklist") + (problems ? " (with warnings)" : "") + (allTicked ? "" : " (list not all ticked)"));
        openSettings(false);
        startStreaming();
    }, true);
    y += 64;
    gfx_.text(L"Turn this off in Settings › Streaming › Pre-live checklist.", {x, y, colW, 20}, Font::Small, pal_.inkFaint, {Align::Left, false, true});
    y += 28;
    endScroll(y - top + 24);
}

}  // namespace ncap
