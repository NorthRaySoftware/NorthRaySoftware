// The ebook cover designer (Publishing): a cover made from the book's title and author, a layout,
// colours and, if wanted, a picture; drawn by studiocore (studio/cover.hpp) at the size ebook stores
// ask for, then kept as the book's cover. And the ebook's look, from the book's Typesetting design.
#include "studio_app.hpp"
#include "cjui/dialog.hpp"
#include "cjui/menu.hpp"

#include "studio/cover.hpp"
#include "studio/ebook.hpp"
#include "studio/typeset.hpp"

#include <nlohmann/json.hpp>

#include <shobjidl.h>

#include <filesystem>
#include <fstream>

namespace nrs {

using nlohmann::json;
using namespace studio;

static std::wstring w(const std::string& s) { return widen(s); }

static int popupAtMouse(HWND owner, const std::vector<std::wstring>& items, int checked) {
    HMENU m = CreatePopupMenu();
    for (size_t i = 0; i < items.size(); ++i)
        AppendMenuW(m, MF_STRING | ((int)i == checked ? MF_CHECKED : 0), 1 + (UINT)i, items[i].c_str());
    POINT pt;
    GetCursorPos(&pt);
    const int cmd = (int)cjui::trackMenu(m, pt, owner);
    DestroyMenu(m);
    return cmd - 1;
}

// ------------------------------------------------------------------ the ebook's look

// Kept per ebook edition: whether the typeface goes inside, and how paragraphs are set.
static json ebookLook(const Library& lib, const Id& edition) {
    json j = json::parse(lib.setting("ebook-look:" + edition), nullptr, false);
    if (!j.is_object()) j = json::object();
    return j;
}

ebook::Style StudioApp::ebookStyleFor(const Id& book, const Id& edition, std::string* fontId) {
    const typeset::Design d = typeset::designFromJson(lib_->setting("typeset:" + book));
    ebook::Style s = ebook::styleFrom(d);
    const json look = ebookLook(*lib_, edition);
    s.spaced = look.value("spaced", false);
    if (fontId) *fontId = look.value("font", std::string("book")) == "book" ? d.font : std::string();
    return s;
}

void StudioApp::setEbookLook(const std::string& key, bool value) {
    const Edition* e = currentEdition();
    if (!e) return;
    json j = ebookLook(*lib_, e->id);
    if (key == "font") j["font"] = value ? "book" : "reader";
    else j[key] = value;
    lib_->setSetting("ebook-look:" + e->id, j.dump());
    invalidate();
}

// ------------------------------------------------------------------ the designer

void StudioApp::openCoverDesigner() {
    const Book* b = lib_->book(pb_.book);
    if (!b) return;
    commitFields();
    cv_ = {};
    cv_.open = true;
    cv_.book = b->id;
    cv_.edition = pb_.edition;
    if (const Edition* e = lib_->edition(pb_.edition)) cv_.print = e->print();
    const std::string saved = lib_->setting("cover-design:" + b->id);
    if (!saved.empty()) {
        cv_.spec = cover::fromJson(saved);
        cv_.pictureBlob = cv_.spec.picturePath;   // kept as the picture's blob
    } else {
        cv_.spec.title = b->title;
        cv_.spec.subtitle = b->subtitle;
        cv_.spec.author = b->penName.empty() ? b->author : b->penName;
        // "Book Three of The Alvora Chronicles", when it's numbered in a series.
        for (const Series* s : lib_->seriesOfBook(b->id))
            if (const Membership* m = lib_->membership(s->id, b->id); m && !m->number.empty()) {
                const int n = std::atoi(m->number.c_str());
                cv_.spec.series = "Book " + (n > 0 && std::to_string(n) == m->number ? studio::typeset::spelledNumber(n) : m->number) + " of " + s->name;
                break;
            }
        cv_.spec.fontId = typeset::designFromJson(lib_->setting("typeset:" + b->id)).font;
    }
    if (cv_.spec.backText.empty()) cv_.spec.backText = b->synopsis;   // the blurb, to start with
    pb_.scroll = 0;
    setFocus({Focus::Cover, b->id, {}});
    renderCoverPreview();
    invalidate();
}

void StudioApp::closeCoverDesigner() {
    commitFields();
    if (!cv_.preview.empty()) {
        gfx_.forgetImage(cv_.preview);
        DeleteFileW(cv_.preview.c_str());
    }
    cv_ = {};
    setFocus({Focus::Edition, pb_.edition, {}});
    invalidate();
}

static std::string fontsFolderOf(const std::wstring& exeDir) { return narrow(exeDir + L"\\fonts"); }

bool StudioApp::coverWrap(cover::Wrap& wp) const {
    const Edition* e = lib_->edition(cv_.edition);
    if (!e || !e->print() || !pb_.result || !pb_.result->ok || pb_.edition != cv_.edition) return false;
    const auto& bk = *pb_.result;
    const int pages = (int)bk.pages.size() + ((int)bk.pages.size() % 2);   // printed on both sides
    wp.trimW = bk.width / 72;
    wp.trimH = bk.height / 72;
    wp.spine = studio::publish::spineInches(pages, e->paper == "cream" ? studio::publish::Paper::Cream : studio::publish::Paper::White);
    wp.bleed = 0.125;
    wp.backText = cv_.spec.backText;
    return true;
}

void StudioApp::renderCoverPreview() {
    cover::Spec s = cv_.spec;
    s.picturePath = cv_.pictureBlob.empty() ? std::string() : lib_->blobPath(cv_.pictureBlob);
    struct Shown { bool ok = false; std::string error, png; };
    Shown shown;
    if (cv_.print) {
        cover::Wrap wp;
        if (!coverWrap(wp)) {
            if (!cv_.preview.empty()) { gfx_.forgetImage(cv_.preview); DeleteFileW(cv_.preview.c_str()); cv_.preview.clear(); }
            cv_.error = "Setting the edition's pages first: the spine's width depends on how many there are.";
            invalidate();
            return;
        }
        wp.dpi = std::max(40, (int)(900 / (wp.trimW * 2 + wp.spine + 0.25)));   // about 900 pixels across
        const auto wr = cover::renderWrap(s, wp, fontsFolderOf(exeDir()), false);
        shown = {wr.ok, wr.error, wr.png};
    } else {
        const auto fr = cover::render(s, fontsFolderOf(exeDir()), 500, 800);
        shown = {fr.ok, fr.error, fr.png};
    }
    const auto& r = shown;
    if (!cv_.preview.empty()) {
        gfx_.forgetImage(cv_.preview);
        DeleteFileW(cv_.preview.c_str());
        cv_.preview.clear();
    }
    cv_.error = r.ok ? std::string() : r.error;
    if (!r.ok) { invalidate(); return; }
    const std::wstring path = stateDir() + L"\\cover-preview-" + std::to_wstring(++cv_.serial) + L".png";
    std::ofstream out(path.c_str(), std::ios::binary);
    out << r.png;
    if (out) cv_.preview = path;
    invalidate();
}

void StudioApp::chooseCoverPicture() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    COMDLG_FILTERSPEC spec[] = {{L"Pictures", L"*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp"}};
    dlg->SetFileTypes(1, spec);
    dlg->SetTitle(L"Choose a picture for the cover");
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
    std::ifstream in(path.c_str(), std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty() || bytes.size() > 200u * 1024 * 1024) { flash(L"That picture couldn't be read.", 6); return; }
    const std::string hash = lib_->storeBlob(bytes);
    if (hash.empty()) { flash(w("Couldn't keep the picture: " + lib_->lastError()), 8); return; }
    cv_.pictureBlob = hash;
    if (cv_.spec.layout == "classic" || cv_.spec.layout == "minimal") cv_.spec.layout = "picture";   // a picture asks for a picture layout
    renderCoverPreview();
    if (!cv_.error.empty()) { cv_.pictureBlob.clear(); flash(L"That file couldn't be read as a picture.", 6); renderCoverPreview(); }
}

