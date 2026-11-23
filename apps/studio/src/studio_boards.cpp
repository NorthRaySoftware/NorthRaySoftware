// Image boards: reference pictures, character art and mood images for a world
// (or an independent book), arranged freely like pins on a board. Drag a
// picture to move it, its corner to resize it; each can have a caption and be
// tied to a bible entry. Pictures are stored once in the library (and listed in
// Artwork); a board keeps only where they sit, as setting
// "boards:world:<id>" or "boards:book:<id>":
// {"boards": [{"id", "name", "items": [{"id", "blob", "x", "y", "w", "caption", "entry"}]}]}.
#include "studio_app.hpp"
#include "cjui/dialog.hpp"
#include "cjui/menu.hpp"

#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace nrs {

using nlohmann::json;
using namespace studio;

static std::wstring w(const std::string& s) { return widen(s); }

static std::string newBoardId() {
    static unsigned n = 0;
    char buf[40];
    std::snprintf(buf, sizeof buf, "%08lx%04x", (unsigned long)GetTickCount64(), (++n) & 0xffff);
    return buf;
}

json StudioApp::boardsData() const {
    const std::string key = !wb_.world.empty() ? "boards:world:" + wb_.world : "boards:book:" + wb_.book;
    json j = lib_ ? json::parse(lib_->setting(key), nullptr, false) : json();
    if (!j.is_object()) j = json::object();
    if (!j.contains("boards") || !j["boards"].is_array()) j["boards"] = json::array();
    for (auto& b : j["boards"]) if (!b.contains("items") || !b["items"].is_array()) b["items"] = json::array();
    return j;
}

void StudioApp::saveBoards(const json& j) {
    const std::string key = !wb_.world.empty() ? "boards:world:" + wb_.world : "boards:book:" + wb_.book;
    if (!lib_->setSetting(key, j.dump())) flash(w("Couldn't save the board: " + lib_->lastError()), 8);
}

void StudioApp::newBoard() {
    commitFields();
    json j = boardsData();
    const std::string id = newBoardId();
    j["boards"].push_back({{"id", id}, {"name", "New board"}, {"items", json::array()}});
    saveBoards(j);
    wb_.board = id;
    wb_.item.clear();
    wb_.entry.clear();
    wb_.scroll = 0;
    wb_.panelScroll = 0;
    setFocus({Focus::Board, id, {}});
    invalidate();
}

// Pictures from files: stored in the library (listed in Artwork too) and laid out in a row below what's there.
void StudioApp::addBoardPictures() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    COMDLG_FILTERSPEC spec[] = {{L"Pictures", L"*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp"}};
    dlg->SetFileTypes(1, spec);
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Add pictures to the board");
    std::vector<std::wstring> paths;
    if (SUCCEEDED(dlg->Show(hwnd()))) {
        IShellItemArray* arr = nullptr;
        if (SUCCEEDED(dlg->GetResults(&arr))) {
            DWORD n = 0;
            arr->GetCount(&n);
            for (DWORD i = 0; i < n && i < 200; ++i) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(arr->GetItemAt(i, &item))) {
                    PWSTR p = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) { paths.push_back(p); CoTaskMemFree(p); }
                    item->Release();
                }
            }
            arr->Release();
        }
    }
    dlg->Release();
    if (paths.empty()) return;
    commitFields();
    json j = boardsData();
    json* board = nullptr;
    for (auto& b : j["boards"]) if (b.value("id", "") == wb_.board) board = &b;
    if (!board) return;
    // Below everything already there.
    float bottom = 0;
    for (auto& it : (*board)["items"]) {
        const Artwork* a = lib_->artworkForBlob(it.value("blob", ""));
        const float iw = it.value("w", 220.0f);
        const float ih = a && a->width > 0 ? iw * a->height / (float)a->width : iw;
        bottom = std::max(bottom, it.value("y", 0.0f) + ih + 40);
    }
    const float tile = 220, gap = 24;
    const int perRow = std::max(1, (int)((std::max(pageRect_.w - 80, tile)) / (tile + gap)));
    int added = 0, failed = 0;
    float rowH = 0;
    std::string lastId;
    for (const std::wstring& p : paths) {
        std::ifstream f(p.c_str(), std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (bytes.empty() || bytes.size() > 80 * 1024 * 1024) { ++failed; continue; }
        std::wstring name = p.substr(p.find_last_of(L"\\/") + 1);
        if (const size_t dot = name.rfind(L'.'); dot != std::wstring::npos) name.resize(dot);
        const Id aid = lib_->addArtwork(bytes, narrow(name), "Image board");
        const Artwork* a = aid.empty() ? nullptr : lib_->artwork(aid);
        if (!a) { ++failed; continue; }
        const int col = added % perRow;
        if (col == 0 && added) { bottom += rowH + gap; rowH = 0; }
        const float ih = a->width > 0 ? tile * a->height / (float)a->width : tile;
        rowH = std::max(rowH, ih + 30);
        lastId = newBoardId();
        (*board)["items"].push_back({{"id", lastId}, {"blob", a->blob}, {"x", col * (tile + gap)}, {"y", bottom}, {"w", tile}, {"caption", ""}, {"entry", ""}});
        ++added;
    }
    saveBoards(j);
    if (!lastId.empty()) { wb_.item = lastId; setFocus({Focus::BoardItem, lastId, wb_.board}); }
    flash(std::to_wstring(added) + (added == 1 ? L" picture added." : L" pictures added.") +
              (failed ? L" " + std::to_wstring(failed) + L" couldn't be read." : L""), 5);
    invalidate();
}

