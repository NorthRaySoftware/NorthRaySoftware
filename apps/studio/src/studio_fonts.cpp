// Worldbuilding › Fonts (0.40): a world's own typefaces.
//
// Fonts the author imports (TrueType files, grouped into families by their names: see studio/fonts.hpp) are copied
// into the library folder for this world, or for an independent book. Every book of the world then offers them for
// typesetting, the cover designer and the ebook, and Share the bible offers them for the website and PDF. The page
// lists them with a line set in each style; the panel renames one, adds styles, or removes it.
#include "studio_app.hpp"
#include "cjui/dialog.hpp"
#include "cjui/menu.hpp"

#include <shobjidl.h>

#include <algorithm>

namespace nrs {

using namespace studio;
using cjui::Align;
using cjui::Font;
using cjui::Rect;

static std::wstring w(const std::string& s) { return widen(s); }

void StudioApp::openFonts(const std::string& select) {
    openEntry({});
    wb_.fontsPage = true;
    wb_.scroll = 0;
    const auto fams = fonts::familiesIn(ownFontsFolder(wb_.world, wb_.book));
    std::string pick = select;
    if (pick.empty() && !fams.empty()) pick = fams[0].id;
    setFocus(pick.empty() ? Focus{} : Focus{Focus::Font, pick, {}});
    invalidate();
}

void StudioApp::importFonts() {
    if (!lib_) return;
    commitFields();
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    COMDLG_FILTERSPEC types[] = {{L"Fonts (.ttf, .otf)", L"*.ttf;*.otf;*.ttc;*.woff;*.woff2"}, {L"All files", L"*.*"}};
    dlg->SetFileTypes(2, types);
    dlg->SetTitle(L"Import fonts for this world");
    FILEOPENDIALOGOPTIONS opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST);
    std::vector<std::string> files;
    if (SUCCEEDED(dlg->Show(hwnd()))) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dlg->GetResults(&items)) && items) {
            DWORD n = 0;
            items->GetCount(&n);
            for (DWORD i = 0; i < n; ++i) {
                IShellItem* item = nullptr;
                if (FAILED(items->GetItemAt(i, &item)) || !item) continue;
                PWSTR p = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) { files.push_back(narrow(p)); CoTaskMemFree(p); }
                item->Release();
            }
            items->Release();
        }
    }
    dlg->Release();
    if (files.empty()) return;
    std::vector<std::string> bundled;
    for (auto& f : typeset::fontFamilies(narrow(exeDir() + L"\\fonts"))) bundled.push_back(f.id);
    const std::string folder = ownFontsFolder(wb_.world, wb_.book);
    if (folder.empty()) return;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    const fonts::ImportResult r = fonts::importFiles(folder, files, bundled);
    fontSamples_.clear();
    std::string pick;
    for (auto& f : fonts::familiesIn(folder)) if (!r.added.empty() && f.name == r.added[0]) pick = f.id;
    openFonts(pick);
    if (!r.problems.empty()) {
        std::wstring msg = r.added.empty() ? L"No fonts were added.\n\n" : L"Added: ";
        if (!r.added.empty()) {
            for (size_t i = 0; i < r.added.size(); ++i) msg += (i ? L", " : L"") + w(r.added[i]);
            msg += L".\n\nSome files weren't used:\n\n";
        }
        for (size_t i = 0; i < r.problems.size() && i < 12; ++i) msg += L"• " + w(r.problems[i]) + L"\n";
        if (r.problems.size() > 12) msg += L"… and " + std::to_wstring(r.problems.size() - 12) + L" more.";
        cjui::messageBox(hwnd(), msg.c_str(), L"Import fonts", MB_OK | (r.added.empty() ? MB_ICONWARNING : MB_ICONINFORMATION));
    } else if (!r.added.empty()) {
        std::wstring names;
        for (size_t i = 0; i < r.added.size(); ++i) names += (i ? L", " : L"") + w(r.added[i]);
        flash(L"Added " + names + L". Every book in this world can use " + (r.added.size() == 1 ? L"it" : L"them") + L" now.", 8);
    }
}

// A line set in one of a family's styles, drawn at `px` (DIPs) tall type, kept until the fonts or the colours change.
float StudioApp::drawFontSample(const std::string& folders, const std::string& id, const std::string& style, const std::string& text, float x,
                                float y, float px, float maxW) {
    const std::uint32_t ink = ((std::uint32_t)(pal_.ink.r * 255) << 16) | ((std::uint32_t)(pal_.ink.g * 255) << 8) | (std::uint32_t)(pal_.ink.b * 255);
    const std::string key = id + "|" + style + "|" + std::to_string(ink) + "|" + std::to_string((int)px) + "|" + text;
    auto it = fontSamples_.find(key);
    if (it == fontSamples_.end()) {
        // Twice the size, drawn at half: sharp on high-density screens.
        it = fontSamples_.emplace(key, fonts::sample(folders, id, style, text, px * 2, ink, (int)(maxW * 2))).first;
    }
    const auto& s = it->second;
    if (s.w <= 0 || s.h <= 0) return 0;
    gfx_.drawPixels(w("font:" + key), s.w, s.h, s.rgba.data(), {x, y, s.w / 2.0f, s.h / 2.0f}, true);
    return s.h / 2.0f;
}

