// Chapter header pictures (0.89): choosing the picture or ornament a chapter or part opens with, the book's defaults
// for chapters and parts (Typesetting's design card), a chapter's or part's own (Planning's details), and a viewpoint
// character's (⋯ on their page). Kept by studio::headers; the typesetter, ebook, review copy and Plans page draw them.
#include "studio_app.hpp"
#include "cjui/dialog.hpp"
#include "cjui/menu.hpp"

#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace nrs {

using namespace studio;

static std::wstring w(const std::string& s) { return widen(s); }

// A picture from the Artwork library (newest first; the menu searches when long) or from a file, which is added to it.
std::string StudioApp::pickPicture(const std::wstring& title) {
    std::vector<const Artwork*> arts;
    for (const Artwork& a : lib_->artworks())
        if (!a.deleted && !a.blob.empty()) arts.push_back(&a);
    std::sort(arts.begin(), arts.end(), [](const Artwork* a, const Artwork* b) { return a->created > b->created; });
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"From a file…");
    if (!arts.empty()) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < arts.size() && i < 400; ++i) {
        std::wstring name = w(arts[i]->name.empty() ? std::string("Picture") : arts[i]->name);
        if (arts[i]->width > 0) name += L"  (" + std::to_wstring(arts[i]->width) + L" × " + std::to_wstring(arts[i]->height) + L")";
        AppendMenuW(m, MF_STRING, 2 + (UINT)i, name.c_str());
    }
    POINT pt;
    GetCursorPos(&pt);
    const int cmd = (int)cjui::trackMenu(m, pt, hwnd());
    DestroyMenu(m);
    if (cmd >= 2) return arts[(size_t)cmd - 2]->blob;
    if (cmd != 1) return {};
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return {};
    COMDLG_FILTERSPEC spec[] = {{L"Pictures (PNG, JPEG, GIF, WebP, BMP)", L"*.png;*.jpg;*.jpeg;*.gif;*.webp;*.bmp"}};
    dlg->SetFileTypes(1, spec);
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(title.c_str());
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
    if (path.empty()) return {};
    std::ifstream f(path.c_str(), std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.empty() || bytes.size() > 80 * 1024 * 1024) { flash(L"That picture couldn't be read.", 5); return {}; }
    std::wstring name = path.substr(path.find_last_of(L"\\/") + 1);
    if (const size_t dot = name.rfind(L'.'); dot != std::wstring::npos) name.resize(dot);
    const Id aid = lib_->addArtwork(bytes, narrow(name), "Chapter headers");
    const Artwork* a = aid.empty() ? nullptr : lib_->artwork(aid);
    if (!a) { flash(L"That file isn't a picture Writing Studio can read (PNG, JPEG, GIF, WebP or BMP).", 6); return {}; }
    return a->blob;
}

