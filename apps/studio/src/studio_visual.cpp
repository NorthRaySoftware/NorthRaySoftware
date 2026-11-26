// NorthRay Writing Studio: the bible at a glance (0.87). Each kind of page has a colour and a symbol; a page shows its
// picture as a portrait, or the symbol the writer chose for it (an emoji or one of the app's drawn symbols), or its
// initials, so lists, links, search results, Who's there and the timeline can be read without reading.

#include "studio_app.hpp"
#include "cjui/menu.hpp"
#include "cjui/dialog.hpp"
#include <algorithm>
#include <optional>
#include <nlohmann/json.hpp>
#include "studio/calendar.hpp"

#include <cmath>
#include <cwctype>

namespace nrs {

using namespace studio;
using cjui::Align;
using cjui::Font;
using cjui::Rect;

static std::wstring w(const std::string& s) { return widen(s); }
std::vector<Id> booksInReadingOrder(const Library& lib, const Id& world, const Id& book);   // studio_world.cpp

cjui::Color entryKindColor(const std::string& kind, bool dark) {
    struct K { const char* k; const char* light; const char* dark; };
    static const K ks[] = {{"character", "#3E7BD6", "#7AA9F0"}, {"creature", "#C26A2E", "#E69A5F"}, {"place", "#2E9A6C", "#5BC99A"},
                           {"race", "#6B8E23", "#A5C76A"},      {"realm", "#5B5FC7", "#9A9DF0"},    {"group", "#7D5BD0", "#AE93F2"},
                           {"thing", "#B08A1E", "#E0BE55"},     {"lore", "#1E97A6", "#55C6D4"},     {"magic", "#B3439E", "#E37FD0"},
                           {"event", "#C9476A", "#F07C98"},     {"language", "#4F7F8F", "#8DB9C8"}};
    for (auto& x : ks) if (kind == x.k) return cjui::hexColor(dark ? x.dark : x.light);
    return cjui::hexColor(dark ? "#7AA9F0" : "#3E7BD6");
}

cjui::Icon entryKindIcon(const std::string& kind) {
    using cjui::Icon;
    if (kind == "character") return Icon::Person;
    if (kind == "race") return Icon::People;
    if (kind == "creature") return Icon::Paw;
    if (kind == "place") return Icon::House;
    if (kind == "realm") return Icon::Mountain;
    if (kind == "group") return Icon::Shield;
    if (kind == "thing") return Icon::Gem;
    if (kind == "lore") return Icon::Scroll;
    if (kind == "magic") return Icon::Star;
    if (kind == "event") return Icon::Flag;
    if (kind == "language") return Icon::Speech;
    return Icon::Book;
}

namespace {
std::wstring initialsOf(const std::string& name) {
    std::wstring out;
    bool start = true;
    for (wchar_t c : widen(name)) {
        if (iswspace(c)) { start = true; continue; }
        if (start && iswalnum(c) && out.size() < 2) out += (wchar_t)towupper(c);
        start = false;
    }
    return out.empty() ? L"?" : out;
}
}  // namespace

cjui::Color StudioApp::entryColour(const Entry& e) const {
    if (e.colour.size() == 7 && e.colour[0] == '#') return cjui::hexColor(e.colour.c_str());
    if (e.kind == "character" || e.kind == "creature") return povColour(e.id);   // as their lanes on the timeline
    return entryKindColor(e.kind, pal_.dark);
}

// Dark ink on a light colour, white on a dark one (the colour's relative luminance, sRGB).
cjui::Color StudioApp::inkOn(cjui::Color c) const {
    auto lin = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
    const float l = 0.2126f * lin(c.r) + 0.7152f * lin(c.g) + 0.0722f * lin(c.b);
    return l > 0.36f ? cjui::hexColor("#15171E") : cjui::hexColor("#FFFFFF");
}

// Its symbol in a small disc of its colour at the picture's lower right, ringed in the surface so it stands off the photo.
void StudioApp::drawSymbolBadge(const Entry& e, const Rect& pic) {
    const float side = std::min(pic.w, pic.h);
    if (e.symbol.empty() || side < 28) return;
    const float bs = std::clamp(side * 0.38f, 14.0f, 34.0f);
    const Rect b{pic.right() - bs * 0.85f, pic.bottom() - bs * 0.85f, bs, bs};
    const cjui::Color c = entryColour(e);
    gfx_.fillRounded(b.inset(-2), bs / 2 + 2, pal_.surface);
    gfx_.fillRounded(b, bs / 2, c);
    if (e.symbol.rfind("blob:", 0) == 0) {
        gfx_.drawImageRounded(w(lib_->blobPath(e.symbol.substr(5))), b, bs / 2);
        return;
    }
    const float isz = bs * 0.62f;
    drawSymbol(e.symbol, {b.x + (bs - isz) / 2, b.y + (bs - isz) / 2, isz, isz}, inkOn(c), cjui::Icon::None);
}

void StudioApp::drawPortrait(const Entry& e, const Rect& r, float radius) {
    // Phase 4a (0.101): its picture with its symbol as a badge; or, with "symbol alone" or no picture, its symbol (or
    // initials) in that colour's ink on a solid disc of its colour, which reads in light and dark themes alike.
    const bool symbolAlone = e.iconStyle == "symbol" && !e.symbol.empty();
    if (!symbolAlone && !e.imageBlob.empty() && gfx_.drawImageRounded(w(lib_->blobPath(e.imageBlob)), r, radius)) {
        gfx_.strokeRounded(r, radius, pal_.rule.withAlpha(0.8f), 1);
        drawSymbolBadge(e, r);
        return;
    }
    const cjui::Color c = entryColour(e);
    const cjui::Color ink = inkOn(c);
    gfx_.fillRounded(r, radius, c);
    gfx_.strokeRounded(r.inset(0.5f), std::max(0.0f, radius - 0.5f), c.mix(pal_.ink, pal_.dark ? 0.0f : 0.25f).withAlpha(pal_.dark ? 0.0f : 0.5f), 1);
    const float side = std::min(r.w, r.h);
    if (e.symbol.rfind("blob:", 0) == 0 && gfx_.drawImageRounded(w(lib_->blobPath(e.symbol.substr(5))), r.inset(side * 0.12f), std::max(0.0f, radius - side * 0.12f)))
        return;
    if (!e.symbol.empty() && e.symbol.rfind("blob:", 0) != 0) {
        const float isz = side * 0.58f;
        drawSymbol(e.symbol, {r.x + (r.w - isz) / 2, r.y + (r.h - isz) / 2, isz, isz}, ink, cjui::Icon::None);
        return;
    }
    const Font f = side >= 60 ? Font::Heading : side >= 30 ? Font::SmallStrong : Font::Label;
    gfx_.text(initialsOf(e.name), r, f, ink, {Align::Center, false, false, true});
}

void StudioApp::drawKindMark(const std::string& kind, const Rect& r, const std::string& tipId) {
    const cjui::Color kc = entryKindColor(kind, pal_.dark);
    const float side = std::min(r.w, r.h);
    const Rect d{r.x + (r.w - side) / 2, r.y + (r.h - side) / 2, side, side};
    gfx_.fillRounded(d, side / 2, kc);
    const float isz = side * 0.62f;
    cjui::drawIcon(gfx_, entryKindIcon(kind), {d.x + (side - isz) / 2, d.y + (side - isz) / 2, isz, isz}, pal_.dark ? pal_.paper : pal_.surface,
                   std::max(1.1f, side / 14));
    if (!tipId.empty()) {
        addHit(tipId, d, nullptr, false);
        tooltip(tipId, w(entryKindLabel(kind)));
    }
}

void StudioApp::drawSymbol(const std::string& symbol, const Rect& r, cjui::Color ink, cjui::Icon fallback) {
    const float side = std::min(r.w, r.h);
    const Rect sq{r.x + (r.w - side) / 2, r.y + (r.h - side) / 2, side, side};
    if (symbol.rfind("icon:", 0) == 0) {
        const cjui::Icon ic = cjui::iconNamed(symbol.substr(5));
        cjui::drawIcon(gfx_, ic != cjui::Icon::None ? ic : fallback, sq, ink, std::max(1.1f, side / 12));
    } else if (!symbol.empty())
        gfx_.textSized(w(symbol), {sq.x - side * 0.2f, sq.y - side * 0.2f, side * 1.4f, side * 1.4f}, std::clamp(side * 0.85f, 8.0f, 200.0f), false, ink,
                       {Align::Center, false, false, true});
    else if (fallback != cjui::Icon::None)
        cjui::drawIcon(gfx_, fallback, sq, ink, std::max(1.1f, side / 12));
}

void StudioApp::chooseSymbol(const std::string& current, const std::wstring& none, std::function<void(const std::string&)> pick, bool drawnToo) {
    // A handful of emoji worlds tend to need, and the app's own drawn symbols. The emoji are drawn by Windows' emoji font.
    static const wchar_t* emoji[] = {L"\U0001F409", L"\U0001F43A", L"\U0001F985", L"\U0001F40D", L"\U0001F984", L"\U0001F9D9", L"\U0001F451",
                                     L"⚔️", L"\U0001F6E1️", L"\U0001F3F0", L"\U0001F30B", L"\U0001F30A", L"\U0001F332",
                                     L"\U0001F3D4️", L"\U0001F319", L"⭐", L"\U0001F525", L"\U0001F48E", L"\U0001F4DC",
                                     L"\U0001F5DD️", L"\U0001F56F️", L"\U0001F9EA", L"⚓", L"\U0001F43E", L"\U0001F480",
                                     L"\U0001F47B", L"\U0001F33F", L"\U0001F33A", L"✨", L"☠️", L"\U0001F386", L"\U0001F3EE",
                                     L"\U0001F56F", L"\U0001F342", L"❄️", L"\U0001F33E", L"\U0001F377", L"\U0001F514"};
    std::vector<cjui::MenuItem> em, drawn;
    for (size_t i = 0; i < sizeof emoji / sizeof *emoji; ++i) em.push_back(cjui::MenuItem::item(emoji[i], 100 + (UINT)i, {}, true, w(current) == emoji[i]));
    const auto& icons = cjui::symbolIcons();
    for (size_t i = 0; i < icons.size(); ++i) {
        std::wstring nm = w(cjui::iconName(icons[i]));
        if (!nm.empty()) nm[0] = (wchar_t)towupper(nm[0]);
        drawn.push_back(cjui::MenuItem::item(nm, 300 + (UINT)i, {}, true, current == std::string("icon:") + cjui::iconName(icons[i])));
    }
    // Ornaments that print in a book (0.89), and the drawn symbols only where they're shown on screen.
    static const wchar_t* orn[] = {L"✧", L"✦", L"❦", L"❧", L"✠", L"☙", L"⁂", L"✵", L"✶", L"❖", L"◆", L"☽", L"☀", L"⚜", L"\U0001F5E1"};
    std::vector<cjui::MenuItem> ornaments;
    for (size_t i = 0; i < sizeof orn / sizeof *orn; ++i) ornaments.push_back(cjui::MenuItem::item(orn[i], 200 + (UINT)i, {}, true, w(current) == orn[i]));
    std::vector<cjui::MenuItem> items{cjui::MenuItem::sub(L"An ornament", ornaments), cjui::MenuItem::sub(L"An emoji", em)};
    if (drawnToo) items.push_back(cjui::MenuItem::sub(L"A drawn symbol", drawn));
    items.push_back(cjui::MenuItem::item(none, 1, {}, true, current.empty()));
    POINT pt;
    GetCursorPos(&pt);
    const UINT c = cjui::showMenu(hwnd(), items, pt, pal_);
    if (!c) return;
    std::string out;
    if (c >= 300) out = std::string("icon:") + cjui::iconName(icons[c - 300]);
    else if (c >= 200) out = narrow(orn[c - 200]);
    else if (c >= 100) out = narrow(emoji[c - 100]);
    if (out != current) pick(out);
}

void StudioApp::chooseEntryColour(const Id& entryId) {
    const Entry* e = lib_->entry(entryId);
    if (!e) return;
    // A dozen colours that hold up in both themes, and the automatic one (0.101).
    static const std::pair<const wchar_t*, const char*> swatches[] = {
        {L"Blue", "#3B7DD8"}, {L"Red", "#D2513A"}, {L"Green", "#2E9E6B"}, {L"Purple", "#9B59B6"}, {L"Amber", "#D48A1F"}, {L"Teal", "#1F9CA8"},
        {L"Rose", "#C2417E"}, {L"Olive", "#6C7A2E"}, {L"Indigo", "#5B5FD6"}, {L"Brown", "#A0522D"}, {L"Slate", "#5E6878"}, {L"Gold", "#C9A227"}};
    std::vector<cjui::MenuItem> items;
    items.push_back(cjui::MenuItem::item(e->kind == "character" || e->kind == "creature" ? L"Automatic (as on the timeline)" : L"Automatic (its kind's)",
                                         1, {}, true, e->colour.empty()));
    for (size_t i = 0; i < sizeof swatches / sizeof *swatches; ++i)
        items.push_back(cjui::MenuItem::item(std::wstring(L"\u25CF  ") + swatches[i].first, 10 + (UINT)i, {}, true, e->colour == swatches[i].second));
    items.push_back(cjui::MenuItem::item(L"Another colour (#RRGGBB)…", 2));
    POINT pt;
    GetCursorPos(&pt);
    const UINT c = cjui::showMenu(hwnd(), items, pt, pal_);
    if (!c) return;
    std::string hex;
    if (c >= 10) hex = swatches[c - 10].second;
    else if (c == 2) {
        std::wstring v = w(e->colour);
        if (!cjui::askText(hwnd(), L"A colour of its own", L"A colour as #RRGGBB, such as #3B7DD8.", v)) return;
        std::string t = narrow(v);
        t.erase(std::remove_if(t.begin(), t.end(), [](char ch) { return std::isspace((unsigned char)ch); }), t.end());
        if (!t.empty() && t[0] != '#') t = "#" + t;
        const bool ok = t.size() == 7 && std::all_of(t.begin() + 1, t.end(), [](char ch) { return std::isxdigit((unsigned char)ch); });
        if (!ok) { flash(L"That isn't a colour as #RRGGBB, so nothing changed."); return; }
        for (auto& ch : t) ch = (char)std::toupper((unsigned char)ch);
        hex = t;
    }
    const Entry* x0 = lib_->entry(entryId);
    if (!x0 || x0->colour == hex) return;
    const Entry before = *x0;
    Entry x = *x0;
    x.colour = hex;
    lib_->updateEntry(x);
    flashUndo(L"Colour changed.", [this, before] { lib_->updateEntry(before); invalidate(); });
    invalidate();
}

void StudioApp::setEntryIconStyle(const Id& entryId, bool symbolAlone) {
    const Entry* x0 = lib_->entry(entryId);
    if (!x0) return;
    const Entry before = *x0;
    Entry x = *x0;
    x.iconStyle = symbolAlone ? "symbol" : "";
    if (x.iconStyle == before.iconStyle) return;
    lib_->updateEntry(x);
    flashUndo(symbolAlone ? L"Its icon shows its symbol alone." : L"Its icon shows its picture, with its symbol as a badge.",
              [this, before] { lib_->updateEntry(before); invalidate(); });
    invalidate();
}

void StudioApp::chooseEntrySymbol(const Id& entryId) {
    const Entry* e = lib_->entry(entryId);
    if (!e) return;
    // A picture or a typed character of its own (0.101), before the ornaments, emoji and drawn symbols.
    {
        std::vector<cjui::MenuItem> first{cjui::MenuItem::item(L"A small picture of its own…", 1, {}, true, e->symbol.rfind("blob:", 0) == 0),
                                          cjui::MenuItem::item(L"A character you type (a letter, a rune, an emoji)…", 2),
                                          cjui::MenuItem::item(L"Ornaments, emoji and drawn symbols…", 3)};
        POINT pt;
        GetCursorPos(&pt);
        const UINT c = cjui::showMenu(hwnd(), first, pt, pal_);
        if (!c) return;
        std::string sym;
        if (c == 1) {
            const std::string blob = pickPicture(L"Its symbol");
            if (blob.empty()) return;
            sym = "blob:" + blob;
        } else if (c == 2) {
            std::wstring v = e->symbol.rfind("blob:", 0) == 0 || e->symbol.rfind("icon:", 0) == 0 ? std::wstring() : w(e->symbol);
            if (!cjui::askText(hwnd(), L"Its symbol", L"Type or paste the symbol: a letter, a rune, an emoji (up to four characters).", v)) return;
            while (!v.empty() && iswspace(v.back())) v.pop_back();
            while (!v.empty() && iswspace(v.front())) v.erase(v.begin());
            if (v.size() > 8) v.resize(8);   // a few characters (an emoji can take several)
            sym = narrow(v);
        }
        if (c == 1 || c == 2) {
            const Entry* x0 = lib_->entry(entryId);
            if (!x0 || x0->symbol == sym) return;
            const Entry before = *x0;
            Entry x = *x0;
            x.symbol = sym;
            lib_->updateEntry(x);
            flashUndo(L"Symbol changed.", [this, before] { lib_->updateEntry(before); invalidate(); });
            invalidate();
            return;
        }
    }
    chooseSymbol(e->symbol, L"Its initials", [this, entryId](const std::string& sym) {
        const Entry* x0 = lib_->entry(entryId);
        if (!x0) return;
        const Entry before = *x0;
        Entry x = *x0;
        x.symbol = sym;
        lib_->updateEntry(x);
        flashUndo(L"Symbol changed.", [this, before] { lib_->updateEntry(before); invalidate(); });
        invalidate();
    });
}

void StudioApp::drawMoonDisc(float mx, float my, float rad, double ph) {
    gfx_.circle(mx, my, rad, pal_.ink.withAlpha(0.55f), true);
    const float k = (float)std::cos(ph * 2 * 3.14159265358979);
    for (float dy = -rad + 0.5f; dy < rad; dy += 0.5f) {
        const float hw = std::sqrt(std::max(0.0f, rad * rad - dy * dy));
        const float a = ph <= 0.5 ? mx + hw * k : mx - hw, b = ph <= 0.5 ? mx + hw : mx - hw * k;
        if (b - a > 0.2f) gfx_.line(a, my + dy, b, my + dy, cjui::hexColor("#F6E7A6"), 0.6f);   // moonlight, bright in either theme
    }
    gfx_.circle(mx, my, rad, pal_.inkFaint, false, 1);
}

void StudioApp::drawDateChip(const Rect& r, int day, const std::wstring& month) {
    gfx_.fillRounded(r, 3, pal_.surface);
    gfx_.strokeRounded(r, 3, pal_.rule.mix(pal_.inkFaint, 0.4f), 1);
    const float head = std::max(6.0f, r.h * 0.34f);
    gfx_.fillRounded({r.x, r.y, r.w, head}, 3, cjui::hexColor(pal_.dark ? "#C0504D" : "#D9534F"));
    gfx_.fill({r.x, r.y + head - 3, r.w, 3}, cjui::hexColor(pal_.dark ? "#C0504D" : "#D9534F"));
    std::wstring m = month.substr(0, std::min<size_t>(3, month.size()));
    for (auto& ch : m) ch = (wchar_t)towupper(ch);
    gfx_.text(m, {r.x, r.y - 1, r.w, head + 2}, Font::Label, cjui::hexColor("#FFFFFF"), {Align::Center, false, false, true});
    gfx_.text(std::to_wstring(day), {r.x, r.y + head, r.w, r.h - head}, Font::SmallStrong, pal_.ink, {Align::Center, false, false, true});
}

void StudioApp::drawProgressRing(float cx, float cy, float rad, float f) {
    constexpr float kPi = 3.14159265f;
    f = std::clamp(f, 0.0f, 1.0f);
    const float wdt = std::max(2.5f, rad * 0.22f);
    gfx_.circle(cx, cy, rad, pal_.rule, false, wdt);
    const cjui::Color c = f >= 1 ? cjui::hexColor(pal_.dark ? "#D6B677" : "#C9A23A") : pal_.ribbon;
    if (f > 0.002f) gfx_.arc(cx, cy, rad, -kPi / 2, -kPi / 2 + 2 * kPi * std::min(f, 0.9999f), c, wdt);
    if (rad >= 14) {
        const std::wstring t = f >= 1 ? L"\u2713" : std::to_wstring((int)std::floor(f * 100)) + L"%";
        gfx_.text(t, {cx - rad, cy - rad, rad * 2, rad * 2}, rad >= 22 ? Font::SmallStrong : Font::Label, pal_.ink, {Align::Center, false, false, true});
    }
}

float StudioApp::drawYearWheel(const Rect& m, float y, const cal::Calendar& c) {
    constexpr float kPi = 3.14159265f;
    if (c.months.empty()) return y;
    // The story's "now": the chapter open in Writing (or its first dated scene), when its book counts in calendar dates.
    std::optional<std::int64_t> now;
    std::wstring nowWhat;
    if (const ManuscriptNode* n = lib_->node(currentNode_); n && !n->deleted) {
        const cal::StorySpan sp = cal::storySpan(*lib_, n->bookId);
        const nlohmann::json tl = nlohmann::json::parse(lib_->setting("timeline:" + n->bookId), nullptr, false);
        if (sp.on && tl.is_object() && tl.contains("nodes") && tl["nodes"].contains(n->id) && tl["nodes"][n->id].contains("day"))
            if (tl["nodes"][n->id]["day"].is_number()) {
                now = sp.start + (std::int64_t)std::floor(tl["nodes"][n->id]["day"].get<double>());
                nowWhat = widen(n->title);
            }
    }
    if (now && !wb_.calYearSet) {
        wb_.calYear = cal::fromDay(c, *now).year;
        wb_.calYearSet = true;
    }
    const std::int64_t year = wb_.calYear;
    // The year, ‹ ›.
    button("calw:prev", {m.x, y, 36, 34}, L"‹", [this] { --wb_.calYear; wb_.calYearSet = true; invalidate(); });
    gfx_.text(w(cal::formatYear(c, year)), {m.x + 46, y, 260, 34}, Font::Heading, pal_.ink, {Align::Left, false, false, true});
    button("calw:next", {m.x + 46 + std::min(260.0f, gfx_.measureWidth(w(cal::formatYear(c, year)), Font::Heading) + 14), y, 36, 34}, L"›",
           [this] { ++wb_.calYear; wb_.calYearSet = true; invalidate(); });
    if (now) {
        float bw = 0;
        smallButton("calw:now", m.right() - buttonWidth(L"Go to the story's now") , y + 2, L"Go to the story's now", [this, c, now] {
            wb_.calYear = cal::fromDay(c, *now).year;
            invalidate();
        }, &bw);
    }
    // As a wheel or a strip of months (0.88).
    segmented("calw:shape", {m.x + 360, y + 3, 170, 28}, {L"Wheel", L"Strip"}, wb_.yearStrip ? 1 : 0, [this](int i) { wb_.yearStrip = i == 1; invalidate(); });
    y += 50;
    const std::int64_t first = cal::toDay(c, {year, 0, 1});
    const std::int64_t len = std::max<std::int64_t>(1, cal::yearLength(c, year));
    auto angle = [&](double dayInYear) { return -kPi / 2 + 2 * kPi * (float)(dayInYear / (double)len); };
    const float D = std::min(m.w * 0.62f, 560.0f);
    const float cx = m.x + D / 2 + 40, cy = y + D / 2 + 30;
    const float R = D / 2, ring = 34;
    // Seasons: a colour each (spring green, summer gold, autumn rust, winter blue by their names; else in turn).
    auto seasonColour = [&](const std::string& name, size_t i) {
        std::string l;
        for (char ch : name) l += (char)std::tolower((unsigned char)ch);
        const bool d = pal_.dark;
        if (l.find("spring") != std::string::npos || l.find("bloom") != std::string::npos) return cjui::hexColor(d ? "#5BAF6A" : "#7CC48A");
        if (l.find("summer") != std::string::npos) return cjui::hexColor(d ? "#D4A93C" : "#E9C25A");
        if (l.find("autumn") != std::string::npos || l.find("fall") != std::string::npos || l.find("harvest") != std::string::npos)
            return cjui::hexColor(d ? "#C2703A" : "#DA8A50");
        if (l.find("winter") != std::string::npos || l.find("frost") != std::string::npos) return cjui::hexColor(d ? "#5C8FC2" : "#86AEDB");
        static const char* p[] = {"#7CC48A", "#E9C25A", "#DA8A50", "#86AEDB", "#B392D9", "#E58FA8"};
        return cjui::hexColor(p[i % 6]);
    };
    if (wb_.yearStrip) {
        // ---- The strip: the months left to right, as wide as their days, in their seasons' colours; holidays and moons
        // above, each book's span below, the story's now as a line through it.
        const float L = m.x, W = m.w;
        auto xOf = [&](double dayInYear) { return L + W * (float)(dayInYear / (double)len); };
        const float barY = y + 58, barH = 44;
        std::int64_t at = 0;
        for (int mo = 0; mo < (int)c.months.size(); ++mo) {
            const int ml = cal::monthLength(c, year, mo);
            if (ml <= 0) continue;
            const cal::Season* sn = cal::seasonOn(c, {year, mo, std::max(1, ml / 2)});
            size_t si = 0;
            if (sn) for (size_t k = 0; k < c.seasons.size(); ++k) if (&c.seasons[k] == sn) si = k;
            const Rect mr{xOf((double)at) + 1, barY, xOf((double)(at + ml)) - xOf((double)at) - 2, barH};
            const std::string id = "calw:sm:" + std::to_string(mo);
            addHit(id, mr, [this, mo, year] { wb_.calMode = 0; wb_.calMonth = mo; wb_.calYear = year; invalidate(); });
            const std::wstring nm = w(cal::monthLabel(c, mo));
            tooltip(id, nm + L": " + std::to_wstring(ml) + L" days" + (sn ? L", " + w(sn->name) : std::wstring()) + L". Click for the month.");
            gfx_.fillRounded(mr, 5, (sn ? seasonColour(sn->name, si) : pal_.rule).withAlpha(0.75f + 0.25f * hoverAmount(id)));
            gfx_.text(nm, {mr.x + 2, mr.y, mr.w - 4, mr.h}, mr.w > 60 ? Font::SmallStrong : Font::Label, pal_.dark ? pal_.paper : pal_.ink,
                      {Align::Center, false, true, true});
            at += ml;
        }
        // Holidays: their symbols over the bar, a tick down to their day.
        for (size_t h = 0; h < c.holidays.size(); ++h) {
            const cal::Holiday& hd = c.holidays[h];
            if (hd.month < 0 || hd.month >= (int)c.months.size()) continue;
            const cal::Date dd = cal::clampDate(c, {year, hd.month, hd.day});
            const float hx = xOf((double)(cal::toDay(c, dd) - first) + 0.5);
            gfx_.line(hx, barY - 12, hx, barY, gold_, 1.2f);
            drawSymbol(hd.symbol, {hx - 8, barY - 30, 16, 16}, gold_, cjui::Icon::Star);
            const std::string id = "calw:sh:" + std::to_string(h);
            addHit(id, {hx - 9, barY - 31, 18, 18}, nullptr, false);
            tooltip(id, w(hd.name) + L"  \u00B7  " + w(cal::formatDate(c, dd, false)) + (hd.notes.empty() ? std::wstring() : L"\n" + w(hd.notes)));
        }
        // The first moon's full and new moons, and eclipses, in a row above.
        if (!c.moons.empty()) {
            double prev = cal::moonPhase(c, c.moons[0], first - 1);
            for (std::int64_t d = 0; d < len; ++d) {
                const double ph = cal::moonPhase(c, c.moons[0], first + d);
                const bool full = prev < 0.5 && ph >= 0.5, fresh = ph < prev;
                prev = ph;
                const auto ecl = cal::eclipsesOn(c, first + d);
                if (!full && !fresh && ecl.empty()) continue;
                const float mx = xOf(d + 0.5), my = barY - 46;
                drawMoonDisc(mx, my, 5, full ? 0.5 : 0.0);
                if (!ecl.empty()) gfx_.circle(mx, my, 8, pal_.dark ? cjui::hexColor("#F08A6C") : cjui::hexColor("#B4442C"), false, 1.8f);
                const std::string id = "calw:smoon:" + std::to_string(d);
                addHit(id, {mx - 7, my - 7, 14, 14}, nullptr, false);
                tooltip(id, w(c.moons[0].name) + L": " + (full ? L"full" : L"new") + L" on " + w(cal::formatDate(c, cal::fromDay(c, first + d), false)) +
                                (ecl.empty() ? L"" : L", an eclipse"));
            }
        }
        // Each book's span below.
        float by = barY + barH + 14;
        for (const Id& b : booksInReadingOrder(*lib_, wb_.world, wb_.book)) {
            const cal::StorySpan sp = cal::storySpan(*lib_, b);
            if (!sp.on || !sp.any || sp.last < first || sp.first >= first + len) continue;
            const studio::Book* bk = lib_->book(b);
            if (!bk) continue;
            const double a = (double)std::max(sp.first, first) - first, z = (double)std::min(sp.last + 1, first + len) - first;
            const Rect br{xOf(a), by, std::max(8.0f, xOf(z) - xOf(a)), 20};
            gfx_.fillRounded(br, 10, povColour(b).withAlpha(0.85f));
            // The title inside its bar when it fits, after the bar when it doesn't, before it near the right edge.
            const std::wstring bt = w(bk->title);
            const float tw = gfx_.measureWidth(bt, Font::Small);
            float tx = br.x + 8;
            if (tw + 16 > br.w) tx = br.x + br.w + 8 + tw <= L + W ? br.x + br.w + 8 : br.x - tw - 8;
            gfx_.text(bt, {tx, br.y, tw + 4, br.h}, Font::Small, pal_.ink, {Align::Left, false, true, true});
            const std::string id = "calw:sb:" + b;
            addHit(id, br, nullptr, false);
            tooltip(id, w(bk->title) + L": " + w(cal::formatDate(c, cal::fromDay(c, sp.first))) + L" to " + w(cal::formatDate(c, cal::fromDay(c, sp.last))));
            by += 26;
        }
        if (now && *now >= first && *now < first + len) {
            const float nx = xOf((double)(*now - first) + 0.5);
            gfx_.line(nx, barY - 6, nx, by, pal_.ribbon, 2.5f);
            gfx_.circle(nx, barY - 6, 4.5f, pal_.ribbon, true);
            addHit("calw:snow", {nx - 6, barY - 12, 12, by - barY + 12}, nullptr, false);
            tooltip("calw:snow", L"Now in the story: \u201C" + nowWhat + L"\u201D, " + w(cal::formatDate(c, cal::fromDay(c, *now))));
        }
        y = by + 16;
        // The key, in a row.
        float kx = L;
        auto key = [&](std::function<void(float, float)> mark, const std::wstring& t) {
            const float tw = gfx_.measureWidth(t, Font::Small);
            if (kx + tw + 30 > L + W) { kx = L; y += 24; }
            mark(kx + 8, y + 10);
            gfx_.text(t, {kx + 22, y, tw + 4, 20}, Font::Small, pal_.ink, {Align::Left, false, false, true});
            kx += tw + 40;
        };
        for (size_t k = 0; k < c.seasons.size(); ++k)
            key([&](float x, float yy) { gfx_.fillRounded({x - 7, yy - 6, 14, 12}, 3, seasonColour(c.seasons[k].name, k)); }, w(c.seasons[k].name));
        key([&](float x, float yy) { cjui::drawIcon(gfx_, cjui::Icon::Star, {x - 7, yy - 7, 14, 14}, gold_, 1.3f); }, L"A holiday");
        if (!c.moons.empty()) {
            key([&](float x, float yy) { drawMoonDisc(x, yy, 5, 0.5); }, L"Full moon");
            key([&](float x, float yy) { drawMoonDisc(x, yy, 5, 0.0); }, L"New moon");
        }
        if (now) key([&](float x, float yy) { gfx_.circle(x, yy, 4.5f, pal_.ribbon, true); }, L"The story's now");
        return y + 40;
    }
    // Each month: an arc of the ring in the season it starts in, its name outside.
    std::int64_t at = 0;
    for (int mo = 0; mo < (int)c.months.size(); ++mo) {
        const int ml = cal::monthLength(c, year, mo);
        if (ml <= 0) continue;
        const cal::Season* s = cal::seasonOn(c, {year, mo, std::max(1, ml / 2)});
        size_t si = 0;
        if (s) for (size_t k = 0; k < c.seasons.size(); ++k) if (&c.seasons[k] == s) si = k;
        const cjui::Color sc = s ? seasonColour(s->name, si) : pal_.rule;
        const float a0 = angle((double)at), a1 = angle((double)(at + ml));
        const std::string id = "calw:m:" + std::to_string(mo);
        const float mid = (a0 + a1) / 2;
        const float tx = cx + (R + 26) * std::cos(mid), ty = cy + (R + 26) * std::sin(mid);
        const std::wstring nm = w(cal::monthLabel(c, mo));
        const float nw = std::min(120.0f, gfx_.measureWidth(nm, Font::Small) + 6);
        const Rect lr{tx - nw / 2, ty - 10, nw, 20};
        addHit(id, lr, [this, mo, year] { wb_.calMode = 0; wb_.calMonth = mo; wb_.calYear = year; invalidate(); });
        tooltip(id, nm + L": " + std::to_wstring(ml) + L" days" + (s ? L", " + w(s->name) : std::wstring()) + L". Click for the month.");
        const float hv = hoverAmount(id);
        gfx_.arc(cx, cy, R - ring / 2, a0 + 0.004f, a1 - 0.004f, sc.withAlpha(0.75f + 0.25f * hv), ring);
        gfx_.text(nm, lr, hv > 0.01f ? Font::SmallStrong : Font::Small, pal_.ink, {Align::Center, false, false, true});
        // The month's first day as a tick.
        gfx_.line(cx + (R - ring) * std::cos(a0), cy + (R - ring) * std::sin(a0), cx + R * std::cos(a0), cy + R * std::sin(a0), pal_.surface, 2);
        at += ml;
    }
    // Holidays: a star on the ring at their day.
    for (size_t h = 0; h < c.holidays.size(); ++h) {
        const cal::Holiday& hd = c.holidays[h];
        if (hd.month < 0 || hd.month >= (int)c.months.size()) continue;
        const std::int64_t d = cal::toDay(c, cal::clampDate(c, {year, hd.month, hd.day})) - first;
        const float a = angle(d + 0.5);
        const float hx = cx + (R - ring / 2) * std::cos(a), hy = cy + (R - ring / 2) * std::sin(a);
        const std::string id = "calw:h:" + std::to_string(h);
        addHit(id, {hx - 9, hy - 9, 18, 18}, nullptr, false);
        tooltip(id, w(hd.name) + L"  ·  " + w(cal::formatDate(c, cal::clampDate(c, {year, hd.month, hd.day}), false)) +
                        (hd.notes.empty() ? std::wstring() : L"\n" + w(hd.notes)));
        gfx_.circle(hx, hy, 9, pal_.surface, true);
        drawSymbol(hd.symbol, {hx - 7, hy - 7, 14, 14}, gold_, cjui::Icon::Star);   // its own symbol, else a star (0.88)
    }
    // The moons: full and new moons (the first moon) just inside the ring; eclipses ringed in red.
    const float moonR = R - ring - 14;
    if (!c.moons.empty()) {
        double prev = cal::moonPhase(c, c.moons[0], first - 1);
        for (std::int64_t d = 0; d < len; ++d) {
            const double ph = cal::moonPhase(c, c.moons[0], first + d);
            const bool full = prev < 0.5 && ph >= 0.5, fresh = ph < prev;
            prev = ph;
            const auto ecl = cal::eclipsesOn(c, first + d);
            if (!full && !fresh && ecl.empty()) continue;
            const float a = angle(d + 0.5);
            const float mx = cx + moonR * std::cos(a), my = cy + moonR * std::sin(a);
            drawMoonDisc(mx, my, 5, full ? 0.5 : 0.0);
            if (!ecl.empty()) gfx_.circle(mx, my, 8, pal_.dark ? cjui::hexColor("#F08A6C") : cjui::hexColor("#B4442C"), false, 1.8f);
            const std::string id = "calw:moon:" + std::to_string(d);
            addHit(id, {mx - 7, my - 7, 14, 14}, nullptr, false);
            std::wstring tip = w(c.moons[0].name) + L": " + (full ? L"full" : L"new") + L" on " + w(cal::formatDate(c, cal::fromDay(c, first + d), false));
            for (auto& e : ecl) tip += e.kind == cal::Eclipse::Solar ? L", a solar eclipse" : L", a lunar eclipse";
            tooltip(id, tip);
        }
    }
    // Each book's span this year, inside: an arc per book in its own colour, a ring further in each.
    std::vector<std::pair<const studio::Book*, cjui::Color>> books;
    float br = moonR - 22;
    for (const Id& b : booksInReadingOrder(*lib_, wb_.world, wb_.book)) {
        const cal::StorySpan sp = cal::storySpan(*lib_, b);
        if (!sp.on || !sp.any || sp.last < first || sp.first >= first + len) continue;
        const studio::Book* bk = lib_->book(b);
        if (!bk || br < 40) continue;
        const cjui::Color bc = povColour(b);
        const double a = (double)std::max(sp.first, first) - first, z = (double)std::min(sp.last + 1, first + len) - first;
        gfx_.arc(cx, cy, br, angle(a), angle(std::max(z, a + 0.6)), bc, 8);
        const std::string id = "calw:b:" + b;
        const float mid = angle((a + z) / 2);
        addHit(id, {cx + br * std::cos(mid) - 8, cy + br * std::sin(mid) - 8, 16, 16}, nullptr, false);
        tooltip(id, w(bk->title) + L": " + w(cal::formatDate(c, cal::fromDay(c, sp.first))) + L" to " + w(cal::formatDate(c, cal::fromDay(c, sp.last))));
        books.push_back({bk, bc});
        br -= 14;
    }
    // The story's now: a needle from the centre.
    if (now && *now >= first && *now < first + len) {
        const float a = angle((double)(*now - first) + 0.5);
        gfx_.line(cx, cy, cx + (R - 2) * std::cos(a), cy + (R - 2) * std::sin(a), pal_.ribbon, 2.5f);
        gfx_.circle(cx + (R - 2) * std::cos(a), cy + (R - 2) * std::sin(a), 5, pal_.ribbon, true);
        addHit("calw:now:mark", {cx + (R - 2) * std::cos(a) - 8, cy + (R - 2) * std::sin(a) - 8, 16, 16}, nullptr, false);
        tooltip("calw:now:mark", L"Now in the story: “" + nowWhat + L"”, " + w(cal::formatDate(c, cal::fromDay(c, *now))));
    }
    gfx_.circle(cx, cy, 5, pal_.inkMuted, true);
    gfx_.text(w(cal::formatYear(c, year)), {cx - 90, cy + 10, 180, 22}, Font::BodyStrong, pal_.ink, {Align::Center, false, false, true});
    gfx_.text(std::to_wstring(len) + L" days", {cx - 90, cy + 32, 180, 18}, Font::Small, pal_.inkMuted, {Align::Center, false, false, true});
    // The key, beside the wheel (below it when narrow).
    float kx = cx + R + 80, ky = y + 10;
    const float kw = m.right() - kx;
    if (kw < 200) { kx = m.x; ky = cy + R + 50; }
    auto keyRow = [&](std::function<void(float, float)> mark, const std::wstring& t) {
        mark(kx + 8, ky + 10);
        gfx_.text(t, {kx + 26, ky, std::max(160.0f, m.right() - kx - 26), 20}, Font::Small, pal_.ink, {Align::Left, false, true, true});
        ky += 24;
    };
    gfx_.text(L"SEASONS", {kx, ky, 200, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    ky += 22;
    for (size_t k = 0; k < c.seasons.size(); ++k)
        keyRow([&](float x, float yy) { gfx_.fillRounded({x - 7, yy - 6, 14, 12}, 3, seasonColour(c.seasons[k].name, k)); }, w(c.seasons[k].name));
    if (c.seasons.empty()) { gfx_.text(L"None yet (Make the calendar › Seasons).", {kx, ky, 300, 20}, Font::Small, pal_.inkFaint); ky += 24; }
    ky += 10;
    gfx_.text(L"MARKS", {kx, ky, 200, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    ky += 22;
    keyRow([&](float x, float yy) { cjui::drawIcon(gfx_, cjui::Icon::Star, {x - 7, yy - 7, 14, 14}, gold_, 1.3f); }, L"A holiday (point at it for its name)");
    if (!c.moons.empty()) {
        keyRow([&](float x, float yy) { drawMoonDisc(x, yy, 5, 0.5); }, w(c.moons[0].name) + L": full moon");
        keyRow([&](float x, float yy) { drawMoonDisc(x, yy, 5, 0.0); }, w(c.moons[0].name) + L": new moon");
        keyRow([&](float x, float yy) { gfx_.circle(x, yy, 7, pal_.dark ? cjui::hexColor("#F08A6C") : cjui::hexColor("#B4442C"), false, 1.8f); }, L"An eclipse");
    }
    if (now) keyRow([&](float x, float yy) { gfx_.circle(x, yy, 5, pal_.ribbon, true); }, L"Now in the story: " + nowWhat);
    if (!books.empty()) {
        ky += 10;
        gfx_.text(L"BOOKS THIS YEAR", {kx, ky, 200, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
        ky += 22;
        for (auto& [bk, bc] : books) keyRow([&](float x, float yy) { gfx_.fillRounded({x - 8, yy - 3, 16, 6}, 3, bc); }, w(bk->title));
    }
    return std::max(cy + R + 50, ky + 20);
}

}  // namespace nrs
