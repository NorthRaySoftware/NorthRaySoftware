// The Publishing workspace: a book's editions (paperback, hardback, large print,
// ebook), the checks before each goes out, the spine and cover measurements, and
// the files themselves (print-ready PDF/X-1a, EPUB, proof PDF).
//
// A print edition's pages are set here on a worker thread from the edition's own
// design and trim (by default the book's Typesetting design), so they always
// match the manuscript and the edition's ISBNs.
#include "studio_app.hpp"

#include "studio/publish.hpp"

#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <thread>

namespace nrs {

using nlohmann::json;
using namespace studio;
namespace ts = studio::typeset;
namespace pub = studio::publish;

static std::wstring w(const std::string& s) { return widen(s); }

// A menu at the mouse; returns the chosen index or -1.
static int popupAtMouse(HWND owner, const std::vector<std::wstring>& items, int checked) {
    HMENU m = CreatePopupMenu();
    for (size_t i = 0; i < items.size(); ++i)
        AppendMenuW(m, MF_STRING | ((int)i == checked ? MF_CHECKED : 0), 1 + (UINT)i, items[i].c_str());
    POINT pt;
    GetCursorPos(&pt);
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD, pt.x, pt.y, 0, owner, nullptr);
    DestroyMenu(m);
    return cmd - 1;
}

static const char* kKinds[] = {"paperback", "hardback", "large-print", "ebook"};

// ------------------------------------------------------------- opening

const Edition* StudioApp::currentEdition() const {
    if (!lib_) return nullptr;
    if (const Edition* e = lib_->edition(pb_.edition); e && e->bookId == pb_.book) return e;
    return nullptr;
}

void StudioApp::openPublishing(const Id& bookId) {
    if (!lib_ || !lib_->book(bookId)) return;
    commitFields();
    if (ws_ == Workspace::Writing && editor_.ready()) editor_.send(R"({"type":"flush"})");
    if (!lib_->ensureEditions(bookId)) flash(w("Couldn't set up the book's editions: " + lib_->lastError()), 8);
    if (pb_.book != bookId) {
        pb_.result.reset();
        const int gen = pb_.generation;
        pb_ = {};
        pb_.generation = gen;   // a setting still running for the old book is ignored when it finishes
        pb_.book = bookId;
        const Id last = lib_->setting("publish-edition:" + bookId);
        const auto eds = lib_->editionsOf(bookId);
        pb_.edition = lib_->edition(last) && lib_->edition(last)->bookId == bookId ? last : eds.empty() ? Id{} : eds.front()->id;
    }
    if (!lib_->edition(pb_.edition)) {
        const auto eds = lib_->editionsOf(bookId);
        pb_.edition = eds.empty() ? Id{} : eds.front()->id;
    }
    ws_ = Workspace::Publishing;
    chooseEdition(pb_.edition);
    invalidate();
}

void StudioApp::chooseEdition(const Id& editionId) {
    commitFields();
    pb_.edition = editionId;
    if (lib_->edition(editionId)) lib_->setSetting("publish-edition:" + pb_.book, editionId);
    setFocus({Focus::Edition, editionId, {}});
    pb_.result.reset();
    pb_.scroll = 0;
    startEditionTypeset();
    refreshChecks();
    invalidate();
}

void StudioApp::editionChanged() {
    // Its pages and checks follow its settings (and its ISBN, on the copyright page).
    startEditionTypeset();
    refreshChecks();
    invalidate();
}

void StudioApp::startEditionTypeset() {
    const Edition* e = currentEdition();
    if (!e || !e->print()) { pb_.busy = false; return; }
    if (pb_.busy) { pb_.again = true; return; }
    pb_.busy = true;
    pb_.again = false;
    const int gen = ++pb_.generation;
    auto source = pub::editionSource(*lib_, *e);
    const ts::Design design = pub::editionDesign(*lib_, *e);
    const ts::Resources res = typesetResources();
    HWND target = hwnd();
    std::thread([source = std::move(source), design, res, gen, target] {
        auto* book = new ts::Book();
        try {
            *book = ts::typesetBook(source, design, res);
        } catch (const std::exception& ex) {
            book->ok = false;
            book->error = std::string("Typesetting stopped: ") + ex.what();
        }
        if (!PostMessageW(target, WM_APP_EDITION, (WPARAM)gen, reinterpret_cast<LPARAM>(book))) delete book;
    }).detach();
}