// The chooser: a preview of how the opening will look, then the picture or symbol, its size, where the title sits and
// (over it) how it reads. `own` is what's chosen here; `fallback`, when given, is what applies without it (a chapter's
// viewpoint's or the book's), shown faintly with what it is.
void StudioApp::headerChooser(const std::string& id, const Rect& c, float& y, const headers::Header& own, bool forNode,
                              const std::optional<headers::Resolved>& fallback, const std::wstring& title,
                              std::function<void(const headers::Header&)> save) {
    const bool chosen = own.none || !own.empty();
    const headers::Header shown = chosen ? own : fallback ? fallback->header : headers::Header{};
    const bool showing = !shown.none && !shown.empty();
    // The preview: a little page top.
    const Rect pv{c.x, y, c.w, 96};
    gfx_.fillRounded(pv, 8, pal_.dark ? pal_.sunken : hexColor("#FBF8F1"));
    gfx_.strokeRounded(pv, 8, pal_.rule);
    {
        const Rect in = pv.inset(12);
        const std::wstring t = title.empty() ? std::wstring(L"Chapter title") : title;
        auto titleAt = [&](float ty, cjui::Color ink) {
            gfx_.text(t, {in.x, ty, in.w, 20}, Font::BodyStrong, ink, {Align::Center, false, true, true});
        };
        if (!showing) {
            titleAt(in.y + in.h / 2 - 10, pal_.ink);
            if (shown.none) gfx_.text(L"No header", {in.x, in.bottom() - 14, in.w, 14}, Font::Small, pal_.inkFaint, {Align::Center, false});
        } else if (shown.blob.empty()) {
            const float s = 30;
            const float oy = shown.title == "above" ? in.bottom() - s : in.y;
            drawSymbol(shown.symbol, {in.x + (in.w - s) / 2, oy, s, s}, pal_.ink, cjui::Icon::Star);
            titleAt(shown.title == "above" ? in.y : in.bottom() - 20, pal_.ink);
        } else {
            const std::wstring path = w(lib_->blobPath(shown.blob));
            float ph = shown.size == "ornament" ? 34.0f : shown.title == "over" ? in.h : shown.size == "band" ? std::min(in.h - 26, in.w / 3.2f) : in.h - 26;
            float pw = shown.size == "ornament" ? 34.0f * 1.4f : shown.size == "band" || shown.title == "over" ? in.w : std::min(in.w, ph * 1.6f);
            const float px = in.x + (in.w - pw) / 2;
            const float py = shown.title == "over" ? in.y : shown.title == "above" ? in.bottom() - ph : in.y;
            const Rect pr{px, py, pw, ph};
            if (!gfx_.drawImageRounded(path, pr, 4)) gfx_.fillRounded(pr, 4, pal_.rule);
            if (shown.title == "over") {
                const bool dark = shown.tone == "dark";
                gfx_.fillRounded(pr, 4, dark ? cjui::Color{0, 0, 0, 0.58f} : pal_.paper.withAlpha(0.7f));
                titleAt(pr.y + pr.h / 2 - 10, dark ? cjui::Color{1, 1, 1, 1} : pal_.ink);
            } else titleAt(shown.title == "above" ? in.y : in.bottom() - 20, pal_.ink);
        }
    }
    y += pv.h + 6;
    // Where it comes from, when it isn't chosen here.
    if (!chosen) {
        std::wstring from;
        if (fallback) {
            if (fallback->from == "viewpoint") {
                const Entry* v = lib_->entry(fallback->viewpoint);
                from = (v ? w(v->name) : std::wstring(L"The viewpoint")) + (fallback->header.blob.empty() ? L"'s symbol, from the viewpoint" : L"'s header picture, from the viewpoint");
            } else if (fallback->from == "chapters") from = L"The book's header for chapters (Typesetting)";
            else if (fallback->from == "parts") from = L"The book's header for parts (Typesetting)";
        } else from = forNode ? L"No header: none chosen here or for the book" : L"None yet";
        y += gfx_.text(from, {c.x, y, c.w, 40}, Font::Small, pal_.inkFaint, {Align::Left, true}) + 8;
    }
    // Picture, symbol, and taking them off.
    {
        float bx = c.x;
        auto small = [&](const std::string& bid, const std::wstring& label, std::function<void()> act) {
            const float bw = buttonWidth(label, Font::Small) + 4;
            if (bx + bw > c.right() + 1 && bx > c.x) { bx = c.x; y += 36; }
            button(bid, {bx, y, bw, 30}, label, std::move(act));
            bx += bw + 8;
        };
        small(id + ":pic", own.blob.empty() ? L"Choose a picture…" : L"Change the picture…", [this, own, save, title] {
            const std::string blob = pickPicture(L"A header picture");
            if (blob.empty()) return;
            headers::Header h = own;
            h.none = false;
            h.blob = blob;
            h.symbol.clear();
            if (h.size == "ornament" && own.blob.empty() && !own.symbol.empty()) h.size = "band";
            save(h);
        });
        small(id + ":sym", L"Symbol…", [this, own, save] {
            chooseSymbol(own.symbol, L"No symbol", [own, save](const std::string& s) {
                headers::Header h = own;
                h.none = false;
                h.symbol = s;
                if (!s.empty()) { h.blob.clear(); h.size = "ornament"; if (h.title == "over") h.title = "below"; }
                save(h);
            }, false);
        });
        if (!own.blob.empty())
            small(id + ":alt", own.alt.empty() ? L"Describe it…" : L"Change the description…", [this, own, save] {
                std::wstring v = w(own.alt);
                if (!cjui::askText(hwnd(), L"Describe the header picture",
                                   L"For readers of the ebook who can't see it. Leave it empty if it's only decoration: readers then skip it.", v, L"Keep"))
                    return;
                headers::Header h = own;
                h.alt = narrow(v);
                while (!h.alt.empty() && h.alt.back() == ' ') h.alt.pop_back();
                save(h);
            });
        if (chosen) small(id + ":clear", forNode ? L"Use the default" : L"Remove", [save] { save(headers::Header{}); });
        if (forNode && !own.none) small(id + ":none", L"No header here", [save] { headers::Header h; h.none = true; save(h); });
        y += 42;
    }
    if (!chosen || own.none) return;
    if (!own.blob.empty()) {
        y += gfx_.text(own.alt.empty() ? L"Not described: the ebook treats it as decoration, and readers skip it."
                                       : L"Described as \u201C" + w(own.alt) + L"\u201D",
                       {c.x, y - 4, c.w, 40}, Font::Small, pal_.inkFaint) + 8;
    }
    // How it sits.
    auto label = [&](const std::wstring& t) {
        gfx_.text(t, {c.x, y, c.w, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
        y += 22;
    };
    if (!own.blob.empty()) {
        label(L"Size");
        segmented(id + ":size", {c.x, y, c.w, 28}, {L"Full width", L"Band", L"Ornament"},
                  own.size == "full" ? 0 : own.size == "ornament" ? 2 : 1, [own, save](int i) {
                      headers::Header h = own;
                      h.size = i == 0 ? "full" : i == 2 ? "ornament" : "band";
                      save(h);
                  });
        y += 38;
    }
    label(L"The title");
    if (own.blob.empty()) {
        segmented(id + ":title", {c.x, y, c.w, 28}, {L"Under it", L"Above it"}, own.title == "above" ? 1 : 0, [own, save](int i) {
            headers::Header h = own;
            h.title = i == 1 ? "above" : "below";
            save(h);
        });
        y += 38;
        return;
    }
    segmented(id + ":title", {c.x, y, c.w, 28}, {L"Under it", L"Above it", L"Over it"},
              own.title == "above" ? 1 : own.title == "over" ? 2 : 0, [own, save](int i) {
                  headers::Header h = own;
                  h.title = i == 1 ? "above" : i == 2 ? "over" : "below";
                  save(h);
              });
    y += 38;
    if (own.title == "over") {
        label(L"Over the picture");
        segmented(id + ":tone", {c.x, y, c.w, 28}, {L"Light title, darkened", L"Ink title, faded"}, own.tone == "fade" ? 1 : 0, [own, save](int i) {
            headers::Header h = own;
            h.tone = i == 1 ? "fade" : "dark";
            save(h);
        });
        y += 38;
    }
}

// A viewpoint character's own header picture: ⋯ on their page.
void StudioApp::chooseViewpointHeader(const Id& entry) {
    const Entry* e = lib_->entry(entry);
    if (!e) return;
    const std::string blob = pickPicture(L"A chapter header picture for " + w(e->name));
    if (blob.empty()) return;
    const Entry before = *lib_->entry(entry);
    Entry u = before;
    u.headerBlob = blob;
    lib_->updateEntry(u);
    flashUndo(L"Chapters seen through " + w(u.name) + L"'s eyes now open with this picture, unless they choose their own.",
              [this, before] { lib_->updateEntry(before); invalidate(); }, 8);
    invalidate();
}

// ------------------------------------------------- the opening over the text in Writing (0.92)

nlohmann::json StudioApp::chapterHeadJson(const ManuscriptNode& n) {
    nlohmann::json j = nlohmann::json::object();
    const headers::BookHeaders hb = headers::read(*lib_, n.bookId);
    if (const auto h = headers::resolve(*lib_, n, hb)) {
        nlohmann::json hj = {{"size", h->header.size}, {"title", h->header.title}, {"tone", h->header.tone}};
        if (!h->header.blob.empty()) { hj["blob"] = h->header.blob; hj["alt"] = h->header.alt; }
        else if (headers::printableSymbol(h->header.symbol)) hj["symbol"] = h->header.symbol;
        if (hj.contains("blob") || hj.contains("symbol")) j["header"] = hj;
    }
    if (n.kind == NodeKind::Part) return j;
    // Its date, written as the book's Dates setting says; a scene's only when it differs from its chapter's.
    const auto times = storyTimes(n.bookId);
    auto textOf = [&](const Id& id) -> std::string {
        auto it = times.find(id);
        if (it == times.end() || !it->second.placed) return {};
        std::string t;
        const cal::Calendar cc = cal::calendarOfBook(*lib_, n.bookId);
        if (timelineInCalendar(n.bookId) && !cc.empty()) {
            const typeset::Design d = typeset::designFromJson(lib_->setting("typeset:" + n.bookId));
            const std::int64_t day = cal::storySpan(*lib_, n.bookId).start + (std::int64_t)std::floor(it->second.day);
            t = cal::formatDateStyled(cc, cal::fromDay(cc, day), d.dateForm, d.dateEra);
        } else t = narrow(storyDayText(n.bookId, it->second.day));
        if (it->second.minutes >= 0 || !it->second.part.empty())
            t += ", " + cal::timeOfDayText(cc.empty() ? cal::gregorian() : cc, {it->second.minutes, it->second.part});
        return t;
    };
    const std::string date = textOf(n.id);
    if (n.kind == NodeKind::Scene) {
        if (!date.empty() && date != textOf(n.parentId)) j["date"] = date;
    } else {
        if (!date.empty()) j["date"] = date;
        else j["dated"] = false;
    }
    // Whose eyes it's seen through: their portrait, else their symbol, else initials, ringed in their colour.
    Id pov = n.povId;
    if (pov.empty() && n.kind == NodeKind::Chapter)
        for (const ManuscriptNode* s : lib_->manuscriptOf(n.bookId))
            if (s->parentId == n.id && !s->povId.empty()) { pov = s->povId; break; }
    if (const Entry* e = pov.empty() ? nullptr : lib_->entry(pov); e && !e->deleted) {
        // The same icon as the bible's (4b, 0.102): its picture with its symbol as a badge, or its symbol or initials
        // on its colour.
        nlohmann::json pj = iconJson(*e);
        pj["id"] = e->id;
        pj["name"] = e->name;
        j["pov"] = pj;
    }
    return j;
}

void StudioApp::sendHead(bool always) {
    if (!editor_.ready() || currentNode_.empty()) return;
    const ManuscriptNode* n = lib_->node(currentNode_);
    if (!n) return;
    const std::string now = chapterHeadJson(*n).dump();
    if (!always && now == lastHead_) return;
    lastHead_ = now;
    editor_.send(nlohmann::json{{"type", "head"}, {"head", nlohmann::json::parse(now)}}.dump());
}

// The chapter's header picture, from Writing's Chapter tools and the text's right-click menu.
std::vector<StudioApp::ToolItem> StudioApp::headerTools(const Id& node) {
    std::vector<ToolItem> t;
    const ManuscriptNode* n = lib_->node(node);
    if (!n) return t;
    const headers::BookHeaders hb = headers::read(*lib_, n->bookId);
    const auto own = hb.nodes.find(node);
    const bool chosen = own != hb.nodes.end();
    auto save = [this, node, book = n->bookId](std::function<void(headers::Header&)> change) {
        headers::BookHeaders b = headers::read(*lib_, book);
        headers::Header h = b.nodes.count(node) ? b.nodes[node] : headers::Header{};
        h.none = false;
        change(h);
        if (h.none || !h.empty()) b.nodes[node] = h;
        else b.nodes.erase(node);
        headers::write(*lib_, book, b);
        sendHead(true);
        invalidate();
    };
    t.push_back(ToolItem::item(chosen && !own->second.blob.empty() ? L"Change the picture…" : L"Choose header picture…", [this, save] {
        const std::string blob = pickPicture(L"A header picture");
        if (blob.empty()) return;
        save([blob](headers::Header& h) { h.blob = blob; h.symbol.clear(); if (h.size == "ornament") h.size = "band"; });
    }));
    t.push_back(ToolItem::item(L"An ornament or symbol…", [this, save, cur = chosen ? own->second.symbol : std::string()] {
        chooseSymbol(cur, L"No symbol", [save](const std::string& s) {
            save([s](headers::Header& h) { h.symbol = s; if (!s.empty()) { h.blob.clear(); h.size = "ornament"; if (h.title == "over") h.title = "below"; } });
        }, false);
    }));
    if (chosen) t.push_back(ToolItem::item(L"Remove (use the default)", [this, node, book = n->bookId] {
        headers::BookHeaders b = headers::read(*lib_, book);
        b.nodes.erase(node);
        headers::write(*lib_, book, b);
        sendHead(true);
    }));
    if (!(chosen && own->second.none))
        t.push_back(ToolItem::item(L"No header for this one", [save](void) { save([](headers::Header& h) { h = headers::Header{}; h.none = true; }); }));
    t.push_back(ToolItem::line());
    t.push_back(ToolItem::item(L"Size, title and the rest…", [this, bid = n->bookId, node] {
        openPlanning(bid);
        pl_.view = 0;
        openPlanPage(node);
    }));
    return t;
}

// The date over the text clicked: the chapter's details, with its date ready to change.
void StudioApp::editWhenInWriting() {
    if (currentNode_.empty()) return;
    setFocus({Focus::Node, currentNode_, {}});
    settings_.writingDetails = true;
    saveSettings();
    focusFieldSoon_ = FStoryDay;
    invalidate();
}

}  // namespace nrs