// A board of an entry's own, opened straight away.
void StudioApp::newEntryBoard() {
    const Entry* e = lib_->entry(wb_.entry);
    if (!e) return;
    commitFields();
    json j = boardsData();
    const std::string id = newBoardId();
    const Id eid = e->id;
    j["boards"].push_back({{"id", id}, {"name", e->name + " board"}, {"entry", eid}, {"items", json::array()}});
    saveBoards(j);
    wb_.board = id;
    wb_.item.clear();
    wb_.boardEntry = eid;
    wb_.entry.clear();
    wb_.scroll = 0;
    wb_.panelScroll = 0;
    setFocus({Focus::Board, id, {}});
    invalidate();
}

// Which entry's page a board is kept in (or none: the bible's own boards in the sidebar).
void StudioApp::moveBoardToEntry(const std::string& boardId) {
    auto all = lib_->entriesOf(wb_.world, wb_.book);
    std::vector<cjui::MenuItem> menu{cjui::MenuItem::item(L"The bible (its own board)", 1), cjui::MenuItem::sep()};
    for (size_t i = 0; i < all.size() && i < 300; ++i)
        menu.push_back(cjui::MenuItem::item(w(all[i]->name) + L"  (" + w(entryKindLabel(all[i]->kind)) + L")", (UINT)(10 + i)));
    POINT pt;
    GetCursorPos(&pt);
    const UINT ch = cjui::showMenu(hwnd(), menu, pt, pal_);
    if (!ch) return;
    commitFields();
    json j = boardsData();
    for (auto& b : j["boards"])
        if (b.value("id", "") == boardId) {
            if (ch == 1) b.erase("entry"); else b["entry"] = all[ch - 10]->id;
        }
    saveBoards(j);
    flash(ch == 1 ? std::wstring(L"The board is listed in the sidebar again.") : L"The board is now kept on " + w(all[ch - 10]->name) + L"'s page.", 5);
    invalidate();
}

// ----------------------------------------------------------------- drawing

