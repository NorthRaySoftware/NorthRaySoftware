// Artwork: the artwork library page, a picture's details, and a picture in the
// text (crop, width, alignment, caption, print resolution).
#include "studio_app.hpp"

#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>

namespace nrs {

using nlohmann::json;
using namespace studio;

static std::wstring w(const std::string& s) { return widen(s); }

static Color qualityColor(PrintQuality q) {
    return q == PrintQuality::Good ? hexColor("#3E8E5E") : q == PrintQuality::Soft ? hexColor("#C9922E") : hexColor("#B4442C");
}

double StudioApp::bookTextWidth(const Id& bookId) const {
    const Book* b = lib_ ? lib_->book(bookId) : nullptr;
    return textWidthInches(b ? b->trim : std::string());
}

std::wstring StudioApp::length(double inches) const { return w(formatLength(inches, unitFrom(settings_.units))); }

Rect StudioApp::fitImage(const Rect& box, int iw, int ih) const {
    if (iw <= 0 || ih <= 0) return box;
    const float s = std::min(box.w / iw, box.h / ih);
    const float fw = iw * s, fh = ih * s;
    return {box.x + (box.w - fw) / 2, box.y + (box.h - fh) / 2, fw, fh};
}

// ---------------------------------------------------------------- adding

void StudioApp::addArtworkFiles() {
    if (!lib_) return;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    COMDLG_FILTERSPEC types[] = {{L"Pictures (PNG, JPEG, GIF, WebP, BMP)", L"*.png;*.jpg;*.jpeg;*.gif;*.webp;*.bmp"}};
    dlg->SetFileTypes(1, types);
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Add pictures to the artwork library");
    int added = 0, failed = 0;
    Id last;
    if (SUCCEEDED(dlg->Show(hwnd()))) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dlg->GetResults(&items))) {
            DWORD n = 0;
            items->GetCount(&n);
            for (DWORD i = 0; i < n; ++i) {
                IShellItem* item = nullptr;
                if (FAILED(items->GetItemAt(i, &item))) continue;
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    std::ifstream f(path, std::ios::binary);
                    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                    std::wstring file = path;
                    file = file.substr(file.find_last_of(L"\\/") + 1);
                    const std::wstring stem = file.substr(0, file.find_last_of(L'.'));
                    const Id id = bytes.size() > 200u * 1024 * 1024 ? Id{} : lib_->addArtwork(bytes, narrow(stem), narrow(file));
                    if (id.empty()) ++failed; else { ++added; last = id; }
                    CoTaskMemFree(path);
                }
                item->Release();
            }
            items->Release();
        }
    }
    dlg->Release();
    if (added) {
        goTo(View::Artwork);
        setFocus({Focus::Artwork, last, {}});
    }
    if (failed) flash(std::to_wstring(failed) + (failed == 1 ? L" file wasn't" : L" files weren't") +
                          L" added: only PNG, JPEG, GIF, WebP and BMP pictures (up to 200 MB) can be used.", 8);
    else if (added) flash(L"Added " + std::to_wstring(added) + (added == 1 ? L" picture." : L" pictures."), 4);
}

// Puts a picture from the artwork library into the chapter open in Writing, at the cursor.
void StudioApp::insertArtwork(const Id& artworkId) {
    const Artwork* a = lib_ ? lib_->artwork(artworkId) : nullptr;
    if (!a) return;
    if (writingBook_.empty() || currentNode_.empty() || !lib_->book(writingBook_)) {
        flash(L"Open a chapter in Writing first; the picture goes where the cursor is.", 6);
        return;
    }
    if (!editor_.ready()) { flash(L"The editor isn't available, so the picture can't be placed.", 6); return; }
    openWriting(writingBook_);
    // Start at the widest size that still prints sharply.
    const double width = sharpWidthFraction(a->width, 1, bookTextWidth(writingBook_));
    editor_.focus();
    editor_.send(json{{"type", "image"}, {"blob", a->blob}, {"alt", ""}, {"width", width < 1 ? std::max(0.1, width) : 1.0}}.dump());
}

// ------------------------------------------------------- picture in the text

