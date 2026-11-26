// Planning's timeline and story structure.
//
// Timeline: each chapter and scene can be placed on the story's own calendar
// (a number of days, weeks, months or years, plus words such as "Midsummer,
// 1204"). Cards sit in a lane for their viewpoint character; a line joins them
// in reading order, so flashbacks show as the line turning back. Drag a card
// along the line to move it in time. Kept per book as setting "timeline:<book>":
// {"unit": "day", "nodes": {"<node id>": {"day": 3, "when": "Midsummer"}}}.
//
// Structure: a story shape of beats (the built-in ones, or the author's own) laid
// over the book by length, each beat carried by one or more chapters and given
// notes for this book. Plot threads tag chapters and scenes. The data and the
// plain-text formats are studiocore's (studio/planning.hpp); the built-in shapes'
// descriptions were written for NorthRay.
#include "studio_app.hpp"
#include "cjui/dialog.hpp"
#include "cjui/menu.hpp"
#include "studio/planning.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <shobjidl.h>
#include <sstream>

namespace nrs {

using nlohmann::json;
using namespace studio;
using cjui::MenuItem;
using cjui::showMenu;

static std::wstring w(const std::string& s) { return widen(s); }
namespace cal = studio::cal;

// ------------------------------------------------------------- the data

std::map<Id, StudioApp::StoryTime> StudioApp::storyTimes(const Id& bookId) const {
    std::map<Id, StoryTime> out;
    if (!lib_) return out;
    const json j = json::parse(lib_->setting("timeline:" + bookId), nullptr, false);
    if (!j.is_object() || !j.contains("nodes") || !j["nodes"].is_object()) return out;
    for (auto& [id, v] : j["nodes"].items()) {
        if (!v.is_object()) continue;
        StoryTime t;
        if (v.contains("day") && v["day"].is_number()) { t.placed = true; t.day = v["day"].get<double>(); }
        if (v.contains("when") && v["when"].is_string()) t.when = v["when"].get<std::string>();
        if (v.contains("time") && v["time"].is_number_integer()) t.minutes = v["time"].get<int>();
        if (v.contains("part") && v["part"].is_string()) t.part = v["part"].get<std::string>();
        out[id] = t;
    }
    return out;
}

void StudioApp::setStoryTime(const Id& bookId, const Id& nodeId, const StoryTime& t) {
    json j = json::parse(lib_->setting("timeline:" + bookId), nullptr, false);
    if (!j.is_object()) j = json::object();
    if (!j.contains("nodes") || !j["nodes"].is_object()) j["nodes"] = json::object();
    json v = json::object();
    if (t.placed) v["day"] = t.day;
    if (!t.when.empty()) v["when"] = t.when;
    if (t.minutes >= 0) v["time"] = t.minutes;
    else if (!t.part.empty()) v["part"] = t.part;
    // A date from a plan's When line (0.82) stays while the card keeps that day, so a changed calendar reads it again;
    // moving the card makes its day the author's own.
    if (j["nodes"].contains(nodeId) && j["nodes"][nodeId].is_object()) {
        const json& old = j["nodes"][nodeId];
        if (t.placed && old.contains("date") && old.contains("day") && old["day"].is_number() && old["day"].get<double>() == t.day)
            v["date"] = old["date"];
    }
    if (v.empty()) j["nodes"].erase(nodeId);
    else j["nodes"][nodeId] = v;
    lib_->setSetting("timeline:" + bookId, j.dump());
}

std::string StudioApp::timelineUnit(const Id& bookId) const {
    const json j = json::parse(lib_->setting("timeline:" + bookId), nullptr, false);
    const std::string u = j.is_object() ? j.value("unit", "day") : "day";
    return u == "week" || u == "month" || u == "year" ? u : "day";
}

void StudioApp::setTimelineUnit(const Id& bookId, const std::string& unit) {
    json j = json::parse(lib_->setting("timeline:" + bookId), nullptr, false);
    if (!j.is_object()) j = json::object();
    if (unit == "calendar") {
        // Calendar dates (0.75): story days count from `start`, a calendar day; the first time, the start of the
        // latest era (or this year, in the Gregorian calendar).
        const cal::Calendar c = cal::calendarOfBook(*lib_, bookId);
        j["unit"] = "day";
        j["calendar"] = true;
        if (!j.contains("start") || !j["start"].is_number()) {
            std::int64_t y = c.eras.empty() ? 1 : c.eras.back().start;
            if (c.name == "Gregorian") y = 2026;
            j["start"] = cal::toDay(c, {y, 0, 1});
        }
    } else {
        j["unit"] = unit;
        j["calendar"] = false;
    }
    lib_->setSetting("timeline:" + bookId, j.dump());
}

bool StudioApp::timelineInCalendar(const Id& bookId) const {
    if (!lib_) return false;
    return cal::storySpan(*lib_, bookId).on && !cal::calendarOfBook(*lib_, bookId).empty();
}

std::wstring StudioApp::storyDayText(const Id& bookId, double day) const {
    if (timelineInCalendar(bookId)) {
        const cal::Calendar c = cal::calendarOfBook(*lib_, bookId);
        return w(cal::formatDate(c, cal::fromDay(c, cal::storySpan(*lib_, bookId).start + (std::int64_t)std::floor(day))));
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", day);
    const std::string unit = timelineUnit(bookId);
    const std::wstring u = unit == "week" ? L"Week " : unit == "month" ? L"Month " : unit == "year" ? L"Year " : L"Day ";
    return u + widen(buf);
}

// --------------------------------------------------------- helpers

namespace {

using plan::Beat;
using plan::Structure;

std::string withCrLf(const std::string& s) {
    std::string out;
    for (char c : s) { if (c == '\n') out += '\r'; out += c; }
    return out;
}

const wchar_t* kColourNames[] = {L"Red", L"Blue", L"Green", L"Amber", L"Purple", L"Teal", L"Pink", L"Olive", L"Slate", L"Copper"};

bool clipboardTextOf(HWND owner, std::wstring& out) {
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(owner)) return false;
    bool ok = false;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT))
        if (const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(h))) { out = p; GlobalUnlock(h); ok = true; }
    CloseClipboard();
    return ok;
}

bool copyToClipboard(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    bool ok = false;
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t))) {
        if (void* p = GlobalLock(h)) {
            memcpy(p, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(h);
            ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
        }
        if (!ok) GlobalFree(h);
    }
    CloseClipboard();
    return ok;
}

// A file's text: UTF-8 (with or without its mark) or UTF-16 with its mark.
std::string fileText(const std::string& bytes) {
    if (bytes.size() >= 2 && (unsigned char)bytes[0] == 0xFF && (unsigned char)bytes[1] == 0xFE) {
        std::wstring ws((bytes.size() - 2) / 2, L'\0');
        memcpy(ws.data(), bytes.data() + 2, ws.size() * 2);
        return narrow(ws);
    }
    return bytes;
}


}  // namespace

// ----------------------------------------------------------- the timeline