void StudioApp::editionTypesetFinished(int generation, ts::Book* book) {
    std::shared_ptr<ts::Book> b(book);
    pb_.busy = false;
    if (generation == pb_.generation) {
        pb_.result = b;
        refreshChecks();
    }
    if (pb_.again) startEditionTypeset();
    invalidate();
}

void StudioApp::refreshChecks() {
    const Edition* e = currentEdition();
    if (!e) { pb_.checks.clear(); return; }
    const ts::Source src = pub::editionSource(*lib_, *e);
    pb_.checks = pub::preflight(src, e->print() && pb_.result ? pb_.result.get() : nullptr);
}

void StudioApp::addEditionMenu() {
    std::vector<std::wstring> items;
    for (const char* k : kKinds) items.push_back(w(editionKindLabel(k)));
    const int i = popupAtMouse(hwnd(), items, -1);
    if (i < 0) return;
    const Id id = lib_->addEdition(pb_.book, kKinds[i]);
    if (id.empty()) { flash(w("Couldn't add the edition: " + lib_->lastError()), 8); return; }
    chooseEdition(id);
}

void StudioApp::removeCurrentEdition() {
    const Edition* e = currentEdition();
    if (!e) return;
    const auto eds = lib_->editionsOf(pb_.book);
    if (eds.size() <= 1) { flash(L"A book keeps at least one edition.", 5); return; }
    const std::wstring name = w(pub::editionLabel(*e));
    const std::wstring msg = L"Remove the " + name + L" edition? Its ISBN and settings go with it. The book itself isn't changed.";
    if (MessageBoxW(hwnd(), msg.c_str(), L"Remove edition", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    const Id gone = e->id;
    if (!lib_->removeEdition(gone)) { flash(w("Couldn't remove the edition: " + lib_->lastError()), 8); return; }
    focus_ = {};   // its fields are gone too
    loadedFocus_ = {Focus::None, "-", ""};
    const auto left = lib_->editionsOf(pb_.book);
    chooseEdition(left.front()->id);
    flash(L"Removed the " + name + L" edition.", 5);
}

std::wstring StudioApp::askSavePath(const wchar_t* kind, const wchar_t* pattern, const wchar_t* ext, const std::wstring& name,
                                    const wchar_t* title) {
    IFileSaveDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return {};
    COMDLG_FILTERSPEC spec[] = {{kind, pattern}};
    dlg->SetFileTypes(1, spec);
    dlg->SetDefaultExtension(ext);
    std::wstring n = name;
    for (auto& c : n) if (wcschr(L"\\/:*?\"<>|", c)) c = L' ';
    dlg->SetFileName((n.empty() ? std::wstring(L"Book") : n).c_str());
    dlg->SetTitle(title);
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
    return path;
}

// Cover measurements need more precision than picture sizes (a spine is a fraction of an inch).
static std::wstring precise(double inches, const std::string& units) {
    wchar_t b[48];
    switch (unitFrom(units)) {
        case Unit::Centimetres: swprintf(b, 48, L"%.2f cm", inches * 2.54); break;
        case Unit::Millimetres: swprintf(b, 48, L"%.1f mm", inches * 25.4); break;
        default: swprintf(b, 48, L"%.3f in", inches); break;
    }
    return b;
}

static std::wstring fileName(const std::wstring& path) { return path.substr(path.find_last_of(L"\\/") + 1); }

void StudioApp::exportEditionPdf(bool print) {
    const Edition* e = currentEdition();
    if (!e || !pb_.result || !pb_.result->ok) return;
    commitFields();
    const std::wstring label = w(pub::editionLabel(*e));
    const std::wstring path = askSavePath(print ? L"PDF/X-1a" : L"PDF", L"*.pdf", L"pdf",
                                          w(pb_.result->title) + L" - " + label + (print ? L" - print" : L" - proof"),
                                          print ? L"Export the print-ready PDF" : L"Export a proof PDF");
    if (path.empty()) return;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    ts::PdfOptions o;
    o.printReady = print;
    o.bleed = print && e->bleed;
    o.colourPictures = e->colour;
    std::string err;
    if (ts::writePdf(*pb_.result, narrow(path), err, o)) {
        pb_.lastExport = (print ? L"Print PDF (" : L"Proof PDF (") + label + L"): " + fileName(path);
        flash(L"Exported the " + std::wstring(print ? L"print-ready" : L"proof") + L" PDF of the " + label + L" (" +
                  std::to_wstring(pb_.result->pages.size()) + L" pages) to " + fileName(path) + L".", 8);
    } else
        MessageBoxW(hwnd(), w("The PDF couldn't be written.\n\n" + err).c_str(), L"Export PDF", MB_OK | MB_ICONWARNING);
    invalidate();
}

void StudioApp::exportEpub() {
    const Edition* e = currentEdition();
    const studio::Book* b = lib_ ? lib_->book(pb_.book) : nullptr;
    if (!b || !e) return;
    commitFields();
    const std::wstring path = askSavePath(L"EPUB", L"*.epub", L"epub", w(b->title), L"Export the ebook (EPUB)");
    if (path.empty()) return;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    const auto r = pub::writeEpub(pub::editionSource(*lib_, *e), narrow(path));
    if (!r.ok) {
        MessageBoxW(hwnd(), w("The EPUB couldn't be written.\n\n" + r.error).c_str(), L"Export EPUB", MB_OK | MB_ICONWARNING);
        return;
    }
    pb_.lastExport = L"EPUB: " + fileName(path);
    std::wstring msg = L"Exported the ebook: " + std::to_wstring(r.chapters) + (r.chapters == 1 ? L" section" : L" sections");
    if (r.pictures) msg += L", " + std::to_wstring(r.pictures) + (r.pictures == 1 ? L" picture" : L" pictures");
    msg += L", to " + fileName(path) + L".";
    if (!r.notes.empty()) msg += L" " + w(r.notes[0]);
    flash(msg, 9);
    invalidate();
}

// ---------------------------------------------------------------- drawing

void StudioApp::drawPublishSidebar(const Rect& r) {
    gfx_.fill(r, pal_.sunken);
    gfx_.line(r.right() - 0.5f, r.y, r.right() - 0.5f, r.bottom(), pal_.rule);
    const studio::Book* b = lib_->book(pb_.book);
    if (!b) return;
    gfx_.pushClip(r);
    setHitClip(r);
    const float x = r.x + 10, wd = r.w - 20, tx0 = x + 12, cw = wd - 24;
    float y = r.y + 14 - pb_.sideScroll;
    linkText("side:pb:library", tx0, y, L"‹ Library", [this] { ws_ = Workspace::Library; goTo(View::Book, pb_.book); });
    y += 30;
    gfx_.text(L"PUBLISHING", {tx0, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 20;
    y += gfx_.text(w(b->title), {tx0, y, cw, 90}, Font::SerifTitle, pal_.ink) + 2;
    gfx_.text(std::to_wstring(lib_->editionsOf(pb_.book).size()) + L" editions", {tx0, y, cw, 18}, Font::Small, pal_.inkMuted,
              {Align::Left, false});
    y += 32;

    // Editions.
    gfx_.text(L"EDITIONS", {tx0, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 24;
    for (const Edition* e : lib_->editionsOf(pb_.book)) {
        const std::string id = "side:pb:ed:" + e->id;
        const Rect rr{x, y, wd, 46};
        addHit(id, rr, [this, eid = e->id] { if (eid != pb_.edition) chooseEdition(eid); });
        const bool sel = e->id == pb_.edition;
        const float hv = hoverAmount(id);
        if (sel) { gfx_.fillRounded(rr, 3, pal_.surface); gfx_.fill({rr.x, rr.y + 7, 3, rr.h - 14}, pal_.ribbon); }
        else if (hv > 0.01f) gfx_.fillRounded(rr, 3, pal_.surface.withAlpha(0.6f * hv));
        std::wstring name = w(e->name.empty() ? std::string(editionKindLabel(e->kind)) : e->name);
        gfx_.text(name, {tx0, rr.y + 5, cw, 20}, sel ? Font::BodyStrong : Font::Body, pal_.ink, {Align::Left, false, true});
        std::wstring sub = e->isbn.empty() ? L"No ISBN yet" : L"ISBN " + w(e->isbn);
        if (!e->name.empty()) sub = w(editionKindLabel(e->kind)) + L" · " + sub;
        gfx_.text(sub, {tx0, rr.y + 25, cw, 18}, Font::Small, e->isbn.empty() ? pal_.inkFaint : pal_.inkMuted, {Align::Left, false, true});
        if (focused(id)) focusRing(rr);
        y += rr.h + 4;
    }
    linkText("side:pb:add", tx0, y + 4, L"+ Add an edition", [this] { addEditionMenu(); });
    y += 40;

    // The page's parts.
    const Edition* cur = currentEdition();
    gfx_.text(L"STEPS", {tx0, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 24;
    int problems = 0, advice = 0;
    for (const auto& c : pb_.checks) {
        if (c.level == pub::Check::Problem) ++problems;
        else if (c.level == pub::Check::Advice) ++advice;
    }
    struct Step { const wchar_t* name; std::wstring note; int section; };
    std::vector<Step> steps = {
        {L"Check the edition", problems ? std::to_wstring(problems) + (problems == 1 ? L" thing to fix" : L" things to fix")
                             : advice ? std::to_wstring(advice) + (advice == 1 ? L" suggestion" : L" suggestions") : L"Ready", 0}};
    if (cur && cur->print()) steps.push_back({L"Spine and cover", L"Measurements for the cover", 1});
    steps.push_back({L"Make the files", cur && cur->print() ? L"Print PDF and proof" : L"EPUB", 2});
    int n = 0;
    for (const Step& s : steps) {
        const std::string id = "side:pb:step:" + std::to_string(s.section);
        const Rect rr{x, y, wd, 48};
        addHit(id, rr, [this, sec = s.section] {
            pb_.scroll = std::max(0.0f, pb_.sectionY[sec] - 10);
            invalidate();
        });
        const float hv = hoverAmount(id);
        if (hv > 0.01f) gfx_.fillRounded(rr, 3, pal_.surface.withAlpha(0.7f * hv));
        const Rect dot{tx0, rr.y + 9, 22, 22};
        gfx_.circle(dot.x + 11, dot.y + 11, 10.5f, pal_.inkMuted, false, 1.2f);
        gfx_.text(std::to_wstring(++n), dot, Font::SmallStrong, pal_.ink, {Align::Center, false, false, true});
        gfx_.text(s.name, {tx0 + 32, rr.y + 6, cw - 32, 20}, Font::Body, pal_.ink, {Align::Left, false});
        const Color nc = s.section == 0 && problems ? pal_.live : pal_.inkMuted;
        gfx_.text(s.note, {tx0 + 32, rr.y + 26, cw - 32, 18}, Font::Small, nc, {Align::Left, false, true});
        if (focused(id)) focusRing(rr);
        y += 52;
    }
    y += 18;
    gfx_.text(L"ALSO", {tx0, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 24;
    linkText("side:pb:typeset", tx0, y, L"Change the book's print design", [this] { openTypesetting(pb_.book); });
    y += 26;
    linkText("side:pb:preview", tx0, y, L"Read it as an ebook", [this] { openPreview(pb_.book); });
    y += 26;
    linkText("side:pb:details", tx0, y, L"Edit the book's details", [this] { ws_ = Workspace::Library; goTo(View::Book, pb_.book); });
    y += 30;
    pb_.sideContent = y + pb_.sideScroll - r.y + 20;
    clearHitClip();
    gfx_.popClip();
}

void StudioApp::drawPublishPage(const Rect& r) {
    const studio::Book* b = lib_->book(pb_.book);
    const Edition* ed = currentEdition();
    if (!b || !ed) return;
    const bool print = ed->print();

    gfx_.pushClip(r);
    setHitClip(r);
    const float pad = r.w > 900 ? 40 : 28;
    const float cw = std::min(r.w - pad * 2, 760.0f);
    const Rect m{r.x + pad, r.y + 26 - pb_.scroll, cw, 0};
    std::wstring title = w(b->title);
    std::wstring kicker = L"PUBLISHING · " + w(ed->name.empty() ? std::string(editionKindLabel(ed->kind)) + " edition" : ed->name);
    for (auto& ch : kicker) ch = (wchar_t)towupper(ch);
    float y = m.y + pageHeader(m, kicker, title,
                               print ? L"Check this edition, give your cover designer its measurements, then make the file for the printer."
                                     : L"Check the ebook edition, then make the EPUB for ebook stores.");
    const bool pagesReady = print && pb_.result && pb_.result->ok;

    // ---- 1. Checks.
    pb_.sectionY[0] = y + pb_.scroll - r.y;
    y += 8;
    int step = 0;
    gfx_.text(std::to_wstring(++step) + L"   CHECK THE EDITION", {m.x, y, cw, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
    linkText("pb:recheck", m.right() - gfx_.measureWidth(L"Check again", Font::Small), y - 2, L"Check again", [this] {
        commitFields();
        editionChanged();
    });
    y += 28;
    if (print && pb_.busy && !pagesReady)
        y += gfx_.text(L"Setting the pages so they can be checked...", {m.x, y, cw, 20}, Font::Small, pal_.inkMuted) + 10;
    if (print && pb_.result && !pb_.result->ok)
        y += gfx_.text(w("The pages couldn't be set: " + pb_.result->error), {m.x, y, cw, 40}, Font::Small, pal_.live) + 10;
    // Problems first, then suggestions, then what's fine.
    for (int pass = 0; pass < 3; ++pass) {
        const pub::Check::Level want = pass == 0 ? pub::Check::Problem : pass == 1 ? pub::Check::Advice : pub::Check::Ok;
        for (const auto& c : pb_.checks) {
            if (c.level != want) continue;
            const float tx = m.x + 34, tw = cw - 34 - 90;
            const std::wstring detail = w(c.detail);
            const float dh = detail.empty() ? 0 : gfx_.measureHeight(detail, tw, Font::Small);
            const float rowH = std::max(34.0f, 26 + dh + 8);
            // The mark: a filled circle for a problem, a ring for a suggestion, a tick for done.
            const float cx = m.x + 11, cy = y + 13;
            if (want == pub::Check::Problem) {
                gfx_.circle(cx, cy, 9, pal_.live, true);
                gfx_.text(L"!", {cx - 9, cy - 10, 18, 20}, Font::SmallStrong, pal_.paper, {Align::Center, false, false, true});
            } else if (want == pub::Check::Advice) {
                gfx_.circle(cx, cy, 8, pal_.demo, false, 1.6f);
            } else {
                gfx_.line(cx - 6, cy, cx - 2, cy + 4.5f, pal_.inkMuted, 1.8f);
                gfx_.line(cx - 2, cy + 4.5f, cx + 6.5f, cy - 5, pal_.inkMuted, 1.8f);
            }
            gfx_.text(w(c.title), {tx, y + 3, tw, 22}, want == pub::Check::Problem ? Font::BodyStrong : Font::Body,
                      want == pub::Check::Ok ? pal_.inkMuted : pal_.ink, {Align::Left, false, true});
            gfx_.text(w(c.area), {m.right() - 90, y + 5, 90, 20}, Font::Small, pal_.inkFaint, {Align::Right, false});
            if (!detail.empty()) gfx_.text(detail, {tx, y + 26, tw, dh + 4}, Font::Small, pal_.inkMuted);
            y += rowH;
            gfx_.line(tx, y - 4.5f, m.right(), y - 4.5f, pal_.rule.withAlpha(0.6f));
        }
    }
    y += 26;

    // ---- 2. Spine and cover (print editions).
    pb_.sectionY[1] = y + pb_.scroll - r.y;
    if (print) {
        gfx_.text(std::to_wstring(++step) + L"   SPINE AND COVER", {m.x, y, cw, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
        y += 28;
        if (!pagesReady) {
            y += gfx_.text(pb_.busy ? L"Waiting for the pages..." : L"The measurements need the typeset pages.", {m.x, y, cw, 20}, Font::Small,
                           pal_.inkMuted) + 16;
        } else {
            const auto& bk = *pb_.result;
            const int pages = (int)bk.pages.size() + ((int)bk.pages.size() % 2);   // printed on both sides
            gfx_.text(L"Paper", {m.x, y, 120, 20}, Font::Small, pal_.inkMuted, {Align::Left, false});
            segmented("pb:paper", {m.x + 120, y - 4, 200, 28}, {L"White", L"Cream"}, ed->paper == "cream" ? 1 : 0, [this](int i) {
                if (const Edition* e = currentEdition()) {
                    Edition u = *e;
                    u.paper = i == 1 ? "cream" : "white";
                    lib_->updateEdition(u);
                    invalidate();
                }
            });
            y += 36;
            const double trimW = bk.width / 72, trimH = bk.height / 72;
            const double spine = pub::spineInches(pages, ed->paper == "cream" ? pub::Paper::Cream : pub::Paper::White);
            const double bleed = 0.125;
            const double coverW = trimW * 2 + spine + bleed * 2, coverH = trimH + bleed * 2;
            // A small diagram: back, spine, front, to scale.
            const float dh = 120, k = dh / (float)coverH;
            const float dw = (float)coverW * k;
            const Rect d{m.x, y, dw, dh};
            gfx_.fill(d, pal_.surface);
            gfx_.stroke(d, pal_.inkFaint, 1);
            const float bl = (float)bleed * k;
            gfx_.stroke({d.x + bl, d.y + bl, d.w - 2 * bl, d.h - 2 * bl}, pal_.inkMuted, 1);
            const float sx = d.x + bl + (float)trimW * k, sw = std::max(1.5f, (float)spine * k);
            gfx_.fill({sx, d.y, sw, d.h}, pal_.ribbon.withAlpha(0.35f));
            gfx_.text(L"Back", {d.x, d.y, sx - d.x, d.h}, Font::Small, pal_.inkFaint, {Align::Center, false, false, true});
            gfx_.text(L"Front", {sx + sw, d.y, d.right() - sx - sw, d.h}, Font::Small, pal_.inkFaint, {Align::Center, false, false, true});
            const float fx = d.right() + 28, fw = m.right() - fx;
            float fy = y;
            auto row = [&](const std::wstring& k2, const std::wstring& v, bool strong = false) {
                gfx_.text(k2, {fx, fy, fw * 0.55f, 20}, Font::Small, pal_.inkMuted, {Align::Left, false});
                gfx_.text(v, {fx + fw * 0.45f, fy, fw * 0.55f, 20}, strong ? Font::SmallStrong : Font::Small, pal_.ink, {Align::Right, false, true});
                fy += 23;
            };
            row(L"Pages", std::to_wstring(pages));
            row(ed->kind == "hardback" ? L"Thickness of the pages" : L"Spine width", precise(spine, settings_.units), true);
            row(L"Trim size", precise(trimW, settings_.units) + L" × " + precise(trimH, settings_.units));
            if (ed->kind != "hardback") row(L"Full cover, with bleed", precise(coverW, settings_.units) + L" × " + precise(coverH, settings_.units), true);
            row(L"Bleed, each edge", precise(bleed, settings_.units));
            y = std::max(y + dh, fy) + 12;
            const std::wstring note =
                ed->kind == "hardback"
                    ? L"A hardback's cover wraps around boards, so its spine and cover are wider than the pages alone. Use your printer's "
                      L"hardback cover template for the final measurements."
                    : L"The spine is an estimate from the page count and paper. Before the cover is finished, check it against your printer's "
                      L"own cover template, which also places the barcode." +
                          std::wstring(pages < 100 ? L" At this length the spine is too narrow for text." : L"");
            y += gfx_.text(note, {m.x, y, cw, 60}, Font::Small, pal_.inkFaint) + 26;
        }
    }

    // ---- Files.
    pb_.sectionY[2] = y + pb_.scroll - r.y;
    gfx_.text(std::to_wstring(++step) + L"   MAKE THE FILES", {m.x, y, cw, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 30;
    auto card = [&](const std::string& id, const std::wstring& ctitle, const std::wstring& kind, const std::wstring& body, const std::wstring& action,
                    bool enabled, std::function<void()> go, std::function<void(float&)> extra) {
        const float bw = gfx_.measureWidth(action, Font::BodyStrong) + 36;
        const float tw = cw - 40 - bw - 20;
        const float bh = gfx_.measureHeight(body, tw, Font::Small);
        const float top = y;
        float iy = y + 18;
        gfx_.text(ctitle, {m.x + 20, iy, tw, 24}, Font::BodyStrong, pal_.ink, {Align::Left, false});
        gfx_.text(kind, {m.x + 20 + gfx_.measureWidth(ctitle, Font::BodyStrong) + 10, iy + 2, 200, 20}, Font::Small, pal_.inkFaint, {Align::Left, false});
        iy += 26;
        gfx_.text(body, {m.x + 20, iy, tw, bh + 4}, Font::Small, pal_.inkMuted);
        iy += bh + 8;
        if (extra) extra(iy);
        const float h = iy - top + 12;
        gfx_.strokeRounded({m.x, top, cw, h}, 4, pal_.rule, 1);
        const Rect btn{m.right() - 20 - bw, top + 18, bw, 34};
        if (enabled) button(id, btn, action, std::move(go), true);
        else disabledButton(btn, action);
        y = top + h + 14;
    };
    auto check = [&](float& iy, const std::string& id, const std::wstring& t, const std::wstring& note, bool on, std::function<void()> flip) {
        const float x0 = m.x + 20;
        const Rect hit{x0 - 4, iy, 28 + gfx_.measureWidth(t, Font::Small) + 8, 26};
        addHit(id, hit, std::move(flip));
        const Rect cb{x0, iy + 4, 18, 18};
        gfx_.strokeRounded(cb, 3, pal_.inkMuted.mix(pal_.ink, hoverAmount(id)), 1.5f);
        const float a = anim(id + "#on", on ? 1.0f : 0.0f, 24.0f);
        if (a > 0.01f) gfx_.fillRounded(cb.inset(4 + 3 * (1 - a)), 2, pal_.ribbon.withAlpha(a));
        gfx_.text(t, {x0 + 28, iy, 260, 26}, Font::Small, pal_.ink, {Align::Left, false, true, true});
        const float nx = x0 + 28 + gfx_.measureWidth(t, Font::Small) + 10;
        gfx_.text(note, {nx, iy, m.right() - 20 - nx, 26}, Font::Small, pal_.inkFaint, {Align::Left, false, true, true});
        if (focused(id)) focusRing(hit);
        iy += 30;
    };
    auto flipEdition = [this](bool Edition::*field) {
        if (const Edition* e = currentEdition()) {
            Edition u = *e;
            u.*field = !(u.*field);
            lib_->updateEdition(u);
            invalidate();
        }
    };
    if (print) {
        const std::wstring pagesNote = pagesReady ? std::to_wstring(pb_.result->pages.size()) + L" pages, " +
                                                        precise(pb_.result->width / 72, settings_.units) + L" × " +
                                                        precise(pb_.result->height / 72, settings_.units) + L". "
                                                  : std::wstring();
        card("pb:print", L"Print-ready PDF", L"PDF/X-1a",
             pagesNote + L"The interior file print-on-demand printers ask for: text in pure black, pictures in grey or print colour, fonts "
                         L"embedded, and the printing standard declared in the file.",
             L"Export...", pagesReady && !pb_.busy, [this] { exportEditionPdf(true); }, [&](float& iy) {
                 check(iy, "pb:bleed", L"Bleed", L"Only needed when a picture runs off the edge of the page.", ed->bleed,
                       [flipEdition] { flipEdition(&Edition::bleed); });
                 check(iy, "pb:colour", L"Colour pictures", L"For a colour interior. Otherwise pictures print in grey.", ed->colour,
                       [flipEdition] { flipEdition(&Edition::colour); });
             });
        card("pb:proof", L"Proof PDF", L"PDF", pagesNote + L"These pages, to read on screen or send to a proofreader.", L"Export...",
             pagesReady && !pb_.busy, [this] { exportEditionPdf(false); }, nullptr);
    } else {
        card("pb:epub", L"Ebook", L"EPUB 3",
             L"For ebook stores and e-readers. The same chapters as the Preview, with your cover, a contents list, the copyright page and "
             L"descriptions of the pictures for screen readers. Readers choose their own font and size.",
             L"Export...", true, [this] { exportEpub(); }, nullptr);
    }
    y += 10;

    pb_.content = y + pb_.scroll - r.y + 30;
    pb_.scroll = std::clamp(pb_.scroll, 0.0f, std::max(0.0f, pb_.content - r.h));
    clearHitClip();
    gfx_.popClip();
}

void StudioApp::drawPublishPanel(const Rect& r) {
    gfx_.fill(r, pal_.paper.mix(pal_.surface, 0.5f));
    gfx_.line(r.x + 0.5f, r.y, r.x + 0.5f, r.bottom(), pal_.rule);
    gfx_.pushClip(r);
    setHitClip(r);
    if (!(focus_ == loadedFocus_)) loadFields();
    const Rect c{r.x + 20, r.y, r.w - 40, 0};
    float y = r.y + 18 - pb_.panelScroll;
    const Edition* ed = currentEdition();
    if (ed) {
        panelHeading(c, y, L"THIS EDITION", true);
        y = panelField(c, y, L"Name (blank: " + w(editionKindLabel(ed->kind)) + L")", FName);
        y = panelField(c, y, L"ISBN", FIsbn);
        panelChoice("pb:kind", c, y, L"Kind", w(editionKindLabel(ed->kind)), [this] {
            const Edition* e = currentEdition();
            if (!e) return;
            std::vector<std::wstring> items;
            int cur = 0;
            for (int i = 0; i < 4; ++i) {
                items.push_back(w(editionKindLabel(kKinds[i])));
                if (e->kind == kKinds[i]) cur = i;
            }
            const int i = popupAtMouse(hwnd(), items, cur);
            if (i < 0 || i == cur) return;
            commitFields();
            Edition u = *currentEdition();
            u.kind = kKinds[i];
            if (u.kind == "large-print" && u.design.empty()) u.design = "large-print";
            lib_->updateEdition(u);
            pb_.result.reset();
            editionChanged();
        });
        if (ed->print()) {
            const studio::Book* b = lib_->book(pb_.book);
            const std::string bookTrim = b ? b->trim : std::string();
            const std::wstring trimLabel = ed->trim.empty() ? L"The book's (" + w(trimSize(bookTrim).label) + L")" : w(trimSize(ed->trim).label);
            panelChoice("pb:trim", c, y, L"Trim size", trimLabel, [this, bookTrim] {
                const Edition* e = currentEdition();
                if (!e) return;
                const auto& sizes = trimSizes();
                std::vector<std::wstring> items{L"The book's (" + w(trimSize(bookTrim).label) + L")"};
                int cur = 0;
                for (size_t i = 0; i < sizes.size(); ++i) {
                    items.push_back(w(sizes[i].label));
                    if (e->trim == sizes[i].id) cur = (int)i + 1;
                }
                const int i = popupAtMouse(hwnd(), items, cur);
                if (i < 0 || i == cur) return;
                Edition u = *e;
                u.trim = i == 0 ? std::string() : sizes[i - 1].id;
                lib_->updateEdition(u);
                editionChanged();
            });
            std::wstring designLabel = L"The book's (from Typesetting)";
            if (!ed->design.empty()) designLabel = w(ts::preset(ed->design).label);
            panelChoice("pb:design", c, y, L"Design", designLabel, [this] {
                const Edition* e = currentEdition();
                if (!e) return;
                const auto& ps = ts::presets();
                std::vector<std::wstring> items{L"The book's (from Typesetting)"};
                int cur = 0;
                for (size_t i = 0; i < ps.size(); ++i) {
                    items.push_back(w(ps[i].label));
                    if (e->design == ps[i].id) cur = (int)i + 1;
                }
                const int i = popupAtMouse(hwnd(), items, cur);
                if (i < 0 || i == cur) return;
                Edition u = *e;
                u.design = i == 0 ? std::string() : ps[i - 1].id;
                lib_->updateEdition(u);
                editionChanged();
            });
        }
        if (lib_->editionsOf(pb_.book).size() > 1) {
            linkText("pb:remove", c.x, y, L"Remove this edition", [this] { removeCurrentEdition(); });
            y += 34;
        }
    }
    if (!pb_.lastExport.empty()) {
        panelHeading(c, y, L"LAST EXPORTED", false);
        y += gfx_.text(pb_.lastExport, {c.x, y, c.w, 60}, Font::Small, pal_.ink) + 14;
    }
    panelHeading(c, y, L"EDITIONS AND ISBNS", false);
    y += gfx_.text(L"Each edition that has an ISBN needs its own: paperback, hardback, large print and ebook. The copyright page "
                   L"lists them all.",
                   {c.x, y, c.w, 160}, Font::Small, pal_.inkMuted) + 14;
    panelHeading(c, y, L"WHERE THE FILES GO", false);
    y += gfx_.text(L"Print-on-demand services take the print-ready PDF for the inside of the book, and a separate cover PDF made to their "
                   L"template.\n\nEbook stores take the EPUB. Upload it as it is; the stores convert it for their own readers.",
                   {c.x, y, c.w, 240}, Font::Small, pal_.inkMuted) + 14;
    panelHeading(c, y, L"BEFORE YOU UPLOAD", false);
    y += gfx_.text(L"Order a printed proof, and read the EPUB on a phone or e-reader. Your printer and each store check the files "
                   L"when you upload them; follow what they report.",
                   {c.x, y, c.w, 160}, Font::Small, pal_.inkFaint) + 14;
    pb_.panelContent = y + pb_.panelScroll - r.y + 20;
    clearHitClip();
    gfx_.popClip();
}

}  // namespace nrs
