// NorthRay Writing Studio: the calendar maker and the month view (0.74).
//
// Bible tools › Calendar. A world's calendar (or an independent book's) is made on a form, piece by piece: months,
// the week, leap years, eras, moons, seasons, holidays and the hours of a day, with the chosen month drawn live in a
// card beside it. The month view is the calendar as a page of the bible: a month at a time with its holidays, the
// seasons' first days and the moons. Every value is edited in place with the one field FWhen (Focus::Calendar: id the
// calendar's setting key, contextSeries what's being edited, such as "m:2:name").

#include "studio_app.hpp"
#include "cjui/dialog.hpp"
#include "cjui/menu.hpp"

#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace nrs {
namespace cal = studio::cal;

using namespace studio;
using cjui::Align;
using cjui::Font;
using cjui::Rect;

static std::wstring w(const std::string& s) { return widen(s); }
std::vector<Id> booksInReadingOrder(const Library& lib, const Id& world, const Id& book);   // studio_world.cpp

namespace {

std::string textFromFileBytes(const std::string& b) {
    if (b.size() >= 3 && (unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB && (unsigned char)b[2] == 0xBF) return b.substr(3);
    if (b.size() >= 2 && (((unsigned char)b[0] == 0xFF && (unsigned char)b[1] == 0xFE) || ((unsigned char)b[0] == 0xFE && (unsigned char)b[1] == 0xFF))) {
        const bool be = (unsigned char)b[0] == 0xFE;
        std::wstring ws;
        for (size_t i = 2; i + 1 < b.size(); i += 2)
            ws += (wchar_t)(be ? ((unsigned char)b[i] << 8 | (unsigned char)b[i + 1]) : ((unsigned char)b[i + 1] << 8 | (unsigned char)b[i]));
        return narrow(ws);
    }
    return b;
}

std::string numText(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s) {
        if (ch == sep) { out.push_back(cur); cur.clear(); }
        else cur += ch;
    }
    out.push_back(cur);
    return out;
}

// The value behind an edit target, and setting it. Targets: name, hours, minutes, before:name, before:abbr,
// m:<i>:name|days, w:<i>, leap:<i>:every|add|offset, era:<i>:name|abbr|start, moon:<i>:name|cycle|offset,
// season:<i>:name|day, hol:<i>:name|day|notes.
std::string calGet(const cal::Calendar& c, const std::string& t) {
    const auto p = split(t, ':');
    auto at = [&](size_t n) { return p.size() > 1 ? (size_t)std::atoi(p[1].c_str()) < n : false; };
    const size_t i = p.size() > 1 ? (size_t)std::atoi(p[1].c_str()) : 0;
    const std::string f = p.size() > 2 ? p[2] : "";
    if (t == "name") return c.name;
    if (t == "hours") return std::to_string(c.hoursPerDay);
    if (t == "minutes") return std::to_string(c.minutesPerHour);
    if (t == "before:name") return c.beforeName;
    if (t == "before:abbr") return c.beforeAbbr;
    if (p[0] == "m" && at(c.months.size())) return f == "days" ? std::to_string(c.months[i].days) : c.months[i].name;
    if (p[0] == "w" && at(c.weekdays.size())) return c.weekdays[i];
    if (p[0] == "leap" && at(c.leap.size()))
        return std::to_string(f == "every" ? c.leap[i].every : f == "add" ? c.leap[i].add : c.leap[i].offset);
    if (p[0] == "era" && at(c.eras.size())) return f == "abbr" ? c.eras[i].abbr : f == "start" ? std::to_string(c.eras[i].start) : c.eras[i].name;
    if (p[0] == "moon" && at(c.moons.size())) return f == "cycle" ? numText(c.moons[i].cycle) : f == "offset" ? numText(c.moons[i].offset) : c.moons[i].name;
    if (p[0] == "season" && at(c.seasons.size())) return f == "day" ? std::to_string(c.seasons[i].day) : c.seasons[i].name;
    if (p[0] == "hol" && at(c.holidays.size()))
        return f == "day" ? std::to_string(c.holidays[i].day) : f == "notes" ? c.holidays[i].notes : c.holidays[i].name;
    return {};
}

bool calSet(cal::Calendar& c, const std::string& t, const std::string& v0) {
    std::string v = v0;
    while (!v.empty() && std::isspace((unsigned char)v.back())) v.pop_back();
    while (!v.empty() && std::isspace((unsigned char)v.front())) v.erase(v.begin());
    const auto p = split(t, ':');
    const size_t i = p.size() > 1 ? (size_t)std::atoi(p[1].c_str()) : 0;
    const std::string f = p.size() > 2 ? p[2] : "";
    auto number = [&](long long& out) {
        if (v.empty()) return false;
        char* end = nullptr;
        out = std::strtoll(v.c_str(), &end, 10);
        return end && *end == 0;
    };
    auto real = [&](double& out) {
        if (v.empty()) return false;
        char* end = nullptr;
        out = std::strtod(v.c_str(), &end);
        return end && *end == 0;
    };
    long long n = 0;
    double d = 0;
    if (t == "name") { c.name = v; return true; }
    if (t == "hours") { if (!number(n) || n < 1 || n > 1000) return false; c.hoursPerDay = (int)n; return true; }
    if (t == "minutes") { if (!number(n) || n < 1 || n > 1000) return false; c.minutesPerHour = (int)n; return true; }
    if (t == "before:name") { c.beforeName = v; return true; }
    if (t == "before:abbr") { c.beforeAbbr = v; return true; }
    if (p[0] == "m" && i < c.months.size()) {
        if (f == "days") { if (!number(n) || n < 0 || n > 10000) return false; c.months[i].days = (int)n; return true; }
        c.months[i].name = v;
        return true;
    }
    if (p[0] == "w" && i < c.weekdays.size()) { c.weekdays[i] = v; return true; }
    if (p[0] == "leap" && i < c.leap.size()) {
        if (!number(n)) return false;
        if (f == "every") { if (n < 1) return false; c.leap[i].every = (int)n; }
        else if (f == "add") c.leap[i].add = (int)n;
        else c.leap[i].offset = (int)n;
        return true;
    }
    if (p[0] == "era" && i < c.eras.size()) {
        if (f == "start") { if (!number(n)) return false; c.eras[i].start = n; }
        else if (f == "abbr") c.eras[i].abbr = v;
        else c.eras[i].name = v;
        return true;
    }
    if (p[0] == "moon" && i < c.moons.size()) {
        if (f == "cycle") { if (!real(d) || d <= 0) return false; c.moons[i].cycle = d; }
        else if (f == "offset") { if (!real(d)) return false; c.moons[i].offset = d; }
        else c.moons[i].name = v;
        return true;
    }
    if (p[0] == "season" && i < c.seasons.size()) {
        if (f == "day") { if (!number(n) || n < 1) return false; c.seasons[i].day = (int)n; }
        else c.seasons[i].name = v;
        return true;
    }
    if (p[0] == "hol" && i < c.holidays.size()) {
        if (f == "day") { if (!number(n) || n < 1) return false; c.holidays[i].day = (int)n; }
        else if (f == "notes") c.holidays[i].notes = v;
        else c.holidays[i].name = v;
        return true;
    }
    return false;
}

}  // namespace

std::string StudioApp::calendarKey() const {
    if (!wb_.world.empty()) return cal::settingKeyForWorld(wb_.world);
    if (!wb_.book.empty()) return cal::settingKeyForBook(wb_.book);
    return {};
}

cal::Calendar StudioApp::scopeCalendar() const {
    const std::string key = calendarKey();
    return key.empty() || !lib_ ? cal::Calendar{} : cal::fromJson(lib_->setting(key));
}