void StudioApp::drawPlanTimeline(const Rect& r) {
    const Book* b = lib_->book(pl_.book);
    if (!b) return;
    gfx_.pushClip(r);
    setHitClip(r);
    const float pad = r.w > 900 ? 36 : 24;
    const float cw = r.w - pad * 2;
    const Rect m{r.x + pad, r.y + 26 - pl_.scroll, cw, 0};
    // In calendar dates (0.75): story day d is the calendar day start + d.
    const bool calOn = timelineInCalendar(pl_.book);
    const cal::Calendar cc = calOn ? cal::calendarOfBook(*lib_, pl_.book) : cal::Calendar{};
    const std::int64_t cStart = calOn ? cal::storySpan(*lib_, pl_.book).start : 0;
    // How far into its day a time of day falls (0 to under 1; 0 with no time).
    auto dayFraction = [&](int minutes, const std::string& part) -> double {
        if (!calOn || (minutes < 0 && part.empty())) return 0;
        const int k = cal::timeSortKey(cc, {minutes, part});
        const double perDay = (double)std::max(1, cc.hoursPerDay) * std::max(1, cc.minutesPerHour);
        return k < 0 ? 0 : std::clamp(k / perDay, 0.0, 0.999);
    };
    auto lab = [&](double d) -> std::wstring {
        if (!calOn) return storyDayText(pl_.book, d);
        return w(cal::formatDate(cc, cal::fromDay(cc, cStart + (std::int64_t)std::floor(d))));
    };
    float y = m.y + planHeader(m, calOn ? L"When things happen, in " + w(cc.name.empty() ? std::string("the world's calendar") : cc.name) +
                                              L". The line joins them in reading order, so a flashback turns back."
                                        : L"When things happen. The line joins them in reading order, so a flashback turns back.");
    // Lanes by viewpoint, plot thread or place, or one lane (0.52); and the zoom.
    const std::string laneBy = planPref(pl_.book, "lanes", "viewpoint");
    {
        float tx = m.x;
        gfx_.text(L"Lanes", {tx, y, 50, 28}, Font::Small, pal_.inkMuted, {Align::Left, false, false, true});
        tx += 50;
        segmented("pl:lanes", {tx, y, 330, 28}, {L"Viewpoint", L"Thread", L"Place", L"One lane"},
                  laneBy == "thread" ? 1 : laneBy == "place" ? 2 : laneBy == "one" ? 3 : 0, [this](int i) {
                      static const char* keys[] = {"viewpoint", "thread", "place", "one"};
                      setPlanPref(pl_.book, "lanes", keys[i]);
                  });
        tx += 330 + 20;
        // What the timeline counts in (0.75: here, beside the lanes, with the world's calendar dates as a choice).
        {
            static const char* units[] = {"day", "week", "month", "year", "calendar"};
            static const wchar_t* names[] = {L"Days", L"Weeks", L"Months", L"Years", L"Calendar dates"};
            const std::string unitNow = timelineUnit(pl_.book);
            int cur = 0;
            for (int i = 0; i < 4; ++i) if (unitNow == units[i]) cur = i;
            if (calOn) cur = 4;
            const bool hasCal = !cal::calendarOfBook(*lib_, pl_.book).empty();
            std::wstring lbl = std::wstring(L"Counts in ") + names[cur];
            for (size_t k = 10; k < lbl.size(); ++k) lbl[k] = (wchar_t)towlower(lbl[k]);
            if (tx + gfx_.measureWidth(lbl, Font::Body) + 44 > m.x + cw) { tx = m.x; y += 36; }
            tx += menuButton("pl:units", tx, y - 2, lbl, [this, cur, hasCal] {
                std::vector<ToolItem> t;
                for (int i = 0; i < 4; ++i)
                    t.push_back(ToolItem::item(names[i], [this, i] { commitFields(); setTimelineUnit(pl_.book, units[i]); loadedFocus_ = {Focus::None, "-", ""}; invalidate(); }, i == cur));
                ToolItem calItem = ToolItem::item(hasCal ? std::wstring(names[4]) : std::wstring(L"Calendar dates (make the world's calendar first)"),
                                                  [this] { commitFields(); setTimelineUnit(pl_.book, "calendar"); loadedFocus_ = {Focus::None, "-", ""}; invalidate(); }, cur == 4);
                calItem.enabled = hasCal;
                t.push_back(calItem);
                return t;
            }) + 16;
        }
        if (tx + 150 > m.x + cw) { tx = m.x; y += 36; }
        float bw = 0;
        smallButton("pl:tlzout", tx, y, L"−", [this] { zoomTimeline(0.8f, pl_.tlDropArea.x + pl_.tlDropArea.w / 2); }, &bw);
        tooltip("pl:tlzout", L"Zoom out (Ctrl and the mouse wheel)");
        tx += bw + 6;
        smallButton("pl:tlfit", tx, y, L"Fit", [this] { pl_.tlZoom = 0; pl_.tlScrollX = 0; invalidate(); }, &bw);
        tooltip("pl:tlfit", L"The whole story");
        tx += bw + 6;
        smallButton("pl:tlzin", tx, y, L"+", [this] { zoomTimeline(1.25f, pl_.tlDropArea.x + pl_.tlDropArea.w / 2); }, &bw);
        tooltip("pl:tlzin", L"Zoom in (Ctrl and the mouse wheel)");
        y += 44;
    }
    const auto all = lib_->manuscriptOf(pl_.book);
    const auto times = storyTimes(pl_.book);
    const plan::Threads threads = plan::threadsOf(*lib_, pl_.book);
    const auto places = plan::nodePlaces(*lib_, pl_.book);
    struct Item { const ManuscriptNode* n; double day; int order; std::wstring label; };
    bool anyTimed = false;   // a card or event has a time of day: days are drawn wide enough to show it (0.81)
    std::vector<Item> placed;
    std::vector<const ManuscriptNode*> unplaced;
    int chapterNo = 0, sceneNo = 0, order = 0;
    for (const ManuscriptNode* n : all) {
        if (n->kind == NodeKind::Part) continue;
        if (n->kind == NodeKind::Chapter) { ++chapterNo; sceneNo = 0; } else ++sceneNo;
        if (!nodeShown(n)) continue;
        const std::wstring lbl = n->kind == NodeKind::Chapter ? L"Ch " + std::to_wstring(chapterNo)
                                                              : std::to_wstring(chapterNo) + L"." + std::to_wstring(sceneNo);
        auto it = times.find(n->id);
        double day = it != times.end() && it->second.placed ? it->second.day : NAN;
        if (pl_.tlDrag.active && pl_.tlDrag.id == n->id && pl_.tlDrag.kind == 0) day = pl_.tlDrag.day;
        // In calendar dates its time of day nudges it along its day (0.80): dawn early, night late. Only where it's drawn:
        // the day it's kept on stays whole.
        if (!std::isnan(day) && calOn && it != times.end() && (it->second.minutes >= 0 || !it->second.part.empty())) {
            day += dayFraction(it->second.minutes, it->second.part);
            anyTimed = true;
        }
        if (std::isnan(day)) unplaced.push_back(n);
        else placed.push_back({n, day, order++, lbl});
    }
    // The bible's events (0.52): those placed on this book's timeline, and the rest.
    std::vector<const Entry*> events;
    for (const Entry* e : b->worldId.empty() ? lib_->entriesOf({}, b->id) : lib_->entriesOf(b->worldId))
        if (e->kind == "event") events.push_back(e);
    auto eventDays = plan::timelineEvents(*lib_, pl_.book);
    if (pl_.tlDrag.active && pl_.tlDrag.kind == 2) eventDays[pl_.tlDrag.id] = pl_.tlDrag.day;
    std::vector<std::pair<const Entry*, double>> placedEvents;
    std::vector<const Entry*> unplacedEvents;
    for (const Entry* e : events) {
        auto it = eventDays.find(e->id);
        if (it != eventDays.end()) {
            const cal::TimeOfDay et = calOn ? cal::eventTime(*lib_, pl_.book, e->id) : cal::TimeOfDay{};
            placedEvents.push_back({e, it->second + dayFraction(et.minutes, et.part)});
            anyTimed |= !et.empty();
        }
        else unplacedEvents.push_back(e);
    }

    // Lanes: what each card is in, in the order they first appear; then those in none.
    struct Lane { std::string key; std::wstring name; Color colour; bool coloured = false; };
    auto lanesOf = [&](const ManuscriptNode* n) -> std::vector<std::string> {
        if (laneBy == "one") return {"*"};
        if (laneBy == "thread") {
            auto it = threads.nodes.find(n->id);
            if (it == threads.nodes.end() || it->second.empty()) return {""};
            return it->second;
        }
        if (laneBy == "place") {
            auto it = places.find(n->id);
            const Entry* e = it != places.end() ? lib_->entry(it->second) : nullptr;
            return {e && !e->deleted ? e->id : std::string()};
        }
        const Entry* e = n->povId.empty() ? nullptr : lib_->entry(n->povId);
        return {e && !e->deleted ? e->id : std::string()};
    };
    std::vector<Lane> lanes;
    for (auto& it : placed)
        for (const std::string& k : lanesOf(it.n)) {
            if (std::any_of(lanes.begin(), lanes.end(), [&](const Lane& l) { return l.key == k; })) continue;
            Lane l{k, {}, {}, false};
            if (k == "*") l.name = L"The story";
            else if (laneBy == "thread") {
                if (const plan::Thread* t = threads.find(k)) { l.name = w(t->name); l.colour = cjui::hexColor(t->colour.c_str()); l.coloured = true; }
                else l.name = L"In no thread";
            } else if (const Entry* e = k.empty() ? nullptr : lib_->entry(k)) {
                l.name = w(e->name);
                l.colour = povColour(e->id);
                l.coloured = true;
            } else l.name = laneBy == "place" ? L"No place" : L"No viewpoint";
            lanes.push_back(l);
        }
    std::stable_partition(lanes.begin(), lanes.end(), [](const Lane& l) { return !l.key.empty(); });
    if (lanes.empty()) lanes.push_back({laneBy == "one" ? "*" : "", laneBy == "one" ? L"The story" : L"", {}, false});

    // The span: everything placed (and a few days to drop onto when nothing is).
    double lo = 0, hi = 10;
    bool anyPlaced = false;
    auto widen2 = [&](double d) { if (!anyPlaced) { lo = hi = d; anyPlaced = true; } else { lo = std::min(lo, d); hi = std::max(hi, d); } };
    for (auto& it : placed) widen2(it.day);
    for (auto& [e, d] : placedEvents) widen2(d);
    if (anyPlaced) { lo = std::floor(lo) - 1; hi = std::ceil(hi) + 1; }
    const float labelW = 150, cardW = 132, cardH = 54;
    const float avail = cw - labelW;
    const double span = std::max(1.0, hi - lo);
    const float tail = placedEvents.empty() ? 0.0f : 170.0f;   // room for an event's flag after the last day
    // With times of day, a day is at least three cards wide, so dawn, midday and night sit clearly apart; the timeline
    // then scrolls along (Shift+wheel) when it's longer than the page.
    const float fit = anyTimed ? std::max(3 * cardW + 24, (float)((avail - cardW - 16 - tail) / span))
                               : std::clamp((float)((avail - cardW - 16 - tail) / span), 36.0f, 220.0f);
    const float px = pl_.tlZoom > 0 ? pl_.tlZoom : fit;
    pl_.tlPx = px;
    pl_.tlDay0 = lo;
    pl_.tlContentW = (float)(span * px) + cardW + 16 + tail;
    pl_.tlScrollX = std::clamp(pl_.tlScrollX, 0.0f, std::max(0.0f, pl_.tlContentW - avail));
    pl_.tlOrigin = m.x + labelW + cardW / 2 + 8;
    const float x0 = pl_.tlOrigin - pl_.tlScrollX;   // where `lo` is (half a card in, so its card fits)
    pl_.tlX0 = x0;
    auto xOf = [&](double d) { return x0 + (float)((d - lo) * px); };
    const Rect area{m.x + labelW, y, avail, 0};
    if (!anyPlaced)
        y += gfx_.text(L"Nothing is on the timeline yet. Drag a card from below onto it, press Place to put one after the last, or "
                       L"give a card a day in its details.",
                       {m.x, y, std::min(cw, 680.0f), 60}, Font::Body, pal_.inkMuted) + 12;
    // Ticks along the top, at a step that leaves room for their labels.
    double step = 1;
    for (double s : {1.0, 2.0, 5.0, 10.0, 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0}) { step = s; if (s * px >= 70) break; }
    const float tickY = y;
    y += 26;
    // The story's events (0.52): a strip of flags above the lanes, each with a line down through them.
    const float eventTop = y;
    const bool eventStrip = !events.empty();
    if (eventStrip) y += 34;
    // Lanes and their cards (stacked when they'd overlap).
    struct Placed { const Item* it; Rect rc; };
    std::vector<Placed> drawn;
    const float laneTop = y;
    std::vector<std::pair<float, float>> laneSpans;
    for (const Lane& lane : lanes) {
        std::vector<std::vector<std::pair<float, float>>> rows;
        const float top = y;
        for (auto& it : placed) {
            const auto ks = lanesOf(it.n);
            if (std::find(ks.begin(), ks.end(), lane.key) == ks.end()) continue;
            const float cx = xOf(it.day);
            const float a = cx - cardW / 2, z = cx + cardW / 2;
            size_t row = 0;
            for (; row < rows.size(); ++row) {
                bool free = true;
                for (auto& [p, q] : rows[row]) if (a < q + 6 && z > p - 6) { free = false; break; }
                if (free) break;
            }
            if (row == rows.size()) rows.emplace_back();
            rows[row].push_back({a, z});
            drawn.push_back({&it, {a, top + 10 + row * (cardH + 8), cardW, cardH}});
        }
        const float h = std::max<size_t>(1, rows.size()) * (cardH + 8) + 12;
        laneSpans.push_back({top, top + h});
        y += h;
    }
    const float laneBottom = y;
    pl_.tlArea = {area.x, laneTop, area.w, laneBottom - laneTop};
    pl_.tlDropArea = {area.x, tickY, area.w, laneBottom - tickY};
    // Lane backgrounds and names.
    for (size_t i = 0; i < lanes.size(); ++i) {
        const auto [a, z] = laneSpans[i];
        if (i % 2 == 0) gfx_.fill({m.x, a, cw, z - a}, pal_.surface.withAlpha(0.45f));
        if (lanes[i].coloured) gfx_.fill({m.x, a + 6, 4, z - a - 12}, lanes[i].colour);   // the lane's colour, as on the cards
        gfx_.text(lanes[i].name, {m.x + 12, a + 10, labelW - 20, 40}, Font::BodyStrong, lanes[i].coloured ? pal_.ink : pal_.inkMuted,
                  {Align::Left, true, true});
        gfx_.line(m.x, z - 0.5f, m.right(), z - 0.5f, pal_.rule);
    }
    if (eventStrip)
        gfx_.text(L"Story events", {m.x + 12, eventTop + 6, labelW - 20, 20}, Font::Label, pal_.inkFaint, {Align::Left, false, true});
    // Ticks and day lines (clipped to the area right of the lane names).
    gfx_.pushClip({area.x, tickY, area.w, laneBottom - tickY});
    if (!calOn) {
        for (double d = std::ceil(lo / step) * step; d <= hi; d += step) {
            const float tx = xOf(d);
            gfx_.line(tx, laneTop, tx, laneBottom, pal_.rule.withAlpha(0.6f));
            gfx_.text(lab(d), {tx - 40, tickY, 80, 20}, Font::Small, pal_.inkFaint, {Align::Center, false});
        }
    } else {
        // Calendar ticks: days ("3 Frostmoon") when there's room, else the first days of months ("Frostmoon 1204"), or of years.
        const double visLo = lo + (area.x - x0) / px - 1, visHi = lo + (area.right() - x0) / px + 1;
        const double monthLen = std::max(1.0, cal::meanYear(cc) / std::max<size_t>(1, cc.months.size()));
        double dstep = 1;   // room for "12 Frostmoon" under each
        for (double s2 : {1.0, 2.0, 3.0, 5.0, 7.0, 10.0, 14.0, 21.0}) { dstep = s2; if (s2 * px >= 120) break; }
        if (dstep * px >= 120 && dstep < monthLen * 0.6) {
            for (double d = std::ceil(std::max(lo, visLo) / dstep) * dstep; d <= std::min(hi, visHi); d += dstep) {
                const float tx = xOf(d);
                const cal::Date dt = cal::fromDay(cc, cStart + (std::int64_t)d);
                gfx_.line(tx, laneTop, tx, laneBottom, pal_.rule.withAlpha(0.6f));
                gfx_.text(std::to_wstring(dt.day) + L" " + w(cal::monthLabel(cc, dt.month)), {tx - 60, tickY, 120, 20}, Font::Small,
                          pal_.inkFaint, {Align::Center, false, true});
            }
        } else {
            const int every = std::max(1, (int)std::ceil(110.0 / (px * monthLen)));
            const bool years = every >= (int)cc.months.size();
            const int yearsEvery = years ? std::max(1, (int)std::ceil(110.0 / (px * cal::meanYear(cc)))) : 1;
            cal::Date dt = cal::fromDay(cc, cStart + (std::int64_t)std::floor(std::max(lo, visLo)));
            dt.day = 1;
            if (years) dt.month = 0;
            for (int guard = 0; guard < 2000; ++guard) {
                const double d = (double)(cal::toDay(cc, dt) - cStart);
                if (d > std::min(hi, visHi)) break;
                if (d >= lo) {
                    const float tx = xOf(d);
                    gfx_.line(tx, laneTop, tx, laneBottom, pal_.rule.withAlpha(0.6f));
                    const std::wstring t = years ? w(cal::formatYear(cc, dt.year))
                                                 : w(cal::monthLabel(cc, dt.month)) + L" " + std::to_wstring(cal::eraYear(cc, dt.year).year);
                    gfx_.text(t, {tx - 70, tickY, 140, 20}, Font::Small, pal_.inkFaint, {Align::Center, false, true});
                }
                if (years) dt.year += yearsEvery;
                else {
                    dt.month += every;
                    while (dt.month >= (int)cc.months.size()) { dt.month -= (int)cc.months.size(); ++dt.year; }
                }
            }
        }
        // The moons: full and new moons along the top of the lanes, when the days are wide enough to show them.
        if (px >= 4 && !cc.moons.empty()) {
            for (double d = std::floor(std::max(lo, visLo)); d <= std::min(hi, visHi); d += 1) {
                const std::int64_t abs = cStart + (std::int64_t)d;
                float mx = xOf(d) + (float)px / 2;
                for (size_t mi = 0; mi < cc.moons.size() && mi < 3; ++mi) {
                    const double ph = cal::moonPhase(cc, cc.moons[mi], abs), prev = cal::moonPhase(cc, cc.moons[mi], abs - 1);
                    const bool full = (prev < 0.5 && ph >= 0.5) || (std::fabs(ph - 0.5) < 1e-9 && std::fabs(prev - 0.5) > 1e-9);
                    const bool fresh = prev - ph > 0.5 || (ph < 1e-9 && prev > 1e-9);   // the day it turns full, or new
                    bool eclipsed = false;
                    for (const auto& e : cal::eclipsesOn(cc, abs)) if (e.moon == &cc.moons[mi]) eclipsed = true;
                    if (!full && !fresh && !eclipsed) continue;
                    const float my = laneTop + 7 + 12 * mi;
                    gfx_.circle(mx, my, 4.5f, full ? gold_.mix(pal_.surface, 0.3f) : pal_.ink.withAlpha(0.55f), true);
                    gfx_.circle(mx, my, 4.5f, pal_.inkFaint, false, 1);
                    if (eclipsed) gfx_.circle(mx, my, 7.0f, pal_.dark ? cjui::hexColor("#F08A6C") : cjui::hexColor("#B4442C"), false, 1.5f);
                    const std::string tid = "pl:moon:" + std::to_string(abs) + ":" + std::to_string(mi);
                    addHit(tid, {mx - 6, my - 6, 12, 12}, nullptr);
                    tooltip(tid, w(cc.moons[mi].name) + (eclipsed ? L": eclipse, " : full ? L": full moon, " : L": new moon, ") + lab(d));
                }
            }
        }
    }
    // Events: a dashed line through the lanes and a flag above them; drag one along, click it for more.
    const Color evc = pal_.dark ? cjui::hexColor("#E0A458") : cjui::hexColor("#B26B12");
    for (auto& [e, d] : placedEvents) {
        const float ex = xOf(d);
        for (float ly = laneTop; ly < laneBottom; ly += 8) gfx_.line(ex, ly, ex, std::min(ly + 4, laneBottom), evc.withAlpha(0.7f), 1.5f);
        const std::wstring nm = w(e->name);
        const float fw = std::min(gfx_.measureWidth(nm, Font::Small) + 22, 200.0f);
        const Rect flag{ex - 6, eventTop + 4, fw, 24};
        const std::string id = "pl:tle:" + e->id;
        // Its time of day (0.80): one of the calendar's named times, from the menu.
        const cal::TimeOfDay et = calOn ? cal::eventTime(*lib_, pl_.book, e->id) : cal::TimeOfDay{};
        addHit(id, flag, [this, eid = e->id, calOn] {
            std::vector<ToolItem> t;
            t.push_back(ToolItem::item(L"Open it in the world's bible", [this, eid] {
                if (const Entry* x = lib_->entry(eid)) { openWorldbuilding(x->worldId, x->bookId); openEntry(eid); }
            }));
            if (calOn) {
                const cal::Calendar c2 = cal::calendarOfBook(*lib_, pl_.book);
                const cal::TimeOfDay now = cal::eventTime(*lib_, pl_.book, eid);
                std::vector<ToolItem> times;
                times.push_back(ToolItem::item(L"Any time", [this, eid] { cal::setEventTime(*lib_, pl_.book, eid, {}); invalidate(); }, now.empty()));
                for (const cal::DayPart& dp : c2.dayParts) {
                    cal::TimeOfDay tt;
                    tt.part = dp.name;
                    times.push_back(ToolItem::item(w(dp.name) + L"  (from " + w(cal::formatTime(c2, dp.start)) + L")",
                                                   [this, eid, tt] { cal::setEventTime(*lib_, pl_.book, eid, tt); invalidate(); }, now.part == dp.name));
                }
                t.push_back(ToolItem::menu(L"Time of day", times));
            }
            t.push_back(ToolItem::line());
            t.push_back(ToolItem::item(L"Take it off this timeline", [this, eid] { plan::setTimelineEvent(*lib_, pl_.book, eid, std::nullopt); invalidate(); }));
            showTools(t, mouseX(), mouseY());
        });
        tooltip(id, nm + L" · " + lab(d) + (et.empty() ? L"" : L", " + w(cal::timeOfDayText(cc, et))) + L". Drag it along to move it; click for more.");
        const float hv = hoverAmount(id);
        gfx_.fillRounded(flag, 4, evc.withAlpha(0.14f + 0.12f * hv));
        gfx_.fill({ex - 1, eventTop + 4, 2.5f, 24}, evc);
        gfx_.text(nm, {flag.x + 12, flag.y, flag.w - 16, flag.h}, Font::Small, pal_.ink, {Align::Left, false, true, true});
    }
    // Reading order: a line from card to card (a card in several lanes, from its first).
    std::vector<const Placed*> byOrder;
    std::set<const Item*> seen;
    for (auto& p : drawn) if (seen.insert(p.it).second) byOrder.push_back(&p);
    std::sort(byOrder.begin(), byOrder.end(), [](auto* a, auto* b) { return a->it->order < b->it->order; });
    for (size_t i = 1; i < byOrder.size(); ++i) {
        const Rect& a = byOrder[i - 1]->rc;
        const Rect& c = byOrder[i]->rc;
        const bool back = c.x < a.x - 1;   // a flashback: earlier in the story, later in the book
        gfx_.line(a.x + a.w / 2, a.y + a.h / 2, c.x + c.w / 2, c.y + c.h / 2,
                  back ? cjui::hexColor("#C2410C").withAlpha(0.7f) : pal_.ribbon.withAlpha(0.45f), back ? 2.0f : 1.5f);
    }
    // Cards.
    for (auto& p : drawn) {
        const ManuscriptNode* n = p.it->n;
        const Rect& c = p.rc;
        const std::string id = "pl:tl:" + n->id;
        addHit(id, c, [this, nid = n->id] { selectCard(nid); });
        const bool sel = pl_.node == n->id;
        const float hv = hoverAmount(id);
        softShadow(c, 6, std::max(hv, sel ? 1.0f : 0.0f));
        gfx_.fillRounded(c, 6, pal_.surface);
        gfx_.strokeRounded(c, 6, sel ? pal_.ribbon : pal_.rule.mix(pal_.inkFaint, hv), sel ? 2.0f : 1.0f);
        gfx_.fill({c.x + 1, c.y + 8, 3, c.h - 16}, statusColour(n->status));
        gfx_.text(p.it->label, {c.x + 12, c.y + 6, c.w - 20, 16}, Font::Label, pal_.inkFaint, {Align::Left, false, true});
        const auto t = times.find(n->id);
        std::wstring when;
        if (t != times.end() && !t->second.when.empty()) when = w(t->second.when);
        else if (calOn) {
            // On a card, the day and month (the ticks give the year); the whole date is in its tooltip.
            const cal::Date dt = cal::fromDay(cc, cStart + (std::int64_t)std::floor(p.it->day));
            when = std::to_wstring(dt.day) + L" " + w(cal::monthLabel(cc, dt.month));
            tooltip(id, lab(p.it->day) + (t != times.end() && (t->second.minutes >= 0 || !t->second.part.empty())
                                              ? L", " + w(cal::timeOfDayText(cc, {t->second.minutes, t->second.part}, true)) : L""));
        } else when = lab(p.it->day);
        if (t != times.end() && (t->second.minutes >= 0 || !t->second.part.empty()) && calOn)
            when += L" \u00B7 " + w(cal::timeOfDayText(cc, {t->second.minutes, t->second.part}));
        const float lw = gfx_.measureWidth(p.it->label, Font::Label) + 8;
        gfx_.text(when, {c.x + 12 + lw, c.y + 6, c.w - 20 - lw, 16}, Font::Label, pal_.ribbon, {Align::Right, false, true});
        gfx_.text(w(n->title.empty() ? std::string(label(n->kind)) : n->title), {c.x + 12, c.y + 24, c.w - 20, 22}, Font::BodyStrong,
                  pal_.ink, {Align::Left, false, true});
        if (focused(id)) focusRing(c);
    }
    // Something from below being dragged onto it: where it would land.
    if (pl_.tlDrag.active && (pl_.tlDrag.kind == 1 || pl_.tlDrag.kind == 3) && pl_.tlDropArea.contains(pl_.tlDrag.mx, pl_.tlDrag.my)) {
        const float gx = xOf(pl_.tlDrag.day);
        gfx_.line(gx, laneTop, gx, laneBottom, pal_.ribbon, 2.5f);
        const std::wstring dl = lab(pl_.tlDrag.day);
        const float dw = gfx_.measureWidth(dl, Font::Small) + 16;
        gfx_.fillRounded({gx - dw / 2, tickY - 2, dw, 22}, 11, pal_.ribbon);
        gfx_.text(dl, {gx - dw / 2, tickY - 2, dw, 22}, Font::Small, pal_.paper, {Align::Center, false, false, true});
    }
    gfx_.popClip();
    if (pl_.tlContentW > avail + 1)
        gfx_.text(L"Shift and the mouse wheel move along the timeline; Ctrl and the wheel zoom.", {m.x, laneBottom + 6, cw, 18}, Font::Small,
                  pal_.inkFaint, {Align::Right, false});
    y = laneBottom + 30;

    // Not placed yet: drag one onto the timeline, or Place it after the last.
    double last = 0;
    bool any = false;
    for (auto& [id, t] : times) if (t.placed) { last = any ? std::max(last, t.day) : t.day; any = true; }
    for (auto& [id, d] : eventDays) { last = any ? std::max(last, d) : d; any = true; }
    const double next = any ? std::floor(last) + 1 : 1;
    auto chips = [&](const std::wstring& heading, const std::vector<std::pair<Id, std::wstring>>& list, const char* prefix, bool isEvent) {
        if (list.empty()) return;
        y += sectionLabel(m.x, y, cw, heading);
        float x = m.x;
        for (auto& [cid, title] : list) {
            const float tw = std::min(gfx_.measureWidth(title, Font::Small) + 86, 280.0f);
            if (x + tw > m.right()) { x = m.x; y += 38; }
            const Rect chip{x, y, tw, 30};
            const std::string id = prefix + cid;
            addHit(id, {chip.x, chip.y, chip.w - 60, chip.h}, [this, cid, isEvent] {
                if (!isEvent) selectCard(cid);
                else flash(L"Drag it onto the timeline, or press Place.", 3);
            });
            tooltip(id, L"Drag it onto the timeline");
            const bool sel = !isEvent && pl_.node == cid;
            const bool lifted = pl_.tlDrag.active && pl_.tlDrag.id == cid;
            gfx_.fillRounded(chip, 15, (sel ? pal_.surface : pal_.sunken).withAlpha(lifted ? 0.4f : 1.0f));
            gfx_.strokeRounded(chip, 15, sel ? pal_.ribbon : isEvent ? evc.withAlpha(0.6f) : pal_.rule);
            gfx_.text(title, {chip.x + 14, chip.y, chip.w - 80, chip.h}, Font::Small, pal_.ink, {Align::Left, false, true, true});
            linkText(std::string(isEvent ? "pl:tleplace:" : "pl:tlplace:") + cid, chip.right() - 52, chip.y + 7, L"Place", [this, cid, next, isEvent] {
                commitFields();
                if (isEvent) { plan::setTimelineEvent(*lib_, pl_.book, cid, next); invalidate(); return; }
                StoryTime t = storyTimes(pl_.book)[cid];
                t.placed = true;
                t.day = next;
                setStoryTime(pl_.book, cid, t);
                selectCard(cid);
            });
            if (focused(id)) focusRing(chip);
            x += tw + 8;
        }
        y += 50;
    };
    {
        std::vector<std::pair<Id, std::wstring>> list;
        for (const ManuscriptNode* n : unplaced) list.push_back({n->id, n->title.empty() ? w(label(n->kind)) : w(n->title)});
        chips(L"Not on the timeline yet", list, "pl:tlu:", false);
        list.clear();
        for (const Entry* e : unplacedEvents) list.push_back({e->id, w(e->name)});
        chips(L"Story events from the bible", list, "pl:tleu:", true);
    }
    // The thing being dragged from below, following the pointer.
    if (pl_.tlDrag.active && (pl_.tlDrag.kind == 1 || pl_.tlDrag.kind == 3)) {
        std::wstring nm;
        if (pl_.tlDrag.kind == 1) { if (const ManuscriptNode* n = lib_->node(pl_.tlDrag.id)) nm = n->title.empty() ? w(label(n->kind)) : w(n->title); }
        else if (const Entry* e = lib_->entry(pl_.tlDrag.id)) nm = w(e->name);
        const float lw = std::min(gfx_.measureWidth(nm, Font::BodyStrong) + 24, 320.0f);
        const Rect g{pl_.tlDrag.mx + 14, pl_.tlDrag.my + 10, lw, 30};
        gfx_.fillRounded(g, 3, pal_.ink.withAlpha(0.88f));
        gfx_.text(nm, g.inset(12, 0), Font::BodyStrong, pal_.paper, {Align::Left, false, true, true});
    }
    if (events.empty() && !b->worldId.empty())
        y += gfx_.text(L"Events from the world's bible (entries of the kind Event) can be placed here too.", {m.x, y, cw, 20}, Font::Small,
                       pal_.inkFaint) + 10;
    pl_.content = y + pl_.scroll - r.y + 30;
    pl_.scroll = std::clamp(pl_.scroll, 0.0f, std::max(0.0f, pl_.content - r.h));
    clearHitClip();
    gfx_.popClip();
}