void StudioApp::pictureMessage(const std::string& text) {
    json m = json::parse(text, nullptr, false);
    if (!m.is_object() || m.value("id", "") != currentNode_) return;
    if (!m.contains("pos") || m["pos"].is_null()) {
        pic_ = {};
        crop_ = {};
        if (focus_.kind == Focus::Picture) setFocus({Focus::Node, currentNode_, {}});
        return;
    }
    const json& a = m["attrs"];
    if (!a.is_object()) return;
    const int pos = m["pos"].get<int>();
    if (crop_.mode && pos == pic_.pos) return;   // our own drag: the panel already shows it
    pic_.pos = pos;
    pic_.blob = a.value("blob", "");
    pic_.alt = a.value("alt", "");
    pic_.caption = a.value("caption", "");
    Placement p;
    p.cropX = a.value("cropX", 0.0); p.cropY = a.value("cropY", 0.0); p.cropW = a.value("cropW", 1.0); p.cropH = a.value("cropH", 1.0);
    p.width = a.value("width", 1.0);
    p.align = a.value("align", "center");
    pic_.place = clampPlacement(p);
    // The same picture whose caption is being typed: keep the fields as they are.
    const Focus f{Focus::Picture, currentNode_, std::to_string(pos)};
    if (!(focus_ == f)) setFocus(f);
    if (!lib_->artworkForBlob(pic_.blob)) lib_->registerArtwork(pic_.blob, "Picture", "From the text");
    invalidate();
}

void StudioApp::sendPicture() {
    if (pic_.pos < 0 || !editor_.ready()) return;
    const Placement p = clampPlacement(pic_.place);
    editor_.send(json{{"type", "setPicture"}, {"pos", pic_.pos},
                      {"attrs", {{"blob", pic_.blob}, {"alt", pic_.alt}, {"caption", pic_.caption}, {"cropX", p.cropX}, {"cropY", p.cropY},
                                 {"cropW", p.cropW}, {"cropH", p.cropH}, {"width", p.width}, {"align", p.align}}}}
                     .dump());
}

