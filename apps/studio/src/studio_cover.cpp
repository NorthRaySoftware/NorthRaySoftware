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

void StudioApp::renderCoverPreview() {
    cover::Spec s = cv_.spec;
    s.picturePath = cv_.pictureBlob.empty() ? std::string() : lib_->blobPath(cv_.pictureBlob);
    const auto r = cover::render(s, fontsFolderOf(exeDir()), 500, 800);
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
    cover::Spec keep = cv_.spec;
    keep.picturePath = cv_.pictureBlob;   // the blob, so it still works if the library moves
    lib_->setSetting("cover-design:" + b->id, cover::toJson(keep));
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
    float y = m.y + pageHeader(m, L"PUBLISHING · EBOOK COVER", L"Design a cover",
                               L"A cover for ebook stores, from the book's title and author. Choose a layout and colours, add a picture "
                               L"if you like, and change the words on the right.");
    (void)b;
    // The preview on the left, choices on the right.
    const float pw = std::min(300.0f, cw * 0.42f), ph = pw * 1.6f;
    const Rect pr{m.x, y, pw, ph};
    gfx_.fillRounded({pr.x + 3, pr.y + 5, pr.w, pr.h}, 3, pal_.ink.withAlpha(0.12f));
    if (cv_.preview.empty() || !gfx_.drawImage(cv_.preview, pr, true)) {
        gfx_.fill(pr, pal_.sunken);
        gfx_.text(cv_.error.empty() ? std::wstring(L"Drawing…") : w(cv_.error), pr.inset(16), Font::Small, pal_.inkMuted,
                  {Align::Center, true, false, true});
    }
    gfx_.stroke(pr, pal_.rule, 1);

    const float cx = pr.right() + 32, cwid = m.right() - cx;
    float cy = y;
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
    {
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
    y += gfx_.text(L"These are only for the cover. The book's own details don't change.", {c.x, y, c.w, 60}, Font::Small, pal_.inkFaint) + 14;
    panelHeading(c, y, L"GOOD COVERS", false);
    y += gfx_.text(L"Covers are seen as small thumbnails first. Check the title can be read at the size of a stamp: a short title "
                   L"large, few words, strong contrast.",
                   {c.x, y, c.w, 160}, Font::Small, pal_.inkMuted) + 14;
    clearHitClip();
    gfx_.popClip();
}

}  // namespace nrs
