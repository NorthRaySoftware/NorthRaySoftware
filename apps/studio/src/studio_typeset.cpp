// The Typesetting workspace: a book's design on the left, its pages as spreads
// in the middle, contents and PDF export on the right. Pages are set on a
// worker thread from a snapshot of the book, so editing never waits for it.
#include "studio_app.hpp"
#include "studio/icons.hpp"
#include "cjui/dialog.hpp"
#include "studio/calendar.hpp"
#include "cjui/dialog.hpp"
#include "cjui/menu.hpp"

#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <thread>

namespace nrs {

using nlohmann::json;
using namespace studio;
namespace ts = studio::typeset;

static std::wstring w(const std::string& s) { return widen(s); }
static const UINT_PTR kTypesetTimer = 4;

ts::Resources StudioApp::typesetResources(const Id& book) const {
    return {book.empty() ? narrow(exeDir() + L"\\fonts") : fontFoldersFor(book), narrow(exeDir() + L"\\hyphenation")};
}

std::string StudioApp::ownFontsFolder(const Id& world, const Id& book) const {
    return lib_ ? studio::fonts::scopeFolder(lib_->folder(), world, world.empty() ? book : Id{}) : std::string();
}

std::string StudioApp::worldFontFolders(const Id& world, const Id& book) const {
    return studio::fonts::joinFolders({narrow(exeDir() + L"\\fonts"), ownFontsFolder(world, book)});
}

std::string StudioApp::fontFoldersFor(const Id& book) const {
    const studio::Book* b = lib_ ? lib_->book(book) : nullptr;
    if (!b) return narrow(exeDir() + L"\\fonts");
    return worldFontFolders(b->worldId, b->worldId.empty() ? book : Id{});
}

// ------------------------------------------------------------- opening

void StudioApp::openTypesetting(const Id& bookId) {
    if (!lib_ || !lib_->book(bookId)) return;
    commitFields();
    if (ws_ == Workspace::Writing && editor_.ready()) editor_.send(R"({"type":"flush"})");
    ws_ = Workspace::Typesetting;
    prepareTypeset(bookId);
    setFocus({Focus::Book, bookId, {}});
    invalidate();
}

void StudioApp::prepareTypeset(const Id& bookId) {
    if (ts_.book != bookId) {
        for (auto& k : ts_.drawnKeys) gfx_.forgetImage(k);
        ts_ = {};
        ts_.book = bookId;
        // The design saved with the book, if any.
        const std::string saved = lib_->setting("typeset:" + bookId);
        ts_.design = ts::designFromJson(saved);
        const json j = json::parse(saved, nullptr, false);
        ts_.presetId = j.is_object() ? j.value("preset", std::string("custom")) : std::string("classic");
        if (saved.empty()) ts_.presetId = "classic";
    }
    startTypeset();
}

bool StudioApp::bookHasViewpointSymbols(const Id& bookId) const {
    for (const ManuscriptNode* n : lib_->manuscriptOf(bookId)) {
        if (n->kind != NodeKind::Chapter) continue;
        if (const Entry* e = lib_->entry(studio::icons::viewpointOf(*lib_, *n)); e && !e->deleted) {
            const studio::icons::Icon ic = studio::icons::iconOf(*e);
            if (!ic.symbol.empty() || !ic.symbolImage.empty()) return true;
        }
    }
    return false;
}

// 4.9: chapter titles typed with their viewpoint's symbol would print it twice once the icon is on; offer to take it off.
void StudioApp::offerTypedSymbols(const Id& bookId) {
    const auto typed = studio::icons::titlesStartingWithSymbol(*lib_, bookId);
    if (typed.empty()) return;
    std::wstring list;
    for (size_t i = 0; i < typed.size() && i < 6; ++i)
        if (const ManuscriptNode* n = lib_->node(typed[i])) list += L"\n   \u2022 " + w(n->title);
    if (typed.size() > 6) list += L"\n   and " + std::to_wstring(typed.size() - 6) + L" more";
    const int c = cjui::choiceBox(hwnd(),
        (typed.size() == 1 ? std::wstring(L"A chapter title begins with its viewpoint's symbol, typed:")
                           : std::to_wstring(typed.size()) + L" chapter titles begin with their viewpoint's symbol, typed:") + list +
            L"\n\nWith the viewpoint icon on, the symbol would print twice. Take the typed copy off these titles?",
        L"Viewpoint icons", {L"Take it off", L"Leave the titles"}, 0, MB_ICONQUESTION);
    if (c != 0) return;
    std::vector<ManuscriptNode> before;
    for (const Id& id : typed) {
        const ManuscriptNode* n = lib_->node(id);
        const Entry* e = n ? lib_->entry(studio::icons::viewpointOf(*lib_, *n)) : nullptr;
        if (!n || !e) continue;
        before.push_back(*n);
        ManuscriptNode u = *n;
        u.title = studio::icons::withoutLeadingSymbol(u.title, studio::icons::iconOf(*e).symbol);
        if (u.title.empty()) u.title = n->title;   // never leave a title empty
        lib_->updateNode(u);
    }
    flashUndo(before.size() == 1 ? L"Took the symbol off 1 title." : L"Took the symbol off " + std::to_wstring(before.size()) + L" titles.",
              [this, before] { for (const ManuscriptNode& n : before) lib_->updateNode(n); designChanged(); invalidate(); });
    designChanged();
}

void StudioApp::designChanged() {
    json j = json::parse(ts::designToJson(ts_.design));
    j["preset"] = ts_.presetId;
    if (ts_.presetId != "custom") {
        // A preset as chosen: save only its name, so improvements to it apply.
        j = {{"preset", ts_.presetId}, {"trim", ts_.design.trim}};
    }
    lib_->setSetting("typeset:" + ts_.book, j.dump());
    // Wait for a pause in the changes (dragging a slider), then set the pages again.
    SetTimer(hwnd(), kTypesetTimer, 250, nullptr);
    invalidate();
}

void StudioApp::startTypeset() {
    KillTimer(hwnd(), kTypesetTimer);
    if (!lib_ || !lib_->book(ts_.book)) return;
    if (ts_.busy) { ts_.again = true; return; }
    ts_.busy = true;
    ts_.again = false;
    const int gen = ++ts_.generation;
    auto source = ts::snapshot(*lib_, ts_.book);
    const ts::Design design = ts_.design;
    const ts::Resources res = typesetResources(ts_.book);
    HWND target = hwnd();
    std::thread([source = std::move(source), design, res, gen, target] {
        auto* book = new ts::Book();
        try {
            *book = ts::typesetBook(source, design, res);
        } catch (const std::exception& e) {
            book->ok = false;
            book->error = std::string("Typesetting stopped: ") + e.what();
        }
        if (!PostMessageW(target, WM_APP_TYPESET, (WPARAM)gen, reinterpret_cast<LPARAM>(book))) delete book;
    }).detach();
    invalidate();
}

void StudioApp::typesetFinished(int generation, ts::Book* book) {
    std::shared_ptr<ts::Book> b(book);
    ts_.busy = false;
    if (generation == ts_.generation) {
        for (auto& k : ts_.drawnKeys) gfx_.forgetImage(k);
        ts_.drawnKeys.clear();
        ts_.result = b;
        ts_.spread = std::clamp(ts_.spread, 0, std::max(0, spreadCount() - 1));
    }
    if (ts_.again) startTypeset();
    invalidate();
}

// ---------------------------------------------------------------- spreads

int StudioApp::spreadCount() const {
    const int n = ts_.result ? (int)ts_.result->pages.size() : 0;
    return n == 0 ? 0 : 1 + n / 2;   // page 1 alone, then pairs
}
int StudioApp::spreadOfPage(int i) const { return (i + 1) / 2; }
void StudioApp::showSpread(int s) {
    ts_.spread = std::clamp(s, 0, std::max(0, spreadCount() - 1));
    ts_.page = ts_.spread == 0 ? 0 : ts_.spread * 2 - 1;
    ts_.panX = ts_.panY = 0;
    invalidate();
}
void StudioApp::showPage(int i) {
    const int n = ts_.result ? (int)ts_.result->pages.size() : 0;
    ts_.page = std::clamp(i, 0, std::max(0, n - 1));
    ts_.spread = spreadOfPage(ts_.page);
    ts_.panX = ts_.panY = 0;
    invalidate();
}
void StudioApp::turnPages(int delta) {
    if (ts_.single) showPage(ts_.page + delta);
    else showSpread(ts_.spread + delta);
}

void StudioApp::exportPdf() {
    if (!ts_.result || !ts_.result->ok) return;
    const std::wstring path = askSavePath(L"PDF", L"*.pdf", L"pdf", w(ts_.result->title), L"Export the typeset book as a PDF");
    if (path.empty()) return;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    std::string err;
    if (ts::writePdf(*ts_.result, narrow(path), err))
        flash(L"Exported " + std::to_wstring(ts_.result->pages.size()) + L" pages to " + path.substr(path.find_last_of(L"\\/") + 1) + L".", 8);
    else
        cjui::messageBox(hwnd(), w("The PDF couldn't be written.\n\n" + err).c_str(), L"Export PDF", MB_OK | MB_ICONWARNING);
}

void StudioApp::exportDocx(const Id& bookId) {
    const studio::Book* b = lib_ ? lib_->book(bookId) : nullptr;
    if (!b) return;
    commitFields();
    const std::wstring path = askSavePath(L"Word document", L"*.docx", L"docx", w(b->title), L"Save the book as a Word document");
    if (path.empty()) return;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    studio::publish::DocxOptions o;
    // The design being worked on in Typesetting, else the book's saved one.
    o.design = ws_ == Workspace::Typesetting && ts_.book == bookId ? ts_.design : ts::designFromJson(lib_->setting("typeset:" + bookId));
    o.resources = typesetResources(bookId);
    const auto r = studio::publish::writeDocx(ts::snapshot(*lib_, bookId), narrow(path), o);
    if (!r.ok) {
        cjui::messageBox(hwnd(), w("The Word document couldn't be written.\n\n" + r.error).c_str(), L"Save as Word", MB_OK | MB_ICONWARNING);
        return;
    }
    std::wstring msg = L"Saved " + std::to_wstring(r.chapters) + (r.chapters == 1 ? L" chapter" : L" chapters");
    if (r.pictures) msg += L" and " + std::to_wstring(r.pictures) + (r.pictures == 1 ? L" picture" : L" pictures");
    msg += L" to " + path.substr(path.find_last_of(L"\\/") + 1) + L".";
    if (!r.notes.empty()) msg += L" " + w(r.notes[0]);
    flash(msg, 9);
}

// ---------------------------------------------------------------- drawing

void StudioApp::drawTypesetSidebar(const Rect& r) {
    if (!panelAsCard_) {   // as a card (0.71), that's its background
        gfx_.fill(r, pal_.sunken);
        gfx_.line(r.right() - 0.5f, r.y, r.right() - 0.5f, r.bottom(), pal_.rule);
    }
    const studio::Book* b = lib_->book(ts_.book);
    if (!b) return;
    gfx_.pushClip(r);
    setHitClip(r);
    const float x = r.x + 10, wd = r.w - 20, tx0 = x + 12, cw = wd - 24;
    float y = r.y + 14 - ts_.sideScroll;
    // (Back to the Library is on the rail, 0.66.)
    if (!panelAsCard_) {   // (in a card, the page's head has these, 0.71)
        gfx_.text(L"TYPESETTING", {tx0, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
        y += 20;
        y += gfx_.text(w(b->title), {tx0, y, cw, 90}, Font::SerifTitle, pal_.ink) + 2;
        std::wstring sub;
        if (ts_.result && ts_.result->ok) sub = std::to_wstring(ts_.result->pages.size()) + L" pages";
        if (ts_.busy) sub = sub.empty() ? L"Setting the pages..." : sub + L" · updating...";
        gfx_.text(sub, {tx0, y, cw, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
        y += 32;
    } else {
        y += 6;
    }
    // Once per book (0.103): its viewpoints have symbols, so say the headings can show them.
    if (!ts_.design.povIcon && lib_->setting("typeset:povHint:" + ts_.book).empty() && bookHasViewpointSymbols(ts_.book)) {
        const std::wstring t = L"Your viewpoint characters have symbols. Print each chapter's viewpoint with its heading, in the book and the ebook?";
        const float th = gfx_.measureHeight(t, cw - 24, Font::Small);
        const Rect nc{x, y, wd, th + 66};
        gfx_.fillRounded(nc, 10, pal_.ribbon.withAlpha(pal_.dark ? 0.16f : 0.09f));
        gfx_.strokeRounded(nc, 10, pal_.ribbon.withAlpha(0.45f));
        gfx_.text(t, {tx0, nc.y + 10, cw - 12, th + 4}, Font::Small, pal_.ink);
        const float by = nc.y + th + 24;
        const float bw1 = buttonWidth(L"Turn it on", Font::Small) + 4, bw2 = buttonWidth(L"No thanks", Font::Small) + 4;
        button("side:ts:povhint:on", {tx0, by, bw1, 30}, L"Turn it on", [this] {
            lib_->setSetting("typeset:povHint:" + ts_.book, "seen");
            ts_.design.povIcon = true;
            ts_.presetId = "custom";
            designChanged();
            offerTypedSymbols(ts_.book);
        }, true);
        button("side:ts:povhint:no", {tx0 + bw1 + 8, by, bw2, 30}, L"No thanks", [this] {
            lib_->setSetting("typeset:povHint:" + ts_.book, "seen");
            flash(L"It's under Viewpoint icons, lower down, if you want it later.");
            invalidate();
        });
        y += nc.h + 18;
    }

    // Designs.
    gfx_.text(L"DESIGN", {tx0, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 24;
    for (const auto& p : ts::presets()) {
        const std::string id = std::string("side:ts:preset:") + p.id;
        // Only the chosen design says what it is (0.67): the rest are a list of names.
        const bool open = ts_.presetId == p.id;
        const float th = open ? gfx_.measureHeight(w(p.description), cw - 12, Font::Small) : 0;
        const Rect rr{x, y, wd, open ? th + 34 : 32};
        addHit(id, rr, [this, pid = std::string(p.id)] {
            const std::string trim = ts_.design.trim;
            const ts::Design was = ts_.design;
            ts_.design = ts::preset(pid).design;
            ts_.design.trim = trim;   // the trim belongs to the book, not the design
            // So do its viewpoint icons (0.103): they come from its characters, not the look.
            ts_.design.povIcon = was.povIcon;
            ts_.design.povShow = was.povShow;
            ts_.design.povPlace = was.povPlace;
            ts_.design.povColour = was.povColour;
            ts_.design.povKey = was.povKey;
            ts_.presetId = pid;
            designChanged();
        });
        const bool sel = ts_.presetId == p.id;
        const float hv = hoverAmount(id);
        if (sel) sideRowFrame({}, rr, true, 0, 0);
        else if (hv > 0.01f) gfx_.fillRounded(rr, 8, pal_.surface.withAlpha(0.6f * hv));
        gfx_.text(w(p.label), {tx0, rr.y + 7, cw, 20}, sel ? Font::BodyStrong : Font::Body, pal_.ink, {Align::Left, false});
        if (open) gfx_.text(w(p.description), {tx0, rr.y + 27, cw - 6, th + 4}, Font::Small, pal_.inkMuted);
        else tooltip(id, w(p.description));
        if (focused(id)) focusRing(rr);
        y += rr.h + 4;
    }
    if (ts_.presetId == "custom")
        gfx_.text(L"Adjusted from a design above.", {tx0, y + 2, cw, 18}, Font::Small, pal_.inkFaint, {Align::Left, false});
    y += 26;

    // Adjustments: any change makes it a custom design.
    auto changed = [this] { ts_.presetId = "custom"; designChanged(); };
    auto label = [&](const std::wstring& t, const std::wstring& v = {}) {
        gfx_.text(t, {tx0, y, cw * 0.6f, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
        if (!v.empty()) gfx_.text(v, {tx0 + cw * 0.4f, y, cw * 0.6f, 18}, Font::Small, pal_.ink, {Align::Right, false});
        y += 22;
    };
    // Trim.
    {
        label(L"Trim size");
        const Rect tb{tx0, y, cw, 30};
        chooserBox("side:ts:trim", tb, w(trimSize(ts_.design.trim.empty() ? b->trim : ts_.design.trim).label), [this] {
            HMENU m = CreatePopupMenu();
            const auto& sizes = trimSizes();
            const studio::Book* bk = lib_->book(ts_.book);
            const std::string cur = trimSize(ts_.design.trim.empty() && bk ? bk->trim : ts_.design.trim).id;
            for (size_t i = 0; i < sizes.size(); ++i)
                AppendMenuW(m, MF_STRING | (cur == sizes[i].id ? MF_CHECKED : 0), 1 + (UINT)i, w(sizes[i].label).c_str());
            POINT pt;
            GetCursorPos(&pt);
            const int cmd = (int)cjui::trackMenu(m, pt, hwnd());
            DestroyMenu(m);
            if (cmd > 0 && lib_->book(ts_.book)) {
                // The trim is the book's: picture sizes and print checks use it too.
                studio::Book u = *lib_->book(ts_.book);
                u.trim = sizes[cmd - 1].id;
                lib_->updateBook(u);
                ts_.design.trim.clear();
                designChanged();
            }
        });
        y += 42;
    }
    // Font.
    {
        const auto fams = ts::fontFamilies(typesetResources(ts_.book).fontsFolder);
        if (!fams.empty()) {
            label(L"Typeface");
            // A list rather than buttons: the built-in typefaces and the world's own (0.40), however many.
            std::wstring cur = w(ts_.design.font);
            for (auto& f : fams) if (f.id == ts_.design.font) cur = w(f.name);
            const Rect tb{tx0, y, cw, 30};
            chooserBox("side:ts:font", tb, cur, [this, fams, changed] {
                HMENU m = CreatePopupMenu();
                const auto own = studio::fonts::familiesIn(studio::fonts::folders(typesetResources(ts_.book).fontsFolder).size() > 1
                                                               ? studio::fonts::folders(typesetResources(ts_.book).fontsFolder)[1]
                                                               : std::string());
                auto isOwn = [&](const std::string& id) { return std::any_of(own.begin(), own.end(), [&](auto& o) { return o.id == id; }); };
                for (size_t i = 0; i < fams.size(); ++i)
                    AppendMenuW(m, MF_STRING | (fams[i].id == ts_.design.font ? MF_CHECKED : 0), 1 + (UINT)i,
                                (w(fams[i].name) + (isOwn(fams[i].id) ? L"  (this world's)" : L"")).c_str());
                POINT pt;
                GetCursorPos(&pt);
                const int cmd = (int)cjui::trackMenu(m, pt, hwnd());
                DestroyMenu(m);
                if (cmd < 1) return;
                ts_.design.font = fams[(size_t)cmd - 1].id;
                changed();
            });
            y += 42;
        }
    }
    // Size and spacing.
    {
        wchar_t v[32];
        swprintf(v, 32, L"%.1f pt", ts_.design.fontSize);
        label(L"Type size", v);
        slider("side:ts:size", {tx0, y, cw, 22}, (float)((ts_.design.fontSize - 9) / 6), [this, changed](float f) {
            const double ratio = ts_.design.leading / ts_.design.fontSize;
            ts_.design.fontSize = std::round((9 + 6 * f) * 4) / 4;
            ts_.design.leading = std::round(ts_.design.fontSize * ratio * 4) / 4;
            changed();
        });
        y += 34;
        const double ratio = ts_.design.leading / ts_.design.fontSize;
        swprintf(v, 32, L"%.1f pt", ts_.design.leading);
        label(L"Line spacing", v);
        slider("side:ts:leading", {tx0, y, cw, 22}, (float)((ratio - 1.1) / 0.6), [this, changed](float f) {
            ts_.design.leading = std::round(ts_.design.fontSize * (1.1 + 0.6 * f) * 4) / 4;
            changed();
        });
        y += 36;
    }
    // Chapter openings.
    label(L"Chapters open with");
    {
        const int cs = ts_.design.chapterStyle == "number" ? 1 : ts_.design.chapterStyle == "numeral" ? 2 : 0;
        segmented("side:ts:chstyle", {tx0, y, cw, 28}, {L"Title", L"Chapter One", L"Numeral"}, cs, [this, changed](int i) {
            ts_.design.chapterStyle = i == 1 ? "number" : i == 2 ? "numeral" : "title";
            changed();
        });
        y += 40;
    }
    // Chapter headers (0.89): the picture or ornament every chapter, and every part, opens with unless it chooses its own.
    {
        y += 6;
        gfx_.text(L"CHAPTER HEADERS", {tx0, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
        y += 22;
        segmented("side:ts:headtab", {tx0, y, cw, 28}, {L"Chapters", L"Parts"}, ts_.headerTab, [this](int i) { ts_.headerTab = i; invalidate(); });
        y += 38;
        const headers::BookHeaders hb = headers::read(*lib_, ts_.book);
        const bool parts = ts_.headerTab == 1;
        std::wstring sample = parts ? L"Part One" : L"Chapter title";
        for (const ManuscriptNode* n : lib_->manuscriptOf(ts_.book))
            if (n->kind == (parts ? NodeKind::Part : NodeKind::Chapter) && n->matter == 0) { sample = w(n->title); break; }
        headerChooser(parts ? "side:ts:hp" : "side:ts:hc", {tx0, y, cw, 0}, y, parts ? hb.parts : hb.chapters, false, std::nullopt, sample,
                      [this, parts](const headers::Header& h) {
                          headers::BookHeaders b = headers::read(*lib_, ts_.book);
                          (parts ? b.parts : b.chapters) = h;
                          headers::write(*lib_, ts_.book, b);
                          designChanged();
                      });
        y += gfx_.text(parts ? L"A part can choose its own in Planning (click its card)."
                             : L"A chapter can choose its own, or none, in Planning (click its card). Without one, a chapter opens with its viewpoint "
                               L"character's header picture or symbol, then this.",
                       {tx0, y, cw, 80}, Font::Small, pal_.inkFaint) + 16;
    }
    // Scene breaks (0.91): a mark, an ornament, or a small picture.
    {
        gfx_.text(L"SCENE BREAKS", {tx0, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
        y += 22;
        const Rect pv{tx0, y, cw, 46};
        gfx_.fillRounded(pv, 8, pal_.dark ? pal_.sunken : hexColor("#FBF8F1"));
        gfx_.strokeRounded(pv, 8, pal_.rule);
        if (!ts_.design.sceneBreakBlob.empty()) {
            const std::wstring path = w(lib_->blobPath(ts_.design.sceneBreakBlob));
            if (!gfx_.drawImage(path, {pv.x + pv.w / 2 - 30, pv.y + 6, 60, 34}, false))
                gfx_.text(L"The picture can't be read", pv, Font::Small, pal_.inkFaint, {Align::Center, false, false, true});
        } else {
            gfx_.text(w(ts_.design.sceneBreak), pv, Font::BodyStrong, pal_.ink, {Align::Center, false, false, true});
        }
        y += 56;
        static const char* marks[] = {"*\u2003*\u2003*", "\u2042", "\u2766", "\u2727\u2003\u2727\u2003\u2727", "~", "\u2022\u2003\u2022\u2003\u2022"};
        float mx = tx0;
        for (size_t i = 0; i < sizeof marks / sizeof *marks; ++i) {
            const std::wstring lw = w(marks[i]);
            const float bw = std::max(40.0f, gfx_.measureWidth(lw, Font::Body) + 20);
            if (mx + bw > tx0 + cw && mx > tx0) { mx = tx0; y += 36; }
            const std::string id = "side:ts:sb:" + std::to_string(i);
            const bool on = ts_.design.sceneBreakBlob.empty() && ts_.design.sceneBreak == marks[i];
            const Rect br{mx, y, bw, 30};
            addHit(id, br, [this, changed, i] { ts_.design.sceneBreak = marks[i]; ts_.design.sceneBreakBlob.clear(); changed(); });
            gfx_.fillRounded(br, 7, on ? pal_.ribbon.withAlpha(0.16f) : pal_.surface.withAlpha(0.5f + 0.5f * hoverAmount(id)));
            gfx_.strokeRounded(br, 7, on ? pal_.ribbon : pal_.rule, on ? 1.5f : 1.0f);
            gfx_.text(lw, br, Font::Body, pal_.ink, {Align::Center, false, false, true});
            if (focused(id)) focusRing(br);
            mx += bw + 6;
        }
        y += 40;
        float bx = tx0;
        auto small = [&](const std::string& id, const std::wstring& l, std::function<void()> act) {
            const float bw = buttonWidth(l, Font::Small) + 4;
            if (bx + bw > tx0 + cw && bx > tx0) { bx = tx0; y += 36; }
            button(id, {bx, y, bw, 30}, l, std::move(act));
            bx += bw + 8;
        };
        small("side:ts:sbown", L"Your own mark…", [this, changed] {
            std::wstring v = w(ts_.design.sceneBreak);
            if (!cjui::askText(hwnd(), L"The scene break", L"Any characters, such as * * * or \u2726 or a word. Spaces are printed wide.", v, L"Use it"))
                return;
            if (v.empty()) return;
            ts_.design.sceneBreak = narrow(v);
            ts_.design.sceneBreakBlob.clear();
            changed();
        });
        small("side:ts:sbpic", ts_.design.sceneBreakBlob.empty() ? L"Choose a picture…" : L"Change the picture…", [this, changed] {
            const std::string blob = pickPicture(L"A picture for the scene break");
            if (blob.empty()) return;
            ts_.design.sceneBreakBlob = blob;
            changed();
        });
        if (!ts_.design.sceneBreakBlob.empty())
            small("side:ts:sbnopic", L"Remove the picture", [this, changed] { ts_.design.sceneBreakBlob.clear(); changed(); });
        y += 40;
        y += gfx_.text(L"The ebook shows the same; a picture is set small and centred, and readers hear only that it's a scene break.",
                       {tx0, y, cw, 60}, Font::Small, pal_.inkFaint) + 16;
    }
    // Text block.
    label(L"Right edge");
    segmented("side:ts:justify", {tx0, y, cw, 28}, {L"Justified", L"Ragged"}, ts_.design.justify ? 0 : 1, [this, changed](int i) {
        ts_.design.justify = i == 0;
        changed();
    });
    y += 40;
    // Switches.
    auto check = [&](const std::string& id, const std::wstring& t, bool on, std::function<void()> flip) {
        const Rect hit{x, y, wd, 26};
        addHit(id, hit, std::move(flip));
        const Rect cb{tx0, y + 4, 18, 18};
        gfx_.strokeRounded(cb, 3, pal_.inkMuted.mix(pal_.ink, hoverAmount(id)), 1.5f);
        const float a = anim(id + "#on", on ? 1.0f : 0.0f, 24.0f);
        if (a > 0.01f) gfx_.fillRounded(cb.inset(4 + 3 * (1 - a)), 2, pal_.ribbon.withAlpha(a));
        gfx_.text(t, {tx0 + 28, y, cw - 28, 26}, Font::Small, pal_.ink, {Align::Left, false, true, true});
        if (focused(id)) focusRing(hit);
        y += 30;
    };
    check("side:ts:hyph", L"Hyphenate", ts_.design.hyphenate, [this, changed] { ts_.design.hyphenate = !ts_.design.hyphenate; changed(); });
    check("side:ts:recto", L"Chapters on right-hand pages", ts_.design.chaptersOnRecto,
          [this, changed] { ts_.design.chaptersOnRecto = !ts_.design.chaptersOnRecto; changed(); });
    check("side:ts:heads", L"Running heads", ts_.design.runningHeads, [this, changed] { ts_.design.runningHeads = !ts_.design.runningHeads; changed(); });
    check("side:ts:title", L"Title page", ts_.design.titlePage, [this, changed] { ts_.design.titlePage = !ts_.design.titlePage; changed(); });
    check("side:ts:copyright", L"Copyright page", ts_.design.copyrightPage,
          [this, changed] { ts_.design.copyrightPage = !ts_.design.copyrightPage; changed(); });
    check("side:ts:contents", L"Contents page", ts_.design.contents, [this, changed] { ts_.design.contents = !ts_.design.contents; changed(); });
    check("side:ts:lead", L"First words in small capitals", ts_.design.leadIn, [this, changed] { ts_.design.leadIn = !ts_.design.leadIn; changed(); });
    check("side:ts:drop", L"Drop cap", ts_.design.dropCap, [this, changed] { ts_.design.dropCap = !ts_.design.dropCap; changed(); });
    // Where they go (0.94), the same in print and the ebook.
    if (ts_.design.leadIn || ts_.design.dropCap)
        y += gfx_.text(L"On every chapter's first paragraph of prose, after any date line, epigraph, heading or picture. "
                       L"Not on front or back matter.",
                       {tx0 + 28, y - 4, cw - 28, 60}, Font::Small, pal_.inkFaint, {Align::Left, false, true}) + 10;
    check("side:ts:roman", L"Front matter in roman numerals", ts_.design.frontRoman,
          [this, changed] { ts_.design.frontRoman = !ts_.design.frontRoman; changed(); });
    // Each chapter's date from Planning's timeline under its heading, in a chosen style (0.85).
    check("side:ts:dates", L"Each chapter's date under its heading", ts_.design.chapterDates,
          [this, changed] { ts_.design.chapterDates = !ts_.design.chapterDates; changed(); });
    if (ts_.design.chapterDates) {
        const cal::Calendar cc = cal::calendarOfBook(*lib_, ts_.book);
        const cal::StorySpan sp = cal::storySpan(*lib_, ts_.book);
        if (cc.empty() || !sp.on) {
            gfx_.text(cc.empty() ? L"The book's world has no calendar yet, so there are no dates to show."
                                 : L"Planning's timeline doesn't count in calendar dates yet (Timeline › Counts in), so there are no dates to show.",
                      {tx0, y, cw, 44}, Font::Small, pal_.inkFaint, {Align::Left, false, true});
            y += 50;
        } else {
            const cal::Date ex = cal::fromDay(cc, sp.any ? sp.first : sp.start);
            label(L"Dates written");
            segmented("side:ts:dform", {tx0, y, cw, 28},
                      {w(cal::formatDateStyled(cc, ex, "words", "none")), w(cal::formatDateStyled(cc, ex, "numbers", "none"))},
                      ts_.design.dateForm == "numbers" ? 1 : 0, [this, changed](int i) {
                          ts_.design.dateForm = i == 1 ? "numbers" : "words";
                          changed();
                      });
            y += 40;
            label(L"The era");
            segmented("side:ts:dera", {tx0, y, cw, 28}, {L"Short", L"Full name", L"None"},
                      ts_.design.dateEra == "name" ? 1 : ts_.design.dateEra == "none" ? 2 : 0, [this, changed](int i) {
                          ts_.design.dateEra = i == 1 ? "name" : i == 2 ? "none" : "abbr";
                          changed();
                      });
            y += 36;
            gfx_.text(L"For example: " + w(cal::formatDateStyled(cc, ex, ts_.design.dateForm, ts_.design.dateEra)), {tx0, y, cw, 22}, Font::Small,
                      pal_.inkFaint, {Align::Left, false, true});
            y += 30;
        }
    }
    // The viewpoint's icon in chapter headings (phase 4c, 0.103), off unless chosen.
    y += 14;
    gfx_.text(L"VIEWPOINT ICONS", {tx0, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 24;
    check("side:ts:pov", L"The viewpoint's icon with each chapter's heading", ts_.design.povIcon, [this, changed] {
        ts_.design.povIcon = !ts_.design.povIcon;
        lib_->setSetting("typeset:povHint:" + ts_.book, "seen");
        changed();
        if (ts_.design.povIcon) offerTypedSymbols(ts_.book);
    });
    if (ts_.design.povIcon) {
        label(L"It shows");
        segmented("side:ts:povshow", {tx0, y, cw, 28}, {L"Picture", L"Symbol", L"Initials"},
                  ts_.design.povShow == "portrait" ? 0 : ts_.design.povShow == "initials" ? 2 : 1, [this, changed](int i) {
                      ts_.design.povShow = i == 0 ? "portrait" : i == 2 ? "initials" : "symbol";
                      changed();
                  });
        y += 36;
        label(L"Where");
        segmented("side:ts:povplace", {tx0, y, cw, 28}, {L"Above", L"Before", L"Beside"},
                  ts_.design.povPlace == "before" ? 1 : ts_.design.povPlace == "beside" ? 2 : 0, [this, changed](int i) {
                      ts_.design.povPlace = i == 1 ? "before" : i == 2 ? "beside" : "above";
                      changed();
                  });
        y += 36;
        label(L"Colour");
        segmented("side:ts:povcolour", {tx0, y, cw, 28}, {L"Full", L"One colour", L"As the text"},
                  ts_.design.povColour == "single" ? 1 : ts_.design.povColour == "text" ? 2 : 0, [this, changed](int i) {
                      ts_.design.povColour = i == 1 ? "single" : i == 2 ? "text" : "full";
                      changed();
                  });
        y += 36;
        label(L"A key of viewpoints");
        segmented("side:ts:povkey", {tx0, y, cw, 28}, {L"None", L"Front", L"Back"},
                  ts_.design.povKey == "front" ? 1 : ts_.design.povKey == "back" ? 2 : 0, [this, changed](int i) {
                      ts_.design.povKey = i == 1 ? "front" : i == 2 ? "back" : "none";
                      changed();
                  });
        y += 36;
        y += gfx_.text(L"Picture uses their picture, else their symbol, else initials. A grey edition prints colours as greys; "
                       L"“As the text” sets them in the text's black. Their symbol's ornament gives way to the icon, so it isn't printed twice. "
                       L"The ebook shows the same, described for screen readers as “Chapter told by …”.",
                       {tx0, y, cw, 90}, Font::Small, pal_.inkFaint) + 8;
        if (const auto typed = studio::icons::titlesStartingWithSymbol(*lib_, ts_.book); !typed.empty()) {
            linkText("side:ts:povtyped", tx0, y,
                     typed.size() == 1 ? L"1 chapter title begins with the symbol: take it off…"
                                       : std::to_wstring(typed.size()) + L" chapter titles begin with the symbol: take it off…",
                     [this] { offerTypedSymbols(ts_.book); });
            y += 28;
        }
    }
    // Margins.
    y += 14;
    gfx_.text(L"MARGINS", {tx0, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 24;
    struct M { const char* id; const wchar_t* name; double* v; };
    const M margins[] = {{"side:ts:m:in", L"Inside (spine)", &ts_.design.marginInside}, {"side:ts:m:out", L"Outside", &ts_.design.marginOutside},
                         {"side:ts:m:top", L"Top", &ts_.design.marginTop}, {"side:ts:m:bottom", L"Bottom", &ts_.design.marginBottom}};
    for (const M& m : margins) {
        label(m.name, length(*m.v));
        double* v = m.v;
        slider(m.id, {tx0, y, cw, 22}, (float)((*v - 0.4) / 1.1), [v, changed](float f) {
            *v = std::round((0.4 + 1.1 * f) * 20) / 20;   // steps of 0.05 in
            changed();
        });
        y += 32;
    }
    ts_.sideContent = y + ts_.sideScroll - r.y + 20;
    clearHitClip();
    gfx_.popClip();
}

void StudioApp::drawTypesetPages(const Rect& r) {
    gfx_.fill(r, pal_.dark ? hexColor("#121214") : hexColor("#DCD7CC"));
    const Rect bar{r.x, r.y, r.w, 46};
    const auto& book = ts_.result;
    if (!book) {
        gfx_.text(ts_.busy ? L"Setting the pages..." : L"", r, Font::Body, pal_.inkMuted, {Align::Center, false, false, true});
        return;
    }
    if (!book->ok) {
        const Rect m{r.x + 40, r.y + 40, std::min(r.w - 80, 560.0f), 200};
        pageHeader(m, L"TYPESETTING", L"These pages can't be set yet", w(book->error));
        return;
    }
    const int n = (int)book->pages.size();
    // Which pages: a spread (the left blank before page 1), or one page.
    int left, right;
    if (ts_.single) { left = -1; right = std::clamp(ts_.page, 0, n - 1); }
    else { left = ts_.spread == 0 ? -1 : ts_.spread * 2 - 1; right = ts_.spread == 0 ? 0 : ts_.spread * 2; }
    const int shownPages = ts_.single ? 1 : 2;

    // The page area and the scale.
    const Rect area{r.x + 30, bar.bottom() + 6, r.w - 60, r.h - bar.h - 36};
    const float pw = (float)book->width, ph = (float)book->height;
    const float fit = std::min(area.w / (pw * shownPages), area.h / ph);
    const float actual = 96.0f / 72.0f;   // a point at its real size on a 96-dpi screen
    const float scale = ts_.zoom == 0 ? fit : ts_.zoom == 1 ? actual : actual * 2;
    const float sw = pw * scale, sh = ph * scale;
    const float totalW = sw * shownPages;
    // Scrolled when it's bigger than the window.
    const float maxX = std::max(0.0f, totalW - area.w), maxY = std::max(0.0f, sh - area.h);
    ts_.panX = std::clamp(ts_.panX, 0.0f, maxX);
    ts_.panY = std::clamp(ts_.panY, 0.0f, maxY);
    const float ox = (maxX > 0 ? area.x - ts_.panX : area.x + (area.w - totalW) / 2);
    const float oy = (maxY > 0 ? area.y - ts_.panY : area.y + (area.h - sh) / 2);

    gfx_.pushClip(area.inset(-24));
    std::vector<std::wstring> keep;
    const float dpi = gfx_.dpiScale() * interfaceZoom();   // the pages are drawn at the zoomed size too (0.98)
    const auto& d = book->design;
    for (int side = 0; side < shownPages; ++side) {
        const int idx = ts_.single ? right : (side == 0 ? left : right);
        const Rect pr{ox + side * sw, oy, sw, sh};
        if (idx < 0 || idx >= n) continue;
        softShadow(pr, 0, 0.5f);
        gfx_.fill(pr, hexColor("#FFFFFF"));
        // Rendered at the screen's real pixel size.
        const int pxH = (int)std::lround(sh * dpi);
        const std::wstring key = L"ts:" + std::to_wstring(ts_.generation) + L":" + std::to_wstring(idx) + L":" + std::to_wstring(pxH);
        keep.push_back(key);
        if (std::find(ts_.drawnKeys.begin(), ts_.drawnKeys.end(), key) == ts_.drawnKeys.end()) {
            const ts::Bitmap bm = ts::renderPage(*book, idx, pxH / (ph / 72.0));
            gfx_.drawPixels(key, bm.width, bm.height, bm.rgba.data(), pr);
        } else {
            gfx_.drawPixels(key, 0, 0, nullptr, pr);   // cached
        }
        if (ts_.guides) {
            // The text block (margins) and the baseline grid.
            const bool recto = (idx + 1) % 2 == 1;
            const float l = (float)((recto ? d.marginInside : d.marginOutside) * 72), rgt = (float)((recto ? d.marginOutside : d.marginInside) * 72);
            const float t = (float)(d.marginTop * 72), btm = (float)(d.marginBottom * 72);
            const Rect block{pr.x + l * scale, pr.y + t * scale, (pw - l - rgt) * scale, (ph - t - btm) * scale};
            const Color guide = hexColor("#4E7DB3");
            const int lines = std::max(5, (int)std::floor((ph - t - btm) / d.leading));
            for (int k = 0; k < lines; ++k) {
                const float yb = block.y + (float)((k * d.leading + d.leading * 0.78) * scale);
                gfx_.line(block.x, yb, block.right(), yb, guide.withAlpha(0.16f));
            }
            gfx_.stroke(block, guide.withAlpha(0.7f), 1);
            // The spine side, marked.
            const float sx = recto ? pr.x : pr.right();
            gfx_.line(sx, pr.y, sx, pr.bottom(), hexColor("#B4442C").withAlpha(0.5f), 2);
        }
    }
    if (!ts_.single && left >= 0 && right < n) gfx_.line(ox + sw, oy, ox + sw, oy + sh, hexColor("#000000").withAlpha(0.12f));
    gfx_.popClip();
    for (auto& k : ts_.drawnKeys) if (std::find(keep.begin(), keep.end(), k) == keep.end()) gfx_.forgetImage(k);
    ts_.drawnKeys = keep;

    // The bar: where you are, turning pages, view and zoom.
    gfx_.fill(bar, pal_.dark ? hexColor("#121214") : hexColor("#DCD7CC"));
    {
        const int a = ts_.single ? right + 1 : (ts_.spread == 0 ? 1 : ts_.spread * 2);
        const int z = ts_.single ? a : std::min(n, ts_.spread == 0 ? 1 : ts_.spread * 2 + 1);
        auto label = [&](int i) {
            const std::string f = book->pages[i - 1].folio;
            return f.empty() ? std::to_wstring(i) : w(f);
        };
        std::wstring where = a == z ? L"Page " + label(a) : L"Pages " + label(a) + L"–" + label(z);
        // Where you are sits between the view switch on the left and the guides and zoom on the right (0.67), losing
        // the "(1 of 7)" when the bar is too narrow for it, so nothing overlaps.
        const std::wstring g = L"Guides";
        const float gw = gfx_.measureWidth(g, Font::Small) + 30;
        const float zx = bar.right() - 16 - 196;
        const float leftEnd = bar.x + 16 + 150 + 8, rightStart = zx - gw - 12 - 8;
        const std::wstring full = where + L"  (" + std::to_wstring(a) + L" of " + std::to_wstring(n) + L")";
        if (gfx_.measureWidth(full, Font::SmallStrong) + 24 + 88 <= rightStart - leftEnd) where = full;
        const float cx = (leftEnd + rightStart) / 2;
        const float tw = gfx_.measureWidth(where, Font::SmallStrong) + 24;
        gfx_.text(where, {cx - tw / 2, bar.y, tw, bar.h}, Font::SmallStrong, pal_.ink, {Align::Center, false, false, true});
        const bool atStart = ts_.single ? ts_.page <= 0 : ts_.spread <= 0;
        const bool atEnd = ts_.single ? ts_.page >= n - 1 : ts_.spread + 1 >= spreadCount();
        if (!atStart) button("ts:prev", {cx - tw / 2 - 44, bar.y + 9, 34, 28}, L"‹", [this] { turnPages(-1); });
        if (!atEnd) button("ts:next", {cx + tw / 2 + 10, bar.y + 9, 34, 28}, L"›", [this] { turnPages(1); });
        // Left: spread or single page; right: zoom and guides.
        segmented("ts:view", {bar.x + 16, bar.y + 9, 150, 28}, {L"Spreads", L"Page"}, ts_.single ? 1 : 0, [this](int i) {
            const int keepPage = ts_.single ? ts_.page : (ts_.spread == 0 ? 0 : ts_.spread * 2 - 1);
            ts_.single = i == 1;
            showPage(keepPage);
        });
        segmented("ts:zoom", {zx, bar.y + 9, 196, 28}, {L"Fit", L"Actual", L"200%"}, ts_.zoom, [this](int i) {
            ts_.zoom = i;
            ts_.panX = ts_.panY = 0;
            invalidate();
        });
        const Rect gb{zx - gw - 12, bar.y + 9, gw, 28};
        const std::string gid = "ts:guides";
        addHit(gid, gb, [this] { ts_.guides = !ts_.guides; invalidate(); });
        const Rect cb{gb.x + 2, gb.y + 6, 16, 16};
        gfx_.strokeRounded(cb, 3, pal_.inkMuted.mix(pal_.ink, hoverAmount(gid)), 1.5f);
        if (ts_.guides) gfx_.fillRounded(cb.inset(4), 2, pal_.ribbon);
        gfx_.text(g, {gb.x + 24, gb.y, gw - 24, gb.h}, Font::Small, pal_.ink, {Align::Left, false, false, true});
        if (focused(gid)) focusRing(gb);
        if (ts_.busy) gfx_.text(L"Updating...", {cx + tw / 2 + 52, bar.y, 100, bar.h}, Font::Small, pal_.inkMuted, {Align::Left, false, false, true});
    }
}

void StudioApp::drawTypesetPanel(const Rect& r) {
    gfx_.fill(r, pal_.paper.mix(pal_.surface, 0.5f));
    gfx_.line(r.x + 0.5f, r.y, r.x + 0.5f, r.bottom(), pal_.rule);
    gfx_.pushClip(r);
    setHitClip(r);
    const Rect c{r.x + 20, r.y, r.w - 40, 0};
    float y = r.y + 18 - ts_.panelScroll;
    panelHeading(c, y, L"EXPORT", true);
    const bool ready = ts_.result && ts_.result->ok;
    if (ready) button("ts:pdf", {c.x, y, c.w, 34}, L"Export PDF...", [this] { exportPdf(); }, true);
    else disabledButton({c.x, y, c.w, 34}, L"Export PDF...");
    y += 44;
    y += gfx_.text(L"A PDF for reading and proofing, with the fonts embedded.", {c.x, y, c.w, 40}, Font::Small, pal_.inkFaint) + 6;
    linkText("ts:publishing", c.x, y, L"Print-ready PDF and EPUB: Publishing", [this] { openPublishing(ts_.book); });
    y += 32;
    if (ready) {
        panelHeading(c, y, L"CONTENTS", false);
        int shown = 0;
        std::string last;
        for (size_t i = 0; i < ts_.result->pages.size(); ++i) {
            const auto& p = ts_.result->pages[i];
            if (p.kind != ts::Page::ChapterOpening && p.kind != ts::Page::PartTitle) continue;
            if (p.chapter == last && p.kind != ts::Page::PartTitle) continue;
            last = p.chapter;
            if (++shown > 200) break;
            const std::string id = "ts:toc:" + std::to_string(i);
            const Rect row{c.x - 6, y, c.w + 12, 26};
            addHit(id, row, [this, i] { showPage((int)i); });
            const float hv = hoverAmount(id);
            const bool here = spreadOfPage((int)i) == ts_.spread;
            if (here) gfx_.fillRounded(row, 3, pal_.surface);
            else if (hv > 0.01f) gfx_.fillRounded(row, 3, pal_.surface.withAlpha(0.7f * hv));
            const bool part = p.kind == ts::Page::PartTitle;
            const float ix = part ? 0.0f : 0.0f;
            gfx_.text(w(p.chapter), {c.x + ix, y, c.w - 44, 26}, part ? Font::SmallStrong : Font::Small, pal_.ink, {Align::Left, false, true, true});
            gfx_.text(std::to_wstring(i + 1), {c.right() - 40, y, 40, 26}, Font::Small, pal_.inkMuted, {Align::Right, false, false, true});
            if (focused(id)) focusRing(row);
            y += 28;
        }
        panelHeading(c, y, L"THIS SETTING", false);
        auto row = [&](const std::wstring& k, const std::wstring& v) {
            gfx_.text(k, {c.x, y, c.w * 0.5f, 20}, Font::Small, pal_.inkMuted, {Align::Left, false});
            gfx_.text(v, {c.x + c.w * 0.4f, y, c.w * 0.6f, 20}, Font::Small, pal_.ink, {Align::Right, false, true});
            y += 22;
        };
        const auto& b = *ts_.result;
        row(L"Pages", std::to_wstring(b.pages.size()));
        row(L"Page size", length(b.width / 72) + L" × " + length(b.height / 72));
        row(L"Text width", length(b.width / 72 - b.design.marginInside - b.design.marginOutside));
        if (!b.warnings.empty()) {
            panelHeading(c, y, L"NOTES", false);
            for (auto& wn : b.warnings) y += gfx_.text(L"•  " + w(wn), {c.x, y, c.w, 80}, Font::Small, pal_.ink) + 6;
        }
        y += 8;
        y += gfx_.text(L"Check a printed proof before you publish.",
                       {c.x, y + 8, c.w, 60}, Font::Small, pal_.inkFaint) + 12;
    }
    ts_.panelContent = y + ts_.panelScroll - r.y + 20;
    clearHitClip();
    gfx_.popClip();
}


// ------------------------------------------------------- the head (0.71)

// Over the pages: the book and how many pages, then Export PDF, Design (the card of choices), Contents and the tools.
void StudioApp::drawTypesetHead(const Rect& r) {
    const studio::Book* b = lib_->book(ts_.book);
    if (!b) return;
    std::wstring note;
    if (ts_.result && ts_.result->ok) note = std::to_wstring(ts_.result->pages.size()) + L" pages";
    if (ts_.busy) note = note.empty() ? L"Setting the pages…" : note + L" · updating…";
    viewerHead(r, L"TYPESETTING", w(b->title), note);
    const float pad = r.w > 900 ? 28 : 18, by = r.y + (r.h - 32) / 2;
    float right = r.right() - pad;
    const bool ready = ts_.result && ts_.result->ok;
    {
        const float tw = gfx_.measureWidth(L"Typeset tools", Font::Body) + 44;
        right -= tw;
        menuButton("ts:head:tools", right, by, L"Typeset tools", [this] {
            std::vector<ToolItem> t;
            t.push_back(ToolItem::item(L"Print-ready PDF and EPUB (Publishing)", [this] { openPublishing(ts_.book); }));
            t.push_back(ToolItem::item(L"Save as Word (.docx)…", [this] { exportDocx(ts_.book); }));
            t.push_back(ToolItem::item(L"Read it as an ebook", [this] { openPreview(ts_.book); }));
            t.push_back(ToolItem::item(L"The book's page", [this] { ws_ = Workspace::Library; goTo(View::Book, ts_.book); }));
            if (ts_.result && ts_.result->ok) {
                const auto& bk = *ts_.result;
                t.push_back(ToolItem::line());
                t.push_back(ToolItem::heading(L"This setting"));
                ToolItem a = ToolItem::item(L"Page size " + length(bk.width / 72) + L" × " + length(bk.height / 72), {});
                a.enabled = false;
                t.push_back(a);
                ToolItem tw2 = ToolItem::item(L"Text width " + length(bk.width / 72 - bk.design.marginInside - bk.design.marginOutside), {});
                tw2.enabled = false;
                t.push_back(tw2);
                if (!bk.warnings.empty()) {
                    t.push_back(ToolItem::heading(L"Notes"));
                    for (auto& wn : bk.warnings) { ToolItem n = ToolItem::item(w(wn), {}); n.enabled = false; t.push_back(n); }
                }
            }
            return t;
        });
        right -= 8;
    }
    if (ready) {
        const float cwid = gfx_.measureWidth(L"Contents", Font::Body) + 44;
        right -= cwid;
        menuButton("ts:head:toc", right, by, L"Contents", [this] {
            std::vector<ToolItem> t;
            if (!ts_.result || !ts_.result->ok) return t;
            std::string last;
            int shown = 0;
            for (size_t i = 0; i < ts_.result->pages.size(); ++i) {
                const auto& p = ts_.result->pages[i];
                if (p.kind != ts::Page::ChapterOpening && p.kind != ts::Page::PartTitle) continue;
                if (p.chapter == last && p.kind != ts::Page::PartTitle) continue;
                last = p.chapter;
                if (++shown > 200) break;
                t.push_back(ToolItem::item(w(p.chapter) + L"  ·  " + std::to_wstring(i + 1), [this, i] { showPage((int)i); },
                                           spreadOfPage((int)i) == ts_.spread));
            }
            return t;
        });
        right -= 8;
    }
    right -= toggleButton("ts:head:design", right, by, L"Design", settings_.typesetDesign, [this] {
        settings_.typesetDesign = !settings_.typesetDesign;
        saveSettings();
        invalidate();
    }, L"The design's choices beside the pages") + 8;
    const float ew = buttonWidth(L"Export PDF…");
    right -= ew;
    if (ready) button("ts:head:pdf", {right, by, ew, 32}, L"Export PDF…", [this] { exportPdf(); }, true);
    else disabledButton({right, by, ew, 32}, L"Export PDF…");
}

}  // namespace nrs
