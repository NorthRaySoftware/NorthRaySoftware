// Planning's timeline and story structure.
//
// Timeline: each chapter and scene can be placed on the story's own calendar
// (a number of days, weeks, months or years, plus words such as "Midsummer,
// 1204"). Cards sit in a lane for their viewpoint character; a line joins them
// in reading order, so flashbacks show as the line turning back. Drag a card
// along the line to move it in time. Kept per book as setting "timeline:<book>":
// {"unit": "day", "nodes": {"<node id>": {"day": 3, "when": "Midsummer"}}}.
//
// Structure: a template of beats (three acts, fifteen beats, the hero's journey,
// seven points, five acts, kishotenketsu) laid over the book by length, each
// beat matched to a chapter. Kept as setting "structure:<book>":
// {"template": "three-act", "beats": {"<beat id>": "<node id>"}}.
// The templates' descriptions were written for NorthRay.
#include "studio_app.hpp"
#include "cjui/dialog.hpp"
#include "cjui/menu.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace nrs {

using nlohmann::json;
using namespace studio;
using cjui::MenuItem;
using cjui::showMenu;

static std::wstring w(const std::string& s) { return widen(s); }

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
    j["unit"] = unit;
    lib_->setSetting("timeline:" + bookId, j.dump());
}

json StudioApp::structureOf(const Id& bookId) const {
    json j = json::parse(lib_->setting("structure:" + bookId), nullptr, false);
    return j.is_object() ? j : json::object();
}

void StudioApp::setStructure(const Id& bookId, const json& s) { lib_->setSetting("structure:" + bookId, s.dump()); }

// --------------------------------------------------------- the templates

