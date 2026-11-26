// NorthRay Writing Studio: the bible at a glance (0.87). Each kind of page has a colour and a symbol; a page shows its
// picture as a portrait, or the symbol the writer chose for it (an emoji or one of the app's drawn symbols), or its
// initials, so lists, links, search results, Who's there and the timeline can be read without reading.

#include "studio_app.hpp"
#include "cjui/menu.hpp"

#include <cmath>
#include <cwctype>

namespace nrs {

using namespace studio;
using cjui::Align;
using cjui::Font;
using cjui::Rect;

static std::wstring w(const std::string& s) { return widen(s); }

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

void StudioApp::drawPortrait(const Entry& e, const Rect& r, float radius) {
    if (!e.imageBlob.empty() && gfx_.drawImageRounded(w(lib_->blobPath(e.imageBlob)), r, radius)) {
        gfx_.strokeRounded(r, radius, pal_.rule.withAlpha(0.8f), 1);
        return;
    }
    const cjui::Color kc = entryKindColor(e.kind, pal_.dark);
    gfx_.fillRounded(r, radius, pal_.surface);   // opaque, so lines under it (the web, the tree) don't show through
    gfx_.fillRounded(r, radius, kc.withAlpha(pal_.dark ? 0.22f : 0.15f));
    gfx_.strokeRounded(r.inset(0.75f), std::max(0.0f, radius - 0.75f), kc.withAlpha(0.45f), 1.2f);
    const float side = std::min(r.w, r.h);
    if (e.symbol.rfind("icon:", 0) == 0) {
        const cjui::Icon ic = cjui::iconNamed(e.symbol.substr(5));
        if (ic != cjui::Icon::None) {
            const float isz = side * 0.56f;
            cjui::drawIcon(gfx_, ic, {r.x + (r.w - isz) / 2, r.y + (r.h - isz) / 2, isz, isz}, kc.mix(pal_.ink, 0.25f), std::max(1.2f, side / 28));
            return;
        }
    } else if (!e.symbol.empty()) {
        gfx_.textSized(w(e.symbol), r, std::clamp(side * 0.52f, 8.0f, 200.0f), false, pal_.ink, {Align::Center, false, false, true});
        return;
    }
    const Font f = side >= 60 ? Font::Heading : side >= 30 ? Font::SmallStrong : Font::Label;
    gfx_.text(initialsOf(e.name), r, f, kc.mix(pal_.ink, 0.35f), {Align::Center, false, false, true});
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

void StudioApp::chooseEntrySymbol(const Id& entryId) {
    const Entry* e = lib_->entry(entryId);
    if (!e) return;
    // A handful of emoji worlds tend to need, and the app's own drawn symbols. The emoji are drawn by Windows' emoji font.
    static const wchar_t* emoji[] = {L"\U0001F409", L"\U0001F43A", L"\U0001F985", L"\U0001F40D", L"\U0001F984", L"\U0001F9D9", L"\U0001F451",
                                     L"⚔️", L"\U0001F6E1️", L"\U0001F3F0", L"\U0001F30B", L"\U0001F30A", L"\U0001F332",
                                     L"\U0001F3D4️", L"\U0001F319", L"⭐", L"\U0001F525", L"\U0001F48E", L"\U0001F4DC",
                                     L"\U0001F5DD️", L"\U0001F56F️", L"\U0001F9EA", L"⚓", L"\U0001F43E", L"\U0001F480",
                                     L"\U0001F47B", L"\U0001F33F", L"\U0001F33A", L"✨", L"☠️"};
    std::vector<cjui::MenuItem> em, drawn;
    for (size_t i = 0; i < sizeof emoji / sizeof *emoji; ++i) em.push_back(cjui::MenuItem::item(emoji[i], 100 + (UINT)i, {}, true, w(e->symbol) == emoji[i]));
    const auto& icons = cjui::symbolIcons();
    for (size_t i = 0; i < icons.size(); ++i) {
        std::wstring nm = w(cjui::iconName(icons[i]));
        if (!nm.empty()) nm[0] = (wchar_t)towupper(nm[0]);
        drawn.push_back(cjui::MenuItem::item(nm, 300 + (UINT)i, {}, true, e->symbol == std::string("icon:") + cjui::iconName(icons[i])));
    }
    std::vector<cjui::MenuItem> items{cjui::MenuItem::heading(L"Shown where the page has no picture"),
                                      cjui::MenuItem::sub(L"An emoji", em), cjui::MenuItem::sub(L"A drawn symbol", drawn),
                                      cjui::MenuItem::item(L"Its initials", 1, {}, true, e->symbol.empty())};
    POINT pt;
    GetCursorPos(&pt);
    const UINT c = cjui::showMenu(hwnd(), items, pt, pal_);
    if (!c) return;
    Entry x = *e;
    const Entry before = *e;
    if (c == 1) x.symbol.clear();
    else if (c >= 300) x.symbol = std::string("icon:") + cjui::iconName(icons[c - 300]);
    else if (c >= 100) x.symbol = narrow(emoji[c - 100]);
    if (x.symbol == before.symbol) return;
    lib_->updateEntry(x);
    flashUndo(L"Symbol changed.", [this, before] { lib_->updateEntry(before); invalidate(); });
    invalidate();
}

void StudioApp::drawMoonDisc(float mx, float my, float rad, double ph) {
    gfx_.circle(mx, my, rad, pal_.ink.withAlpha(0.55f), true);
    const float k = (float)std::cos(ph * 2 * 3.14159265358979);
    for (float dy = -rad + 0.5f; dy < rad; dy += 0.5f) {
        const float hw = std::sqrt(std::max(0.0f, rad * rad - dy * dy));
        const float a = ph <= 0.5 ? mx + hw * k : mx - hw, b = ph <= 0.5 ? mx + hw : mx - hw * k;
        if (b - a > 0.2f) gfx_.line(a, my + dy, b, my + dy, gold_.mix(pal_.surface, 0.35f), 0.6f);
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

}  // namespace nrs