void StudioApp::saveScopeCalendar(const cal::Calendar& c) {
    const std::string key = calendarKey();
    if (key.empty()) return;
    lib_->setSetting(key, c.empty() ? std::string() : cal::toJson(c));
    invalidate();
}

void StudioApp::changeCalendar(const std::function<void(cal::Calendar&)>& f) {
    commitFields();
    // A change may move what's being edited (a month inserted before it): stop editing first.
    if (focus_.kind == Focus::Calendar) setFocus({});
    cal::Calendar c = scopeCalendar();
    f(c);
    saveScopeCalendar(c);
}

void StudioApp::openCalendar() {
    openEntry({});
    wb_.calendarPage = true;
    wb_.scroll = 0;
    setFocus({});
    const cal::Calendar c = scopeCalendar();
    wb_.calMonth = std::clamp(wb_.calMonth, 0, std::max(0, (int)c.months.size() - 1));
    // The first time, the latest era's first year (or this year, in the Gregorian calendar).
    if (wb_.calYear == 1 && !c.eras.empty()) wb_.calYear = c.name == "Gregorian" ? 2026 : c.eras.back().start;
    invalidate();
}

void StudioApp::loadCalendarFields() {
    setFieldText(FWhen, calGet(scopeCalendar(), focus_.contextSeries));
}

void StudioApp::commitCalendarFields(const Focus& f) {
    if (f.id != calendarKey()) return;
    const std::string v = fieldText(FWhen);
    if (f.contextSeries == "goto") {
        const cal::Calendar c = scopeCalendar();
        if (const auto d = cal::parseDate(c, v)) { wb_.calYear = d->year; wb_.calMonth = d->month; }
        else {
            // A year alone, with its era if given ("1204", "1204 AE", "-12").
            const size_t sp = v.find(' ');
            const std::string num = v.substr(0, sp), era = sp == std::string::npos ? std::string() : v.substr(sp + 1);
            bool isYear = !num.empty() && num.find_first_not_of("-0123456789") == std::string::npos && num.find('-', 1) == std::string::npos &&
                          num != "-";
            bool eraKnown = era.empty();
            for (auto& e : c.eras) if (e.abbr == era || e.name == era) eraKnown = true;
            if (!era.empty() && (era == c.beforeAbbr || era == c.beforeName)) eraKnown = true;
            if (isYear && eraKnown) wb_.calYear = cal::plainYear(c, std::strtoll(num.c_str(), nullptr, 10), era);
            else if (!v.empty()) flash(L"That isn't a date or a year in this calendar.", 4);
        }
        return;
    }
    cal::Calendar c = scopeCalendar();
    if (calSet(c, f.contextSeries, v)) saveScopeCalendar(c);
    else if (!v.empty()) flash(L"That doesn't fit there: a number was expected.", 4);
}

// A month as a grid of days, with its holidays, the seasons that begin in it, and each moon's phase. `big` for the
// month view (room for names); small for the maker's preview. Returns the height drawn.
float StudioApp::drawMonthGrid(const Rect& r, const cal::Calendar& c, std::int64_t year, int month, bool big,
                               std::function<void(int)> pick, const std::string& hitPrefix) {
    if (c.months.empty()) return 0;
    month = std::clamp(month, 0, (int)c.months.size() - 1);
    const int days = cal::monthLength(c, year, month);
    const int cols = c.weekdays.empty() ? 7 : (int)c.weekdays.size();
    const std::int64_t first = cal::toDay(c, {year, month, 1});
    const int lead = c.weekdays.empty() ? 0 : cal::weekday(c, first);
    const int rows = std::max(1, (lead + days + cols - 1) / cols);
    const float cw = r.w / cols;
    const float ch = big ? std::max(84.0f, cw * 0.72f) : std::max(30.0f, std::min(44.0f, cw * 0.9f));
    float y = r.y;
    if (!c.weekdays.empty()) {
        for (int i = 0; i < cols; ++i) {
            std::wstring n = w(c.weekdays[i]);
            if (!big && n.size() > 3) n = n.substr(0, 2);
            gfx_.text(big ? n : n, {r.x + i * cw, y, cw, 20}, big ? Font::SmallStrong : Font::Label, pal_.inkMuted,
                      {Align::Center, false, true, true});
        }
        y += big ? 26 : 22;
    }
    for (int k = 0; k < rows * cols; ++k) {
        const int dayNo = k - lead + 1;
        const Rect cell{r.x + (k % cols) * cw, y + (k / cols) * ch, cw, ch};
        if (dayNo < 1 || dayNo > days) {
            if (big) gfx_.fillRounded(cell.inset(2), 6, pal_.sunken.withAlpha(0.35f));
            continue;
        }
        const cal::Date d{year, month, dayNo};
        const auto hols = cal::holidaysOn(c, d);
        if (pick) {
            const std::string hid = hitPrefix + std::to_string(dayNo);
            addHit(hid, cell, [pick, dayNo] { pick(dayNo); });
            if (const float hv = hoverAmount(hid); hv > 0.01f) gfx_.fillRounded(cell.inset(2), 5, pal_.ribbon.withAlpha(0.14f * hv));
        }
        bool seasonStarts = false;
        std::wstring seasonName;
        for (auto& s : c.seasons) if (s.month == month && s.day == dayNo) { seasonStarts = true; seasonName = w(s.name); }
        if (big) {
            gfx_.fillRounded(cell.inset(2), 6, hols.empty() ? pal_.surface : gold_.withAlpha(0.12f));
            gfx_.strokeRounded(cell.inset(2), 6, pal_.rule);
        } else if (!hols.empty()) {
            gfx_.fillRounded(cell.inset(2), 5, gold_.withAlpha(0.16f));
        }
        gfx_.text(std::to_wstring(dayNo), {cell.x + (big ? 10 : 0), cell.y + (big ? 6 : 0), big ? cell.w - 20 : cell.w, big ? 20 : cell.h},
                  big ? Font::BodyStrong : Font::Small, pal_.ink, {big ? Align::Left : Align::Center, false, true, !big});
        // The moons: a small disc each, lit by the phase, with the phase's name to point at.
        const std::int64_t dn = first + dayNo - 1;
        float mx = cell.right() - (big ? 16 : 8);
        for (size_t mi = 0; mi < c.moons.size() && mi < 3; ++mi) {
            const double ph = cal::moonPhase(c.moons[mi], dn);
            const std::string name = cal::phaseName(ph);
            if (!big && name != "Full moon" && name != "New moon") continue;
            const float rad = big ? 7.5f : 3.5f;
            const float my = cell.y + (big ? 16 : 7);
            // The disc in shadow, then its lit part row by row: waxing lit from the right, waning from the left.
            gfx_.circle(mx, my, rad, pal_.ink.withAlpha(0.55f), true);
            const float k = (float)std::cos(ph * 2 * 3.14159265358979);
            for (float dy = -rad + 0.5f; dy < rad; dy += 0.5f) {
                const float hw = std::sqrt(std::max(0.0f, rad * rad - dy * dy));
                const float a = ph <= 0.5 ? mx + hw * k : mx - hw, b = ph <= 0.5 ? mx + hw : mx - hw * k;
                if (b - a > 0.2f) gfx_.line(a, my + dy, b, my + dy, gold_.mix(pal_.surface, 0.35f), 0.6f);
            }
            gfx_.circle(mx, my, rad, pal_.inkFaint, false, 1);
            if (big) tooltip("calmoon:" + std::to_string(dn) + ":" + std::to_string(mi), w(c.moons[mi].name + ": " + name));
            mx -= rad * 2 + 4;
        }
        if (big) {
            float ty = cell.y + 28;
            for (const cal::Holiday* h : hols) {
                const Rect chip{cell.x + 8, ty, cell.w - 16, 20};
                gfx_.text(w(h->name), chip, Font::Small, gold_, {Align::Left, false, true, true});
                if (!h->entry.empty())
                    addHit("calhol:" + std::to_string(dn) + h->name, chip, [this, id = h->entry] { if (lib_->entry(id)) openEntry(id); });
                if (!h->notes.empty()) tooltip("calhol:" + std::to_string(dn) + h->name, w(h->notes));
                ty += 20;
                if (ty > cell.bottom() - 18) break;
            }
            if (seasonStarts && ty <= cell.bottom() - 18) {
                gfx_.text(seasonName + L" begins", {cell.x + 8, ty, cell.w - 16, 18}, Font::Small, pal_.ribbon, {Align::Left, false, true, true});
                ty += 18;
            }
            // The bible's dates that fall on it (this year): births, deaths, events, anything dated (0.75).
            for (const cal::DatedFact& f : wb_.facts) {
                if (f.day != dn) continue;
                if (ty > cell.bottom() - 18) break;
                const Entry* en = lib_->entry(f.entry);
                if (!en) continue;
                const std::wstring t = f.kind == cal::DatedFact::Birth ? w(en->name) + L" born"
                                     : f.kind == cal::DatedFact::Death ? w(en->name) + L" dies"
                                     : f.kind == cal::DatedFact::EventDate ? w(en->name)
                                                                           : w(en->name) + L": " + w(f.label);
                const Rect fr{cell.x + 8, ty, cell.w - 16, 18};
                const std::string fid = "calfact:" + std::to_string(dn) + ":" + f.entry + f.label;
                addHit(fid, fr, [this, id = f.entry] { openEntry(id); });
                tooltip(fid, t);
                gfx_.text(t, fr, Font::Small, hoverAmount(fid) > 0.3f ? pal_.ribbon : pal_.ink, {Align::Left, false, true, true});
                ty += 18;
            }
        } else if (seasonStarts) {
            gfx_.fill({cell.x + cell.w * 0.3f, cell.bottom() - 4, cell.w * 0.4f, 2}, pal_.ribbon);
        }
    }
    return y + rows * ch - r.y;
}