namespace {

struct Beat { const char* id; const char* name; double at; const char* about; };
struct Template { const char* id; const char* name; const char* about; std::vector<Beat> beats; };

const std::vector<Template>& templates() {
    static const std::vector<Template> t = {
        {"three-act", "Three acts", "The classic shape: set up a world and a want, complicate it until everything is at stake, then resolve it.",
         {{"setup", "Setup", 0.0, "Who we're with, what they want, and the world they want it in."},
          {"inciting", "Inciting incident", 0.12, "Something happens that the hero can't ignore. The story begins to move."},
          {"turn1", "First turning point", 0.25, "A decision that can't be undone carries the hero into the new situation of the middle."},
          {"midpoint", "Midpoint", 0.5, "A reversal or revelation: the goal or the stakes change, and the hero stops reacting and starts acting."},
          {"turn2", "Second turning point", 0.75, "The worst moment: what the hero relied on fails, and a last, costly choice is made."},
          {"climax", "Climax", 0.88, "The final confrontation, where the want and the need meet."},
          {"resolution", "Resolution", 0.97, "The new normal. What changed, and what it cost."}}},
        {"fifteen", "Fifteen beats", "A detailed beat sheet popularised for screenwriting (Blake Snyder's Save the Cat!) and widely used for novels.",
         {{"opening", "Opening image", 0.0, "A snapshot of the hero's life before: the 'before' picture."},
          {"theme", "Theme stated", 0.05, "Someone hints at what the hero must learn, though they don't see it yet."},
          {"setup", "Setup", 0.06, "The hero's world, flaws and the things that need fixing."},
          {"catalyst", "Catalyst", 0.1, "The event that knocks life off course."},
          {"debate", "Debate", 0.15, "Doubt: should the hero go? Can they?"},
          {"break2", "Break into two", 0.2, "The hero chooses to step into the new world."},
          {"bstory", "B story", 0.22, "A second thread, often a relationship, that carries the theme."},
          {"fun", "Fun and games", 0.3, "The promise of the premise: the new world explored."},
          {"midpoint", "Midpoint", 0.5, "A false victory or false defeat; the stakes rise."},
          {"closein", "Bad guys close in", 0.55, "Pressure from outside and doubt from within."},
          {"lost", "All is lost", 0.75, "The low point; something (or someone) is lost."},
          {"dark", "Dark night of the soul", 0.78, "The hero sits with the loss, and finds the lesson."},
          {"break3", "Break into three", 0.8, "The answer, from the A and B stories together."},
          {"finale", "Finale", 0.85, "The hero puts the lesson to work and wins (or loses well)."},
          {"final", "Final image", 1.0, "The 'after' picture, mirroring the opening."}}},
        {"hero", "The hero's journey", "Twelve stages of the mythic journey, after Joseph Campbell as set out for writers by Christopher Vogler.",
         {{"ordinary", "Ordinary world", 0.0, "Home, before the adventure."},
          {"call", "Call to adventure", 0.08, "A problem or challenge appears."},
          {"refusal", "Refusal of the call", 0.12, "Fear or duty holds the hero back."},
          {"mentor", "Meeting the mentor", 0.17, "Advice, a gift or training."},
          {"threshold", "Crossing the threshold", 0.25, "Into the special world; no way back."},
          {"tests", "Tests, allies, enemies", 0.35, "Learning the rules of the new world, and who's who."},
          {"approach", "Approach to the inmost cave", 0.45, "Preparing for the central ordeal."},
          {"ordeal", "The ordeal", 0.5, "A brush with death, real or figurative."},
          {"reward", "Reward", 0.6, "The prize claimed: an object, a truth, a reconciliation."},
          {"road", "The road back", 0.75, "Consequences follow; the way home is dangerous."},
          {"resurrection", "Resurrection", 0.88, "A final test where everything learned is used."},
          {"elixir", "Return with the elixir", 0.97, "Home again, changed, bringing something back."}}},
        {"seven", "Seven points", "Work from the end backwards: the resolution mirrors the hook, with turns and pinches between (after Dan Wells).",
         {{"hook", "Hook", 0.0, "The starting state, the opposite of the resolution."},
          {"turn1", "Plot turn 1", 0.15, "The call to adventure: the world changes."},
          {"pinch1", "Pinch 1", 0.35, "Pressure: the antagonist shows its strength; the hero must act."},
          {"midpoint", "Midpoint", 0.5, "The hero moves from reaction to action."},
          {"pinch2", "Pinch 2", 0.65, "Things go wrong; the plan fails, the jaws of defeat."},
          {"turn2", "Plot turn 2", 0.85, "The hero finds the last piece needed to win."},
          {"resolution", "Resolution", 1.0, "The climax and the end state."}}},
        {"five-act", "Five acts", "Gustav Freytag's pyramid, from classical drama: rising to a peak and falling to a close.",
         {{"exposition", "Exposition", 0.0, "The situation, the people and the conflict to come."},
          {"rising", "Rising action", 0.2, "Complications build."},
          {"climax", "Climax", 0.5, "The turning point, at the top of the pyramid."},
          {"falling", "Falling action", 0.7, "Consequences unwind."},
          {"denouement", "Dénouement", 0.9, "The knots are untied; the end."}}},
        {"kishotenketsu", "Kishōtenketsu", "Four parts from East Asian storytelling, built on contrast rather than conflict; also used for four-panel comics.",
         {{"ki", "Ki (introduction)", 0.0, "Characters and setting."},
          {"sho", "Shō (development)", 0.25, "The situation develops, without a major change."},
          {"ten", "Ten (twist)", 0.5, "Something unexpected appears, seemingly unconnected."},
          {"ketsu", "Ketsu (reconciliation)", 0.75, "The parts come together and make a new sense."}}},
    };
    return t;
}

const Template* findTemplate(const std::string& id) {
    for (auto& t : templates()) if (id == t.id) return &t;
    return nullptr;
}

std::wstring dayLabel(const std::string& unit, double d) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", d);
    const std::wstring u = unit == "week" ? L"Week " : unit == "month" ? L"Month " : unit == "year" ? L"Year " : L"Day ";
    return u + widen(buf);
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
    const std::string unit = timelineUnit(pl_.book);
    float y = m.y + pageHeader(m, L"TIMELINE", w(b->title),
                               L"Chapters and scenes in the order they happen. Drag a card along to move it in time, or set when it happens "
                               L"in the panel. The line joins them in reading order, so a flashback shows as the line turning back.");
    const auto all = lib_->manuscriptOf(pl_.book);
    const auto times = storyTimes(pl_.book);
    struct Item { const ManuscriptNode* n; double day; int order; std::wstring label; };
    std::vector<Item> placed;
    std::vector<const ManuscriptNode*> unplaced;
    int chapterNo = 0, sceneNo = 0, order = 0;
    for (const ManuscriptNode* n : all) {
        if (n->kind == NodeKind::Part) continue;
        if (n->kind == NodeKind::Chapter) { ++chapterNo; sceneNo = 0; } else ++sceneNo;
        if (pl_.status >= 0 && (int)n->status != pl_.status) continue;
        const std::wstring lbl = n->kind == NodeKind::Chapter ? L"Ch " + std::to_wstring(chapterNo)
                                                              : std::to_wstring(chapterNo) + L"." + std::to_wstring(sceneNo);
        auto it = times.find(n->id);
        double day = it != times.end() && it->second.placed ? it->second.day : NAN;
        if (pl_.tlDrag.active && pl_.tlDrag.id == n->id) day = pl_.tlDrag.day;
        if (std::isnan(day)) unplaced.push_back(n);
        else placed.push_back({n, day, order++, lbl});
    }
    // Lanes: one per viewpoint character, in the order they first appear; then no viewpoint.
    std::vector<Id> lanes;
    for (auto& it : placed) if (std::find(lanes.begin(), lanes.end(), it.n->povId) == lanes.end()) lanes.push_back(it.n->povId);
    std::stable_partition(lanes.begin(), lanes.end(), [](const Id& id) { return !id.empty(); });