// Zoom the timeline about a point across it, keeping the day under that point still.
void StudioApp::zoomTimeline(float factor, float atX) {
    const float px = (float)std::max(0.5, pl_.tlPx);
    const float np = std::clamp(px * factor, 2.0f, 600.0f);
    const double d = pl_.tlDay0 + (atX - pl_.tlX0) / px;
    pl_.tlZoom = np;
    pl_.tlScrollX = std::max(0.0f, pl_.tlOrigin - (atX - (float)((d - pl_.tlDay0) * np)));
    invalidate();
}

// Dragging on the timeline: a card or event along it, or one from below onto it.
bool StudioApp::planMouseDown(float x, float y) {
    if (ws_ != Workspace::Planning || pl_.view != 1) return false;
    const std::string id = hitAt(x, y);
    int kind = -1;
    Id nid;
    for (auto [prefix, k] : {std::pair{"pl:tl:", 0}, std::pair{"pl:tlu:", 1}, std::pair{"pl:tle:", 2}, std::pair{"pl:tleu:", 3}})
        if (id.rfind(prefix, 0) == 0) { kind = k; nid = id.substr(std::strlen(prefix)); break; }
    if (kind < 0) return false;
    pl_.tlDrag = {};
    if (kind == 0) {
        const auto times = storyTimes(pl_.book);
        const auto it = times.find(nid);
        if (it == times.end() || !it->second.placed) return false;
        pl_.tlDrag.startDay = it->second.day;
    } else if (kind == 2) {
        const auto ev = plan::timelineEvents(*lib_, pl_.book);
        const auto it = ev.find(nid);
        if (it == ev.end()) return false;
        pl_.tlDrag.startDay = it->second;
    }
    pl_.tlDrag.armed = true;
    pl_.tlDrag.id = nid;
    pl_.tlDrag.kind = kind;
    pl_.tlDrag.sx = pl_.tlDrag.mx = x;
    pl_.tlDrag.sy = pl_.tlDrag.my = y;
    pl_.tlDrag.day = pl_.tlDrag.startDay;
    return false;   // a click still selects it
}