void StudioApp::drawPicturePanel(const Rect& c, float& y) {
    const Artwork* art = lib_->artworkForBlob(pic_.blob);
    panelHeading(c, y, L"PICTURE", true);
    const std::wstring name = art ? w(art->name) : L"Picture";
    gfx_.text(name, {c.x, y, c.w - 70, 20}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
    linkText("pic:back", c.right() - gfx_.measureWidth(L"‹ Chapter", Font::Small), y + 2, L"‹ Chapter", [this] {
        commitFields();
        pic_ = {};
        setFocus({Focus::Node, currentNode_, {}});
        if (editor_.ready()) editor_.send(R"({"type":"focus"})");
    });
    y += 30;

    // The picture, with its crop: drag inside to move it, drag a corner to resize it.
    const int iw = art ? art->width : 0, ih = art ? art->height : 0;
    const Rect area{c.x, y, c.w, std::min(220.0f, c.w * 0.8f)};
    gfx_.fillRounded(area, 3, pal_.sunken);
    const Rect img = fitImage(area.inset(8), iw, ih);
    cropBox_ = img;
    if (!gfx_.drawImage(w(lib_->blobPath(pic_.blob)), img, true)) {
        gfx_.text(L"The picture's file is missing", area, Font::Small, pal_.inkFaint, {Align::Center, true, false, true});
        cropBox_ = {};
    } else {
        const Placement& p = pic_.place;
        const Rect cr{img.x + img.w * (float)p.cropX, img.y + img.h * (float)p.cropY, img.w * (float)p.cropW, img.h * (float)p.cropH};
        const Color veil = pal_.paper.withAlpha(0.72f);
        gfx_.fill({img.x, img.y, img.w, cr.y - img.y}, veil);
        gfx_.fill({img.x, cr.bottom(), img.w, img.bottom() - cr.bottom()}, veil);
        gfx_.fill({img.x, cr.y, cr.x - img.x, cr.h}, veil);
        gfx_.fill({cr.right(), cr.y, img.right() - cr.right(), cr.h}, veil);
        gfx_.stroke(cr, pal_.ribbon, 1.5f);
        for (auto [hx, hy] : {std::pair{cr.x, cr.y}, {cr.right(), cr.y}, {cr.x, cr.bottom()}, {cr.right(), cr.bottom()}}) {
            gfx_.fill({hx - 4, hy - 4, 8, 8}, pal_.surface);
            gfx_.stroke({hx - 4, hy - 4, 8, 8}, pal_.ribbon, 1.5f);
        }
    }
    y = area.bottom() + 8;
    {
        const bool cropped = pic_.place.cropX > 0 || pic_.place.cropY > 0 || pic_.place.cropW < 1 || pic_.place.cropH < 1;
        gfx_.text(cropped ? L"Drag the frame to move the crop, or a corner to resize it."
                          : L"Drag a corner to crop. The stored picture is never changed.",
                  {c.x, y, c.w - 60, 34}, Font::Small, pal_.inkFaint);
        if (cropped)
            linkText("pic:uncrop", c.right() - gfx_.measureWidth(L"Reset", Font::Small), y, L"Reset", [this] {
                pic_.place.cropX = pic_.place.cropY = 0;
                pic_.place.cropW = pic_.place.cropH = 1;
                sendPicture();
            });
        y += 40;
    }

    // Size and alignment.
    const double tw = bookTextWidth(writingBook_);
    const PrintSize ps = printSize(iw, ih, pic_.place, tw);
    gfx_.text(L"Width", {c.x, y, c.w * 0.5f, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
    gfx_.text(std::to_wstring((int)std::lround(pic_.place.width * 100)) + L"% of the text · " + length(ps.widthIn),
              {c.x + c.w * 0.3f, y, c.w * 0.7f, 18}, Font::Small, pal_.ink, {Align::Right, false, true});
    y += 22;
    slider("pic:width", {c.x, y, c.w, 22}, (float)((pic_.place.width - 0.1) / 0.9), [this](float v) {
        pic_.place.width = std::round((0.1 + 0.9 * v) * 100) / 100;
        sendPicture();
    });
    y += 34;
    gfx_.text(L"Position", {c.x, y, c.w, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
    y += 20;
    const int alignIdx = pic_.place.align == "left" ? 0 : pic_.place.align == "right" ? 2 : 1;
    segmented("pic:align", {c.x, y, c.w, 28}, {L"Left", L"Centre", L"Right"}, alignIdx, [this](int i) {
        pic_.place.align = i == 0 ? "left" : i == 2 ? "right" : "center";
        sendPicture();
    });
    y += 42;
    y = panelField(c, y, L"Caption (printed under it)", FCaption);
    y = panelField(c, y, L"Description (read aloud by screen readers)", FAlt);

    // Print resolution.
    panelHeading(c, y, L"IN PRINT", false);
    if (iw > 0) {
        const PrintQuality q = printQuality(ps.ppi);
        gfx_.circle(c.x + 4, y + 9, 4, qualityColor(q), true);
        gfx_.text(length(ps.widthIn) + L" × " + length(ps.heightIn) + L" at " + std::to_wstring((int)std::lround(ps.ppi)) + L" ppi",
                  {c.x + 14, y, c.w - 14, 20}, Font::SmallStrong, pal_.ink, {Align::Left, false, true});
        y += 22;
        y += gfx_.text(w(describe(q)), {c.x + 14, y, c.w - 14, 40}, Font::Small, pal_.inkMuted) + 4;
        if (q != PrintQuality::Good) {
            const double sharp = std::max(0.1, sharpWidthFraction(iw, pic_.place.cropW, tw));
            const std::wstring fix = L"Make it " + std::to_wstring((int)std::lround(sharp * 100)) + L"% wide (sharp)";
            linkText("pic:sharp", c.x + 14, y, fix, [this, sharp] { pic_.place.width = std::floor(sharp * 100) / 100; sendPicture(); });
            y += 26;
        }
        const Book* b = lib_->book(writingBook_);
        y += gfx_.text(L"For the book's " + w(trimSize(b ? b->trim : "").label) + L" trim, " + length(tw) +
                           L" of text width. Change the trim size in the book's details.",
                       {c.x, y + 4, c.w, 40}, Font::Small, pal_.inkFaint) + 10;
    }
    panelHeading(c, y, L"MORE", false);
    float lx = c.x;
    if (art) {
        linkText("pic:library", lx, y, L"Show in the artwork library", [this, id = art->id] {
            commitFields();
            ws_ = Workspace::Library;
            goTo(View::Artwork);
            setFocus({Focus::Artwork, id, {}});
        });
        lx += gfx_.measureWidth(L"Show in the artwork library", Font::Small) + 18;
    }
    linkText("pic:remove", lx, y, L"Remove from the text", [this] {
        if (pic_.pos >= 0 && editor_.ready()) editor_.send(json{{"type", "removePicture"}, {"pos", pic_.pos}}.dump());
    });
    y += 34;
}

// ------------------------------------------------------------ artwork page

float StudioApp::pageArtwork(const Rect& r) {
    float y = r.y;
    const auto all = lib_->liveArtworks();
    y += pageHeader(r, L"AUTHOR LIBRARY", L"Artwork",
                    all.empty() ? L"Illustrations, maps, ornaments and photographs for your books. Each picture is stored once, "
                                  L"however many chapters use it."
                                : std::to_wstring(all.size()) + (all.size() == 1 ? L" picture." : L" pictures.") +
                                      L" Each is stored once, however many chapters use it.");
    float bw = 0;
    smallButton("art:add", r.x, y, L"Add pictures...", [this] { addArtworkFiles(); }, &bw);
    y += 46;
    if (all.empty()) {
        y += gfx_.text(L"Pictures you put in a chapter, or that come with an imported book, appear here too. "
                       L"PNG, JPEG, GIF, WebP and BMP files can be used.",
                       {r.x, y, std::min(r.w, 640.0f), 60}, Font::Body, pal_.inkMuted);
        return y - r.y;
    }
    const float cw = 168, gap = 22, thumbH = 150;
    const int perRow = std::max(1, (int)((r.w + gap) / (cw + gap)));
    const float rowH = thumbH + 64;
    const double tw = textWidthInches({});
    for (size_t i = 0; i < all.size(); ++i) {
        const Artwork& a = *all[i];
        const Rect card{r.x + (i % perRow) * (cw + gap), y + (i / perRow) * (rowH + 18), cw, rowH};
        const std::string id = "art:" + a.id;
        addHit(id, card, [this, aid = a.id] { setFocus({Focus::Artwork, aid, {}}); });
        const bool sel = focus_.kind == Focus::Artwork && focus_.id == a.id;
        const float hv = hoverAmount(id);
        const Rect th{card.x, card.y - 2 * hv, cw, thumbH};
        softShadow(th, 3, 0.25f + 0.6f * hv);
        gfx_.fillRounded(th, 3, pal_.sunken);
        gfx_.drawImage(w(lib_->blobPath(a.blob)), fitImage(th.inset(6), a.width, a.height), true);
        if (sel) gfx_.strokeRounded(th.inset(-3), 4, pal_.ribbon, 2);
        float ty = card.y + thumbH + 8;
        gfx_.text(w(a.name), {card.x, ty, cw, 20}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
        ty += 20;
        const PrintQuality q = printQuality(printSize(a.width, a.height, {}, tw).ppi);
        gfx_.circle(card.x + 3, ty + 9, 3, qualityColor(q), true);
        gfx_.text(std::to_wstring(a.width) + L" × " + std::to_wstring(a.height) + L" px · " + w(label(artworkKindFrom(a.kind))),
                  {card.x + 11, ty, cw - 11, 18}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
        if (focused(id)) focusRing(th);
    }
    const size_t rows = (all.size() + perRow - 1) / perRow;
    y += rows * (rowH + 18);
    y += gfx_.text(L"The dot shows how each picture would print across the full width of a 6 × 9 in book's text: green sharp, "
                   L"amber soft, red blurred. Printed smaller, a picture is sharper; select one to see how large it can go.",
                   {r.x, y + 6, std::min(r.w, 700.0f), 60}, Font::Small, pal_.inkFaint);
    return y - r.y + 20;
}

void StudioApp::drawArtworkPanel(const Rect& c, float& y) {
    const Artwork* a = lib_->artwork(focus_.id);
    if (!a) return;
    panelHeading(c, y, L"ARTWORK", true);
    const Rect area{c.x, y, c.w, std::min(200.0f, c.w * 0.75f)};
    gfx_.fillRounded(area, 3, pal_.sunken);
    if (!gfx_.drawImage(w(lib_->blobPath(a->blob)), fitImage(area.inset(8), a->width, a->height), true))
        gfx_.text(L"The picture's file is missing", area, Font::Small, pal_.inkFaint, {Align::Center, true, false, true});
    y = area.bottom() + 14;
    y = panelField(c, y, L"Name", FName);
    panelChoice("panel:artkind", c, y, L"Kind", w(label(artworkKindFrom(a->kind))), [this] {
        HMENU m = CreatePopupMenu();
        const Artwork* x = lib_->artwork(focus_.id);
        for (int k = 0; k <= (int)ArtworkKind::Other; ++k)
            AppendMenuW(m, MF_STRING | (x && artworkKindFrom(x->kind) == (ArtworkKind)k ? MF_CHECKED : 0), 1 + k, w(label((ArtworkKind)k)).c_str());
        POINT pt;
        GetCursorPos(&pt);
        const int cmd = TrackPopupMenu(m, TPM_RETURNCMD, pt.x, pt.y, 0, hwnd(), nullptr);
        DestroyMenu(m);
        if (cmd > 0 && lib_->artwork(focus_.id)) {
            commitFields();
            Artwork u = *lib_->artwork(focus_.id);
            u.kind = toString((ArtworkKind)(cmd - 1));
            lib_->updateArtwork(u);
        }
    });
    y = panelField(c, y, L"Notes", FNotes, 90);

    panelHeading(c, y, L"SIZE", false);
    auto row = [&](const std::wstring& k, const std::wstring& v) {
        gfx_.text(k, {c.x, y, c.w * 0.45f, 20}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
        gfx_.text(v, {c.x + c.w * 0.45f, y, c.w * 0.55f, 20}, Font::Small, pal_.ink, {Align::Right, false, true});
        y += 22;
    };
    row(L"Pixels", std::to_wstring(a->width) + L" × " + std::to_wstring(a->height) + L" (" + w(a->format) + L")");
    row(L"Sharp in print up to", length(a->width / 300.0) + L" × " + length(a->height / 300.0));
    if (!a->source.empty()) row(L"From", w(a->source));
    y += 6;

    panelHeading(c, y, L"USED IN", false);
    const auto used = lib_->artworkUsage(a->blob);
    if (used.empty()) y += gfx_.text(L"Not in any chapter yet.", {c.x, y, c.w, 20}, Font::Small, pal_.inkFaint) + 6;
    for (auto* n : used) {
        const Book* b = lib_->book(n->bookId);
        const std::wstring t = (b ? w(b->title) + L" › " : L"") + nodeLabel(*n);
        linkText("art:use:" + n->id, c.x, y, t, [this, nid = n->id, bid = n->bookId] {
            commitFields();
            writingBook_ = bid;
            currentNode_ = nid;
            openWriting(bid);
        });
        y += 24;
    }
    y += 6;
    const bool canInsert = !writingBook_.empty() && !currentNode_.empty() && lib_->book(writingBook_) && editor_.ready();
    if (canInsert) {
        const ManuscriptNode* n = lib_->node(currentNode_);
        button("art:insert", {c.x, y, c.w, 34}, L"Insert in “" + (n ? nodeLabel(*n) : L"the chapter") + L"”",
               [this, id = a->id] { insertArtwork(id); }, true);
        y += 46;
    }
}

void StudioApp::popupTrimMenu() {
    const Book* b = focus_.kind == Focus::Book ? lib_->book(focus_.id) : nullptr;
    if (!b) return;
    HMENU m = CreatePopupMenu();
    const auto& sizes = trimSizes();
    const std::string current = trimSize(b->trim).id;
    for (size_t i = 0; i < sizes.size(); ++i)
        AppendMenuW(m, MF_STRING | (current == sizes[i].id ? MF_CHECKED : 0), 1 + (UINT)i, w(sizes[i].label).c_str());
    POINT pt;
    GetCursorPos(&pt);
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD, pt.x, pt.y, 0, hwnd(), nullptr);
    DestroyMenu(m);
    if (cmd > 0 && lib_->book(focus_.id)) {
        commitFields();
        Book u = *lib_->book(focus_.id);
        u.trim = sizes[cmd - 1].id;
        if (lib_->updateBook(u)) flash(L"Trim size: " + w(sizes[cmd - 1].label) + L". Picture sizes and print checks use it.", 5);
    }
}

}  // namespace nrs