void StudioApp::drawBoard(const Rect& r) {
    json j = boardsData();
    const json* board = nullptr;
    for (auto& b : j["boards"]) if (b.value("id", "") == wb_.board) board = &b;
    if (!board) { wb_.board.clear(); invalidate(); return; }
    gfx_.pushClip(r);
    setHitClip(r);
    const float pad = r.w > 900 ? 36 : 24;
    const float cw = r.w - pad * 2;
    const Rect m{r.x + pad, r.y + 26 - wb_.scroll, cw, 0};
    float y = m.y;
    {
        // A board of an entry's goes back to its page.
        const Entry* owner = lib_->entry(board->value("entry", ""));
        if (owner && owner->deleted) owner = nullptr;
        linkText("wb:boardback", m.x, y, owner ? L"‹ " + w(owner->name) : std::wstring(L"‹ The bible"), [this, oid = owner ? owner->id : Id{}] {
            commitFields();
            wb_.board.clear();
            wb_.item.clear();
            wb_.boardEntry.clear();
            if (!oid.empty()) openEntry(oid); else setFocus({});
            invalidate();
        });
    }
    y += 30;
    y += pageHeader({m.x, y, m.w, 0}, L"IMAGE BOARD", w(board->value("name", "Board")),
                    L"Drag pictures to arrange them, and their corner to resize them. Click one to caption it or tie it to an entry.");
    button("wb:boardadd", {m.x, y - 6, 170, 32}, L"+ Add pictures…", [this] { addBoardPictures(); }, true);
    y += 44;
    const float ox = m.x, oy = y;
    wb_.boardOriginX = ox;
    wb_.boardOriginY = oy;
    const auto& items = (*board)["items"];
    if (items.empty())
        gfx_.text(L"No pictures yet. Add character art, maps, places, costumes, colour palettes: anything that helps you see the story.",
                  {m.x, y, std::min(cw, 620.0f), 60}, Font::Body, pal_.inkMuted);
    float bottom = y + 60;
    for (auto& it : items) {
        const std::string iid = it.value("id", ""), blob = it.value("blob", "");
        float ix = it.value("x", 0.0f), iy = it.value("y", 0.0f), iw = it.value("w", 220.0f);
        if (auto p = wb_.boardDragPos.find(iid); p != wb_.boardDragPos.end()) { ix = p->second.first; iy = p->second.second; }
        if (wb_.boardDrag.active && wb_.boardDrag.resize && wb_.boardDrag.item == iid) iw = wb_.boardDragW;
        ix = std::clamp(ix, 0.0f, std::max(0.0f, cw - std::min(iw, cw)));
        iw = std::min(iw, cw);
        const std::wstring path = w(lib_->blobPath(blob));
        const auto [pw, ph] = gfx_.imageSize(path);
        const float ih = pw > 0 ? iw * ph / (float)pw : iw * 0.75f;
        const Rect pic{ox + ix, oy + iy, iw, ih};
        const std::string hid = "wb:bi:" + iid;
        addHit(hid, pic, [this, iid] { commitFields(); wb_.item = iid; setFocus({Focus::BoardItem, iid, wb_.board}); invalidate(); });
        const bool sel = wb_.item == iid;
        const float hv = hoverAmount(hid);
        softShadow(pic, 4, std::max(0.4f + 0.6f * hv, sel ? 1.0f : 0.0f));
        if (!gfx_.drawImage(path, pic, false)) {
            gfx_.fillRounded(pic, 4, pal_.sunken);
            gfx_.text(L"Picture missing", pic, Font::Small, pal_.inkFaint, {Align::Center, false, false, true});
        }
        gfx_.stroke(pic, sel ? pal_.ribbon : pal_.rule.withAlpha(0.6f), sel ? 2.0f : 1.0f);
        float cy = pic.bottom() + 6;
        const std::string cap = it.value("caption", "");
        if (!cap.empty()) cy += gfx_.text(w(cap), {pic.x, cy, pic.w, 60}, Font::Small, pal_.inkMuted) + 2;
        // Tied to an entry: its name, which opens it.
        if (const Entry* e = lib_->entry(it.value("entry", "")); e && !e->deleted) {
            const std::wstring nm = w(e->name);
            const float tw = std::min(gfx_.measureWidth(nm, Font::Small) + 20, pic.w);
            const Rect tag{pic.x + 6, pic.y + 6, tw, 22};
            const std::string tid = "wb:bt:" + iid;
            addHit(tid, tag, [this, eid = e->id] { commitFields(); wb_.board.clear(); wb_.item.clear(); openEntry(eid); });
            gfx_.fillRounded(tag, 11, pal_.paper.withAlpha(0.92f));
            gfx_.text(nm, tag, Font::Small, pal_.ribbon.mix(pal_.ink, 0.3f * hoverAmount(tid)), {Align::Center, false, true, true});
        }
        if (sel) {
            const Rect handle{pic.right() - 7, pic.bottom() - 7, 14, 14};
            addHit("wb:bir:" + iid, handle.inset(-4), nullptr, false);
            gfx_.fillRounded(handle, 3, pal_.ribbon);
            gfx_.strokeRounded(handle, 3, pal_.paper, 1.5f);
        }
        if (focused(hid)) focusRing(pic);
        bottom = std::max(bottom, cy + 30);
    }
    wb_.content = bottom + wb_.scroll - r.y + 60;
    wb_.scroll = std::clamp(wb_.scroll, 0.0f, std::max(0.0f, wb_.content - r.h));
    clearHitClip();
    gfx_.popClip();
}