void StudioApp::drawCalendar(const Rect& r) {
    gfx_.pushClip(r);
    setHitClip(r);
    const float pad = r.w > 900 ? 40 : 28;
    const float cw = std::min(r.w - pad * 2, wb_.calMode == 0 ? 1100.0f : 820.0f);
    const Rect m{r.x + pad, r.y + 26 - wb_.scroll, cw, 0};
    float y = m.y;
    const std::string key = calendarKey();
    const cal::Calendar c = scopeCalendar();
    std::wstring scopeName;
    if (const World* wo = lib_->world(wb_.world)) scopeName = w(wo->name);
    else if (const studio::Book* b = lib_->book(wb_.book)) scopeName = w(b->title);
    gfx_.text(L"WORLDBUILDING", {m.x, y, m.w, 16}, Font::Label, gold_, {Align::Left, false});
    y += 22;
    gfx_.text(c.empty() ? L"Calendar" : w(c.name.empty() ? std::string("Calendar") : c.name), {m.x, y, m.w - 220, 40}, Font::SerifTitle,
              pal_.ink, {Align::Left, false, true});
    if (!c.empty()) {
        menuButton("cal:tools", m.right() - (gfx_.measureWidth(L"Calendar tools", Font::Body) + 44), y + 4, L"Calendar tools", [this] {
            std::vector<ToolItem> t;
            t.push_back(ToolItem::item(L"Copy as text", [this] { copyCalendarText(); }));
            t.push_back(ToolItem::item(L"Export as a text file…", [this] { exportCalendarText(); }));
            t.push_back(ToolItem::item(L"Import from a text file…", [this] { importCalendarText(); }));
            t.push_back(ToolItem::line());
            t.push_back(ToolItem::item(L"Start again from the Gregorian calendar", [this] { startCalendar(cal::gregorian()); }));
            t.push_back(ToolItem::item(L"Start again from a blank calendar", [this] { startCalendar(cal::blank()); }));
            t.push_back(ToolItem::item(L"Remove the calendar", [this] {
                const cal::Calendar before = scopeCalendar();
                setFocus({});
                saveScopeCalendar({});
                flashUndo(L"Removed the calendar.", [this, before] { saveScopeCalendar(before); });
            }));
            return t;
        });
    }
    y += 48;
    if (c.empty()) {
        y += gfx_.text(L"The calendar " + (scopeName.empty() ? std::wstring(L"this world") : scopeName) +
                       L" keeps time by: its months and weeks, years and eras, leap years, moons, seasons and holidays. "
                       L"Dates in the bible and on Planning's timeline will use it.",
                       {m.x, y, std::min(m.w, 700.0f), 60}, Font::Body, pal_.inkMuted) + 18;
        float bx = m.x, bw = 0;
        smallButton("cal:start:g", bx, y, L"Start from the Gregorian calendar", [this] { startCalendar(cal::gregorian()); }, &bw);
        bx += bw + 10;
        smallButton("cal:start:b", bx, y, L"Start blank", [this] { startCalendar(cal::blank()); }, &bw);
        bx += bw + 10;
        smallButton("cal:start:i", bx, y, L"Import from a text file…", [this] { importCalendarText(); }, &bw);
        y += 52;
        y += gfx_.text(L"The manual has a calendar written as text to start from.", {m.x, y, m.w, 22}, Font::Small, pal_.inkFaint) + 10;
        wb_.content = y + wb_.scroll - r.y + 40;
        clearHitClip();
        gfx_.popClip();
        return;
    }
    {
        std::vector<PageTab> tabs;
        tabs.push_back({L"Month by month", -1, wb_.calMode == 0, [this] { commitFields(); setFocus({}); wb_.calMode = 0; wb_.scroll = 0; invalidate(); }});
        tabs.push_back({L"Make the calendar", -1, wb_.calMode == 1, [this] { commitFields(); wb_.calMode = 1; wb_.scroll = 0; invalidate(); }});
        y += pageTabs("cal:tabs", m.x, y, m.right(), tabs) + 18;
    }
    const auto probs = cal::problems(c);
    if (!probs.empty()) {
        std::wstring all;
        for (auto& p : probs) all += (all.empty() ? L"" : L"\n") + w(p);
        const float h = gfx_.measureHeight(all, m.w - 32, Font::Small) + 24;
        gfx_.fillRounded({m.x, y, m.w, h}, 8, pal_.live.withAlpha(0.08f));
        gfx_.text(all, {m.x + 16, y + 12, m.w - 32, h}, Font::Small, pal_.live);
        y += h + 16;
    }
    const bool editing = focus_.kind == Focus::Calendar && focus_.id == key;
    // An editable value: the field over it while it's being edited, else its text and a click to edit.
    auto cell = [&](const std::string& target, const Rect& box, const std::wstring& shown, const std::wstring& cue = L"") {
        const bool on = editing && focus_.contextSeries == target;
        gfx_.fillRounded(box, 7, pal_.surface);
        gfx_.strokeRounded(box, 7, on ? pal_.ribbon : pal_.rule);
        if (on) {
            if (box.y >= r.y && box.bottom() <= r.bottom()) placeField(FWhen, box.inset(8, 6));
            return;
        }
        const std::string hid = "cal:e:" + target;
        const float hv = hoverAmount(hid);
        if (hv > 0.01f) gfx_.strokeRounded(box, 7, pal_.ribbon.withAlpha(0.6f * hv));
        gfx_.text(shown.empty() ? cue : shown, box.inset(10, 0), Font::Body, shown.empty() ? pal_.inkFaint : pal_.ink,
                  {Align::Left, false, true, true});
        addHit(hid, box, [this, key, target] {
            commitFields();
            setFocus({Focus::Calendar, key, target});
            loadFields();
            focusFieldSoon_ = FWhen;
            invalidate();
        });
    };
    auto removeX = [&](const std::string& id, float x, float yy, std::function<void(cal::Calendar&)> f) {
        linkText(id, x, yy, L"Remove", [this, f] { setFocus({}); changeCalendar(f); });
    };
    auto heading = [&](const std::wstring& t) {
        y += 10;
        gfx_.line(m.x, y + 0.5f, m.right(), y + 0.5f, pal_.rule);
        y += 18;
        gfx_.text(t, {m.x, y, m.w, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
        y += 26;
    };
    auto monthMenu = [this](int current, std::function<void(int)> pick) {
        const cal::Calendar c2 = scopeCalendar();
        std::vector<ToolItem> items;
        for (int i = 0; i < (int)c2.months.size(); ++i)
            items.push_back(ToolItem::item(std::to_wstring(cal::monthNumber(c2, i)) + L"  " + w(cal::monthLabel(c2, i)),
                                           [pick, i] { pick(i); }, i == current));
        showTools(items, mouseX(), mouseY());
    };

    if (wb_.calMode == 0) {
        // ---- Month by month: the bible's calendar page.
        if (const auto ch = lib_->changesMade(); ch != wb_.factsAt) {
            wb_.factsAt = ch;
            wb_.facts = cal::datedFacts(c, lib_->entriesOf(wb_.world, wb_.book));
        }
        wb_.calMonth = std::clamp(wb_.calMonth, 0, (int)c.months.size() - 1);
        const cal::Date at{wb_.calYear, wb_.calMonth, 1};
        float bx = m.x;
        auto step = [this](int dir) {
            const cal::Calendar c2 = scopeCalendar();
            int mo = wb_.calMonth + dir;
            if (mo < 0) { mo = (int)c2.months.size() - 1; --wb_.calYear; }
            if (mo >= (int)c2.months.size()) { mo = 0; ++wb_.calYear; }
            wb_.calMonth = mo;
            invalidate();
        };
        button("cal:prev", {bx, y, 36, 34}, L"‹", [step] { step(-1); });
        bx += 42;
        const std::wstring title = w(cal::monthLabel(c, wb_.calMonth)) + L"  " + w(cal::formatYear(c, wb_.calYear));
        gfx_.text(title, {bx, y, 360, 34}, Font::Heading, pal_.ink, {Align::Left, false, true, true});
        bx += std::min(360.0f, gfx_.measureWidth(title, Font::Heading)) + 12;
        button("cal:next", {bx, y, 36, 34}, L"›", [step] { step(1); });
        // Go to a date or a year.
        const float gw = 260;
        cell("goto", {m.right() - gw, y, gw, 34}, {}, L"Go to a date or year…");
        y += 44;
        {
            std::wstring line;
            if (const cal::Season* s = cal::seasonOn(c, at)) line = w(s->name);
            const std::int64_t len = cal::yearLength(c, wb_.calYear);
            line += std::wstring(line.empty() ? L"" : L"  ·  ") + L"Month " + std::to_wstring(cal::monthNumber(c, wb_.calMonth)) + L" of " +
                    std::to_wstring(c.months.size()) + L"  ·  this year has " + std::to_wstring(len) + L" days";
            gfx_.text(line, {m.x, y, m.w, 20}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
            y += 30;
        }
        y += drawMonthGrid({m.x, y, m.w, 0}, c, wb_.calYear, wb_.calMonth, true) + 24;
        // The year's holidays, in order.
        if (!c.holidays.empty()) {
            gfx_.text(L"HOLIDAYS", {m.x, y, m.w, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
            y += 26;
            std::vector<const cal::Holiday*> hs;
            for (auto& h : c.holidays) hs.push_back(&h);
            std::stable_sort(hs.begin(), hs.end(), [](auto* a, auto* b) { return a->month * 1000 + a->day < b->month * 1000 + b->day; });
            for (const cal::Holiday* h : hs) {
                const std::wstring when = std::to_wstring(h->day) + L" " + w(cal::monthLabel(c, h->month));
                const std::string hid = "cal:holrow:" + std::to_string(h->month) + ":" + std::to_string(h->day) + h->name;
                addHit(hid, {m.x, y, m.w, 26}, [this, mo = h->month] { wb_.calMonth = mo; invalidate(); });
                gfx_.text(when, {m.x, y, 180, 26}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
                gfx_.text(w(h->name), {m.x + 180, y, 260, 26}, Font::BodyStrong, pal_.ink, {Align::Left, false, true, true});
                if (const Entry* e = h->entry.empty() ? nullptr : lib_->entry(h->entry))
                    linkText("cal:holent:" + hid, m.x + 450, y + 2, w(e->name), [this, id = e->id] { openEntry(id); });
                else if (!h->notes.empty())
                    gfx_.text(w(h->notes), {m.x + 450, y, m.w - 450, 26}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
                y += 30;
            }
        }
    } else {
        // ---- Making it.
        float y0 = y;
        gfx_.text(L"Name", {m.x, y, m.w, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
        y += 22;
        cell("name", {m.x, y, m.w, 34}, w(c.name), L"The calendar's name");
        y += 48;
        const float cxL = m.x + 220, cwL = std::min(440.0f, m.w - 220);
        gfx_.text(L"Months are numbered", {m.x, y, 220, 28}, Font::Body, pal_.ink, {Align::Left, false, true, true});
        segmented("cal:zero", {cxL, y, cwL, 28}, {L"From 1", L"From 0", L"Month 0 + 1, 2…"}, (int)c.monthZero, [this](int i) {
            changeCalendar([i](cal::Calendar& x) {
                const cal::MonthZero before = x.monthZero;
                x.monthZero = (cal::MonthZero)i;
                // An extra month 0 is a month of its own, put before month 1.
                if (x.monthZero == cal::MonthZero::Extra && before != cal::MonthZero::Extra) {
                    x.months.insert(x.months.begin(), {"Month zero", 5});
                    for (auto& r : x.leap) ++r.month;
                    for (auto& s : x.seasons) ++s.month;
                    for (auto& h : x.holidays) ++h.month;
                }
            });
        });
        y += 36;
        if (c.monthZero != cal::MonthZero::None)
            y += gfx_.text(c.monthZero == cal::MonthZero::Count ? L"Every month is numbered from 0: the first month is month 0."
                                                                 : L"Month 0 is a month of its own before month 1 (a few days of the year's turning, say).",
                           {cxL, y, cwL, 40}, Font::Small, pal_.inkFaint) + 8;
        gfx_.text(L"Dates are written", {m.x, y, 220, 28}, Font::Body, pal_.ink, {Align::Left, false, true, true});
        segmented("cal:order", {cxL, y, cwL, 28}, {L"3 Frostmoon 1204", L"Frostmoon 3, 1204", L"1204, 3 Frostmoon"},
                  c.order == "mdy" ? 1 : c.order == "ymd" ? 2 : 0, [this](int i) {
                      changeCalendar([i](cal::Calendar& x) { x.order = i == 1 ? "mdy" : i == 2 ? "ymd" : "dmy"; });
                  });
        y += 40;
        {
            cal::Date ex = cal::fromDay(c, cal::toDay(c, {wb_.calYear, std::min(wb_.calMonth, (int)c.months.size() - 1), 3}));
            gfx_.text(L"For example: " + w(cal::formatDate(c, ex)), {cxL, y, cwL, 20}, Font::Small, pal_.inkFaint, {Align::Left, false, true});
            y += 26;
        }

        heading(L"MONTHS");
        for (int i = 0; i < (int)c.months.size(); ++i) {
            const std::string p = "m:" + std::to_string(i);
            const bool chosen = wb_.calMonth == i;
            const Rect row{m.x - 8, y - 4, m.w + 16, 42};
            if (chosen) gfx_.fillRounded(row, 8, pal_.ribbon.withAlpha(0.07f));
            addHit("cal:mrow:" + std::to_string(i), row, [this, i] { wb_.calMonth = i; invalidate(); });
            gfx_.text(std::to_wstring(cal::monthNumber(c, i)), {m.x, y, 34, 34}, Font::BodyStrong, pal_.inkMuted, {Align::Left, false, true, true});
            cell(p + ":name", {m.x + 40, y, m.w * 0.5f, 34}, w(c.months[i].name), L"Name");
            cell(p + ":days", {m.x + 50 + m.w * 0.5f, y, 90, 34}, std::to_wstring(c.months[i].days));
            gfx_.text(L"days", {m.x + 148 + m.w * 0.5f, y, 50, 34}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
            float lx = m.x + 200 + m.w * 0.5f;
            if (i > 0) linkText("cal:mup:" + std::to_string(i), lx, y + 6, L"Up", [this, i] {
                setFocus({});
                changeCalendar([i](cal::Calendar& x) {
                    std::swap(x.months[i], x.months[i - 1]);
                    auto fix = [i](int& mo) { if (mo == i) mo = i - 1; else if (mo == i - 1) mo = i; };
                    for (auto& r : x.leap) fix(r.month);
                    for (auto& s : x.seasons) fix(s.month);
                    for (auto& h : x.holidays) fix(h.month);
                });
                wb_.calMonth = i - 1;
            });
            lx += 44;
            if (c.months.size() > 1)
                removeX("cal:mx:" + std::to_string(i), lx, y + 6, [i](cal::Calendar& x) {
                    x.months.erase(x.months.begin() + i);
                    auto fix = [i](int& mo) { if (mo > i) --mo; };
                    x.leap.erase(std::remove_if(x.leap.begin(), x.leap.end(), [i](auto& r) { return r.month == i; }), x.leap.end());
                    x.seasons.erase(std::remove_if(x.seasons.begin(), x.seasons.end(), [i](auto& s) { return s.month == i; }), x.seasons.end());
                    x.holidays.erase(std::remove_if(x.holidays.begin(), x.holidays.end(), [i](auto& h) { return h.month == i; }), x.holidays.end());
                    for (auto& r : x.leap) fix(r.month);
                    for (auto& s : x.seasons) fix(s.month);
                    for (auto& h : x.holidays) fix(h.month);
                });
            y += 42;
        }
        smallButton("cal:m:add", m.x, y + 4, L"+ Add a month", [this] {
            changeCalendar([](cal::Calendar& x) { x.months.push_back({"New month", 30}); });
            const cal::Calendar now = scopeCalendar();
            wb_.calMonth = (int)now.months.size() - 1;
            setFocus({Focus::Calendar, calendarKey(), "m:" + std::to_string(wb_.calMonth) + ":name"});
            loadFields();
            focusFieldSoon_ = FWhen;
        });
        y += 50;
        {
            const double mean = cal::meanYear(c);
            char buf[64];
            std::snprintf(buf, sizeof buf, "%.4g", mean);
            gfx_.text(L"A year has " + std::to_wstring(cal::yearLength(c, 1)) + L" days in year 1; " + w(buf) + L" on average.",
                      {m.x, y, m.w, 20}, Font::Small, pal_.inkFaint, {Align::Left, false, true});
            y += 26;
        }

        heading(L"THE WEEK");
        {
            float x = m.x;
            const float bw = 150;
            for (int i = 0; i < (int)c.weekdays.size(); ++i) {
                if (x + bw > m.right() + 1) { x = m.x; y += 64; }
                cell("w:" + std::to_string(i), {x, y, bw, 34}, w(c.weekdays[i]), L"Day name");
                removeX("cal:wx:" + std::to_string(i), x + 2, y + 38, [i](cal::Calendar& cc) {
                    cc.weekdays.erase(cc.weekdays.begin() + i);
                    cc.firstWeekday = std::clamp(cc.firstWeekday, 0, std::max(0, (int)cc.weekdays.size() - 1));
                });
                x += bw + 10;
            }
            if (x + bw > m.right() + 1) { x = m.x; y += 64; }
            smallButton("cal:w:add", x, y + 2, L"+ Add a day", [this] {
                changeCalendar([](cal::Calendar& cc) { cc.weekdays.push_back("New day"); });
                const cal::Calendar now = scopeCalendar();
                setFocus({Focus::Calendar, calendarKey(), "w:" + std::to_string(now.weekdays.size() - 1)});
                loadFields();
                focusFieldSoon_ = FWhen;
            });
            y += 66;
            if (c.weekdays.empty())
                y += gfx_.text(L"No week: the month view shows the days in rows of seven.", {m.x, y, m.w, 20}, Font::Small, pal_.inkFaint) + 8;
            else {
                gfx_.text(L"Year 1 begins on a", {m.x, y, 220, 32}, Font::Body, pal_.ink, {Align::Left, false, true, true});
                chooserBox("cal:firstwd", {cxL, y, 220, 32}, w(c.weekdays[std::clamp(c.firstWeekday, 0, (int)c.weekdays.size() - 1)]), [this] {
                    const cal::Calendar c2 = scopeCalendar();
                    std::vector<ToolItem> items;
                    for (int i = 0; i < (int)c2.weekdays.size(); ++i)
                        items.push_back(ToolItem::item(w(c2.weekdays[i]), [this, i] { changeCalendar([i](cal::Calendar& x) { x.firstWeekday = i; }); },
                                                       i == c2.firstWeekday));
                    showTools(items, mouseX(), mouseY());
                });
                y += 42;
            }
        }

        heading(L"LEAP YEARS");
        if (c.leap.empty())
            y += gfx_.text(L"None: every year is the same length.", {m.x, y, m.w, 20}, Font::Small, pal_.inkFaint) + 10;
        for (int i = 0; i < (int)c.leap.size(); ++i) {
            const std::string p = "leap:" + std::to_string(i);
            float x = m.x;
            gfx_.text(L"Every", {x, y, 50, 34}, Font::Body, pal_.ink, {Align::Left, false, true, true}); x += 50;
            cell(p + ":every", {x, y, 70, 34}, std::to_wstring(c.leap[i].every)); x += 78;
            gfx_.text(L"years, add", {x, y, 80, 34}, Font::Body, pal_.ink, {Align::Left, false, true, true}); x += 80;
            cell(p + ":add", {x, y, 60, 34}, std::to_wstring(c.leap[i].add)); x += 68;
            gfx_.text(L"days to", {x, y, 58, 34}, Font::Body, pal_.ink, {Align::Left, false, true, true}); x += 58;
            chooserBox("cal:leapm:" + std::to_string(i), {x, y, 170, 34}, w(cal::monthLabel(c, c.leap[i].month)), [this, i, monthMenu] {
                monthMenu(scopeCalendar().leap[i].month, [this, i](int mo) { changeCalendar([i, mo](cal::Calendar& x) { x.leap[i].month = mo; }); });
            });
            x += 178;
            removeX("cal:lx:" + std::to_string(i), x, y + 6, [i](cal::Calendar& cc) { cc.leap.erase(cc.leap.begin() + i); });
            y += 42;
            if (c.leap[i].offset != 0 || (editing && focus_.contextSeries == p + ":offset")) {
                gfx_.text(L"counting from year", {m.x + 50, y, 140, 30}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
                cell(p + ":offset", {m.x + 190, y, 80, 30}, std::to_wstring(c.leap[i].offset));
                y += 38;
            } else {
                linkText("cal:loff:" + std::to_string(i), m.x + 46, y - 4, L"Count from another year", [this, key, p] {
                    commitFields();
                    setFocus({Focus::Calendar, key, p + ":offset"});
                    loadFields();
                    focusFieldSoon_ = FWhen;
                });
                y += 30;
            }
        }
        smallButton("cal:l:add", m.x, y + 2, L"+ Add a leap rule", [this] { changeCalendar([](cal::Calendar& x) { x.leap.push_back({4, 0, 0, 1}); }); });
        y += 50;

        heading(L"YEARS AND ERAS");
        for (int i = 0; i < (int)c.eras.size(); ++i) {
            const std::string p = "era:" + std::to_string(i);
            cell(p + ":name", {m.x, y, m.w * 0.42f, 34}, w(c.eras[i].name), L"Era's name");
            cell(p + ":abbr", {m.x + m.w * 0.42f + 8, y, 80, 34}, w(c.eras[i].abbr), L"AL");
            gfx_.text(L"from year", {m.x + m.w * 0.42f + 96, y, 74, 34}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
            cell(p + ":start", {m.x + m.w * 0.42f + 170, y, 90, 34}, std::to_wstring(c.eras[i].start));
            removeX("cal:ex:" + std::to_string(i), m.x + m.w * 0.42f + 270, y + 6, [i](cal::Calendar& cc) { cc.eras.erase(cc.eras.begin() + i); });
            y += 42;
        }
        smallButton("cal:e:add", m.x, y + 2, L"+ Add an era", [this] {
            changeCalendar([](cal::Calendar& x) {
                std::int64_t start = 1;
                for (auto& e : x.eras) start = std::max(start, e.start + 100);
                x.eras.push_back({"New era", "", start});
            });
        });
        y += 48;
        if (!c.eras.empty()) {
            gfx_.text(L"Before the first era", {m.x, y, 220, 34}, Font::Body, pal_.ink, {Align::Left, false, true, true});
            cell("before:name", {cxL, y, 220, 34}, w(c.beforeName), L"Before Light");
            cell("before:abbr", {cxL + 228, y, 80, 34}, w(c.beforeAbbr), L"BL");
            y += 42;
            y += gfx_.text(L"Years before it count back: the year before year 1 of the first era is 1 " +
                           (c.beforeAbbr.empty() ? std::wstring(L"before it") : w(c.beforeAbbr)) + L".",
                           {m.x, y, m.w, 20}, Font::Small, pal_.inkFaint) + 10;
        } else {
            y += gfx_.text(L"With no era, years are plain numbers (and 0 and below before year 1).", {m.x, y, m.w, 20}, Font::Small, pal_.inkFaint) + 10;
        }

        heading(L"MOONS");
        for (int i = 0; i < (int)c.moons.size(); ++i) {
            const std::string p = "moon:" + std::to_string(i);
            cell(p + ":name", {m.x, y, m.w * 0.36f, 34}, w(c.moons[i].name), L"Moon's name");
            float x = m.x + m.w * 0.36f + 8;
            cell(p + ":cycle", {x, y, 80, 34}, w(numText(c.moons[i].cycle))); x += 86;
            gfx_.text(L"days a cycle;", {x, y, 96, 34}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true}); x += 96;
            cell(p + ":offset", {x, y, 70, 34}, w(numText(c.moons[i].offset))); x += 76;
            gfx_.text(L"days in at the start", {x, y, 140, 34}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true}); x += 140;
            removeX("cal:mox:" + std::to_string(i), x, y + 6, [i](cal::Calendar& cc) { cc.moons.erase(cc.moons.begin() + i); });
            y += 42;
        }
        smallButton("cal:mo:add", m.x, y + 2, L"+ Add a moon", [this] { changeCalendar([](cal::Calendar& x) { x.moons.push_back({"New moon", 29.5, 0}); }); });
        y += 48;

        heading(L"SEASONS");
        for (int i = 0; i < (int)c.seasons.size(); ++i) {
            const std::string p = "season:" + std::to_string(i);
            cell(p + ":name", {m.x, y, m.w * 0.36f, 34}, w(c.seasons[i].name), L"Season's name");
            float x = m.x + m.w * 0.36f + 8;
            gfx_.text(L"begins on", {x, y, 70, 34}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true}); x += 70;
            cell(p + ":day", {x, y, 60, 34}, std::to_wstring(c.seasons[i].day)); x += 66;
            chooserBox("cal:seam:" + std::to_string(i), {x, y, 170, 34}, w(cal::monthLabel(c, c.seasons[i].month)), [this, i, monthMenu] {
                monthMenu(scopeCalendar().seasons[i].month, [this, i](int mo) { changeCalendar([i, mo](cal::Calendar& x) { x.seasons[i].month = mo; }); });
            });
            x += 178;
            removeX("cal:sx:" + std::to_string(i), x, y + 6, [i](cal::Calendar& cc) { cc.seasons.erase(cc.seasons.begin() + i); });
            y += 42;
        }
        smallButton("cal:s:add", m.x, y + 2, L"+ Add a season", [this] { changeCalendar([](cal::Calendar& x) { x.seasons.push_back({"New season", 0, 1}); }); });
        y += 48;

        heading(L"HOLIDAYS");
        for (int i = 0; i < (int)c.holidays.size(); ++i) {
            const std::string p = "hol:" + std::to_string(i);
            cell(p + ":name", {m.x, y, m.w * 0.36f, 34}, w(c.holidays[i].name), L"Holiday's name");
            float x = m.x + m.w * 0.36f + 8;
            gfx_.text(L"on", {x, y, 26, 34}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true}); x += 26;
            cell(p + ":day", {x, y, 60, 34}, std::to_wstring(c.holidays[i].day)); x += 66;
            chooserBox("cal:holm:" + std::to_string(i), {x, y, 170, 34}, w(cal::monthLabel(c, c.holidays[i].month)), [this, i, monthMenu] {
                monthMenu(scopeCalendar().holidays[i].month, [this, i](int mo) { changeCalendar([i, mo](cal::Calendar& x) { x.holidays[i].month = mo; }); });
            });
            x += 178;
            removeX("cal:hx:" + std::to_string(i), x, y + 6, [i](cal::Calendar& cc) { cc.holidays.erase(cc.holidays.begin() + i); });
            y += 40;
            cell(p + ":notes", {m.x + 20, y, m.w * 0.6f, 30}, w(c.holidays[i].notes), L"What happens on it (optional)");
            {
                const Entry* e = c.holidays[i].entry.empty() ? nullptr : lib_->entry(c.holidays[i].entry);
                chooserBox("cal:hole:" + std::to_string(i), {m.x + 28 + m.w * 0.6f, y, m.w * 0.4f - 28, 30},
                           e ? L"About: " + w(e->name) : L"About an entry… (optional)", [this, i] {
                    std::vector<ToolItem> items;
                    items.push_back(ToolItem::item(L"No entry", [this, i] { changeCalendar([i](cal::Calendar& x) { x.holidays[i].entry.clear(); }); }));
                    items.push_back(ToolItem::line());
                    for (const Entry* en : lib_->entriesOf(wb_.world, wb_.book))
                        items.push_back(ToolItem::item(w(en->name) + L"  (" + w(entryKindLabel(en->kind)) + L")",
                                                       [this, i, id = en->id] { changeCalendar([i, id](cal::Calendar& x) { x.holidays[i].entry = id; }); }));
                    showTools(items, mouseX(), mouseY());
                });
            }
            y += 44;
        }
        smallButton("cal:h:add", m.x, y + 2, L"+ Add a holiday", [this] {
            changeCalendar([this](cal::Calendar& x) { x.holidays.push_back({"New holiday", std::clamp(wb_.calMonth, 0, (int)x.months.size() - 1), 1, {}, {}}); });
        });
        y += 48;

        heading(L"THE DAY");
        {
            float x = m.x;
            cell("hours", {x, y, 70, 34}, std::to_wstring(c.hoursPerDay)); x += 78;
            gfx_.text(L"hours of", {x, y, 70, 34}, Font::Body, pal_.ink, {Align::Left, false, true, true}); x += 70;
            cell("minutes", {x, y, 70, 34}, std::to_wstring(c.minutesPerHour)); x += 78;
            gfx_.text(L"minutes. Scenes can be given a time of day in these.", {x, y, m.right() - x, 34}, Font::Small, pal_.inkMuted,
                      {Align::Left, false, true, true});
            y += 48;
        }
        (void)y0;
    }
    wb_.content = y + wb_.scroll - r.y + 60;
    wb_.scroll = std::clamp(wb_.scroll, 0.0f, std::max(0.0f, wb_.content - r.h));
    clearHitClip();
    gfx_.popClip();
}

// The card beside the form: the chosen month drawn as it will look, live.
void StudioApp::drawCalendarPanel(const Rect& r) {
    if (focus_.kind == Focus::Calendar && !(focus_ == loadedFocus_)) loadFields();
    const cal::Calendar c = scopeCalendar();
    gfx_.pushClip(r);
    setHitClip(r);
    const Rect m{r.x + 20, r.y + 18, r.w - 40, 0};
    float y = m.y;
    if (c.empty()) { clearHitClip(); gfx_.popClip(); return; }
    const int mo = std::clamp(wb_.calMonth, 0, (int)c.months.size() - 1);
    gfx_.text(L"PREVIEW", {m.x, y, m.w, 16}, Font::Label, gold_, {Align::Left, false});
    y += 22;
    button("cal:pv:prev", {m.x, y, 30, 30}, L"‹", [this] {
        const cal::Calendar c2 = scopeCalendar();
        if (--wb_.calMonth < 0) { wb_.calMonth = (int)c2.months.size() - 1; --wb_.calYear; }
        invalidate();
    });
    button("cal:pv:next", {m.right() - 30, y, 30, 30}, L"›", [this] {
        const cal::Calendar c2 = scopeCalendar();
        if (++wb_.calMonth >= (int)c2.months.size()) { wb_.calMonth = 0; ++wb_.calYear; }
        invalidate();
    });
    gfx_.text(w(cal::monthLabel(c, mo)), {m.x + 36, y - 2, m.w - 72, 20}, Font::BodyStrong, pal_.ink, {Align::Center, false, true});
    gfx_.text(L"Month " + std::to_wstring(cal::monthNumber(c, mo)) + L" · " + w(cal::formatYear(c, wb_.calYear)),
              {m.x + 36, y + 16, m.w - 72, 16}, Font::Small, pal_.inkMuted, {Align::Center, false, true});
    y += 42;
    y += drawMonthGrid({m.x, y, m.w, 0}, c, wb_.calYear, mo, false) + 14;
    const int len = cal::monthLength(c, wb_.calYear, mo);
    std::wstring facts = std::to_wstring(len) + L" days";
    if (len != c.months[mo].days) facts += L" this year (a leap year)";
    if (!c.weekdays.empty())
        facts += L". It begins on a " + w(c.weekdays[cal::weekday(c, cal::toDay(c, {wb_.calYear, mo, 1}))]) + L".";
    y += gfx_.text(facts, {m.x, y, m.w, 60}, Font::Small, pal_.inkMuted) + 8;
    for (const auto& h : c.holidays)
        if (h.month == mo) y += gfx_.text(std::to_wstring(h.day) + L": " + w(h.name), {m.x, y, m.w, 40}, Font::Small, gold_) + 4;
    for (const auto& s : c.seasons)
        if (s.month == mo) y += gfx_.text(std::to_wstring(s.day) + L": " + w(s.name) + L" begins", {m.x, y, m.w, 40}, Font::Small, pal_.ribbon) + 4;
    y += 8;
    gfx_.text(L"The arrows step through the months and years; a month's row on the form shows it here.", {m.x, y, m.w, 60}, Font::Small,
              pal_.inkFaint);
    clearHitClip();
    gfx_.popClip();
}

// ------------------------------------------------------------------ picking a date (0.75)

void StudioApp::openDatePicker(Field field) {
    const cal::Calendar c = scopeCalendar();
    if (c.empty()) { flash(L"Make the world's calendar first (Bible tools › Calendar).", 5); return; }
    // Start at a date already on the caret's line, else the month last looked at in the calendar.
    wb_.pickYear = wb_.calYear;
    if (wb_.pickYear == 1 && !c.eras.empty()) wb_.pickYear = c.name == "Gregorian" ? 2026 : c.eras.back().start;
    wb_.pickMonth = std::clamp(wb_.calMonth, 0, (int)c.months.size() - 1);
    {
        HWND h = fields_[field];
        DWORD a = 0, b = 0;
        SendMessageW(h, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
        const int line = (int)SendMessageW(h, EM_LINEFROMCHAR, a, 0);
        wchar_t buf[400] = {};
        *(WORD*)buf = 390;
        const int n = (int)SendMessageW(h, EM_GETLINE, line, (LPARAM)buf);
        std::string text = narrow(std::wstring(buf, std::max(0, n)));
        const size_t colon = text.find(':');
        if (colon != std::string::npos)
            if (const auto d = cal::parseDate(c, text.substr(colon + 1))) { wb_.pickYear = d->year; wb_.pickMonth = d->month; }
    }
    wb_.datePick = true;
    ie_.hold = true;
    invalidate();
}

float StudioApp::drawDatePicker(const Rect& r, Field field) {
    const cal::Calendar c = scopeCalendar();
    if (c.empty()) { wb_.datePick = false; ie_.hold = false; return 0; }
    wb_.pickMonth = std::clamp(wb_.pickMonth, 0, (int)c.months.size() - 1);
    const int cols = c.weekdays.empty() ? 7 : (int)c.weekdays.size();
    const float gw = std::min(r.w, 340.0f) - 24;
    const int lead = c.weekdays.empty() ? 0 : cal::weekday(c, cal::toDay(c, {wb_.pickYear, wb_.pickMonth, 1}));
    const int rows = std::max(1, (lead + cal::monthLength(c, wb_.pickYear, wb_.pickMonth) + cols - 1) / cols);
    const float cellH = std::max(30.0f, std::min(44.0f, gw / cols * 0.9f));
    const float h = 12 + 36 + (c.weekdays.empty() ? 0 : 22) + rows * cellH + 6 + 40 + 34;
    const Rect cr{r.x, r.y, std::min(r.w, 340.0f), h};
    card(cr, 10, 1.5f);
    float y = cr.y + 12;
    auto step = [this](int dir) {
        const cal::Calendar c2 = scopeCalendar();
        wb_.pickMonth += dir;
        if (wb_.pickMonth < 0) { wb_.pickMonth = (int)c2.months.size() - 1; --wb_.pickYear; }
        if (wb_.pickMonth >= (int)c2.months.size()) { wb_.pickMonth = 0; ++wb_.pickYear; }
        invalidate();
    };
    button("dp:prev", {cr.x + 12, y, 30, 28}, L"‹", [step] { step(-1); });
    button("dp:next", {cr.right() - 42, y, 30, 28}, L"›", [step] { step(1); });
    button("dp:prevy", {cr.x + 46, y, 30, 28}, L"«", [this] { --wb_.pickYear; invalidate(); });
    button("dp:nexty", {cr.right() - 76, y, 30, 28}, L"»", [this] { ++wb_.pickYear; invalidate(); });
    gfx_.text(w(cal::monthLabel(c, wb_.pickMonth)) + L" " + w(cal::formatYear(c, wb_.pickYear)), {cr.x + 80, y, cr.w - 160, 28},
              Font::SmallStrong, pal_.ink, {Align::Center, false, true, true});
    y += 36;
    y += drawMonthGrid({cr.x + 12, y, cr.w - 24, 0}, c, wb_.pickYear, wb_.pickMonth, false, [this, field](int day) {
        const cal::Calendar c2 = scopeCalendar();
        const std::wstring t = w(cal::formatDate(c2, {wb_.pickYear, wb_.pickMonth, day}));
        HWND h = fields_[field];
        SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)t.c_str());
        wb_.datePick = false;
        ie_.hold = false;
        SetFocus(h);
        invalidate();
    }, "dp:day:") + 6;
    gfx_.text(L"Pick a day to put its date where the caret is. You can type dates too.", {cr.x + 14, y, cr.w - 28, 34}, Font::Small,
              pal_.inkFaint);
    linkText("dp:close", cr.right() - gfx_.measureWidth(L"Close", Font::Small) - 22, cr.bottom() - 30, L"Close", [this, field] {
        wb_.datePick = false;
        ie_.hold = false;
        SetFocus(fields_[field]);
        invalidate();
    });
    return h;
}

// Under an entry's details: how old they are through the books, and how long they lived (0.75).
float StudioApp::drawEntryDates(const Entry& e, float x, float y, float wd) {
    const cal::Calendar c = scopeCalendar();
    if (c.empty()) return 0;
    const auto facts = cal::datedFactsOf(c, e);
    if (facts.empty()) return 0;
    const cal::DatedFact* born = nullptr;
    const cal::DatedFact* died = nullptr;
    for (auto& f : facts) {
        if (f.kind == cal::DatedFact::Birth && !born) born = &f;
        if (f.kind == cal::DatedFact::Death && !died) died = &f;
    }
    float dy = y;
    std::wstring line;
    for (auto& f : facts) {
        if (!c.weekdays.empty() && f.kind != cal::DatedFact::Other) {
            const std::wstring wd2 = w(c.weekdays[cal::weekday(c, f.day)]);
            const std::wstring said = f.kind == cal::DatedFact::Birth ? L"Born on a " : f.kind == cal::DatedFact::Death ? L"Died on a " : L"On a ";
            line += (line.empty() ? L"" : L"   ·   ") + said + wd2;
        }
    }
    if (born && died) {
        const int a = cal::ageAt(c, born->date, died->date);
        line += (line.empty() ? L"" : L"   ·   ") + std::wstring(L"lived ") + std::to_wstring(a) + (a == 1 ? L" year" : L" years");
    }
    if (!line.empty()) dy += gfx_.text(line, {x, dy, wd, 40}, Font::Small, pal_.inkMuted) + 4;
    if (born) {
        std::vector<Id> order;
        for (const Id& b : booksInReadingOrder(*lib_, wb_.world, wb_.book)) order.push_back(b);
        const auto ages = cal::agesInBooks(*lib_, c, born->date, order, died ? std::optional<cal::Date>(died->date) : std::nullopt);
        if (!ages.empty()) {
            std::wstring a = L"Age";
            for (size_t i = 0; i < ages.size(); ++i) {
                const studio::Book* bk = lib_->book(ages[i].book);
                if (!bk) continue;
                a += (i ? L";  " : L" ") + std::wstring(L"in ") + w(bk->title) + L": " + std::to_wstring(ages[i].from) +
                     (ages[i].to != ages[i].from ? L"\u2013" + std::to_wstring(ages[i].to) : L"");
            }
            dy += gfx_.text(a, {x, dy, wd, 60}, Font::Small, pal_.ribbon) + 4;
        }
    }
    return dy - y;
}

void StudioApp::startCalendar(const cal::Calendar& c) {
    const cal::Calendar before = scopeCalendar();
    setFocus({});
    cal::Calendar next = c;
    if (!before.empty() && next.name.empty()) next.name = before.name;
    saveScopeCalendar(next);
    wb_.calMode = 1;
    wb_.calMonth = 0;
    wb_.calYear = c.eras.empty() ? 1 : c.eras.back().start;
    if (c.name == "Gregorian") wb_.calYear = 2026;
    if (!before.empty()) flashUndo(L"Started the calendar again.", [this, before] { saveScopeCalendar(before); });
}

void StudioApp::copyCalendarText() {
    const std::wstring t = w(cal::toText(scopeCalendar()));
    if (!OpenClipboard(hwnd())) { flash(L"The clipboard is busy; try again.", 4); return; }
    EmptyClipboard();
    std::wstring crlf;
    for (wchar_t ch : t) { if (ch == L'\n') crlf += L'\r'; crlf += ch; }
    const size_t bytes = (crlf.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        memcpy(GlobalLock(h), crlf.c_str(), bytes);
        GlobalUnlock(h);
        if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
    }
    CloseClipboard();
    flash(L"Copied the calendar as text.", 3);
}

void StudioApp::exportCalendarText() {
    commitFields();
    const cal::Calendar c = scopeCalendar();
    IFileSaveDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    COMDLG_FILTERSPEC spec[] = {{L"Calendar as text", L"*.txt"}};
    dlg->SetFileTypes(1, spec);
    dlg->SetDefaultExtension(L"txt");
    dlg->SetFileName(w((c.name.empty() ? std::string("Calendar") : c.name) + ".txt").c_str());
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
    std::string text;
    for (char ch : cal::toText(c)) { if (ch == '\n') text += '\r'; text += ch; }
    std::ofstream f(path.c_str(), std::ios::binary);
    f << "\xEF\xBB\xBF" << text;
    if (f) flash(L"Saved the calendar as text.", 4);
    else cjui::messageBox(hwnd(), L"The file couldn't be written.", L"Export the calendar", MB_OK | MB_ICONWARNING);
}

void StudioApp::importCalendarText() {
    commitFields();
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    COMDLG_FILTERSPEC types[] = {{L"Calendar as text", L"*.txt;*.md"}, {L"All files", L"*.*"}};
    dlg->SetFileTypes(2, types);
    dlg->SetTitle(L"Import a calendar");
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
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.size() > 1024 * 1024) {
        cjui::messageBox(hwnd(), L"That file is too big to be a calendar.", L"Import a calendar", MB_OK | MB_ICONWARNING);
        return;
    }
    std::vector<std::string> warnings;
    cal::Calendar c = cal::fromText(textFromFileBytes(bytes), &warnings);
    if (c.empty()) {
        std::wstring msg = L"No calendar could be read from that file.";
        if (!warnings.empty()) msg += L"\n\n" + w(warnings.back());
        cjui::messageBox(hwnd(), msg.c_str(), L"Import a calendar", MB_OK | MB_ICONWARNING);
        return;
    }
    startCalendar(c);
    if (!warnings.empty()) {
        std::wstring msg = L"The calendar was imported, but some lines weren't understood and were left out:\n";
        for (size_t i = 0; i < warnings.size() && i < 12; ++i) msg += L"\n• " + w(warnings[i]);
        cjui::messageBox(hwnd(), msg.c_str(), L"Import a calendar", MB_OK | MB_ICONINFORMATION);
    }
}

}  // namespace nrs