void StudioApp::drawFonts(const Rect& r) {
    const std::string own = ownFontsFolder(wb_.world, wb_.book);
    const std::string all = worldFontFolders(wb_.world, wb_.book);
    const auto fams = fonts::familiesIn(own);
    gfx_.pushClip(r);
    setHitClip(r);
    const float pad = r.w > 900 ? 40 : 28;
    const float cw = std::min(r.w - pad * 2, 860.0f);
    const Rect m{r.x + pad, r.y + 26 - wb_.scroll, cw, 0};
    float y = m.y;
    y += pageHeader(m, L"WORLDBUILDING", L"Fonts",
                    L"Typefaces of this world's own. Every book in it can set its pages, its cover and its ebook in them, and the shared "
                    L"bible can use them too. They're kept in the library folder, so they go wherever the library goes.");
    {
        float bw = 0;
        smallButton("fonts:import", m.x, y, L"Import fonts…", [this] { importFonts(); }, &bw);
        y += 48;
    }
    std::string worldName;
    if (const World* wd = lib_->world(wb_.world)) worldName = wd->name;
    const std::string line = (worldName.empty() ? std::string("The light broke over the harbour") : worldName + ": the light broke over the harbour") + " \xE2\x80\x94 1234";
    if (fams.empty()) {
        y += gfx_.text(L"None yet. Import TrueType font files (.ttf): a family's regular, italic, bold and bold italic files are put "
                       L"together by their names. Fonts with PostScript outlines (many .otf files) can't be put in print PDFs yet, so "
                       L"they're turned away with a note; most fonts also come as .ttf.",
                       {m.x, y, m.w, 90}, Font::Small, pal_.inkMuted) + 20;
    }
    for (const auto& f : fams) {
        const std::string id = "fonts:row:" + f.id;
        const bool sel = focus_.kind == Focus::Font && focus_.id == f.id;
        const float top = y;
        // Height: name line, two samples, styles line.
        const float h = 158;
        const Rect row{m.x, top, m.w, h};
        addHit(id, row, [this, fid = f.id] { commitFields(); setFocus({Focus::Font, fid, {}}); invalidate(); });
        const float hv = hoverAmount(id);
        if (sel) gfx_.fillRounded(row, 8, pal_.surface);
        else if (hv > 0.01f) gfx_.fillRounded(row, 8, pal_.surface.withAlpha(0.6f * hv));
        if (sel) gfx_.fill({row.x, row.y + 10, 3, row.h - 20}, pal_.ribbon);
        gfx_.text(w(f.name), {row.x + 18, top + 12, row.w * 0.6f, 24}, Font::BodyStrong, pal_.ink, {Align::Left, false, true, true});
        std::wstring styles;
        for (auto& s : f.styles) styles += (styles.empty() ? L"" : L" · ") + (s == "BoldItalic" ? std::wstring(L"Bold italic") : w(s));
        gfx_.text(styles + (f.licence ? L"   ·   licence included" : L""), {row.x + 18, top + 12, row.w - 36, 24}, Font::Small,
                  pal_.inkFaint, {Align::Right, false, true, true});
        float sy = top + 44;
        sy += drawFontSample(all, f.id, "Regular", line, row.x + 18, sy, 30, row.w - 36) + 6;
        const bool italic = std::find(f.styles.begin(), f.styles.end(), "Italic") != f.styles.end();
        const bool bold = std::find(f.styles.begin(), f.styles.end(), "Bold") != f.styles.end();
        float sx = row.x + 18;
        if (italic) {
            drawFontSample(all, f.id, "Italic", "Italic: the old tongue", sx, sy, 22, (row.w - 36) / 2);
            sx += (row.w - 36) / 2;
        }
        if (bold) drawFontSample(all, f.id, "Bold", "Bold: Chapter One", sx, sy, 22, (row.w - 36) / 2);
        if (focused(id)) focusRing(row);
        y += h + 10;
    }
    y += 16;
    gfx_.text(L"WHERE THEY'RE USED", {m.x, y, m.w, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 24;
    y += gfx_.text(L"Typesetting › Typeface, for a book's pages and PDFs. The cover designer › Typeface. Publishing › an ebook "
                   L"with the book's typeface, which goes inside the EPUB when its licence allows. Share the bible › Headings and "
                   L"Text for the website (copied into it), and the typeface of the bible's PDF.",
                   {m.x, y, m.w, 120}, Font::Small, pal_.inkMuted) + 16;
    {
        std::wstring built;
        for (auto& f : typeset::fontFamilies(narrow(exeDir() + L"\\fonts"))) built += (built.empty() ? L"" : L", ") + w(f.name);
        gfx_.text(L"BUILT IN", {m.x, y, m.w, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
        y += 24;
        y += gfx_.text(built + L": always there for every world.", {m.x, y, m.w, 60}, Font::Small, pal_.inkMuted) + 20;
    }
    wb_.content = y + wb_.scroll - r.y + 40;
    wb_.scroll = std::clamp(wb_.scroll, 0.0f, std::max(0.0f, wb_.content - r.h));
    clearHitClip();
    gfx_.popClip();
}

// Books of this world that use the family: their typesetting design or their cover.
static std::vector<std::wstring> booksUsing(const Library& lib, const Id& world, const Id& book, const std::string& fid) {
    std::vector<std::wstring> out;
    std::vector<const studio::Book*> books;
    if (!world.empty()) {
        for (const studio::Book& b : lib.books())
            if (b.worldId == world && !b.deleted) books.push_back(&b);
    } else if (const studio::Book* b = lib.book(book)) {
        books.push_back(b);
    }
    for (const studio::Book* b : books) {
        const bool type = typeset::designFromJson(lib.setting("typeset:" + b->id)).font == fid;
        const bool cov = lib.setting("cover-design:" + b->id).find("\"" + fid + "\"") != std::string::npos;
        if (type || cov) out.push_back(widen(b->title));
    }
    return out;
}

void StudioApp::drawFontsPanel(const Rect& r) {
    if (!(focus_ == loadedFocus_)) loadFields();
    gfx_.pushClip(r);
    setHitClip(r);
    const Rect c{r.x + 20, r.y, r.w - 40, 0};
    float y = r.y + 18 - wb_.panelScroll;
    const std::string own = ownFontsFolder(wb_.world, wb_.book);
    const fonts::Family* fam = nullptr;
    const auto fams = fonts::familiesIn(own);
    for (auto& f : fams) if (focus_.kind == Focus::Font && f.id == focus_.id) fam = &f;
    if (fam) {
        panelHeading(c, y, L"TYPEFACE", true);
        y = panelField(c, y, L"Name (as the lists show it)", FName);
        std::wstring styles;
        for (auto& s : fam->styles) styles += (styles.empty() ? L"" : L", ") + (s == "BoldItalic" ? std::wstring(L"bold italic") : widen(s == "Regular" ? "regular" : s == "Italic" ? "italic" : "bold"));
        y += gfx_.text(L"Styles: " + styles + L".", {c.x, y, c.w, 40}, Font::Small, pal_.inkMuted) + 8;
        if (fam->styles.size() < 4)
            y += gfx_.text(L"Where a style is missing, the regular one is used in its place.",
                           {c.x, y, c.w, 60}, Font::Small, pal_.inkFaint) + 8;
        y += gfx_.text(fam->licence ? L"Its licence file came with it and goes into ebooks and the bible website with it."
                                    : L"No licence file came with it. Make sure its licence lets you use it in books you publish.",
                       {c.x, y, c.w, 60}, Font::Small, pal_.inkFaint) + 12;
        linkText("fonts:add", c.x, y, L"Import more styles or fonts…", [this] { importFonts(); });
        y += 34;
        linkText("fonts:remove", c.x, y, L"Remove this typeface", [this, own, fid = fam->id, name = fam->name] {
            const auto using_ = booksUsing(*lib_, wb_.world, wb_.book, fid);
            std::wstring q = L"Remove “" + widen(name) + L"” from this world?";
            if (!using_.empty()) {
                q += L"\n\nUsed by: ";
                for (size_t i = 0; i < using_.size(); ++i) q += (i ? L", " : L"") + using_[i];
                q += L". Their pages and covers will be set in the default typeface instead until you choose another.";
            }
            q += L"\n\nThe font files are deleted from the library folder (not from where you imported them).";
            if (cjui::messageBox(hwnd(), q.c_str(), L"Fonts", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) return;
            loadedFocus_ = focus_ = Focus{};
            std::string err;
            if (!fonts::removeFamily(own, fid, err)) {
                cjui::messageBox(hwnd(), (L"It couldn't be removed.\n\n" + widen(err)).c_str(), L"Fonts", MB_OK | MB_ICONWARNING);
                return;
            }
            fontSamples_.clear();
            openFonts({});
        });
        y += 34;
    } else {
        panelHeading(c, y, L"FONTS", true);
        y += gfx_.text(L"Import a typeface for this world, or pick one on the left to rename or remove it.", {c.x, y, c.w, 60}, Font::Small,
                       pal_.inkMuted) + 12;
    }
    wb_.panelContent = y + wb_.panelScroll - r.y + 20;
    clearHitClip();
    gfx_.popClip();
}

void StudioApp::loadFontFields() {
    for (auto& f : fonts::familiesIn(ownFontsFolder(wb_.world, wb_.book)))
        if (f.id == focus_.id) setFieldText(FName, f.name);
}

void StudioApp::commitFontFields(const Focus& f) {
    const std::string name = fieldText(FName);
    if (name.empty()) return;
    const std::string own = ownFontsFolder(wb_.world, wb_.book);
    for (auto& fam : fonts::familiesIn(own))
        if (fam.id == f.id && fam.name != name) fonts::renameFamily(own, f.id, name);
}

}  // namespace nrs