void StudioApp::drawBoardPanel(const Rect& r) {
    gfx_.fill(r, pal_.paper.mix(pal_.surface, 0.5f));
    gfx_.line(r.x + 0.5f, r.y, r.x + 0.5f, r.bottom(), pal_.rule);
    if (!(focus_ == loadedFocus_)) loadFields();
    gfx_.pushClip(r);
    setHitClip(r);
    const Rect c{r.x + 20, r.y, r.w - 40, 0};
    float y = r.y + 18 - wb_.panelScroll;
    json j = boardsData();
    const json* board = nullptr;
    for (auto& b : j["boards"]) if (b.value("id", "") == wb_.board) board = &b;
    const json* item = nullptr;
    if (board) for (auto& it : (*board)["items"]) if (it.value("id", "") == wb_.item) item = &it;
    if (item && focus_.kind == Focus::BoardItem) {
        panelHeading(c, y, L"PICTURE", true);
        const std::string blob = item->value("blob", "");
        const Artwork* a = lib_->artworkForBlob(blob);
        if (a) {
            gfx_.text(w(a->name), {c.x, y, c.w, 20}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
            y += 22;
            gfx_.text(std::to_wstring(a->width) + L" × " + std::to_wstring(a->height) + L" pixels", {c.x, y, c.w, 18}, Font::Small,
                      pal_.inkMuted, {Align::Left, false});
            y += 28;
        }
        y = panelField(c, y, L"Caption", FCaption);
        const Entry* e = lib_->entry(item->value("entry", ""));
        if (e && e->deleted) e = nullptr;
        panelChoice("wb:bientry", c, y, L"Tied to", e ? w(e->name) : L"Nothing", [this] {
            const auto all = lib_->entriesOf(wb_.world, wb_.book);
            std::vector<cjui::MenuItem> menu{cjui::MenuItem::item(L"Nothing", 1), cjui::MenuItem::sep()};
            for (size_t i = 0; i < all.size() && i < 300; ++i)
                menu.push_back(cjui::MenuItem::item(w(all[i]->name) + L"  (" + w(entryKindLabel(all[i]->kind)) + L")", (UINT)(10 + i)));
            POINT pt;
            GetCursorPos(&pt);
            const UINT ch = cjui::showMenu(hwnd(), menu, pt, pal_);
            if (!ch) return;
            commitFields();
            json jj = boardsData();
            for (auto& b : jj["boards"])
                if (b.value("id", "") == wb_.board)
                    for (auto& it : b["items"])
                        if (it.value("id", "") == wb_.item) it["entry"] = ch == 1 ? std::string() : all[ch - 10]->id;
            saveBoards(jj);
            invalidate();
        });
        if (e) {
            linkText("wb:bisetpic", c.x, y, L"Use as " + w(e->name) + L"'s picture", [this, eid = e->id, blob] {
                if (const Entry* x = lib_->entry(eid)) {
                    Entry u = *x;
                    u.imageBlob = blob;
                    lib_->updateEntry(u);
                    flash(L"It's now the entry's picture.", 4);
                }
            });
            y += 28;
        }
        panelHeading(c, y, L"MORE", false);
        linkText("wb:biremove", c.x, y, L"Remove from the board", [this] {
            json jj = boardsData();
            for (auto& b : jj["boards"])
                if (b.value("id", "") == wb_.board) {
                    auto& items = b["items"];
                    for (size_t i = 0; i < items.size(); ++i)
                        if (items[i].value("id", "") == wb_.item) { items.erase(items.begin() + (long)i); break; }
                }
            saveBoards(jj);
            wb_.item.clear();
            setFocus({Focus::Board, wb_.board, {}});
            flash(L"Removed from the board. The picture stays in Artwork.", 5);
        });
        y += 30;
    } else if (board) {
        panelHeading(c, y, L"IMAGE BOARD", true);
        y = panelField(c, y, L"Name", FName);
        {
            const Entry* owner = lib_->entry(board->value("entry", ""));
            if (owner && owner->deleted) owner = nullptr;
            panelChoice("wb:bowner", c, y, L"Kept in", owner ? w(owner->name) + L"'s page" : std::wstring(L"The bible (its own board)"),
                        [this] { moveBoardToEntry(wb_.board); });
        }
        y += gfx_.text(std::to_wstring((*board)["items"].size()) + L" pictures. They're kept in the library and listed in Artwork, "
                       L"so removing one from the board doesn't delete it.",
                       {c.x, y, c.w, 80}, Font::Small, pal_.inkMuted) + 14;
        panelHeading(c, y, L"MORE", false);
        linkText("wb:bdelete", c.x, y, L"Delete this board", [this] {
            json jj = boardsData();
            const json before = jj;
            const std::string was = wb_.board;
            auto& boards = jj["boards"];
            for (size_t i = 0; i < boards.size(); ++i)
                if (boards[i].value("id", "") == wb_.board) { boards.erase(boards.begin() + (long)i); break; }
            saveBoards(jj);
            wb_.board.clear();
            wb_.item.clear();
            setFocus({});
            flashUndo(L"Deleted the board. Its pictures stay in Artwork.", [this, before, was] {
                saveBoards(before);
                wb_.board = was;
                invalidate();
            });
        });
        y += 30;
    }
    wb_.panelContent = y + wb_.panelScroll - r.y + 20;
    clearHitClip();
    gfx_.popClip();
}

// --------------------------------------------------------------- the mouse

bool StudioApp::boardMouseDown(float x, float y) {
    if (ws_ != Workspace::Worldbuilding || wb_.board.empty()) return false;
    const std::string id = hitAt(x, y);
    const bool resize = id.rfind("wb:bir:", 0) == 0;
    if (!resize && id.rfind("wb:bi:", 0) != 0) return false;
    const std::string iid = id.substr(resize ? 7 : 6);
    json j = boardsData();
    for (auto& b : j["boards"])
        if (b.value("id", "") == wb_.board)
            for (auto& it : b["items"])
                if (it.value("id", "") == iid) {
                    wb_.boardDrag = {};
                    wb_.boardDrag.armed = true;
                    wb_.boardDrag.resize = resize;
                    wb_.boardDrag.item = iid;
                    wb_.boardDrag.sx = x;
                    wb_.boardDrag.sy = y;
                    wb_.boardDrag.ox = it.value("x", 0.0f);
                    wb_.boardDrag.oy = it.value("y", 0.0f);
                    wb_.boardDrag.ow = it.value("w", 220.0f);
                    wb_.boardDragW = wb_.boardDrag.ow;
                }
    return resize;   // the handle has no click of its own
}

bool StudioApp::boardMouseMove(float x, float y, bool leftDown) {
    auto& d = wb_.boardDrag;
    if (!d.armed) return false;
    if (!leftDown) { d = {}; wb_.boardDragPos.clear(); return false; }
    if (!d.active && std::hypot(x - d.sx, y - d.sy) > 4) { d.active = true; cancelPress(); }
    if (!d.active) return false;
    if (d.resize) wb_.boardDragW = std::clamp(d.ow + (x - d.sx), 60.0f, 1600.0f);
    else wb_.boardDragPos[d.item] = {std::max(0.0f, d.ox + (x - d.sx)), std::max(0.0f, d.oy + (y - d.sy))};
    invalidate();
    return true;
}

bool StudioApp::boardMouseUp(float, float) {
    auto& d = wb_.boardDrag;
    if (!d.armed) return false;
    const bool was = d.active;
    if (was) {
        json j = boardsData();
        for (auto& b : j["boards"])
            if (b.value("id", "") == wb_.board)
                for (auto& it : b["items"])
                    if (it.value("id", "") == d.item) {
                        if (d.resize) it["w"] = std::round(wb_.boardDragW);
                        else if (auto p = wb_.boardDragPos.find(d.item); p != wb_.boardDragPos.end()) {
                            it["x"] = std::round(p->second.first);
                            it["y"] = std::round(p->second.second);
                        }
                    }
        saveBoards(j);
        commitFields();
        wb_.item = d.item;
        setFocus({Focus::BoardItem, d.item, wb_.board});
    }
    d = {};
    wb_.boardDragPos.clear();
    invalidate();
    return was;
}

// 0.60: a picture dragged onto one of the entry's boards goes onto that board, below what's there.
void StudioApp::addPictureToBoard(size_t index, const Id& boardId) {
    const Entry* e = lib_->entry(wb_.entry);
    if (!e || index >= e->gallery.size()) return;
    const std::string blob = e->gallery[index].blob;
    json j = boardsData();
    for (auto& bd : j["boards"]) {
        if (bd.value("id", "") != boardId) continue;
        float bottom = 0;
        for (auto& it : bd["items"]) {
            const Artwork* a = lib_->artworkForBlob(it.value("blob", ""));
            const float iw = it.value("w", 220.0f);
            const float ih = a && a->width > 0 ? iw * a->height / (float)a->width : iw;
            bottom = std::max(bottom, it.value("y", 0.0f) + ih + 40);
        }
        bd["items"].push_back({{"id", newBoardId()}, {"blob", blob}, {"x", 0}, {"y", bottom}, {"w", 220}, {"caption", e->gallery[index].caption}, {"entry", ""}});
        saveBoards(j);
        // it's on the board: no note (0.64)
        invalidate();
        return;
    }
}

}  // namespace nrs