bool StudioApp::planMouseMove(float x, float y, bool leftDown) {
    if (!pl_.tlDrag.armed) return false;
    if (!leftDown) { pl_.tlDrag = {}; return false; }
    pl_.tlDrag.mx = x;
    pl_.tlDrag.my = y;
    const bool along = pl_.tlDrag.kind == 0 || pl_.tlDrag.kind == 2;
    if (!pl_.tlDrag.active && (along ? std::abs(x - pl_.tlDrag.sx) > 6 : std::hypot(x - pl_.tlDrag.sx, y - pl_.tlDrag.sy) > 6)) {
        pl_.tlDrag.active = true;
        cancelPress();
    }
    if (pl_.tlDrag.active) {
        // Whole steps of the unit: from where it started, or where the pointer is.
        const double perPx = 1.0 / std::max(0.5, (double)pl_.tlPx);
        if (along) pl_.tlDrag.day = pl_.tlDrag.startDay + std::round((x - pl_.tlDrag.sx) * perPx);
        else pl_.tlDrag.day = std::round(pl_.tlDay0 + (x - pl_.tlX0) * perPx);
        invalidate();
        return true;
    }
    return false;
}

bool StudioApp::planMouseUp(float x, float y) {
    if (!pl_.tlDrag.armed) return false;
    const auto d = pl_.tlDrag;
    pl_.tlDrag = {};
    const bool onto = d.kind == 0 || d.kind == 2 || pl_.tlDropArea.contains(x, y);
    const bool moved = d.active && onto && (d.kind == 1 || d.kind == 3 || d.day != d.startDay);
    if (moved) {
        commitFields();
        if (d.kind <= 1) {
            StoryTime t = storyTimes(pl_.book)[d.id];
            t.placed = true;
            t.day = d.day;
            setStoryTime(pl_.book, d.id, t);
            selectCard(d.id);
            loadFields();
        } else plan::setTimelineEvent(*lib_, pl_.book, d.id, d.day);
        flash((d.kind <= 1 ? L"Moved to " : L"The event is now at ") + storyDayText(pl_.book, d.day) + L".", 3);
    }
    invalidate();
    return d.active;
}

// ------------------------------------------------------------ the structure

