// The Typesetting workspace: a book's design on the left, its pages as spreads
// in the middle, contents and PDF export on the right. Pages are set on a
// worker thread from a snapshot of the book, so editing never waits for it.
#include "studio_app.hpp"

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

ts::Resources StudioApp::typesetResources() const {
    return {narrow(exeDir() + L"\\fonts"), narrow(exeDir() + L"\\hyphenation")};
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
    const ts::Resources res = typesetResources();
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
        MessageBoxW(hwnd(), w("The PDF couldn't be written.\n\n" + err).c_str(), L"Export PDF", MB_OK | MB_ICONWARNING);
}

// ---------------------------------------------------------------- drawing

void StudioApp::drawTypesetSidebar(const Rect& r) {
    gfx_.fill(r, pal_.sunken);
    gfx_.line(r.right() - 0.5f, r.y, r.right() - 0.5f, r.bottom(), pal_.rule);
    const studio::Book* b = lib_->book(ts_.book);
    if (!b) return;
    gfx_.pushClip(r);
    setHitClip(r);
    const float x = r.x + 10, wd = r.w - 20, tx0 = x + 12, cw = wd - 24;
    float y = r.y + 14 - ts_.sideScroll;
    linkText("side:ts:library", tx0, y, L"‹ Library", [this] { ws_ = Workspace::Library; goTo(View::Book, ts_.book); });
    y += 30;
    gfx_.text(L"TYPESETTING", {tx0, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 20;
    y += gfx_.text(w(b->title), {tx0, y, cw, 90}, Font::SerifTitle, pal_.ink) + 2;
    {
        std::wstring sub;
        if (ts_.result && ts_.result->ok) sub = std::to_wstring(ts_.result->pages.size()) + L" pages";
        if (ts_.busy) sub = sub.empty() ? L"Setting the pages..." : sub + L" · updating...";
        gfx_.text(sub, {tx0, y, cw, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
        y += 32;
    }

    // Designs.
    gfx_.text(L"DESIGN", {tx0, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 24;
    for (const auto& p : ts::presets()) {
        const std::string id = std::string("side:ts:preset:") + p.id;
        const float th = gfx_.measureHeight(w(p.description), cw - 12, Font::Small);
        const Rect rr{x, y, wd, th + 34};
        addHit(id, rr, [this, pid = std::string(p.id)] {
            const std::string trim = ts_.design.trim;
            ts_.design = ts::preset(pid).design;
            ts_.design.trim = trim;   // the trim belongs to the book, not the design
            ts_.presetId = pid;
            designChanged();
        });
        const bool sel = ts_.presetId == p.id;
        const float hv = hoverAmount(id);
        if (sel) { gfx_.fillRounded(rr, 3, pal_.surface); gfx_.fill({rr.x, rr.y + 7, 3, rr.h - 14}, pal_.ribbon); }
        else if (hv > 0.01f) gfx_.fillRounded(rr, 3, pal_.surface.withAlpha(0.6f * hv));
        gfx_.text(w(p.label), {tx0, rr.y + 7, cw, 20}, sel ? Font::BodyStrong : Font::Body, pal_.ink, {Align::Left, false});
        gfx_.text(w(p.description), {tx0, rr.y + 27, cw - 6, th + 4}, Font::Small, pal_.inkMuted);
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
        button("side:ts:trim", tb, w(trimSize(ts_.design.trim.empty() ? b->trim : ts_.design.trim).label), [this] {
            HMENU m = CreatePopupMenu();
            const auto& sizes = trimSizes();
            const studio::Book* bk = lib_->book(ts_.book);
            const std::string cur = trimSize(ts_.design.trim.empty() && bk ? bk->trim : ts_.design.trim).id;
            for (size_t i = 0; i < sizes.size(); ++i)
                AppendMenuW(m, MF_STRING | (cur == sizes[i].id ? MF_CHECKED : 0), 1 + (UINT)i, w(sizes[i].label).c_str());
            POINT pt;
            GetCursorPos(&pt);
            const int cmd = TrackPopupMenu(m, TPM_RETURNCMD, pt.x, pt.y, 0, hwnd(), nullptr);
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
        gfx_.fillTriangle(tb.right() - 20, tb.y + 13, tb.right() - 10, tb.y + 13, tb.right() - 15, tb.y + 18, pal_.inkMuted);
        y += 42;
    }
    // Font.
    {
        const auto fams = ts::fontFamilies(typesetResources().fontsFolder);
        if (!fams.empty()) {
            label(L"Typeface");
            std::vector<std::wstring> names;
            int sel = 0;
            for (size_t i = 0; i < fams.size(); ++i) {
                std::wstring n = w(fams[i].name);
                if (n.rfind(L"Gentium", 0) == 0) n = L"Gentium";
                if (n.rfind(L"Crimson", 0) == 0) n = L"Crimson";
                names.push_back(n);
                if (fams[i].id == ts_.design.font) sel = (int)i;
            }
            segmented("side:ts:font", {tx0, y, cw, 28}, names, sel, [this, fams, changed](int i) {
                ts_.design.font = fams[i].id;
                changed();
            });
            y += 40;
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
    check("side:ts:roman", L"Front matter in roman numerals", ts_.design.frontRoman,
          [this, changed] { ts_.design.frontRoman = !ts_.design.frontRoman; changed(); });
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
    const float dpi = gfx_.dpiScale();
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
        where += L"  (" + std::to_wstring(a) + L" of " + std::to_wstring(n) + L")";
        const float cx = bar.x + bar.w / 2;
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
        const float zx = bar.right() - 16 - 196;
        segmented("ts:zoom", {zx, bar.y + 9, 196, 28}, {L"Fit", L"Actual", L"200%"}, ts_.zoom, [this](int i) {
            ts_.zoom = i;
            ts_.panX = ts_.panY = 0;
            invalidate();
        });
        const std::wstring g = L"Guides";
        const float gw = gfx_.measureWidth(g, Font::Small) + 30;
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

}  // namespace nrs