    if (placed.empty()) {
        y += gfx_.text(L"Nothing is on the timeline yet. Choose a card below (or on the corkboard) and give it a day in the panel, "
                       L"or press Place to put it after the last one.",
                       {m.x, y, std::min(cw, 680.0f), 60}, Font::Body, pal_.inkMuted) + 16;
    } else {
        double lo = placed[0].day, hi = placed[0].day;
        for (auto& it : placed) { lo = std::min(lo, it.day); hi = std::max(hi, it.day); }
        lo = std::floor(lo) - 1;
        hi = std::ceil(hi) + 1;
        const float labelW = 150, cardW = 132, cardH = 54;
        const float avail = cw - labelW;
        const double span = std::max(1.0, hi - lo);
        const float px = std::clamp((float)((avail - cardW - 16) / span), 36.0f, 220.0f);
        pl_.tlPx = px;
        pl_.tlDay0 = lo;
        pl_.tlContentW = (float)(span * px) + cardW + 16;
        pl_.tlScrollX = std::clamp(pl_.tlScrollX, 0.0f, std::max(0.0f, pl_.tlContentW - avail));
        const float x0 = m.x + labelW + cardW / 2 + 8 - pl_.tlScrollX;   // where `lo` is (half a card in, so its card fits)
        auto xOf = [&](double d) { return x0 + (float)((d - lo) * px); };
        const Rect area{m.x + labelW, y, avail, 0};
        // Ticks along the top.
        const int step = std::max(1, (int)std::ceil(70.0 / px));
        const float tickY = y;
        y += 26;
        // Lanes and their cards (stacked when they'd overlap).
        struct Placed { const Item* it; Rect rc; };
        std::vector<Placed> drawn;
        const float laneTop = y;
        std::vector<std::pair<float, float>> laneSpans;
        for (const Id& lane : lanes) {
            std::vector<std::vector<std::pair<float, float>>> rows;
            const float top = y;
            for (auto& it : placed) {
                if (it.n->povId != lane) continue;
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
        // Lane backgrounds and names.
        for (size_t i = 0; i < lanes.size(); ++i) {
            const auto [a, z] = laneSpans[i];
            if (i % 2 == 0) gfx_.fill({m.x, a, cw, z - a}, pal_.surface.withAlpha(0.45f));
            const Entry* e = lanes[i].empty() ? nullptr : lib_->entry(lanes[i]);
            const std::wstring name = e && !e->deleted ? w(e->name) : L"No viewpoint";
            if (e && !e->deleted) gfx_.fill({m.x, a + 6, 4, z - a - 12}, povColour(e->id));   // the viewpoint's colour, as on the cards
            gfx_.text(name, {m.x + 12, a + 10, labelW - 20, 40}, Font::BodyStrong, e ? pal_.ink : pal_.inkMuted, {Align::Left, true, true});
            gfx_.line(m.x, z - 0.5f, m.right(), z - 0.5f, pal_.rule);
        }
        // Ticks and day lines (clipped to the area right of the lane names).
        gfx_.pushClip({area.x, tickY, area.w, laneBottom - tickY});
        for (double d = std::ceil(lo / step) * step; d <= hi; d += step) {
            const float tx = xOf(d);
            gfx_.line(tx, laneTop, tx, laneBottom, pal_.rule.withAlpha(0.6f));
            gfx_.text(dayLabel(unit, d), {tx - 40, tickY, 80, 20}, Font::Small, pal_.inkFaint, {Align::Center, false});
        }
        // Reading order: a line from card to card.
        std::vector<const Placed*> byOrder;
        for (auto& p : drawn) byOrder.push_back(&p);
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
            const std::wstring when = t != times.end() && !t->second.when.empty() ? w(t->second.when) : dayLabel(unit, p.it->day);
            gfx_.text(when, {c.x + 12, c.y + 6, c.w - 20, 16}, Font::Label, pal_.ribbon, {Align::Right, false, true});
            gfx_.text(w(n->title.empty() ? std::string(label(n->kind)) : n->title), {c.x + 12, c.y + 24, c.w - 20, 22}, Font::BodyStrong,
                      pal_.ink, {Align::Left, false, true});
            if (focused(id)) focusRing(c);
        }
        gfx_.popClip();
        if (pl_.tlContentW > avail + 1)
            gfx_.text(L"Shift and the mouse wheel move along the timeline.", {m.x, laneBottom + 6, cw, 18}, Font::Small, pal_.inkFaint,
                      {Align::Right, false});
        y = laneBottom + 30;
    }
    // Not placed yet.
    if (!unplaced.empty()) {
        y += sectionLabel(m.x, y, cw, L"Not on the timeline yet");
        double last = 0;
        bool any = false;
        for (auto& [id, t] : times) if (t.placed) { last = any ? std::max(last, t.day) : t.day; any = true; }
        float x = m.x;
        for (const ManuscriptNode* n : unplaced) {
            const std::wstring title = n->title.empty() ? w(label(n->kind)) : w(n->title);
            const float tw = std::min(gfx_.measureWidth(title, Font::Small) + 86, 280.0f);
            if (x + tw > m.right()) { x = m.x; y += 38; }
            const Rect chip{x, y, tw, 30};
            const std::string id = "pl:tlu:" + n->id;
            addHit(id, {chip.x, chip.y, chip.w - 60, chip.h}, [this, nid = n->id] { selectCard(nid); });
            const bool sel = pl_.node == n->id;
            gfx_.fillRounded(chip, 15, sel ? pal_.surface : pal_.sunken);
            gfx_.strokeRounded(chip, 15, sel ? pal_.ribbon : pal_.rule);
            gfx_.text(title, {chip.x + 14, chip.y, chip.w - 80, chip.h}, Font::Small, pal_.ink, {Align::Left, false, true, true});
            const double next = any ? std::floor(last) + 1 : 1;
            linkText("pl:tlplace:" + n->id, chip.right() - 52, chip.y + 7, L"Place", [this, nid = n->id, next] {
                commitFields();
                StoryTime t = storyTimes(pl_.book)[nid];
                t.placed = true;
                t.day = next;
                setStoryTime(pl_.book, nid, t);
                selectCard(nid);
            });
            if (focused(id)) focusRing(chip);
            x += tw + 8;
        }
        y += 50;
    }
    pl_.content = y + pl_.scroll - r.y + 30;
    pl_.scroll = std::clamp(pl_.scroll, 0.0f, std::max(0.0f, pl_.content - r.h));
    clearHitClip();
    gfx_.popClip();
}

// Dragging a card along the timeline.
bool StudioApp::planMouseDown(float x, float y) {
    if (ws_ != Workspace::Planning || pl_.view != 1) return false;
    const std::string id = hitAt(x, y);
    if (id.rfind("pl:tl:", 0) != 0) return false;
    const Id nid = id.substr(6);
    const auto times = storyTimes(pl_.book);
    const auto it = times.find(nid);
    if (it == times.end() || !it->second.placed) return false;
    pl_.tlDrag = {};
    pl_.tlDrag.armed = true;
    pl_.tlDrag.id = nid;
    pl_.tlDrag.sx = x;
    pl_.tlDrag.startDay = pl_.tlDrag.day = it->second.day;
    return false;   // a click still selects it
}

bool StudioApp::planMouseMove(float x, float, bool leftDown) {
    if (!pl_.tlDrag.armed) return false;
    if (!leftDown) { pl_.tlDrag = {}; return false; }
    if (!pl_.tlDrag.active && std::abs(x - pl_.tlDrag.sx) > 6) { pl_.tlDrag.active = true; cancelPress(); }
    if (pl_.tlDrag.active) {
        // Whole steps of the unit, from where it started.
        pl_.tlDrag.day = pl_.tlDrag.startDay + std::round((x - pl_.tlDrag.sx) / std::max(1.0, pl_.tlPx));
        invalidate();
        return true;
    }
    return false;
}

bool StudioApp::planMouseUp(float, float) {
    if (!pl_.tlDrag.armed) return false;
    const bool moved = pl_.tlDrag.active && pl_.tlDrag.day != pl_.tlDrag.startDay;
    const Id id = pl_.tlDrag.id;
    const double day = pl_.tlDrag.day;
    const bool wasActive = pl_.tlDrag.active;
    pl_.tlDrag = {};
    if (moved) {
        commitFields();
        StoryTime t = storyTimes(pl_.book)[id];
        t.placed = true;
        t.day = day;
        setStoryTime(pl_.book, id, t);
        selectCard(id);
        loadFields();
        flash(L"Moved to " + dayLabel(timelineUnit(pl_.book), day) + L".", 3);
    }
    invalidate();
    return wasActive;
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
    const json st = structureOf(pl_.book);
    const Template* tp = findTemplate(st.value("template", ""));
    float y = m.y;
    if (!tp) {
        y += pageHeader(m, L"STRUCTURE", w(b->title),
                        L"Choose a story shape to lay over the book. Its beats are placed by length, so you can see which chapter falls "
                        L"where, and match each beat to the chapter that carries it. Structures are guides, not rules.");
        const float gap = 14, cardW = 300, cardH = 150;
        const int cols = std::max(1, (int)((cw + gap) / (cardW + gap)));
        const float wid = (cw - gap * (cols - 1)) / cols;
        int col = 0;
        for (const Template& t : templates()) {
            const Rect c{m.x + col * (wid + gap), y, wid, cardH};
            const std::string id = std::string("pl:tpl:") + t.id;
            addHit(id, c, [this, tid = std::string(t.id)] {
                json s = structureOf(pl_.book);
                s["template"] = tid;
                s["beats"] = json::object();
                setStructure(pl_.book, s);
                pl_.beat.clear();
                invalidate();
            });
            const float hv = hoverAmount(id);
            softShadow(c, 6, hv);
            gfx_.fillRounded(c, 6, pal_.surface);
            gfx_.strokeRounded(c, 6, pal_.rule.mix(pal_.ribbon, hv));
            gfx_.text(w(t.name), {c.x + 18, c.y + 16, c.w - 36, 24}, Font::Heading, pal_.ink, {Align::Left, false, true});
            gfx_.text(std::to_wstring(t.beats.size()) + L" beats", {c.x + 18, c.y + 16, c.w - 36, 24}, Font::Small, pal_.inkFaint,
                      {Align::Right, false, false, true});
            gfx_.pushClip({c.x, c.y + 46, c.w, c.h - 60});
            gfx_.text(w(t.about), {c.x + 18, c.y + 48, c.w - 36, c.h - 62}, Font::Small, pal_.inkMuted);
            gfx_.popClip();
            // A little map of where its beats fall.
            const float bx = c.x + 18, bw = c.w - 36, by = c.bottom() - 18;
            gfx_.line(bx, by, bx + bw, by, pal_.rule, 2);
            for (const Beat& bt : t.beats) gfx_.circle(bx + (float)bt.at * bw, by, 3.5f, pal_.ribbon, true);
            if (focused(id)) focusRing(c);
            if (++col == cols) { col = 0; y += cardH + gap; }
        }
        if (col) y += cardH + gap;
    } else {
        y += pageHeader(m, L"STRUCTURE: " + w(tp->name), w(b->title), w(tp->about));
        linkText("pl:tplchange", m.x, y - 8, L"Choose another structure", [this] {
            json s = structureOf(pl_.book);
            s.erase("template");
            setStructure(pl_.book, s);
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
        for (auto& s : segs) {
            if (!byWords) s.w = s.n->targetWords > 0 ? (double)s.n->targetWords : 1.0;
            total += s.w;
        }
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
        const json assigned = st.contains("beats") && st["beats"].is_object() ? st["beats"] : json::object();
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
                gfx_.fill(s, (i % 2 ? pal_.surface : pal_.sunken).mix(pal_.ribbon, sel ? 0.25f : 0.0f));
                if (s.w > 26) gfx_.text(std::to_wstring(segs[i].no), s, Font::Small, pal_.inkMuted, {Align::Center, false, false, true});
            }
        }
        gfx_.strokeRounded(bar, 4, pal_.rule);
        for (const Beat& bt : tp->beats) {
            const float bx = bar.x + (float)bt.at * bar.w;
            const bool has = assigned.contains(bt.id);
            gfx_.line(bx, bar.y - 6, bx, bar.bottom() + 6, has ? pal_.ribbon : pal_.inkFaint, 2);
            gfx_.circle(bx, bar.y - 8, 4, has ? pal_.ribbon : pal_.inkFaint, true);
        }
        y = bar.bottom() + 30;
        // Each beat.
        for (const Beat& bt : tp->beats) {
            const std::string bid = bt.id;
            const Seg* at = segs.empty() ? nullptr : chapterAt(bt.at);
            const std::string chosen = assigned.value(bid, "");
            const ManuscriptNode* cn = chosen.empty() ? nullptr : lib_->node(chosen);
            if (cn && cn->deleted) cn = nullptr;
            const float rowH = 74;
            const Rect row{m.x, y, cw, rowH};
            const std::string rid = "pl:beat:" + bid;
            const bool sel = pl_.beat == bid;
            addHit(rid, {row.x, row.y, row.w - 250, row.h}, [this, bid, chosen] {
                pl_.beat = bid;
                if (!chosen.empty() && lib_->node(chosen)) selectCard(chosen);
                invalidate();
            });
            gfx_.fillRounded(row, 6, sel ? pal_.surface : pal_.surface.withAlpha(0.55f));
            gfx_.strokeRounded(row, 6, sel ? pal_.ribbon : pal_.rule);
            gfx_.text(std::to_wstring((int)std::round(bt.at * 100)) + L"%", {row.x + 14, row.y + 12, 44, 20}, Font::BodyStrong, pal_.ribbon,
                      {Align::Left, false});
            gfx_.text(w(bt.name), {row.x + 64, row.y + 10, row.w - 330, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
            gfx_.text(w(bt.about), {row.x + 64, row.y + 32, row.w - 330, 36}, Font::Small, pal_.inkMuted, {Align::Left, true, true});
            // Which chapter carries it: chosen, and where it falls by length.
            const float rx = row.right() - 250;
            std::wstring pick = cn ? (cn->title.empty() ? w(label(cn->kind)) : w(cn->title)) : L"Choose a chapter";
            button("pl:beatpick:" + bid, {rx, row.y + 12, 236, 30}, pick, [this, bid, segs] {
                std::vector<MenuItem> items;
                items.push_back(MenuItem::item(L"None", 1));
                items.push_back(MenuItem::sep());
                for (size_t i = 0; i < segs.size(); ++i)
                    items.push_back(MenuItem::item(std::to_wstring(segs[i].no) + L". " +
                                                       (segs[i].n->title.empty() ? std::wstring(L"Chapter") : w(segs[i].n->title)),
                                                   (UINT)(10 + i)));
                POINT pt;
                GetCursorPos(&pt);
                const UINT c = showMenu(hwnd(), items, pt, pal_);
                if (!c) return;
                json s = structureOf(pl_.book);
                if (!s.contains("beats") || !s["beats"].is_object()) s["beats"] = json::object();
                if (c == 1) s["beats"].erase(bid);
                else if (c - 10 < segs.size()) s["beats"][bid] = segs[c - 10].n->id;
                setStructure(pl_.book, s);
                pl_.beat = bid;
                invalidate();
            }, cn != nullptr);
            std::wstring where = at ? L"By length: chapter " + std::to_wstring(at->no) : L"";
            if (cn && at && cn->id != at->n->id) {
                int cno = 0;
                for (auto& s : segs) if (s.n->id == cn->id) cno = s.no;
                if (cno) where += cno < at->no ? L" (yours comes early)" : L" (yours comes late)";
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
    const json st = structureOf(pl_.book);
    const Template* tp = findTemplate(st.value("template", ""));
    if (!tp) return;
    bool hasChapters = false;
    for (auto* n : lib_->manuscriptOf(pl_.book)) if (n->kind == NodeKind::Chapter) hasChapters = true;
    if (hasChapters &&
        cjui::messageBox(hwnd(), (L"Add " + std::to_wstring(tp->beats.size()) + L" chapters, one for each beat, after the book's last chapter? "
                             L"Each gets the beat's name as its title and its description as the synopsis.").c_str(),
                    L"Outline from a structure", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
    commitFields();
    json s = st;
    if (!s.contains("beats") || !s["beats"].is_object()) s["beats"] = json::object();
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
        if (!s["beats"].contains(bt.id)) s["beats"][bt.id] = id;
        if (first.empty()) first = id;
    }
    setStructure(pl_.book, s);
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
        const std::wstring unit = w(timelineUnit(pl_.book));
        y = panelField(c, y, std::wstring(L"Story ") + unit + L" (a number; blank: not on the timeline)", FStoryDay);
        y = panelField(c, y, L"When, in words (optional)", FWhen);
    } else if (!n && pl_.view == 1) {
        panelHeading(c, y, L"THE TIMELINE", false);
        const std::string unit = timelineUnit(pl_.book);
        static const char* units[] = {"day", "week", "month", "year"};
        static const wchar_t* names[] = {L"Days", L"Weeks", L"Months", L"Years"};
        int cur = 0;
        for (int i = 0; i < 4; ++i) if (unit == units[i]) cur = i;
        panelChoice("pl:unit", c, y, L"Counts in", names[cur], [this, cur] {
            std::vector<MenuItem> items;
            for (int i = 0; i < 4; ++i) items.push_back(MenuItem::item(names[i], (UINT)(i + 1), {}, true, i == cur));
            POINT pt;
            GetCursorPos(&pt);
            const UINT c2 = showMenu(hwnd(), items, pt, pal_);
            if (c2) { setTimelineUnit(pl_.book, units[c2 - 1]); invalidate(); }
        });
        y += gfx_.text(L"Number the story's days (or weeks, months, years) from any start you like: day 1 can be the first chapter's "
                       L"morning; a prologue a hundred years before is -36500, or use years. Words such as \"Midsummer, 1204\" show on "
                       L"the card instead of the number.",
                       {c.x, y, c.w, 120}, Font::Small, pal_.inkMuted) + 14;
    }
}

}  // namespace nrs