void StudioApp::saveCoverDesign() {
    cover::Spec keep = cv_.spec;
    keep.picturePath = cv_.pictureBlob;   // the blob, so it still works if the library moves
    lib_->setSetting("cover-design:" + cv_.book, cover::toJson(keep));
}

void StudioApp::exportPrintCover() {
    commitFields();
    const Book* b = lib_->book(cv_.book);
    const Edition* e = lib_->edition(cv_.edition);
    cover::Wrap wp;
    if (!b || !e || !coverWrap(wp)) { flash(L"The edition's pages aren't set yet. Try again in a moment.", 6); return; }
    const std::wstring path = askSavePath(L"PDF", L"*.pdf", L"pdf", w(b->title) + L" - " + w(studio::publish::editionLabel(*e)) + L" - cover",
                                          L"Export the print cover");
    if (path.empty()) return;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    cover::Spec s = cv_.spec;
    s.picturePath = cv_.pictureBlob.empty() ? std::string() : lib_->blobPath(cv_.pictureBlob);
    wp.dpi = 300;
    const auto r = cover::renderWrap(s, wp, fontsFolderOf(exeDir()), true);
    if (!r.ok) { cjui::messageBox(hwnd(), w(r.error), L"Print cover", MB_OK | MB_ICONWARNING); return; }
    std::ofstream out(path.c_str(), std::ios::binary);
    out << r.pdf;
    if (!out) { cjui::messageBox(hwnd(), L"Couldn't save the cover there.", L"Print cover", MB_OK | MB_ICONWARNING); return; }
    out.close();
    saveCoverDesign();
    wchar_t size[96];
    swprintf(size, 96, L"%.3f \u00D7 %.3f in", wp.trimW * 2 + wp.spine + 2 * wp.bleed, wp.trimH + 2 * wp.bleed);
    std::wstring msg = L"Saved the print cover: " + std::wstring(size) + L" with bleed, at 300 pixels per inch, spine " +
                       [&] { wchar_t s2[32]; swprintf(s2, 32, L"%.3f in", wp.spine); return std::wstring(s2); }() + L".\n\n";
    msg += r.spineText ? L"The spine has the title and author on it. " : L"The spine is too narrow for words, so it's left plain. ";
    msg += L"The white space on the back is for the ISBN barcode your printer adds.\n\nBefore uploading, check the measurements against your "
           L"printer's own cover template for this book.";
    if (e->kind == "hardback") msg += L" A hardback's case cover wraps around boards, so it's larger: use the printer's case template.";
    cjui::messageBox(hwnd(), msg, L"Print cover", MB_OK | MB_ICONINFORMATION);
}

