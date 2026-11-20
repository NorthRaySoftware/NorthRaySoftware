// Worldbuilding › Share the bible (0.38): a page for choosing how the bible is shared before it's made.
//
// What to make (a website, or a printable PDF), who it's for (everything, or a reader's guide up to a book), what goes
// in (pictures, family and relationships, languages, the glossary), and for a website how it looks (theme, light or
// dark, colour, typefaces, cards or lists, search, kind colours), with a sketch of the look beside the choices; for a
// PDF, the design and typeface. The site's title, tagline and an own colour are written in the side panel. The
// choices are kept with the world (setting "bible-share:world:<id>" or "bible-share:book:<id>").
#include "studio_app.hpp"
#include "cjui/dialog.hpp"
#include "cjui/menu.hpp"

#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <nlohmann/json.hpp>

namespace nrs {

using namespace studio;
using nlohmann::json;
using cjui::Align;
using cjui::Color;
using cjui::Font;
using cjui::Rect;

static std::wstring w(const std::string& s) { return widen(s); }

static int popupAt(HWND owner, const std::vector<std::wstring>& items, int checked = -1) {
    HMENU m = CreatePopupMenu();
    for (size_t i = 0; i < items.size(); ++i)
        AppendMenuW(m, MF_STRING | ((int)i == checked ? MF_CHECKED : 0), 1 + (UINT)i, items[i].c_str());
    POINT pt;
    GetCursorPos(&pt);
    const int cmd = (int)cjui::trackMenu(m, pt, owner);
    DestroyMenu(m);
    return cmd - 1;
}

static std::string shareKey(const Id& world, const Id& book) { return world.empty() ? "bible-share:book:" + book : "bible-share:world:" + world; }

void StudioApp::loadShare() {
    const std::string key = shareKey(wb_.world, wb_.book);
    if (wb_.shareKey == key) return;
    wb_.shareKey = key;
    ShareState s;
    const json j = json::parse(lib_->setting(key), nullptr, false);
    if (j.is_object()) {
        s.pdf = j.value("pdf", false);
        s.upTo = j.value("upTo", std::string());
        s.guide = j.value("guide", false);
        s.pictures = j.value("pictures", true);
        s.family = j.value("family", true);
        s.languages = j.value("languages", true);
        s.glossary = j.value("glossary", true);
        s.preset = j.value("preset", 0);
        s.font = j.value("font", std::string());
        if (j.contains("look") && j["look"].is_object()) s.look = bible::lookFromJson(j["look"].dump());
    }
    wb_.shareState = s;
}

void StudioApp::saveShare() {
    const ShareState& s = wb_.shareState;
    json j{{"pdf", s.pdf}, {"upTo", s.upTo}, {"guide", s.guide}, {"pictures", s.pictures}, {"family", s.family}, {"languages", s.languages},
           {"glossary", s.glossary}, {"preset", s.preset}, {"font", s.font}, {"look", json::parse(bible::toJson(s.look))}};
    lib_->setSetting(wb_.shareKey, j.dump());
    invalidate();
}

void StudioApp::shareBible() {
    commitFields();
    openEntry({});
    wb_.share = true;
    wb_.scroll = 0;
    loadShare();
    setFocus({Focus::Share, wb_.shareKey, {}});
    invalidate();
}

// A sketch of the website with the chosen look: a header, the banner, chips and a few cards.
void StudioApp::drawSharePreview(const Rect& r) {
    const ShareState& s = wb_.shareState;
    const bool asked = s.look.mode == "dark" || (s.look.mode == "auto" && pal_.dark);
    const auto tc = bible::themeColours(s.look.theme, asked);
    const bool dark = asked || tc.alwaysDark;
    auto hex = [](const std::string& h) { return cjui::hexColor(h); };
    const Color paper = hex(tc.paper), card = hex(tc.card), ink = hex(tc.ink), muted = hex(tc.muted), rule = hex(tc.rule), head = hex(tc.head);
    Color accent = s.look.accent.empty() ? hex(tc.accent) : cjui::hexColor(s.look.accent, hex(tc.accent));
    if (dark && !s.look.accent.empty()) accent = accent.mix(cjui::hexColor("#FFFFFF"), 0.38f);
    const World* wo = lib_->world(wb_.world);
    std::string title = s.look.title.empty() ? (wo ? wo->name : std::string("World bible")) : s.look.title;
    if (tc.capitals) for (auto& c : title) c = (char)std::toupper((unsigned char)c);
    gfx_.fillRounded({r.x, r.y + 4, r.w, r.h}, 12, cjui::hexColor("#000000").withAlpha(0.10f));
    gfx_.fillRounded(r, 12, paper);
    gfx_.pushClip(r);
    // Header.
    gfx_.fill({r.x, r.y, r.w, 30}, head);
    gfx_.line(r.x, r.y + 30, r.right(), r.y + 30, rule);
    gfx_.text(w(title), {r.x + 12, r.y, r.w * 0.45f, 30}, Font::SmallStrong, ink, {Align::Left, false, true, true});
    gfx_.text(L"Characters   Places   Glossary", {r.x + r.w * 0.45f, r.y, r.w * 0.5f, 30}, Font::Small, muted, {Align::Left, false, true, true});
    if (s.look.search) gfx_.fillRounded({r.right() - 70, r.y + 8, 58, 15}, 7, card);
    // Banner.
    const Rect ban{r.x + 12, r.y + 42, r.w - 24, r.h * 0.34f};
    if (!wo || wo->bannerBlob.empty() || !gfx_.drawImage(w(lib_->blobPath(wo->bannerBlob)), ban, true)) {
        gfx_.fillRounded(ban, 8, accent.mix(cjui::hexColor("#000000"), 0.45f));
        gfx_.fill({ban.x, ban.y + ban.h * 0.5f, ban.w, ban.h * 0.5f}, cjui::hexColor("#000000").withAlpha(0.25f));
    }
    gfx_.text(L"WORLD BIBLE", {ban.x + 14, ban.bottom() - 62, ban.w - 28, 14}, Font::Label, cjui::hexColor("#FFFFFF").withAlpha(0.8f), {Align::Left, false});
    gfx_.text(w(title), {ban.x + 14, ban.bottom() - 48, ban.w - 28, 30}, Font::SerifTitle, cjui::hexColor("#FFFFFF"), {Align::Left, false, true, true});
    if (!s.look.tagline.empty())
        gfx_.text(w(s.look.tagline), {ban.x + 14, ban.bottom() - 20, ban.w - 28, 16}, Font::Small, cjui::hexColor("#FFFFFF").withAlpha(0.9f), {Align::Left, false, true, true});
    // Chips and cards.
    float y = ban.bottom() + 12;
    static const char* kinds[] = {"character", "place", "group"};
    static const char* kindHexL[] = {"#3E7BD6", "#2E9A6C", "#7D5BD0"};
    static const char* kindHexD[] = {"#7AA9F0", "#5BC99A", "#AE93F2"};
    float cx = r.x + 12;
    for (int k = 0; k < 3; ++k) {
        const Color kc = s.look.kindColours ? cjui::hexColor(dark ? kindHexD[k] : kindHexL[k]) : accent;
        const std::wstring lbl = w(entryKindPlural(kinds[k]));
        const float cw = gfx_.measureWidth(lbl, Font::Small) + 28;
        gfx_.fillRounded({cx, y, cw, 20}, 10, card);
        gfx_.strokeRounded({cx, y, cw, 20}, 10, rule);
        gfx_.circle(cx + 10, y + 10, 3, kc, true);
        gfx_.text(lbl, {cx + 17, y, cw - 17, 20}, Font::Small, ink, {Align::Left, false, true, true});
        cx += cw + 6;
    }
    y += 32;
    const Color kc = s.look.kindColours ? cjui::hexColor(dark ? kindHexD[0] : kindHexL[0]) : accent;
    std::wstring head1 = L"Characters";
    gfx_.text(tc.capitals ? L"CHARACTERS" : head1, {r.x + 12, y, r.w - 24, 22}, Font::Heading, ink, {Align::Left, false, true, true});
    y += 26;
    gfx_.fill({r.x + 12, y, r.w - 24, 2}, kc);
    y += 10;
    // Entries of the bible as the first cards (their pictures when they have them).
    std::vector<const Entry*> people;
    for (const Entry* e : lib_->entriesOf(wb_.world, wb_.book)) if (e->kind == "character" && people.size() < 3) people.push_back(e);
    const float cardW = (r.w - 24 - 16) / 3;
    for (int i = 0; i < 3; ++i) {
        const Rect c{r.x + 12 + i * (cardW + 8), y, cardW, s.look.cards ? 70.0f : 18.0f};
        const std::wstring name = i < (int)people.size() ? w(people[(size_t)i]->name) : (i == 0 ? L"A character" : i == 1 ? L"Another" : L"A third");
        if (!s.look.cards) {
            gfx_.text(name, c, Font::Small, accent, {Align::Left, false, true, true});
            continue;
        }
        gfx_.fillRounded(c, 6, card);
        gfx_.strokeRounded(c, 6, rule);
        gfx_.fill({c.x + 4, c.y, c.w - 8, 2.5f}, kc);
        const Rect pic{c.x + 6, c.y + 8, 34, 52};
        if (i >= (int)people.size() || !s.pictures || people[(size_t)i]->imageBlob.empty() ||
            !gfx_.drawImage(w(lib_->blobPath(people[(size_t)i]->imageBlob)), pic, true))
            gfx_.fillRounded(pic, 4, kc.withAlpha(0.18f));
        gfx_.text(name, {c.x + 46, c.y + 18, c.w - 50, 18}, Font::SmallStrong, ink, {Align::Left, false, true, true});
        gfx_.fillRounded({c.x + 46, c.y + 42, c.w - 58, 5}, 2, muted.withAlpha(0.35f));
        gfx_.fillRounded({c.x + 46, c.y + 51, (c.w - 58) * 0.7f, 5}, 2, muted.withAlpha(0.35f));
    }
    gfx_.popClip();
    gfx_.strokeRounded(r, 12, pal_.rule);
}

void StudioApp::drawShare(const Rect& r) {
    loadShare();
    ShareState& s = wb_.shareState;
    gfx_.pushClip(r);
    setHitClip(r);
    const float pad = r.w > 900 ? 40 : 28;
    const float cw = std::min(r.w - pad * 2, 1100.0f);
    const Rect m{r.x + pad, r.y + 26 - wb_.scroll, cw, 0};
    float y = m.y;
    std::wstring name;
    if (const World* wo = lib_->world(wb_.world)) name = w(wo->name);
    else if (const studio::Book* bk = lib_->book(wb_.book)) name = w(bk->title);
    y += pageHeader(m, L"WORLDBUILDING", L"Share the bible",
                    L"Make " + name + L"'s bible into a website to put online or a PDF to print, for yourself or for readers.");
    // Two columns when there's room: the choices, and a sketch of the look.
    const bool wide = cw > 860;
    const float colW = wide ? cw - 400 : cw;
    const Rect col{m.x, y, colW, 0};
    if (wide && !s.pdf) drawSharePreview({m.x + colW + 40, y + 6, 360, 330});
    auto label = [&](const std::wstring& t) {
        gfx_.text(t, {col.x, y, col.w, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
        y += 24;
    };
    auto save = [this] { saveShare(); };
    // Chips that switch on and off.
    auto toggles = [&](const std::vector<std::pair<std::wstring, bool*>>& items, const std::string& idBase) {
        float x = col.x;
        for (size_t i = 0; i < items.size(); ++i) {
            const std::wstring& t = items[i].first;
            bool* v = items[i].second;
            const float bw = gfx_.measureWidth(t, Font::Small) + 44;
            if (x + bw > col.right() && x > col.x) { x = col.x; y += 38; }
            const Rect chip{x, y, bw, 30};
            const std::string id = idBase + std::to_string(i);
            addHit(id, chip, [v, save] { *v = !*v; save(); });
            const float hv = hoverAmount(id);
            gfx_.fillRounded(chip, 15, *v ? pal_.ribbon.withAlpha(0.14f) : pal_.surface.mix(pal_.sunken, 0.4f * hv));
            gfx_.strokeRounded(chip, 15, *v ? pal_.ribbon : pal_.rule.mix(pal_.ribbon, hv));
            // A tick box.
            const Rect box{chip.x + 10, chip.y + 8, 14, 14};
            gfx_.strokeRounded(box, 3, *v ? pal_.ribbon : pal_.inkFaint, 1.4f);
            if (*v) {
                gfx_.fillRounded(box, 3, pal_.ribbon);
                gfx_.line(box.x + 3, box.y + 7, box.x + 6, box.y + 10, pal_.paper, 1.8f);
                gfx_.line(box.x + 6, box.y + 10, box.x + 11, box.y + 4, pal_.paper, 1.8f);
            }
            gfx_.text(t, {chip.x + 30, chip.y, chip.w - 34, chip.h}, Font::Small, pal_.ink, {Align::Left, false, true, true});
            if (focused(id)) focusRing(chip);
            x += bw + 8;
        }
        y += 46;
    };
    // A labelled menu button.
    auto chooser = [&](const std::string& id, const std::wstring& lbl, const std::wstring& value, std::function<void()> act) {
        gfx_.text(lbl, {col.x, y, 150, 32}, Font::Body, pal_.inkMuted, {Align::Left, false, true, true});
        const Rect b{col.x + 150, y, std::min(360.0f, col.w - 150), 32};
        addHit(id, b, std::move(act));
        const float hv = hoverAmount(id);
        gfx_.fillRounded(b, 4, pal_.surface.mix(pal_.sunken, 0.4f * hv));
        gfx_.strokeRounded(b, 4, pal_.rule.mix(pal_.ribbon, hv));
        gfx_.text(value, {b.x + 12, b.y, b.w - 36, b.h}, Font::Body, pal_.ink, {Align::Left, false, true, true});
        gfx_.fillTriangle(b.right() - 20, b.y + 13, b.right() - 10, b.y + 13, b.right() - 15, b.y + 19, pal_.inkFaint);
        if (focused(id)) focusRing(b);
        y += 42;
    };

    label(L"MAKE");
    segmented("share:what", {col.x, y, std::min(420.0f, col.w), 32}, {L"A website", L"A printable PDF"}, s.pdf ? 1 : 0, [this](int i) {
        wb_.shareState.pdf = i == 1;
        saveShare();
    });
    y += 44;
    y += gfx_.text(s.pdf ? L"A book of the bible, typeset like your manuscripts: a part for each kind, an entry after entry, languages and the glossary at the back."
                         : L"A folder of linked pages that opens in any browser from your computer, and can go online as it is (any web host that takes files).",
                   {col.x, y, col.w, 60}, Font::Small, pal_.inkMuted) + 16;

    label(L"WHO IT'S FOR");
    {
        const auto books = bible::booksInOrder(*lib_, wb_.world, wb_.book);
        std::wstring now = L"You: everything, with your notes";
        if (s.guide)
            if (const studio::Book* b = lib_->book(s.upTo)) now = L"Readers of " + w(b->title) + L" (no later spoilers)";
        chooser("share:who", L"For", now, [this, books] {
            std::vector<std::wstring> items{L"You: everything, with your notes"};
            int cur = wb_.shareState.guide ? -1 : 0;
            for (size_t i = 0; i < books.size(); ++i)
                if (const studio::Book* b = lib_->book(books[i])) {
                    items.push_back(L"A reader's guide up to the end of " + w(b->title));
                    if (wb_.shareState.guide && books[i] == wb_.shareState.upTo) cur = (int)i + 1;
                }
            const int i = popupAt(hwnd(), items, cur);
            if (i < 0) return;
            wb_.shareState.guide = i > 0;
            wb_.shareState.upTo = i > 0 ? books[(size_t)i - 1] : Id{};
            saveShare();
        });
        if (s.guide) y += gfx_.text(L"A reader's guide leaves out your notes and notes per book, and anything marked as a spoiler from a later book.",
                                   {col.x, y - 4, col.w, 40}, Font::Small, pal_.inkFaint) + 12;
    }

    label(L"WHAT GOES IN");
    {
        const std::size_t nl = conlangs(wb_.world, wb_.world.empty() ? wb_.book : Id{}).size();
        std::vector<std::pair<std::wstring, bool*>> items{{L"Pictures and maps", &s.pictures}, {L"Family and relationships", &s.family},
                                                          {L"Glossary", &s.glossary}};
        if (nl) items.push_back({L"Languages (" + std::to_wstring(nl) + L")", &s.languages});
        toggles(items, "share:inc:");
    }

    if (!s.pdf) {
        label(L"THEME");
        {
            const auto& ts = bible::themes();
            const float tw = std::min(190.0f, (col.w - 8 * 2) / 3);
            float x = col.x;
            for (size_t i = 0; i < ts.size(); ++i) {
                if (x + tw > col.right() + 1 && x > col.x) { x = col.x; y += 92; }
                const Rect t{x, y, tw, 84};
                const std::string id = std::string("share:theme:") + ts[i].id;
                const bool on = s.look.theme == ts[i].id;
                addHit(id, t, [this, tid = std::string(ts[i].id)] {
                    wb_.shareState.look.theme = tid;
                    wb_.shareState.look.accent.clear();   // the theme's own colour
                    saveShare();
                });
                tooltip(id, w(ts[i].description));
                const auto tc = bible::themeColours(ts[i].id, pal_.dark && s.look.mode != "light");
                const float hv = hoverAmount(id);
                gfx_.fillRounded(t, 8, cjui::hexColor(tc.paper));
                gfx_.fill({t.x + 1, t.y + 1, t.w - 2, 16}, cjui::hexColor(tc.head));
                gfx_.fillRounded({t.x + 8, t.y + 24, t.w - 16, 22}, 4, cjui::hexColor(tc.accent).mix(cjui::hexColor("#000000"), 0.4f));
                gfx_.fillRounded({t.x + 8, t.y + 52, (t.w - 22) / 2, 10}, 3, cjui::hexColor(tc.card));
                gfx_.fillRounded({t.x + 14 + (t.w - 22) / 2, t.y + 52, (t.w - 22) / 2, 10}, 3, cjui::hexColor(tc.card));
                gfx_.text(w(ts[i].label), {t.x + 8, t.y + 64, t.w - 16, 18}, Font::SmallStrong, cjui::hexColor(tc.ink), {Align::Left, false, true, true});
                gfx_.strokeRounded(t, 8, on ? pal_.ribbon : pal_.rule.mix(pal_.ribbon, hv), on ? 2.5f : 1);
                if (focused(id)) focusRing(t);
                x += tw + 8;
            }
            y += 100;
            for (auto& t : ts) if (s.look.theme == t.id) y += gfx_.text(w(t.description), {col.x, y - 6, col.w, 40}, Font::Small, pal_.inkMuted) + 12;
        }
        label(L"LIGHT OR DARK");
        segmented("share:mode", {col.x, y, std::min(460.0f, col.w), 32}, {L"Follow the reader's device", L"Light", L"Dark"},
                  s.look.mode == "light" ? 1 : s.look.mode == "dark" ? 2 : 0, [this](int i) {
                      wb_.shareState.look.mode = i == 1 ? "light" : i == 2 ? "dark" : "auto";
                      saveShare();
                  });
        y += 40;
        if (s.look.theme == "night") y += gfx_.text(L"Night sky is dark either way.", {col.x, y, col.w, 20}, Font::Small, pal_.inkFaint) + 6;
        y += 10;
        label(L"COLOUR");
        {
            // The theme's own, the world's, a few more; your own is written in the side panel.
            std::vector<std::pair<std::wstring, std::string>> sw{{L"The theme's own", ""}};
            if (const World* wo = lib_->world(wb_.world); wo && !wo->accent.empty()) sw.push_back({L"The world's colour", wo->accent});
            for (auto& [l, h] : std::vector<std::pair<std::wstring, std::string>>{{L"Crimson", "#9C2F3A"}, {L"Gold", "#A87B1E"}, {L"Forest", "#2E7D4F"},
                                                                                  {L"Sea", "#1F7A8C"}, {L"Royal blue", "#2F55B5"}, {L"Violet", "#6D4AC2"},
                                                                                  {L"Rose", "#C2477A"}, {L"Ember", "#C8642A"}})
                sw.push_back({l, h});
            float x = col.x;
            const auto tc = bible::themeColours(s.look.theme, pal_.dark);
            for (size_t i = 0; i < sw.size(); ++i) {
                const Rect c{x, y, 32, 32};
                const std::string id = "share:col:" + std::to_string(i);
                const bool on = s.look.accent == sw[i].second;
                addHit(id, c, [this, h = sw[i].second] { wb_.shareState.look.accent = h; saveShare(); loadedFocus_ = {Focus::None, "-", ""}; });
                tooltip(id, sw[i].first);
                const Color col2 = cjui::hexColor(sw[i].second.empty() ? tc.accent : sw[i].second);
                gfx_.circle(c.x + 16, c.y + 16, 13, col2, true);
                if (sw[i].second.empty()) gfx_.text(L"T", c, Font::SmallStrong, cjui::hexColor("#FFFFFF"), {Align::Center, false, false, true});
                if (i == 1 && sw.size() > 9) gfx_.text(L"W", c, Font::SmallStrong, cjui::hexColor("#FFFFFF"), {Align::Center, false, false, true});
                if (on) gfx_.circle(c.x + 16, c.y + 16, 16, pal_.ink, false, 2);
                else if (hoverAmount(id) > 0.01f) gfx_.circle(c.x + 16, c.y + 16, 16, pal_.rule, false, 1.5f);
                if (focused(id)) focusRing(c);
                x += 38;
                if (x + 32 > col.right() && i + 1 < sw.size()) { x = col.x; y += 40; }
            }
            y += 44;
            const bool custom = !s.look.accent.empty() && std::none_of(sw.begin(), sw.end(), [&](auto& p) { return p.second == s.look.accent; });
            y += gfx_.text(custom ? L"Your own colour, " + w(s.look.accent) + L", from the side panel." : L"Or write your own colour (#2D4A6E) in the side panel.",
                           {col.x, y - 4, col.w, 20}, Font::Small, pal_.inkFaint) + 14;
        }
        label(L"TYPE");
        {
            const auto& fs = bible::fonts();
            // The world's own typefaces (0.40) come after the built-in choices, as "family:<id>".
            const auto own = studio::fonts::familiesIn(ownFontsFolder(wb_.world, wb_.book));
            auto fontName = [&](const std::string& id) -> std::wstring {
                if (id.empty() || id == "theme") return L"The theme's own";
                for (auto& f : fs) if (id == f.id) return w(f.label);
                for (auto& f : own) if (id == "family:" + f.id) return w(f.name) + L" (this world's, included)";
                return L"The theme's own";
            };
            auto pick = [this, &fs, own](bool headings) {
                std::vector<std::wstring> items{L"The theme's own"};
                for (auto& f : fs) items.push_back(w(f.label));
                for (auto& f : own) items.push_back(w(f.name) + L" (this world's, included)");
                const std::string now = headings ? wb_.shareState.look.headings : wb_.shareState.look.body;
                int cur = 0;
                for (size_t i = 0; i < fs.size(); ++i) if (now == fs[i].id) cur = (int)i + 1;
                for (size_t i = 0; i < own.size(); ++i) if (now == "family:" + own[i].id) cur = (int)(fs.size() + i) + 1;
                const int i = popupAt(hwnd(), items, cur);
                if (i < 0) return;
                std::string id = "theme";
                if (i > 0 && (size_t)i <= fs.size()) id = fs[(size_t)i - 1].id;
                else if ((size_t)i > fs.size()) id = "family:" + own[(size_t)i - 1 - fs.size()].id;
                (headings ? wb_.shareState.look.headings : wb_.shareState.look.body) = id;
                saveShare();
            };
            chooser("share:hf", L"Headings", fontName(s.look.headings), [pick] { pick(true); });
            chooser("share:bf", L"Text", fontName(s.look.body), [pick] { pick(false); });
            y += gfx_.text(L"Typefaces marked “included” are copied into the site, so every reader sees them; the others use what the reader's computer has.",
                           {col.x, y - 6, col.w, 40}, Font::Small, pal_.inkFaint) + 14;
        }
        label(L"PAGES");
        segmented("share:cards", {col.x, y, std::min(460.0f, col.w), 32}, {L"Entries as picture cards", L"Entries as name lists"}, s.look.cards ? 0 : 1,
                  [this](int i) { wb_.shareState.look.cards = i == 0; saveShare(); });
        y += 44;
        toggles({{L"A search box", &s.look.search}, {L"Each kind in its own colour", &s.look.kindColours}}, "share:opt:");
        if (!wide) {
            label(L"HOW IT LOOKS");
            drawSharePreview({col.x, y, std::min(col.w, 420.0f), 330});
            y += 350;
        }
    } else {
        label(L"DESIGN");
        const auto& ps = typeset::presets();
        const int pi = std::clamp(s.preset, 0, (int)ps.size() - 1);
        chooser("share:preset", L"Design", w(ps[(size_t)pi].label), [this] {
            std::vector<std::wstring> items;
            for (auto& p : typeset::presets()) items.push_back(w(p.label));
            const int i = popupAt(hwnd(), items, wb_.shareState.preset);
            if (i < 0) return;
            wb_.shareState.preset = i;
            saveShare();
        });
        const auto fams = typeset::fontFamilies(worldFontFolders(wb_.world, wb_.book));
        std::wstring fname = L"The design's own";
        for (auto& f : fams) if (f.id == s.font) fname = w(f.name);
        chooser("share:font", L"Typeface", fname, [this, fams] {
            std::vector<std::wstring> items{L"The design's own"};
            int cur = 0;
            for (size_t i = 0; i < fams.size(); ++i) {
                items.push_back(w(fams[i].name));
                if (fams[i].id == wb_.shareState.font) cur = (int)i + 1;
            }
            const int i = popupAt(hwnd(), items, cur);
            if (i < 0) return;
            wb_.shareState.font = i == 0 ? std::string() : fams[(size_t)i - 1].id;
            saveShare();
        });
        y += gfx_.text(L"The page size is your first book's. The title page reads “The " + name + L" Bible”, or “" + name +
                           L": A Reader's Guide”.",
                       {col.x, y - 6, col.w, 40}, Font::Small, pal_.inkFaint) + 14;
    }

    y += 8;
    {
        const std::wstring go = s.pdf ? L"Make the PDF…" : L"Make the website…";
        const float bw = gfx_.measureWidth(go, Font::BodyStrong) + 48;
        button("share:go", {col.x, y, bw, 40}, go, [this] { makeSharedBible(); }, true);
        y += 60;
    }
    wb_.content = y + wb_.scroll - r.y + 40;
    wb_.scroll = std::clamp(wb_.scroll, 0.0f, std::max(0.0f, wb_.content - r.h));
    clearHitClip();
    gfx_.popClip();
}

void StudioApp::drawSharePanel(const Rect& r) {
    if (!(focus_ == loadedFocus_)) loadFields();
    gfx_.pushClip(r);
    setHitClip(r);
    const Rect c{r.x + 20, r.y, r.w - 40, 0};
    float y = r.y + 18 - wb_.panelScroll;
    if (!wb_.shareState.pdf) {
        panelHeading(c, y, L"THE WEBSITE'S WORDS", true);
        y = panelField(c, y, L"Title (empty: the world's name)", FName);
        y = panelField(c, y, L"A line under the title", FDescription, 70);
        y = panelField(c, y, L"Your own colour (like #2D4A6E)", FAccent);
        y += gfx_.text(L"The home page also shows the world's description and banner, from its details in the Library.", {c.x, y, c.w, 60}, Font::Small,
                       pal_.inkFaint) + 12;
    } else {
        panelHeading(c, y, L"THE PDF", true);
        y += gfx_.text(L"Entries follow one another by kind, each headed by its name with its picture, details and sections. Maps and gallery "
                       L"pictures come after.",
                       {c.x, y, c.w, 100}, Font::Small, pal_.inkMuted) + 12;
    }
    wb_.panelContent = y + wb_.panelScroll - r.y + 20;
    clearHitClip();
    gfx_.popClip();
}

void StudioApp::loadShareFields() {
    const auto& l = wb_.shareState.look;
    setFieldText(FName, l.title);
    std::string tag = l.tagline;
    setFieldText(FDescription, tag);
    bool preset = l.accent.empty();
    for (const char* h : {"#9C2F3A", "#A87B1E", "#2E7D4F", "#1F7A8C", "#2F55B5", "#6D4AC2", "#C2477A", "#C8642A"}) preset |= l.accent == h;
    if (const World* wo = lib_->world(wb_.world)) preset |= l.accent == wo->accent;
    setFieldText(FAccent, preset ? std::string() : l.accent);
}

void StudioApp::commitShareFields() {
    auto& l = wb_.shareState.look;
    const std::string title = fieldText(FName), tag = fieldText(FDescription);
    std::string acc = fieldText(FAccent);
    while (!acc.empty() && acc.back() == ' ') acc.pop_back();
    while (!acc.empty() && acc.front() == ' ') acc.erase(0, 1);
    if (!acc.empty() && acc[0] != '#') acc = "#" + acc;
    const bool okAccent = acc.size() == 7 && acc.find_first_not_of("0123456789abcdefABCDEF", 1) == std::string::npos;
    bool changed = title != l.title || tag != l.tagline;
    l.title = title;
    l.tagline = tag;
    if (okAccent && acc != l.accent) { l.accent = acc; changed = true; }
    if (changed) saveShare();
}

void StudioApp::makeSharedBible() {
    commitFields();
    const ShareState& s = wb_.shareState;
    bible::Options o{wb_.world, wb_.world.empty() ? wb_.book : Id{}, s.guide, s.guide ? s.upTo : Id{}};
    o.pictures = s.pictures;
    o.family = s.family;
    o.languages = s.languages;
    o.glossary = s.glossary;
    // Family trees as pictures (0.45): drawn with the app's fonts; the PDF's are written beside the app's state.
    o.treeFonts = narrow(exeDir() + L"\\fonts");
    o.treeFolder = narrow(stateDir() + L"\\family-trees");
    const auto books = bible::booksInOrder(*lib_, wb_.world, wb_.book);
    std::wstring name;
    if (const World* wo = lib_->world(wb_.world)) name = w(wo->name);
    else if (const studio::Book* bk = lib_->book(wb_.book)) name = w(bk->title);
    if (!s.pdf) {
        IFileOpenDialog* dlg = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dlg->SetTitle(L"Choose a folder for the website (a new, empty one is best)");
        std::string folder;
        if (SUCCEEDED(dlg->Show(hwnd()))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR p = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) { folder = narrow(p); CoTaskMemFree(p); }
                item->Release();
            }
        }
        dlg->Release();
        if (folder.empty()) return;
        std::string err;
        int pages = 0;
        SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        if (!bible::exportWebsite(*lib_, o, folder, err, &pages, s.look, worldFontFolders(wb_.world, wb_.book))) {
            cjui::messageBox(hwnd(), w("The website couldn't be made.\n\n" + err).c_str(), L"Share the bible", MB_OK | MB_ICONWARNING);
            return;
        }
        flash(L"Made " + std::to_wstring(pages) + L" pages. Its index.html opens it; the whole folder can go online as it is.", 8);
        ShellExecuteW(hwnd(), L"open", w(folder + "\\index.html").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    IFileSaveDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    COMDLG_FILTERSPEC spec[] = {{L"PDF", L"*.pdf"}};
    dlg->SetFileTypes(1, spec);
    dlg->SetDefaultExtension(L"pdf");
    dlg->SetFileName(((o.readersGuide ? name + L" reader's guide" : name + L" bible") + L".pdf").c_str());
    std::string path;
    if (SUCCEEDED(dlg->Show(hwnd()))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) { path = narrow(p); CoTaskMemFree(p); }
            item->Release();
        }
    }
    dlg->Release();
    if (path.empty()) return;
    std::string author;
    for (const Id& b : books) if (const studio::Book* bk = lib_->book(b)) { author = !bk->penName.empty() ? bk->penName : bk->author; if (!author.empty()) break; }
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    const auto src = bible::printableSource(*lib_, o, narrow(o.readersGuide ? name + L": A Reader's Guide" : L"The " + name + L" Bible"), author);
    const auto& ps = typeset::presets();
    typeset::Design design = ps[(size_t)std::clamp(s.preset, 0, (int)ps.size() - 1)].design;
    if (!s.font.empty()) design.font = s.font;
    design.chaptersOnRecto = false;   // entry after entry, not each on a right-hand page
    design.chapterSinkLines = std::min(design.chapterSinkLines, 3);
    typeset::Resources res = typesetResources();
    res.fontsFolder = worldFontFolders(wb_.world, wb_.book);
    const auto book = typeset::typesetBook(src, design, res);
    std::string err;
    if (book.pages.empty() || !typeset::writePdf(book, path, err)) {
        cjui::messageBox(hwnd(), w("The PDF couldn't be made.\n\n" + (err.empty() ? std::string("Nothing to set.") : err)).c_str(), L"Share the bible",
                         MB_OK | MB_ICONWARNING);
        return;
    }
    flash(L"Made a " + std::to_wstring(book.pages.size()) + L"-page PDF.", 6);
    ShellExecuteW(hwnd(), L"open", w(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace nrs