void StudioApp::drawPlanStructure(const Rect& r) {
    const Book* b = lib_->book(pl_.book);
    if (!b) return;
    gfx_.pushClip(r);
    setHitClip(r);
    const float pad = r.w > 900 ? 36 : 24;
    const float cw = r.w - pad * 2;
    const Rect m{r.x + pad, r.y + 26 - pl_.scroll, cw, 0};
    const std::string scope = plan::structureScope(*lib_, pl_.book);
    const auto shapes = plan::allStructures(*lib_, scope);
    const plan::BookStructure bs = plan::bookStructure(*lib_, pl_.book);
    const Structure* tp = plan::findStructure(shapes, bs.templateId);
    float y = m.y;
    if (!tp) {
        y += planHeader(m,
                        L"Choose a story shape to lay over the book, or make your own. Its beats are placed by length, so you can see "
                        L"which chapter falls where, and match each beat to the chapters that carry it. Structures are guides, not rules.");
        const float gap = 14, cardH = 150;
        const float cardW = 300;
        const int cols = std::max(1, (int)((cw + gap) / (cardW + gap)));
        const float wid = (cw - gap * (cols - 1)) / cols;
        int col = 0;
        auto place = [&]() { const Rect c{m.x + col * (wid + gap), y, wid, cardH}; if (++col == cols) { col = 0; y += cardH + gap; } return c; };
        for (const Structure& t : shapes) {
            const Rect c = place();
            const std::string id = "pl:tpl:" + t.id;
            addHit(id, c, [this, tid = t.id] {
                plan::BookStructure s = plan::bookStructure(*lib_, pl_.book);
                if (s.templateId != tid) { s = {}; s.templateId = tid; }
                plan::setBookStructure(*lib_, pl_.book, s);
                pl_.beat.clear();
                invalidate();
            });
            const float hv = hoverAmount(id);
            softShadow(c, 6, hv);
            gfx_.fillRounded(c, 6, pal_.surface);
            gfx_.strokeRounded(c, 6, pal_.rule.mix(pal_.ribbon, hv));
            gfx_.text(w(t.name), {c.x + 18, c.y + 16, c.w - 120, 24}, Font::Heading, pal_.ink, {Align::Left, false, true});
            gfx_.text((t.own ? L"Yours · " : L"") + std::to_wstring(t.beats.size()) + L" beats", {c.x + 18, c.y + 16, c.w - 36, 24},
                      Font::Small, t.own ? pal_.ribbon : pal_.inkFaint, {Align::Right, false, false, true});
            gfx_.pushClip({c.x, c.y + 46, c.w, c.h - 60});
            gfx_.text(w(t.about.empty() ? std::string("Your own structure.") : t.about), {c.x + 18, c.y + 48, c.w - 36, c.h - 62}, Font::Small, pal_.inkMuted);
            gfx_.popClip();
            // A little map of where its beats fall.
            const float bx = c.x + 18, bw = c.w - 36, by = c.bottom() - 18;
            gfx_.line(bx, by, bx + bw, by, pal_.rule, 2);
            for (const Beat& bt : t.beats) gfx_.circle(bx + (float)bt.at * bw, by, 3.5f, pal_.ribbon, true);
            if (focused(id)) focusRing(c);
        }
        // Your own: starts from a simple shape, written as text in the panel.
        {
            const Rect c = place();
            const std::string id = "pl:tpl:new";
            addHit(id, c, [this, scope] {
                auto own = plan::ownStructures(*lib_, scope);
                auto made = plan::parseStructures(
                    "Structure: My structure\nAbout: What this shape is for.\n"
                    "0% Beginning: Who, where, and what they want.\n25% The turn: The story changes direction.\n"
                    "50% The middle: Something is learned or lost.\n75% The low point: Everything is at stake.\n"
                    "90% The end: How it resolves.\n");
                if (made.empty()) return;
                std::set<std::string> taken;
                for (auto& x : plan::allStructures(*lib_, scope)) taken.insert(x.id);
                std::string sid = made[0].id;
                for (int i = 2; taken.count(sid); ++i) sid = made[0].id + "-" + std::to_string(i);
                made[0].id = sid;
                own.push_back(made[0]);
                plan::setOwnStructures(*lib_, scope, own);
                plan::BookStructure s;
                s.templateId = sid;
                plan::setBookStructure(*lib_, pl_.book, s);
                commitFields();
                pl_.node.clear();
                setFocus({Focus::Structure, sid, pl_.book});
                invalidate();
            });
            const float hv = hoverAmount(id);
            gfx_.fillRounded(c, 6, pal_.surface.withAlpha(0.5f + 0.5f * hv));
            gfx_.strokeRounded(c, 6, pal_.rule.mix(pal_.ribbon, 0.4f + 0.6f * hv), 1.5f);
            gfx_.text(L"+ Make your own", {c.x + 18, c.y + 16, c.w - 36, 24}, Font::Heading, pal_.ribbon, {Align::Left, false, true});
            gfx_.text(L"Start from a simple shape and write your own beats beside them, as plain text. Or bring them in with "
                      L"Import planning in Plan tools.",
                      {c.x + 18, c.y + 48, c.w - 36, c.h - 62}, Font::Small, pal_.inkMuted);
            if (focused(id)) focusRing(c);
        }
        if (col) y += cardH + gap;
    } else {
        y += planHeader(m, w(tp->name) + L": " + w(tp->about));
        float lx = m.x, lw = 0;
        linkText("pl:tplchange", lx, y - 8, L"Choose another structure", [this] {
            plan::BookStructure s = plan::bookStructure(*lib_, pl_.book);
            s.templateId.clear();
            plan::setBookStructure(*lib_, pl_.book, s);
            pl_.beat.clear();
            if (focus_.kind == Focus::Beat || focus_.kind == Focus::Structure) selectCard({});
            invalidate();
        });
        lw = gfx_.measureWidth(L"Choose another structure", Font::Small);
        lx += lw + 26;
        if (tp->own)
            linkText("pl:tpledit", lx, y - 8, L"Edit this structure", [this, sid = tp->id] {
                commitFields();
                pl_.node.clear();
                pl_.beat.clear();
                setFocus({Focus::Structure, sid, pl_.book});
                invalidate();
            });
        y += 22;
        // The book by length: chapters as segments (their words; or their targets, or equal shares, before there's text).
        const auto all = lib_->manuscriptOf(pl_.book);
        struct Seg { const ManuscriptNode* n; double w; int no; };
        std::vector<Seg> segs;
        int no = 0;
        double total = 0, totalWords = 0;
        for (const ManuscriptNode* n : all) {
            if (n->kind != NodeKind::Chapter) continue;
            std::int64_t words = n->wordCount;
            for (auto* c : all) if (c->parentId == n->id) words += c->wordCount;
            segs.push_back({n, (double)words, ++no});
            totalWords += (double)words;
        }
        const bool byWords = totalWords > 0;
        // Before there's text: by target, and chapters without one as long as the average target.
        double targets = 0;
        int withTarget = 0;
        for (auto& s : segs) if (s.n->targetWords > 0) { targets += (double)s.n->targetWords; ++withTarget; }
        const double usual = withTarget ? targets / withTarget : 1.0;
        for (auto& s : segs) {
            if (!byWords) s.w = s.n->targetWords > 0 ? (double)s.n->targetWords : usual;
            total += s.w;
        }
        auto numberOf = [&](const Id& id) { for (auto& s : segs) if (s.n->id == id) return s.no; return 0; };
        // Where a fraction of the book falls: which chapter.
        auto chapterAt = [&](double at) -> const Seg* {
            if (at <= 0) return segs.empty() ? nullptr : &segs.front();   // the opening is the first chapter, however short
            double acc = 0;
            for (auto& s : segs) {
                if (at * total < acc + s.w || &s == &segs.back()) return &s;
                acc += s.w;
            }
            return nullptr;
        };
        // The chapters (still in the book) that carry a beat.
        auto carriers = [&](const std::string& beat) {
            std::vector<const ManuscriptNode*> out;
            auto it = bs.beats.find(beat);
            if (it != bs.beats.end())
                for (const Id& id : it->second)
                    if (const ManuscriptNode* n = lib_->node(id); n && !n->deleted && n->bookId == pl_.book) out.push_back(n);
            return out;
        };
        const Rect bar{m.x, y + 40, cw, 34};
        gfx_.text(byWords ? L"The book by length so far" : L"The book (chapters by target, or equal, until there's text)",
                  {m.x, y, cw, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
        if (segs.empty()) {
            gfx_.fillRounded(bar, 4, pal_.sunken);
            gfx_.text(L"No chapters yet", bar, Font::Small, pal_.inkFaint, {Align::Center, false, false, true});
        } else {
            double acc = 0;
            for (size_t i = 0; i < segs.size(); ++i) {
                const float a = bar.x + (float)(acc / total) * bar.w, z = bar.x + (float)((acc + segs[i].w) / total) * bar.w;
                acc += segs[i].w;
                const Rect s{a, bar.y, std::max(1.0f, z - a - 1), bar.h};
                const std::string sid = "pl:seg:" + segs[i].n->id;
                addHit(sid, s, [this, nid = segs[i].n->id] { selectCard(nid); }, false);
                const bool sel = pl_.node == segs[i].n->id;
                // Dimmed when a filter hides it.
                const Color base = (i % 2 ? pal_.surface : pal_.sunken).mix(pal_.ribbon, sel ? 0.25f : 0.0f);
                gfx_.fill(s, nodeShown(segs[i].n) ? base : base.withAlpha(0.35f));
                if (s.w > 26) {
                    std::wstring t = std::to_wstring(segs[i].no);
                    if (s.w > 90 && !segs[i].n->title.empty()) t += L"  " + w(segs[i].n->title);
                    gfx_.text(t, s.inset(6, 0), Font::Small, pal_.inkMuted, {Align::Center, false, true, true});
                }
                tooltip(sid, std::to_wstring(segs[i].no) + L". " + (segs[i].n->title.empty() ? std::wstring(L"Chapter") : w(segs[i].n->title)));
            }
        }
        gfx_.strokeRounded(bar, 4, pal_.rule);
        for (const Beat& bt : tp->beats) {
            const float bx = bar.x + (float)bt.at * bar.w;
            const bool has = !carriers(bt.id).empty();
            gfx_.line(bx, bar.y - 6, bx, bar.bottom() + 6, has ? pal_.ribbon : pal_.inkFaint, 2);
            gfx_.circle(bx, bar.y - 8, 4, has ? pal_.ribbon : pal_.inkFaint, true);
        }
        y = bar.bottom() + 30;
        // Each beat.
        for (const Beat& bt : tp->beats) {
            const std::string bid = bt.id;
            const Seg* at = segs.empty() ? nullptr : chapterAt(bt.at);
            const auto chosen = carriers(bid);
            auto note = bs.notes.find(bid);
            const std::wstring noteText = note != bs.notes.end() ? w(note->second) : std::wstring();
            const float rowH = noteText.empty() ? 74.0f : 96.0f;
            const Rect row{m.x, y, cw, rowH};
            const std::string rid = "pl:beat:" + bid;
            const bool sel = pl_.beat == bid && focus_.kind == Focus::Beat;
            addHit(rid, {row.x, row.y, row.w - 250, row.h}, [this, bid] { selectBeat(bid); });
            gfx_.fillRounded(row, 6, sel ? pal_.surface : pal_.surface.withAlpha(0.55f));
            gfx_.strokeRounded(row, 6, sel ? pal_.ribbon : pal_.rule);
            gfx_.text(std::to_wstring((int)std::round(bt.at * 100)) + L"%", {row.x + 14, row.y + 12, 44, 20}, Font::BodyStrong, pal_.ribbon,
                      {Align::Left, false});
            gfx_.text(w(bt.name), {row.x + 64, row.y + 10, row.w - 330, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
            gfx_.text(w(bt.about), {row.x + 64, row.y + 32, row.w - 330, 36}, Font::Small, pal_.inkMuted, {Align::Left, true, true});
            if (!noteText.empty()) {
                std::wstring one;
                for (wchar_t ch : noteText) one += ch == L'\n' ? L' ' : ch;
                gfx_.text(L"Your notes: " + one, {row.x + 64, row.y + 70, row.w - 330, 18}, Font::Small, pal_.ink, {Align::Left, false, true});
            }
            // The chapters that carry it, chosen from a box you drop down (several allowed, 0.50).
            const float rx = row.right() - 250;
            std::wstring pick;
            if (chosen.size() == 1) {
                const int n1 = numberOf(chosen[0]->id);
                pick = (n1 ? std::to_wstring(n1) + L"  ·  " : std::wstring()) + (chosen[0]->title.empty() ? w(label(chosen[0]->kind)) : w(chosen[0]->title));
            } else if (chosen.size() > 1) {
                std::vector<int> nums;
                for (auto* c : chosen) nums.push_back(numberOf(c->id));
                std::sort(nums.begin(), nums.end());
                pick = L"Chapters ";
                for (size_t i = 0; i < nums.size(); ++i) pick += (i ? L", " : L"") + std::to_wstring(nums[i]);
            } else pick = L"Choose chapters…";
            const Rect pr{rx, row.y + 12, 236, 30};
            const std::string pid = "pl:beatpick:" + bid;
            {
                const float ph = hoverAmount(pid);
                gfx_.fillRounded(pr, 6, pal_.surface);
                gfx_.strokeRounded(pr, 6, pal_.rule.mix(pal_.ink, 0.25f * ph));
                gfx_.text(pick, {pr.x + 10, pr.y, pr.w - 34, pr.h}, Font::Small, chosen.empty() ? pal_.inkFaint : pal_.ink, {Align::Left, false, true, true});
                const float cx = pr.right() - 15, cy = pr.y + pr.h / 2;
                gfx_.line(cx - 4, cy - 2, cx, cy + 2, pal_.inkMuted, 1.4f);
                gfx_.line(cx, cy + 2, cx + 4, cy - 2, pal_.inkMuted, 1.4f);
                if (focused(pid)) focusRing(pr);
            }
            std::vector<std::pair<Id, std::wstring>> options;
            for (auto& sg : segs)
                options.push_back({sg.n->id, std::to_wstring(sg.no) + L". " + (sg.n->title.empty() ? std::wstring(L"Chapter") : w(sg.n->title))});
            addHit(pid, pr, [this, bid, options] {
                plan::BookStructure s = plan::bookStructure(*lib_, pl_.book);
                auto& v = s.beats[bid];
                std::vector<MenuItem> items;
                items.push_back(MenuItem::heading(L"Carried by"));
                for (size_t i = 0; i < options.size(); ++i)
                    items.push_back(MenuItem::item(options[i].second, (UINT)(10 + i), {}, true,
                                                   std::find(v.begin(), v.end(), options[i].first) != v.end()));
                items.push_back(MenuItem::sep());
                items.push_back(MenuItem::item(L"None", 1, {}, !v.empty()));
                POINT pt;
                GetCursorPos(&pt);
                const UINT c = showMenu(hwnd(), items, pt, pal_);
                if (!c) return;
                if (c == 1) v.clear();
                else if (c - 10 < options.size()) {
                    const Id& id = options[c - 10].first;
                    auto it = std::find(v.begin(), v.end(), id);
                    if (it != v.end()) v.erase(it);
                    else v.push_back(id);
                }
                plan::setBookStructure(*lib_, pl_.book, s);
                invalidate();
            });
            std::wstring where = at ? L"By length: chapter " + std::to_wstring(at->no) : L"";
            if (!chosen.empty() && at) {
                const int first = numberOf(chosen[0]->id);
                bool covers = false;
                for (auto* c : chosen) if (c->id == at->n->id) covers = true;
                if (!covers && first) where += first < at->no ? L" (yours comes early)" : L" (yours comes late)";
            }
            gfx_.text(where, {rx, row.y + 46, 236, 18}, Font::Small, pal_.inkFaint, {Align::Left, false, true});
            if (focused(rid)) focusRing(row);
            y += rowH + 8;
        }
        y += 10;
        const bool empty = segs.empty();
        button("pl:outline", {m.x, y, empty ? 230.0f : 290.0f, 34}, empty ? L"Outline the book from this" : L"Add the beats as chapters at the end",
               [this] { outlineFromStructure(); }, empty);
        y += 50;
    }
    pl_.content = y + pl_.scroll - r.y + 30;
    pl_.scroll = std::clamp(pl_.scroll, 0.0f, std::max(0.0f, pl_.content - r.h));
    clearHitClip();
    gfx_.popClip();
}

// A chapter for every beat (title, the beat's guidance as its synopsis, status Outline), each matched to its beat.
void StudioApp::outlineFromStructure() {
    const auto shapes = plan::allStructures(*lib_, plan::structureScope(*lib_, pl_.book));
    plan::BookStructure bs = plan::bookStructure(*lib_, pl_.book);
    const Structure* tp = plan::findStructure(shapes, bs.templateId);
    if (!tp) return;
    bool hasChapters = false;
    for (auto* n : lib_->manuscriptOf(pl_.book)) if (n->kind == NodeKind::Chapter) hasChapters = true;
    if (hasChapters &&
        cjui::messageBox(hwnd(), (L"Add " + std::to_wstring(tp->beats.size()) + L" chapters, one for each beat, after the book's last chapter? "
                             L"Each gets the beat's name as its title and its description as the synopsis.").c_str(),
                    L"Outline from a structure", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
    commitFields();
    Id first;
    for (const Beat& bt : tp->beats) {
        const Id id = lib_->createNode(pl_.book, {}, NodeKind::Chapter);
        if (id.empty()) { flash(w(lib_->lastError()), 6); break; }
        if (const ManuscriptNode* n = lib_->node(id)) {
            ManuscriptNode u = *n;
            u.title = bt.name;
            u.synopsis = bt.about;
            u.status = NodeStatus::Outline;
            lib_->updateNode(u);
        }
        if (bs.beats[bt.id].empty()) bs.beats[bt.id].push_back(id);
        if (first.empty()) first = id;
    }
    plan::setBookStructure(*lib_, pl_.book, bs);
    pl_.appearances = lib_->appearancesIn(pl_.book);
    if (!first.empty()) selectCard(first);
    flash(L"Added a chapter for each beat. Rename and rewrite them as your story takes shape.", 6);
}

// ------------------------------------------------------------ the panel

// When it happens (all views), and the timeline's unit (the book, in the timeline).
void StudioApp::planPanelExtras(const Rect& c, float& y) {
    const ManuscriptNode* n = pl_.node.empty() ? nullptr : lib_->node(pl_.node);
    if (n && !n->deleted && n->kind != NodeKind::Part) {
        panelHeading(c, y, L"WHEN IN THE STORY", false);
        if (timelineInCalendar(pl_.book)) {
            {
                // An example in this calendar's own way of writing dates (0.80: words or numbers).
                const cal::Calendar cc = cal::calendarOfBook(*lib_, pl_.book);
                const std::int64_t y0 = cc.eras.empty() ? 1204 : cc.eras.back().start + 203;
                const std::wstring ex = cc.empty() ? L"3 Frostmoon 1204" : w(cal::formatDate(cc, {y0, std::min(1, (int)cc.months.size() - 1), 3}));
                y = panelField(c, y, L"Date (" + ex + L"; blank: not on the timeline)", FStoryDay);
            }
            y = panelField(c, y, L"Time of day (14:30, dawn, evening; optional)", FTimeOfDay);
        } else {
            const std::wstring unit = w(timelineUnit(pl_.book));
            y = panelField(c, y, std::wstring(L"Story ") + unit + L" (a number; blank: not on the timeline)", FStoryDay);
        }
        y = panelField(c, y, L"When, in words (optional)", FWhen);
    } else if (!n && pl_.view == 1) {
        panelHeading(c, y, L"THE TIMELINE", false);
        const std::string unit = timelineUnit(pl_.book);
        static const char* units[] = {"day", "week", "month", "year", "calendar"};
        static const wchar_t* names[] = {L"Days", L"Weeks", L"Months", L"Years", L"Calendar dates"};
        const bool hasCal = !cal::calendarOfBook(*lib_, pl_.book).empty();
        int cur = 0;
        for (int i = 0; i < 4; ++i) if (unit == units[i]) cur = i;
        if (timelineInCalendar(pl_.book)) cur = 4;
        panelChoice("pl:unit", c, y, L"Counts in", names[cur], [this, cur, hasCal] {
            std::vector<MenuItem> items;
            for (int i = 0; i < 4; ++i) items.push_back(MenuItem::item(names[i], (UINT)(i + 1), {}, true, i == cur));
            items.push_back(MenuItem::item(hasCal ? names[4] : L"Calendar dates (make the world's calendar first)", 5, {}, hasCal, cur == 4));
            POINT pt;
            GetCursorPos(&pt);
            const UINT c2 = showMenu(hwnd(), items, pt, pal_);
            if (c2) { commitFields(); setTimelineUnit(pl_.book, units[c2 - 1]); loadedFocus_ = {Focus::None, "-", ""}; invalidate(); }
        });
        if (cur == 4) {
            y += gfx_.text(L"Story days are the world's calendar dates: give a card its date in its details, or drag it along a day at a "
                           L"time. Full and new moons show along the top.", {c.x, y, c.w, 120}, Font::Small, pal_.inkMuted) + 14;
        } else
            y += gfx_.text(L"Start counting wherever you like; words such as \"Midsummer, 1204\" can show instead.",
                           {c.x, y, c.w, 120}, Font::Small, pal_.inkMuted) + 14;
    }
}

// ------------------------------------------------------------ plot threads, beats, your structures (0.50)

bool StudioApp::nodeShown(const ManuscriptNode* n) const {
    if (!n) return false;
    if (pl_.status >= 0 && (int)n->status != pl_.status) return false;
    if (!pl_.pov.empty() && (pl_.pov == "-" ? !(n->povId.empty() || !lib_->entry(n->povId)) : n->povId != pl_.pov)) return false;
    if (!pl_.thread.empty()) {
        const plan::Threads th = plan::threadsOf(*lib_, pl_.book);
        if (!th.has(n->id, pl_.thread)) {
            // A chapter shows when one of its scenes is in the thread.
            bool inside = false;
            if (n->kind == NodeKind::Chapter)
                for (auto& [id, ts] : th.nodes)
                    if (const ManuscriptNode* c = lib_->node(id); c && !c->deleted && c->parentId == n->id && th.has(id, pl_.thread)) inside = true;
            if (!inside) return false;
        }
    }
    return true;
}

void StudioApp::selectThread(const std::string& id) {
    commitFields();
    pl_.node.clear();
    pl_.beat.clear();
    setFocus(id.empty() ? Focus{Focus::Plan, pl_.book, {}} : Focus{Focus::Thread, id, pl_.book});
    invalidate();
}

void StudioApp::selectBeat(const std::string& id) {
    commitFields();
    pl_.node.clear();
    pl_.beat = id;
    setFocus(id.empty() ? Focus{Focus::Plan, pl_.book, {}} : Focus{Focus::Beat, id, pl_.book});
    invalidate();
}

void StudioApp::newThread(const Id& tagNode) {
    commitFields();
    plan::Threads th = plan::threadsOf(*lib_, pl_.book);
    plan::Thread t{plan::newThreadId(th), "", plan::threadColour(th.threads.size()), ""};
    t.name = "Thread " + std::to_string(th.threads.size() + 1);
    th.threads.push_back(t);
    if (!tagNode.empty()) th.nodes[tagNode].push_back(t.id);
    plan::setThreads(*lib_, pl_.book, th);
    selectThread(t.id);
    focusFieldSoon_ = FName;   // its name, all selected, once its details are drawn
}

void StudioApp::loadPlanFields() {
    if (focus_.kind == Focus::Thread) {
        const plan::Threads th = plan::threadsOf(*lib_, focus_.contextSeries);
        if (const plan::Thread* t = th.find(focus_.id)) {
            setFieldText(FName, t->name);
            setFieldText(FNotes, withCrLf(t->about));
        }
    } else if (focus_.kind == Focus::Beat) {
        const plan::BookStructure bs = plan::bookStructure(*lib_, focus_.contextSeries);
        auto it = bs.notes.find(focus_.id);
        setFieldText(FNotes, withCrLf(it != bs.notes.end() ? it->second : std::string()));
    } else if (focus_.kind == Focus::Structure) {
        for (auto& s : plan::ownStructures(*lib_, plan::structureScope(*lib_, focus_.contextSeries)))
            if (s.id == focus_.id) setFieldText(FTemplate, withCrLf(plan::structureText(s)));
    }
}

void StudioApp::commitPlanFields(const Focus& f) {
    if (!lib_->book(f.contextSeries)) return;
    if (f.kind == Focus::Thread) {
        plan::Threads th = plan::threadsOf(*lib_, f.contextSeries);
        for (auto& t : th.threads)
            if (t.id == f.id) {
                const std::string nm = fieldText(FName), about = fieldText(FNotes);
                if ((nm.empty() || nm == t.name) && about == t.about) return;
                if (!nm.empty()) t.name = nm;
                t.about = about;
                plan::setThreads(*lib_, f.contextSeries, th);
            }
    } else if (f.kind == Focus::Beat) {
        plan::BookStructure bs = plan::bookStructure(*lib_, f.contextSeries);
        const std::string note = fieldText(FNotes);
        auto it = bs.notes.find(f.id);
        if ((it == bs.notes.end() ? std::string() : it->second) == note) return;
        bs.notes[f.id] = note;
        plan::setBookStructure(*lib_, f.contextSeries, bs);
    } else if (f.kind == Focus::Structure) {
        const std::string scope = plan::structureScope(*lib_, f.contextSeries);
        auto own = plan::ownStructures(*lib_, scope);
        const std::string text = fieldText(FTemplate);
        for (auto& s : own) {
            if (s.id != f.id || plan::structureText(s) == text + (text.empty() || text.back() == '\n' ? "" : "\n")) continue;
            auto parsed = plan::parseStructures(text);
            if (parsed.empty()) { flash(L"That structure needs at least one beat, such as “50% Midpoint: what happens”. It wasn't changed.", 8); return; }
            parsed[0].id = s.id;
            s = parsed[0];
            plan::setOwnStructures(*lib_, scope, own);
            return;
        }
    }
}

// The panel for a plot thread: its name, colour and notes, and where it runs.
void StudioApp::drawThreadPanel(const Rect& c, float& y) {
    const plan::Threads th = plan::threadsOf(*lib_, pl_.book);
    const plan::Thread* t = th.find(focus_.id);
    if (!t) { selectThread({}); return; }
    panelHeading(c, y, L"PLOT THREAD", true);
    y = panelField(c, y, L"Name", FName);
    int cur = -1;
    for (int i = 0; i < 10; ++i) if (plan::threadColour(i) == t->colour) cur = i;
    panelChoice("pl:thcolour", c, y, L"Colour", cur >= 0 ? kColourNames[cur] : w(t->colour), [this, cur, id = t->id] {
        std::vector<MenuItem> items;
        for (int i = 0; i < 10; ++i) items.push_back(MenuItem::item(kColourNames[i], (UINT)(i + 1), {}, true, i == cur));
        POINT pt;
        GetCursorPos(&pt);
        const UINT k = showMenu(hwnd(), items, pt, pal_);
        if (!k) return;
        commitFields();
        plan::Threads x = plan::threadsOf(*lib_, pl_.book);
        for (auto& tt : x.threads) if (tt.id == id) tt.colour = plan::threadColour(k - 1);
        plan::setThreads(*lib_, pl_.book, x);
        invalidate();
    });
    // The swatch beside the choice.
    gfx_.fillRounded({c.right() - 54, y - 35, 14, 14}, 3, cjui::hexColor(t->colour.c_str()));
    y = panelField(c, y, L"Notes: what it's about, where it's going", FNotes, 120);
    panelHeading(c, y, L"WHERE IT RUNS", false);
    int count = 0, chapterNo = 0;
    for (const ManuscriptNode* n : lib_->manuscriptOf(pl_.book)) {
        if (n->kind == NodeKind::Chapter) ++chapterNo;
        if (!th.has(n->id, t->id)) continue;
        ++count;
        if (count > 30) continue;
        const std::wstring lbl = (n->kind == NodeKind::Chapter ? std::to_wstring(chapterNo) + L". " : n->kind == NodeKind::Scene ? std::to_wstring(chapterNo) + L" \u00B7 " : std::wstring()) +
                                 (n->title.empty() ? w(label(n->kind)) : w(n->title));
        linkText("pl:thnode:" + n->id, c.x, y, lbl, [this, id = n->id] { selectCard(id); });
        y += 24;
    }
    if (count > 30) { gfx_.text(L"and " + std::to_wstring(count - 30) + L" more", {c.x, y, c.w, 18}, Font::Small, pal_.inkFaint, {Align::Left, false}); y += 24; }
    if (!count) {
        y += gfx_.text(L"Not in any chapter or scene yet. Click a card and tick this thread in its details.", {c.x, y, c.w, 60}, Font::Small,
                       pal_.inkMuted) + 10;
    }
    y += 8;
    const bool filtered = pl_.thread == t->id;
    linkText("pl:thfilter", c.x, y, filtered ? L"Show every card again" : L"Show only its cards", [this, id = t->id, filtered] {
        pl_.thread = filtered ? std::string() : id;
        pl_.scroll = 0;
        invalidate();
    });
    y += 30;
    linkText("pl:thdelete", c.x, y, L"Delete this thread", [this, id = t->id, name = t->name] {
        if (cjui::messageBox(hwnd(), L"Delete the plot thread “" + w(name) + L"”? The chapters and scenes stay as they are; "
                                     L"only the thread and its tags go.",
                             L"Delete a plot thread", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
            return;
        plan::Threads x = plan::threadsOf(*lib_, pl_.book);
        x.threads.erase(std::remove_if(x.threads.begin(), x.threads.end(), [&](const plan::Thread& tt) { return tt.id == id; }), x.threads.end());
        plan::setThreads(*lib_, pl_.book, x);
        if (pl_.thread == id) pl_.thread.clear();
        focus_ = {};   // nothing to save into
        selectThread({});
    });
    y += 30;
}

// The panel for a beat: what it is, and notes for this book.
void StudioApp::drawBeatPanel(const Rect& c, float& y) {
    const auto shapes = plan::allStructures(*lib_, plan::structureScope(*lib_, pl_.book));
    const plan::BookStructure bs = plan::bookStructure(*lib_, pl_.book);
    const Structure* tp = plan::findStructure(shapes, bs.templateId);
    const Beat* bt = nullptr;
    if (tp) for (auto& b : tp->beats) if (b.id == focus_.id) bt = &b;
    if (!bt) { selectBeat({}); return; }
    panelHeading(c, y, L"BEAT · " + std::to_wstring((int)std::round(bt->at * 100)) + L"% IN", true);
    y += gfx_.text(w(bt->name), {c.x, y, c.w, 60}, Font::Heading, pal_.ink) + 6;
    if (!bt->about.empty()) y += gfx_.text(w(bt->about), {c.x, y, c.w, 120}, Font::Small, pal_.inkMuted) + 14;
    y = panelField(c, y, L"Your notes for this book", FNotes, 150);
    panelHeading(c, y, L"CARRIED BY", false);
    int shown = 0;
    if (auto it = bs.beats.find(bt->id); it != bs.beats.end())
        for (const Id& id : it->second) {
            const ManuscriptNode* n = lib_->node(id);
            if (!n || n->deleted || n->bookId != pl_.book) continue;
            ++shown;
            linkText("pl:bnode:" + id, c.x, y, n->title.empty() ? w(label(n->kind)) : w(n->title), [this, id] { selectCard(id); });
            y += 24;
        }
    if (!shown)
        y += gfx_.text(L"No chapter yet. Choose the chapters that carry it from the box on its row.", {c.x, y, c.w, 60}, Font::Small,
                       pal_.inkMuted) + 10;
    y += 8;
}

// The panel for one of your own structures: the whole of it as text.
void StudioApp::drawOwnStructurePanel(const Rect& c, float& y) {
    const std::string scope = plan::structureScope(*lib_, pl_.book);
    const auto own = plan::ownStructures(*lib_, scope);
    const Structure* s = plan::findStructure(own, focus_.id);
    if (!s) { selectCard({}); return; }
    panelHeading(c, y, L"YOUR STRUCTURE", true);
    y += gfx_.text(L"Write it as text: its name, what it's for, then a line for each beat, starting with how far into the book it "
                   L"falls. Lines without a percentage are spaced out evenly.",
                   {c.x, y, c.w, 120}, Font::Small, pal_.inkMuted) + 10;
    y = panelField(c, y, L"The structure", FTemplate, 330);
    y += gfx_.text(L"Structure: The heist\nAbout: A job, a crew, a double-cross.\n0% The job: what they're after\n25% The crew: ...\n"
                   L"50% The plan goes wrong: ...",
                   {c.x, y, c.w, 120}, Font::Small, pal_.inkFaint) + 10;
    y += gfx_.text(L"Changes are kept when you click away. Renaming a beat clears the chapters matched to it.", {c.x, y, c.w, 60},
                   Font::Small, pal_.inkMuted) + 12;
    linkText("pl:owndelete", c.x, y, L"Delete this structure", [this, scope, id = s->id, name = s->name] {
        if (cjui::messageBox(hwnd(), L"Delete your structure “" + w(name) + L"”? Books that follow it go back to choosing one; "
                                     L"their chapters stay as they are.",
                             L"Delete a structure", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
            return;
        auto list = plan::ownStructures(*lib_, scope);
        list.erase(std::remove_if(list.begin(), list.end(), [&](const Structure& x) { return x.id == id; }), list.end());
        plan::setOwnStructures(*lib_, scope, list);
        // Books of this world (or this book) that followed it.
        for (const Book& bk : lib_->books())
            if (!bk.deleted && plan::structureScope(*lib_, bk.id) == scope) {
                plan::BookStructure bs = plan::bookStructure(*lib_, bk.id);
                if (bs.templateId == id) { bs.templateId.clear(); plan::setBookStructure(*lib_, bk.id, bs); }
            }
        focus_ = {};
        selectCard({});
    });
    y += 30;
}

// A card's plot threads, to tick on and off.
void StudioApp::cardThreads(const Rect& c, float& y) {
    const plan::Threads th = plan::threadsOf(*lib_, pl_.book);
    panelHeading(c, y, L"PLOT THREADS", false);
    for (const plan::Thread& t : th.threads) {
        const std::string id = "pl:tick:" + t.id;
        const bool on = th.has(pl_.node, t.id);
        const Rect rr{c.x, y, c.w, 26};
        addHit(id, rr, [this, tid = t.id] {
            commitFields();
            plan::Threads x = plan::threadsOf(*lib_, pl_.book);
            auto& v = x.nodes[pl_.node];
            auto it = std::find(v.begin(), v.end(), tid);
            if (it != v.end()) v.erase(it);
            else v.push_back(tid);
            plan::setThreads(*lib_, pl_.book, x);
            invalidate();
        });
        const float hv = hoverAmount(id);
        if (hv > 0.01f) gfx_.fillRounded(rr, 8, pal_.surface.withAlpha(0.7f * hv));
        const Rect box{rr.x + 2, rr.y + 6, 14, 14};
        gfx_.fillRounded(box, 3, on ? cjui::hexColor(t.colour.c_str()) : pal_.surface);
        gfx_.strokeRounded(box, 3, cjui::hexColor(t.colour.c_str()), 1.5f);
        if (on) {
            gfx_.line(box.x + 3, box.y + 7, box.x + 6, box.y + 10, cjui::hexColor("#FFFFFF"), 1.8f);
            gfx_.line(box.x + 6, box.y + 10, box.x + 11, box.y + 4, cjui::hexColor("#FFFFFF"), 1.8f);
        }
        gfx_.text(w(t.name), {rr.x + 24, rr.y, rr.w - 24, rr.h}, on ? Font::BodyStrong : Font::Body, pal_.ink, {Align::Left, false, true, true});
        if (focused(id)) focusRing(rr);
        y += 28;
    }
    if (th.threads.empty())
        y += gfx_.text(L"Follow a subplot, a relationship or a mystery through the book: make a thread, then tick it on each card it "
                       L"runs through.",
                       {c.x, y, c.w, 80}, Font::Small, pal_.inkMuted) + 8;
    y += 4;
    linkText("pl:newthread", c.x, y, L"+ New thread", [this] { newThread(pl_.node); });
    y += 30;
}

// The whole plan as text: from a file or what's copied, and out again (0.50).
void StudioApp::importPlanText() {
    std::vector<MenuItem> items{MenuItem::item(L"From a text file (.txt or .md)…", 1), MenuItem::item(L"From what you've copied", 2)};
    POINT pt;
    GetCursorPos(&pt);
    const UINT how = showMenu(hwnd(), items, pt, pal_);
    if (!how) return;
    std::string text;
    if (how == 1) {
        IFileOpenDialog* dlg = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
        COMDLG_FILTERSPEC spec[] = {{L"Text", L"*.txt;*.md;*.markdown;*.text"}};
        dlg->SetFileTypes(1, spec);
        dlg->SetTitle(L"Import planning: an outline, plot threads or structures");
        std::wstring path;
        if (SUCCEEDED(dlg->Show(hwnd()))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR p = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) { path = p; CoTaskMemFree(p); }
                item->Release();
            }
        }
        dlg->Release();
        if (path.empty()) return;
        std::ifstream f(path.c_str(), std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        if (!f && ss.str().empty()) { cjui::messageBox(hwnd(), L"Couldn't read that file.", L"Import planning", MB_OK | MB_ICONWARNING); return; }
        text = fileText(ss.str());
    } else {
        std::wstring ws;
        if (!clipboardTextOf(hwnd(), ws) || ws.empty()) { flash(L"There's no text copied. Copy your outline, threads or structure, then try again.", 6); return; }
        text = narrow(ws);
    }
    commitFields();
    // A plan with chapters replaces the book's plan (0.62). The question names the book (0.82), and warns when most of
    // the plan's chapter titles aren't the book's, since that plan may be meant for another book.
    if (!plan::parseOutline(text).empty()) {
        const Book* bk = lib_->book(pl_.book);
        const std::wstring title = bk ? L"\u201C" + w(bk->title) + L"\u201D" : L"this book";
        const plan::PlanFit fit = plan::planFit(*lib_, pl_.book, text);
        const bool poorFit = fit.bookChapters > 0 && fit.planned >= 2 && fit.matched * 2 < fit.planned;
        std::wstring q = L"Import this plan into " + title + L"?";
        if (poorFit)
            q += L"\n\nOnly " + std::to_wstring(fit.matched) + L" of the plan's " + std::to_wstring(fit.planned) +
                 L" chapter titles match chapters in " + title + L" (which has " + std::to_wstring(fit.bookChapters) +
                 L"). It may be meant for another book: choose Cancel, open that book's Planning, and import it there.";
        if (fit.bookChapters > 0)
            q += L"\n\nThe plan replaces the book's plan. Chapters already in the book are matched to it and take its synopsis, "
                 L"viewpoint, place, who's there, time, threads and beats, in its order; their titles and writing stay. "
                 L"Empty chapters it doesn't have go to the Trash. Chapters with writing are always kept. Plot threads the plan "
                 L"doesn't list are removed, and so is the book's structure when the plan names none.";
        else q += L"\n\nThe book has no chapters yet, so the plan's " + std::to_wstring(fit.planned) + L" chapters are made.";
        const int a = cjui::messageBox(hwnd(), q, L"Import planning", MB_OKCANCEL | (poorFit ? MB_ICONWARNING : MB_ICONINFORMATION));
        if (a != IDOK) return;
    }
    plan::PlanningImport r;
    plan::importPlanning(*lib_, pl_.book, plan::structureScope(*lib_, pl_.book), text, r);
    if (!r.any()) {
        cjui::messageBox(hwnd(), L"Nothing in that text was recognised as planning.\n\nAn outline uses lines such as “Chapter: The Forge” "
                                 L"(or headings), plot threads start “Thread:” and structures “Structure:”. The manual "
                                 L"has a template for each.",
                         L"Import planning", MB_OK | MB_ICONINFORMATION);
        return;
    }
    auto plural = [](int n, const wchar_t* one, const wchar_t* many) { return std::to_wstring(n) + L" " + (n == 1 ? one : many); };
    std::vector<std::wstring> made;
    const auto& o = r.outline;
    if (o.parts) made.push_back(plural(o.parts, L"part", L"parts"));
    if (o.chapters) made.push_back(plural(o.chapters, L"chapter", L"chapters"));
    if (o.scenes) made.push_back(plural(o.scenes, L"scene", L"scenes"));
    const int newThreads = o.threads + r.threads.added;
    if (newThreads) made.push_back(plural(newThreads, L"new plot thread", L"new plot threads"));
    if (r.structuresAdded) made.push_back(plural(r.structuresAdded, L"structure of your own", L"structures of your own"));
    std::wstring msg;
    if (!made.empty()) {
        msg = L"Added ";
        for (size_t i = 0; i < made.size(); ++i) msg += (i ? (i + 1 == made.size() ? L" and " : L", ") : L"") + made[i];
        msg += L".";
    }
    // Parts, chapters and scenes already in the book, filled in rather than added again (0.54).
    std::vector<std::wstring> filled;
    if (o.updatedParts) filled.push_back(plural(o.updatedParts, L"part", L"parts"));
    if (o.updatedChapters) filled.push_back(plural(o.updatedChapters, L"chapter", L"chapters"));
    if (o.updatedScenes) filled.push_back(plural(o.updatedScenes, L"scene", L"scenes"));
    if (!filled.empty()) {
        std::wstring f;
        for (size_t i = 0; i < filled.size(); ++i) f += (i ? (i + 1 == filled.size() ? L" and " : L", ") : L"") + filled[i];
        msg += (msg.empty() ? L"" : L"\n\n") + std::wstring(L"The plan replaced the old one for ") + f +
               L" already in the book (matched by title, ignoring numbers and symbols, or by chapter number), and they're now in the plan's order. Their titles and writing are as they were.";
    }
    if (r.structuresReplaced) msg += L"\n\nUpdated " + plural(r.structuresReplaced, L"structure", L"structures") + L" of yours with the same name.";
    if (r.threads.updated) msg += L"\n\nUpdated " + plural(r.threads.updated, L"thread", L"threads") + L" you already had.";
    if (!o.structure.empty()) msg += L"\n\nThe book follows " + w(o.structure) + (o.beats ? L", with " + plural(o.beats, L"beat", L"beats") + L" matched." : L".");
    if (o.unknownStructure) msg += L"\n\nThe structure named on the “Book structure” line isn't one you have, so none was set.";
    auto listOf = [](const std::vector<std::string>& v) {
        std::wstring s;
        for (size_t i = 0; i < v.size() && i < 8; ++i) s += (i ? L", " : L"") + widen(v[i]);
        if (v.size() > 8) s += L"…";
        return s;
    };
    // What the plan replaced with nothing (0.83).
    if (!r.removedThreads.empty())
        msg += L"\n\nRemoved " + plural((int)r.removedThreads.size(), L"plot thread", L"plot threads") + L" the plan doesn't list: " +
               listOf(r.removedThreads) + L".";
    if (!o.removedStructure.empty())
        msg += L"\n\nThe plan names no structure, so the book no longer follows " + w(o.removedStructure) +
               L" (add a \u201CBook structure:\u201D line to keep one).";
    if (!o.unknownViewpoints.empty()) msg += L"\n\nNo character of that name, so no viewpoint was set: " + listOf(o.unknownViewpoints) + L".";
    if (!o.unknownBeats.empty()) msg += L"\n\nNot beats of the book's structure: " + listOf(o.unknownBeats) + L".";
    if (o.datesRead)
        msg += L"\n\nRead " + plural(o.datesRead, L"date", L"dates") + L" in the world's calendar and put " + (o.datesRead == 1 ? L"it" : L"them") +
               L" on the timeline.";
    if (!o.unreadDates.empty()) {
        // Kept as words, never Day 0 (0.82); a calendar made or changed later reads them again.
        const bool hasCal = !cal::calendarOfBook(*lib_, pl_.book).empty();
        msg += L"\n\n" + plural((int)o.unreadDates.size(), L"date wasn't", L"dates weren't") +
               (hasCal ? L" read in the world's calendar" : L" read, because the world has no calendar yet") +
               (o.unreadDates.size() == 1 ? L", so it's kept as words and isn't on the timeline: " : L", so they're kept as words and aren't on the timeline: ") +
               listOf(o.unreadDates) + L"." +
               (hasCal ? L" Check the month and era names against the calendar; saving the calendar reads them again."
                       : L" Make or import the calendar and they'll be read then.");
    }
    if (!o.unknownCast.empty()) msg += L"\n\nNo character or creature of that name, so they weren't added to who's present: " + listOf(o.unknownCast) + L".";
    if (!r.threads.unknown.empty()) msg += L"\n\nThreads named chapters that aren't in the book: " + listOf(r.threads.unknown) + L".";
    if (o.trashed) msg += L"\n\nMoved " + plural(o.trashed, L"empty part, chapter or scene", L"empty parts, chapters and scenes") +
                          L" the plan doesn't have to the Trash (Library, Trash brings them back).";
    if (!o.keptWithWriting.empty())
        msg += L"\n\nKept, because they have writing in them, though the plan doesn't have them: " + listOf(o.keptWithWriting) + L".";
    pl_.appearances = lib_->appearancesIn(pl_.book);
    if (!o.made.empty()) pl_.node.clear();
    selectCard({});
    cjui::messageBox(hwnd(), msg.empty() ? std::wstring(L"Done.") : msg, L"Import planning", MB_OK | MB_ICONINFORMATION);
}

void StudioApp::exportPlanText() {
    const Book* b = lib_->book(pl_.book);
    if (!b) return;
    commitFields();
    std::vector<MenuItem> items{MenuItem::item(L"Save as a text file…", 1), MenuItem::item(L"Copy it", 2)};
    POINT pt;
    GetCursorPos(&pt);
    const UINT how = showMenu(hwnd(), items, pt, pal_);
    if (!how) return;
    const std::string text = plan::planningText(*lib_, pl_.book, plan::structureScope(*lib_, pl_.book));
    if (text.empty()) { flash(L"There's nothing planned yet to export.", 5); return; }
    if (how == 2) {
        std::wstring ws = widen(text), crlf;
        for (wchar_t ch : ws) { if (ch == L'\n') crlf += L'\r'; crlf += ch; }
        flash(copyToClipboard(hwnd(), crlf) ? L"Copied the plan. Paste it anywhere, or into another book with Import planning." : L"Couldn't copy it.", 5);
        return;
    }
    const std::wstring path = askSavePath(L"Text", L"*.txt", L"txt", w(b->title) + L" - plan", L"Export the plan");
    if (path.empty()) return;
    std::ofstream out(path.c_str(), std::ios::binary);
    out << "\xEF\xBB\xBF" << text;
    if (!out) { cjui::messageBox(hwnd(), L"Couldn't save the plan there.", L"Export planning", MB_OK | MB_ICONWARNING); return; }
    flash(L"Saved the plan: outline, plot threads and structure.", 5);
}

}  // namespace nrs