void StudioApp::useDesignedCover() {
    commitFields();
    const Book* b = lib_->book(cv_.book);
    if (!b) return;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    cover::Spec s = cv_.spec;
    s.picturePath = cv_.pictureBlob.empty() ? std::string() : lib_->blobPath(cv_.pictureBlob);
    const auto r = cover::render(s, fontsFolderOf(exeDir()));
    if (!r.ok) { cjui::messageBox(hwnd(), w(r.error), L"Cover", MB_OK | MB_ICONWARNING); return; }
    const std::string hash = lib_->storeBlob(r.png);
    if (hash.empty()) { cjui::messageBox(hwnd(), w("The cover couldn't be kept: " + lib_->lastError()), L"Cover", MB_OK | MB_ICONWARNING); return; }
    Book n = *b;
    n.coverBlob = hash;
    lib_->updateBook(n);
    saveCoverDesign();
    closeCoverDesigner();
    editionChanged();   // the checks look at the cover again
    flash(L"The new cover is the book's cover (1600 × 2560 pixels). Change it any time with “Design a cover”.", 7);
}

// The page: the cover as it'll look, and its layout, colours and picture.
void StudioApp::drawCoverDesigner(const Rect& r) {
    gfx_.pushClip(r);
    setHitClip(r);
    const float pad = r.w > 900 ? 40 : 28;
    const float cw = std::min(r.w - pad * 2, 820.0f);
    const Rect m{r.x + pad, r.y + 26 - pb_.scroll, cw, 0};
    const Book* b = lib_->book(cv_.book);
    float y = m.y + pageHeader(m, cv_.print ? L"PUBLISHING \u00B7 PRINT COVER" : L"PUBLISHING \u00B7 EBOOK COVER",
                               cv_.print ? L"Design the print cover" : L"Design a cover",
                               cv_.print ? L"The whole cover for the printer: back, spine and front, at this edition's size, with bleed. Choose a "
                                           L"layout and colours, add a picture if you like, and change the words and the blurb on the right."
                                         : L"A cover for ebook stores, from the book's title and author. Choose a layout and colours, add a picture "
                                           L"if you like, and change the words on the right.");
    (void)b;
    // An ebook cover: the preview on the left, choices on the right. A print cover is wide: it goes above them.
    Rect pr;
    if (cv_.print) {
        cover::Wrap wp;
        const float aspect = coverWrap(wp) ? (float)((wp.trimH + 0.25) / (wp.trimW * 2 + wp.spine + 0.25)) : 0.7f;
        pr = {m.x, y, cw, cw * aspect};
    } else {
        const float pw = std::min(300.0f, cw * 0.42f);
        pr = {m.x, y, pw, pw * 1.6f};
    }
    gfx_.fillRounded({pr.x + 3, pr.y + 5, pr.w, pr.h}, 3, pal_.ink.withAlpha(0.12f));
    if (cv_.preview.empty() || !gfx_.drawImage(cv_.preview, pr, true)) {
        gfx_.fill(pr, pal_.sunken);
        gfx_.text(cv_.error.empty() ? std::wstring(L"Drawing…") : w(cv_.error), pr.inset(16), Font::Small, pal_.inkMuted,
                  {Align::Center, true, false, true});
    }
    gfx_.stroke(pr, pal_.rule, 1);

    const float cx = cv_.print ? m.x : pr.right() + 32, cwid = m.right() - cx;
    float cy = cv_.print ? pr.bottom() + 28 : y;
    if (cv_.print && !cv_.preview.empty()) {
        cover::Wrap wp;
        if (coverWrap(wp)) {
            // Where the trim and the spine are.
            const float k = pr.w / (float)(wp.trimW * 2 + wp.spine + 0.25), bl = 0.125f * k;
            gfx_.stroke({pr.x + bl, pr.y + bl, pr.w - 2 * bl, pr.h - 2 * bl}, cjui::Color{1, 1, 1, 0.55f}, 1);
            const float s0 = pr.x + bl + (float)wp.trimW * k, s1 = s0 + (float)wp.spine * k;
            gfx_.line(s0, pr.y, s0, pr.bottom(), cjui::Color{1, 1, 1, 0.45f}, 1);
            gfx_.line(s1, pr.y, s1, pr.bottom(), cjui::Color{1, 1, 1, 0.45f}, 1);
            wchar_t note[160];
            swprintf(note, 160, L"Back  \u00B7  spine %.3f in  \u00B7  front. %.3f \u00D7 %.3f in with bleed; the fine line is where it's trimmed.",
                     wp.spine, wp.trimW * 2 + wp.spine + 0.25, wp.trimH + 0.25);
            gfx_.text(note, {pr.x, pr.bottom() + 6, pr.w, 18}, Font::Small, pal_.inkFaint, {Align::Left, false, true});
        }
    }
    auto label = [&](const std::wstring& t) {
        gfx_.text(t, {cx, cy, cwid, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
        cy += 24;
    };
    // Layout: four small cards.
    label(L"LAYOUT");
    {
        const auto& ls = cover::layouts();
        const float gap = 8, lw = (cwid - gap) / 2, lh = 38;
        for (size_t i = 0; i < ls.size(); ++i) {
            const Rect lr{cx + (i % 2) * (lw + gap), cy + (i / 2) * (lh + gap), lw, lh};
            const std::string id = std::string("cv:layout:") + ls[i].id;
            const bool on = cv_.spec.layout == ls[i].id;
            addHit(id, lr, [this, l = std::string(ls[i].id)] { cv_.spec.layout = l; renderCoverPreview(); });
            const float hv = hoverAmount(id);
            gfx_.fillRounded(lr, 4, on ? pal_.surface : pal_.sunken.withAlpha(0.4f + 0.4f * hv));
            gfx_.strokeRounded(lr, 4, on ? pal_.ribbon : pal_.rule, on ? 1.6f : 1);
            gfx_.text(w(ls[i].label), {lr.x + 12, lr.y, lr.w - 20, lh}, on ? Font::SmallStrong : Font::Small, pal_.ink, {Align::Left, false, true, true});
            if (focused(id)) focusRing(lr);
        }
        cy += 2 * (lh + gap) + 2;
        for (const auto& l : ls)
            if (cv_.spec.layout == l.id) cy += gfx_.text(w(l.description), {cx, cy, cwid, 40}, Font::Small, pal_.inkFaint) + 18;
    }
    // Colours: the ready-made sets as swatches.
    label(L"COLOURS");
    {
        const auto& ps = cover::palettes();
        const float sw = 34, gap = 10;
        float sx = cx;
        auto hex = [](const char* h) { return cjui::hexColor(h); };
        for (const auto& p : ps) {
            if (sx + sw > cx + cwid) { sx = cx; cy += sw + gap; }
            const Rect sr{sx, cy, sw, sw};
            const std::string id = std::string("cv:colours:") + p.id;
            const bool on = cv_.spec.background == p.background && cv_.spec.ink == p.ink;
            addHit(id, sr, [this, &p] {
                cv_.spec.background = p.background;
                cv_.spec.ink = p.ink;
                cv_.spec.accent = p.accent;
                renderCoverPreview();
            });
            gfx_.fillRounded(sr, sw / 2, hex(p.background));
            gfx_.fillRounded({sr.x + sw * 0.28f, sr.y + sw * 0.28f, sw * 0.44f, sw * 0.44f}, sw * 0.22f, hex(p.ink));
            gfx_.fillRounded({sr.x + sw * 0.40f, sr.y + sw * 0.40f, sw * 0.20f, sw * 0.20f}, sw * 0.10f, hex(p.accent));
            gfx_.strokeRounded(sr.inset(-3), sw / 2 + 3, on ? pal_.ribbon : pal_.rule.withAlpha(hoverAmount(id)), on ? 2.0f : 1.0f);
            if (hoverAmount(id) > 0.5f)
                gfx_.text(w(p.label), {cx, cy + sw + 6, cwid, 18}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
            if (focused(id)) focusRing(sr);
            sx += sw + gap;
        }
        cy += sw + 32;
    }
    // The picture.
    label(L"PICTURE");
    {
        const std::wstring choose = cv_.pictureBlob.empty() ? L"Choose a picture…" : L"Change the picture…";
        const float bw = gfx_.measureWidth(choose, Font::Small) + 28;
        button("cv:picture", {cx, cy, bw, 30}, choose, [this] { chooseCoverPicture(); }, false);
        if (!cv_.pictureBlob.empty()) {
            const float rw = gfx_.measureWidth(L"No picture", Font::Small) + 28;
            button("cv:nopicture", {cx + bw + 8, cy, rw, 30}, L"No picture", [this] { cv_.pictureBlob.clear(); renderCoverPreview(); }, false);
            cy += 42;
            gfx_.text(L"Show", {cx, cy, 60, 28}, Font::Small, pal_.inkMuted, {Align::Left, false, false, true});
            const int at = cv_.spec.focusY < 0.25 ? 0 : cv_.spec.focusY > 0.75 ? 2 : 1;
            segmented("cv:focus", {cx + 52, cy, std::min(260.0f, cwid - 52), 28}, {L"Top", L"Middle", L"Bottom"}, at, [this](int i) {
                cv_.spec.focusY = i * 0.5;
                renderCoverPreview();
            });
            cy += 36;
            gfx_.text(L"Which part of the picture shows when it's a different shape from the space.", {cx, cy, cwid, 20}, Font::Small,
                      pal_.inkFaint, {Align::Left, false, true});
            cy += 30;
        } else {
            cy += 40;
            gfx_.text(L"For the Picture layouts, or in a window on the Classic one. Use a picture you made or have the rights to.",
                      {cx, cy - 4, cwid, 40}, Font::Small, pal_.inkFaint);
            cy += 40;
        }
    }
    // Type.
    label(L"TYPE");
    {
        std::wstring fontName = w(cv_.spec.fontId);
        const auto fams = typeset::fontFamilies(fontsFolderOf(exeDir()));
        for (const auto& f : fams) if (f.id == cv_.spec.fontId) fontName = w(f.name);
        const std::wstring fl = L"Typeface: " + fontName + L"\u2026";
        const float fw = gfx_.measureWidth(fl, Font::Small) + 28;
        button("cv:font", {cx, cy, fw, 30}, fl, [this, fams] {
            std::vector<std::wstring> items;
            int cur = 0;
            for (size_t i = 0; i < fams.size(); ++i) {
                if (fams[i].id == "symbols" || fams[i].id == "courier-prime") continue;
                if (fams[i].id == cv_.spec.fontId) cur = (int)items.size();
                items.push_back(w(fams[i].name));
            }
            const int i = popupAtMouse(hwnd(), items, cur);
            if (i < 0) return;
            for (const auto& f : fams) if (w(f.name) == items[(size_t)i]) cv_.spec.fontId = f.id;
            renderCoverPreview();
        }, false);
        const std::wstring capsL = cv_.spec.titleCaps ? L"Title in capitals: on" : L"Title in capitals: off";
        const float kw = gfx_.measureWidth(capsL, Font::Small) + 28;
        button("cv:caps", {cx + fw + 8, cy, kw, 30}, capsL, [this] { cv_.spec.titleCaps = !cv_.spec.titleCaps; renderCoverPreview(); }, false);
        cy += 48;
    }
    // Done.
    if (cv_.print) {
        const std::wstring ex = L"Export the print cover (PDF)\u2026";
        const float ew = gfx_.measureWidth(ex, Font::BodyStrong) + 36;
        cover::Wrap wp;
        if (coverWrap(wp)) button("cv:export", {cx, cy, ew, 36}, ex, [this] { exportPrintCover(); }, true);
        else disabledButton({cx, cy, ew, 36}, ex);
        const std::wstring front = L"Use the front as the ebook cover";
        const float fw = gfx_.measureWidth(front, Font::Body) + 30;
        button("cv:usefront", {cx + ew + 10, cy, fw, 36}, front, [this] { useDesignedCover(); }, false);
        const float cw2 = gfx_.measureWidth(L"Close", Font::Body) + 30;
        button("cv:cancel", {cx + ew + fw + 20, cy, cw2, 36}, L"Close", [this] { saveCoverDesign(); closeCoverDesigner(); }, false);
        cy += 52;
        gfx_.text(L"300 pixels per inch, with 0.125 in bleed. The white space on the back is left for the ISBN barcode your printer adds. "
                  L"Check the measurements against your printer's cover template before you upload it.",
                  {cx, cy, cwid, 40}, Font::Small, pal_.inkFaint);
        cy += 44;
    } else {
        const std::wstring use = L"Use as the book's cover";
        const float uw = gfx_.measureWidth(use, Font::BodyStrong) + 36;
        button("cv:use", {cx, cy, uw, 36}, use, [this] { useDesignedCover(); }, true);
        const float cw2 = gfx_.measureWidth(L"Cancel", Font::Body) + 30;
        button("cv:cancel", {cx + uw + 10, cy, cw2, 36}, L"Cancel", [this] { closeCoverDesigner(); }, false);
        cy += 52;
        gfx_.text(L"The cover is made at 1600 × 2560 pixels, the size ebook stores ask for. Your print cover is made separately, to "
                  L"your printer's template.",
                  {cx, cy, cwid, 40}, Font::Small, pal_.inkFaint);
        cy += 44;
    }
    y = std::max(pr.bottom(), cy) + 30;
    pb_.content = y + pb_.scroll - r.y + 30;
    pb_.scroll = std::clamp(pb_.scroll, 0.0f, std::max(0.0f, pb_.content - r.h));
    clearHitClip();
    gfx_.popClip();
}

// The panel: the words on the cover.
void StudioApp::drawCoverPanel(const Rect& r) {
    gfx_.fill(r, pal_.paper.mix(pal_.surface, 0.5f));
    gfx_.line(r.x + 0.5f, r.y, r.x + 0.5f, r.bottom(), pal_.rule);
    gfx_.pushClip(r);
    setHitClip(r);
    if (!(focus_ == loadedFocus_)) loadFields();
    const Rect c{r.x + 20, r.y, r.w - 40, 0};
    float y = r.y + 18;
    panelHeading(c, y, L"THE WORDS", true);
    y = panelField(c, y, L"Title", FName);
    y = panelField(c, y, L"Subtitle (blank: none)", FSubtitle);
    y = panelField(c, y, L"Author", FAuthor);
    y = panelField(c, y, L"Series line (blank: none)", FDescription);
    if (cv_.print) y = panelField(c, y, L"Back cover (the blurb)", FSynopsis, 170);
    y += gfx_.text(L"These are only for the cover. The book's own details don't change.", {c.x, y, c.w, 60}, Font::Small, pal_.inkFaint) + 14;
    panelHeading(c, y, L"GOOD COVERS", false);
    y += gfx_.text(L"Covers are seen as small thumbnails first. Check the title can be read at the size of a stamp: a short title "
                   L"large, few words, strong contrast.",
                   {c.x, y, c.w, 160}, Font::Small, pal_.inkMuted) + 14;
    clearHitClip();
    gfx_.popClip();
}

}  // namespace nrs
