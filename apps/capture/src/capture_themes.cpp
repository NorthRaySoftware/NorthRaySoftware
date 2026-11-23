// NorthRay Capture: the stream's theme (0.21). One switch for every design and sky that follows it: the designs are
// made again in the new theme (keeping their words and what was added by hand), and the skies take its style and
// colours. Your own themes are copies of one, with their own colours, fonts and corners.
#include "capture_window.hpp"

#include "log.hpp"
#include "ncap/aurora_sky.hpp"
#include "ncap/design.hpp"
#include "video/design_render.hpp"

#include "cjui/menu.hpp"

#include <algorithm>

namespace ncap {

using cjui::Align;
using cjui::Color;
using cjui::Font;
using cjui::MenuItem;
using cjui::narrow;
using cjui::Rect;
using cjui::widen;
namespace dz = ncap::design;
namespace su = ncap::sourceutil;

dz::Theme CaptureWindow::currentTheme() const { return dz::themeById(ownThemes_, settings_.streamTheme); }

bool CaptureWindow::worldAccentFor(const std::string& id) const {
    auto it = settings_.themeWorldAccent.find(id);
    if (it != settings_.themeWorldAccent.end()) return it->second;
    return id == "northray" ? settings_.writing.worldColour : false;   // NorthRay: as the setting before themes
}

void CaptureWindow::loadOwnThemes() {
    ownThemes_ = settings_.ownThemesJson.empty() ? std::vector<dz::Theme>{} : dz::themesFromJson(settings_.ownThemesJson);
    if (dz::themeById(ownThemes_, settings_.streamTheme).id != settings_.streamTheme) settings_.streamTheme = "northray";
}

void CaptureWindow::pushThemes() {
    video::setDesignThemes(ownThemes_, settings_.streamTheme, worldAccentFor(settings_.streamTheme));
    designRendered_.clear();
}

// An aurora sky, pulse or wave in the stream's theme: NorthRay's are in the brand kit's colours, as they always were.
void CaptureWindow::themeSky(aurora_sky::Settings& a) const {
    const dz::Theme t = currentTheme();
    const dz::SkyLook k = dz::skyLook(t);
    a.style = k.style;
    a.colour1 = k.c1, a.colour2 = k.c2, a.colour3 = k.c3, a.night = k.night;
    if (t.id == "northray") brandSky(a);
}

// Every design and sky in this collection that follows the theme, made in `to`. `previous` is a theme's values before
// they were changed (your own theme edited), for designs in it.
bool CaptureWindow::rethemeCollection(const dz::Theme& to, const dz::Theme* previous) {
    bool changed = false;
    std::vector<std::pair<Id, std::string>> updates;
    for (auto& s : scenes_.sources()) {
        if (s.kind == SourceKind::Design) {
            const dz::Document d = dz::fromJson(s.settings);
            if (!d.followTheme) continue;
            const dz::Theme from = previous && previous->id == d.theme ? *previous : dz::themeById(ownThemes_, d.theme.empty() ? "northray" : d.theme);
            const dz::Document n = dz::retheme(d, from, to);
            const std::string j = dz::toJson(n);
            if (j != s.settings) updates.push_back({s.id, j});
        } else if (s.kind == SourceKind::AuroraSky || s.kind == SourceKind::AuroraPulse) {
            aurora_sky::Settings a = aurora_sky::fromJson(s.settings);
            if (!a.followBrand) continue;
            const aurora_sky::Settings before = a;
            themeSky(a);
            if (!(a == before)) updates.push_back({s.id, aurora_sky::toJson(a)});
        }
    }
    for (auto& [id, j] : updates) {
        scenes_.setSourceSettings(id, j);
        changed = true;
    }
    return changed;
}

void CaptureWindow::setStreamTheme(const std::string& id, const dz::Theme* previous) {
    const dz::Theme to = dz::themeById(ownThemes_, id);
    settings_.streamTheme = to.id;
    settings_.themeApplied[settings_.sceneCollection] = to.id;
    settings_.save(settingsPath_);
    pushThemes();
    if (rethemeCollection(to, previous)) {
        syncSources();
        scenesChanged();
    }
    if (!previous) {
        log::info("Theme: " + to.name);
        flash(L"Your stream is in " + widen(to.name) + L" now. Ctrl+Z puts the designs back as they were.", 6);
    }
    invalidate();
}

// A collection loaded: brought into the stream's theme if it was last in another (once; an unknown one takes the
// theme as it is).
void CaptureWindow::catchUpTheme() {
    auto it = settings_.themeApplied.find(settings_.sceneCollection);
    if (it != settings_.themeApplied.end() && it->second != settings_.streamTheme) {
        if (rethemeCollection(currentTheme(), nullptr)) scenesChanged();
        log::info("Theme: " + settings_.sceneCollection + " brought into " + settings_.streamTheme);
    }
    settings_.themeApplied[settings_.sceneCollection] = settings_.streamTheme;
}

// A new element in a design: in its theme's look, unless it's NorthRay (whose new elements follow the brand kit).
dz::Element CaptureWindow::themedElement(dz::Element e) const {
    if (designDoc_.followTheme && !designDoc_.theme.empty() && designDoc_.theme != "northray") dz::styleElement(e, dz::themeById(ownThemes_, designDoc_.theme));
    return e;
}

void CaptureWindow::saveOwnThemes() {
    settings_.ownThemesJson = dz::themesToJson(ownThemes_);
    settings_.save(settingsPath_);
}

void CaptureWindow::openThemes() {
    openSettings(true);
    themesOpen_ = true;
    invalidate();
}

// Your own theme changed: kept, and if it's the stream's, every design made again in it.
void CaptureWindow::editOwnTheme(const std::string& id, std::function<void(dz::Theme&)> change) {
    for (auto& t : ownThemes_)
        if (t.id == id) {
            const dz::Theme before = t;
            change(t);
            if (t == before) return;
            saveOwnThemes();
            if (settings_.streamTheme == id) setStreamTheme(id, &before);
            else pushThemes();
            invalidate();
            return;
        }
}

void CaptureWindow::drawThemes(const Rect& area) {
    gfx_.fill(area, pal_.paper);
    const float panelW = std::min(380.0f, area.w * 0.34f);
    const Rect left{area.x, area.y, area.w - panelW, area.h}, panel{area.right() - panelW, area.y, panelW, area.h};
    float x = left.x + 28, y = left.y + 22;
    const float w = left.w - 56;
    gfx_.text(L"Theme", {x, y, w, 34}, Font::Title, pal_.ink, {Align::Left, false, true});
    y += 40;
    y += gfx_.text(L"One switch for the whole stream. Every design made from a template (holding screens, lower third, facecam frame, "
                   L"chat, now playing, the Writing card…) and every aurora sky, pulse and wave is made again in the theme you choose: "
                   L"each theme has its own designs, not just its own colours. Words you've changed and things you've added are kept.",
                   {x, y, w, 80}, Font::Small, pal_.inkMuted) + 16;
    // The themes as cards with a picture of their Starting Soon screen.
    std::vector<dz::Theme> all(dz::builtInThemes().begin(), dz::builtInThemes().end());
    for (auto& t : ownThemes_) all.push_back(t);
    // As many columns as it takes for every card to fit the page's height (no scrolling), at least two.
    int cols = std::max(2, (int)((w + 16) / 340));
    auto cardW = [&](int c) { return (w - 16.0f * (c - 1)) / c; };
    while (cols < 8) {
        const int rows = (int)((all.size() + cols - 1) / cols);
        if (y + rows * (cardW(cols) * 9 / 16 + 64) <= left.bottom() - 10) break;
        ++cols;
    }
    const float cw = cardW(cols), ph = cw * 9 / 16;
    const dz::Theme cur = currentTheme();
    for (std::size_t i = 0; i < all.size(); ++i) {
        const dz::Theme& t = all[i];
        const float cx = x + (cw + 16) * (i % cols), cy = y + (ph + 64) * (i / cols);
        if (cy > left.bottom()) break;
        const Rect card{cx, cy, cw, ph + 52};
        const std::string hid = "th:card:" + t.id;
        addHit(hid, card, [this, id = t.id] {
            themeSel_ = id;
            if (id != settings_.streamTheme) setStreamTheme(id);
            invalidate();
        });
        const bool on = t.id == cur.id;
        gfx_.fillRounded(card, 10, pal_.ink.withAlpha(0.04f + 0.04f * hoverAmount(hid)));
        // Its picture, rendered once per theme as it is now.
        const std::string key = t.id + "|" + dz::themesToJson({[&] { dz::Theme c = t; c.builtIn = false; return c; }()});
        auto& pic = themeThumbs_[t.id];
        if (pic.first != key) {
            video::DesignPicture p;
            std::wstring err;
            const dz::Document d = dz::themedTemplate("starting-soon", t);
            if (video::renderDesign(d, 1e9f, std::max(0.05f, cw * gfx_.dpiScale() / d.width), profile_, p, err)) {
                pic.second.w = p.w, pic.second.h = p.h;
                pic.second.bgra.resize(p.bgra.size());
                for (std::size_t k = 0; k < p.bgra.size(); k += 4) {   // BGRA → RGBA
                    pic.second.bgra[k] = p.bgra[k + 2], pic.second.bgra[k + 1] = p.bgra[k + 1], pic.second.bgra[k + 2] = p.bgra[k], pic.second.bgra[k + 3] = 255;
                }
            }
            pic.first = key;
            gfx_.forgetImage(widen("theme:" + t.id));
        }
        const Rect img{cx, cy, cw, ph};
        if (!pic.second.bgra.empty()) gfx_.drawPixels(widen("theme:" + t.id), pic.second.w, pic.second.h, pic.second.bgra.data(), img, false);
        gfx_.strokeRounded(card, 10, on ? pal_.ribbon : pal_.rule, on ? 3.0f : 1.0f);
        std::wstring style;
        for (auto& st : dz::themeStyles())
            if (t.style == st.key) style = widen(st.name);
        gfx_.text(widen(t.name), {cx + 12, cy + ph + 6, cw - 90, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false, true, true});
        gfx_.text(t.builtIn ? style + L" designs" : L"Yours · " + style + L" designs", {cx + 12, cy + ph + 28, cw - 24, 18}, Font::Small, pal_.inkMuted,
                  {Align::Left, false, true, true});
        if (on) tag(cx + cw - 70, cy + ph + 8, L"In use", pal_.ribbon);
    }

    // The panel: the theme in use.
    gfx_.fill(panel, pal_.ink.withAlpha(0.03f));
    float px = panel.x + 20, py = panel.y + 22;
    const float pw = panel.w - 40;
    gfx_.text(widen(cur.name), {px, py, pw, 28}, Font::Heading, pal_.ink, {Align::Left, false, true, true});
    py += 32;
    for (auto& st : dz::themeStyles())
        if (cur.style == st.key) py += gfx_.text(widen(st.about) + L".", {px, py, pw, 60}, Font::Small, pal_.inkMuted) + 14;
    toggle("th:world", px, py + 1, worldAccentFor(cur.id), [this, id = cur.id] {
        settings_.themeWorldAccent[id] = !worldAccentFor(id);
        settings_.save(settingsPath_);
        pushThemes();
        invalidate();
    });
    gfx_.text(L"The book's world colour", {px + 60, py, pw - 60, 22}, Font::Body, pal_.ink, {Align::Left, false, true});
    py += 24;
    py += gfx_.text(L"While you write a book in Writing Studio, its world's colour is this theme's main colour.", {px + 60, py, pw - 60, 60}, Font::Small,
                    pal_.inkMuted) + 16;
    button("th:copy", {px, py, pw, 34}, L"Make my own from this theme…", [this, cur] {
        if (ownThemes_.size() >= 40) {
            flash(L"You have 40 themes of your own; remove one first.", 5);
            return;
        }
        dz::Theme t = dz::copyTheme(cur, ownThemes_);
        ownThemes_.push_back(t);
        saveOwnThemes();
        setStreamTheme(t.id);
    });
    py += 46;
    if (cur.builtIn) {
        py += gfx_.text(L"Your own theme starts as a copy of this one, with its designs. Change its colours, fonts and corners, and everything "
                        L"in it follows.",
                        {px, py, pw, 80}, Font::Small, pal_.inkFaint) + 10;
        return;
    }
    // Your own theme: its name, colours, fonts and corners.
    py += 4;
    py += sectionLabel(px, py, pw, L"YOUR THEME");
    button("th:rename", {px, py, pw, 30}, L"Rename…", [this, cur] {
        askText(L"The theme's name", widen(cur.name), [this, id = cur.id](const std::wstring& v) {
            if (v.empty()) return;
            editOwnTheme(id, [&](dz::Theme& t) { t.name = narrow(v).substr(0, 60); });
        });
    });
    py += 38;
    struct Slot {
        const char* key;
        const wchar_t* name;
        su::Rgba dz::Theme::*m;
    };
    static const Slot slots[] = {{"page", L"Page, top", &dz::Theme::page},        {"page2", L"Page, bottom", &dz::Theme::page2},
                                 {"panel", L"Cards, top", &dz::Theme::panel},     {"panel2", L"Cards, bottom", &dz::Theme::panel2},
                                 {"edge", L"Edges and frames", &dz::Theme::edge}, {"accent", L"Main colour", &dz::Theme::accent},
                                 {"accent2", L"Second colour", &dz::Theme::accent2}, {"accent3", L"Third colour", &dz::Theme::accent3},
                                 {"title", L"Titles", &dz::Theme::title},         {"body", L"Words", &dz::Theme::body},
                                 {"muted", L"Quieter words", &dz::Theme::muted},  {"onAccent", L"Words on the main colour", &dz::Theme::onAccent},
                                 {"sky", L"Skies' background", &dz::Theme::sky}};
    const float rowH = std::clamp((panel.bottom() - py - 96) / 13.0f, 18.0f, 30.0f);
    for (auto& sl : slots) {
        const su::Rgba c = cur.*(sl.m);
        const Rect r{px, py, pw, rowH - 2};
        const std::string hid = std::string("th:c:") + sl.key;
        addHit(hid, r, [this, cur, sl] {
            askText(std::wstring(sl.name) + L": a colour as #RRGGBB", widen(su::colourText(cur.*(sl.m))), [this, id = cur.id, m = sl.m](const std::wstring& v) {
                su::Rgba nc;
                if (!su::parseColour(narrow(v), nc)) {
                    flash(L"That isn't a colour: write it as #RRGGBB.", 5);
                    return;
                }
                editOwnTheme(id, [&](dz::Theme& t) { t.*m = {nc.r, nc.g, nc.b, 255}; });
            });
        });
        gfx_.fillRounded(r, 6, pal_.ink.withAlpha(0.05f * hoverAmount(hid)));
        gfx_.fillRounded({r.x + 4, r.y + 3, rowH - 8, rowH - 8}, 4, Color{c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1});
        gfx_.strokeRounded({r.x + 4, r.y + 3, rowH - 8, rowH - 8}, 4, pal_.rule, 1);
        gfx_.text(sl.name, {r.x + rowH + 6, r.y, pw - rowH - 90, r.h}, Font::Small, pal_.ink, {Align::Left, false, true, true});
        gfx_.text(widen(su::colourText(c)), {r.right() - 84, r.y, 80, r.h}, Font::Small, pal_.inkMuted, {Align::Right, false, true, true});
        py += rowH;
    }
    py += 8;
    // The fonts, in one row: headings, words, labels.
    const float fw = (pw - 12) / 3;
    auto fontButton = [&](int i, const char* hid, const wchar_t* label, const std::string& now, std::string dz::Theme::*m) {
        const Rect b{px + i * (fw + 6), py, fw, 30};
        button(hid, b, label, [this, id = cur.id, now, m] {
            pickFont(now, false, [this, id, m](const std::string& f) { editOwnTheme(id, [&](dz::Theme& t) { t.*m = f; }); });
        });
        tooltip(hid, std::wstring(label) + L": " + widen(now));
    };
    fontButton(0, "th:f:h", L"Headings…", cur.headingFont, &dz::Theme::headingFont);
    fontButton(1, "th:f:b", L"Words…", cur.bodyFont, &dz::Theme::bodyFont);
    fontButton(2, "th:f:l", L"Labels…", cur.labelFont, &dz::Theme::labelFont);
    py += 36;
    const int corner = cur.corners < 0.5f ? 0 : cur.corners < 1.3f ? 1 : 2;
    segmented("th:corners", {px, py, pw - 96, 30}, {L"Square", L"As designed", L"Rounder"}, corner, [this, id = cur.id](int i) {
        editOwnTheme(id, [&](dz::Theme& t) { t.corners = i == 0 ? 0.0f : i == 1 ? 1.0f : 1.7f; });
    });
    tooltip("th:remove", L"Remove this theme (its designs go back to the built-in theme of the same style)");
    button("th:remove", {px + pw - 88, py, 88, 30}, L"Remove", [this, cur] {
        // Its designs go to the built-in theme of the same style.
        std::string to = "northray";
        for (auto& b : dz::builtInThemes())
            if (b.style == cur.style) {
                to = b.id;
                break;
            }
        setStreamTheme(to);
        ownThemes_.erase(std::remove_if(ownThemes_.begin(), ownThemes_.end(), [&](const dz::Theme& t) { return t.id == cur.id; }), ownThemes_.end());
        settings_.themeWorldAccent.erase(cur.id);
        saveOwnThemes();
        pushThemes();
        themeThumbs_.erase(cur.id);
        flash(L"Removed " + widen(cur.name) + L"; the stream is in " + widen(dz::themeById(ownThemes_, to).name) + L".", 6);
    });
}

}  // namespace ncap
