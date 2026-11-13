// The Writing workspace (manuscript outline, editor, details) and the import preview.
#include "studio_app.hpp"

#include "cjui/format.hpp"
#include "cjui/http.hpp"
#include "studio/document.hpp"

#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <set>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>
#include <cstdio>
#include <filesystem>

namespace nrs {

using nlohmann::json;
using namespace studio;


const UINT_PTR kCloseTimer = 3;



static std::wstring w(const std::string& s) { return widen(s); }

static std::wstring thousands(std::int64_t n) {
    std::wstring s = std::to_wstring(n < 0 ? -n : n), out;
    int c = 0;
    for (auto it = s.rbegin(); it != s.rend(); ++it) {
        if (c && c % 3 == 0) out.insert(out.begin(), L',');
        out.insert(out.begin(), *it);
        ++c;
    }
    return (n < 0 ? L"-" : L"") + out;
}
static std::wstring wordsLabel(std::int64_t n) { return thousands(n) + (n == 1 ? L" word" : L" words"); }

std::wstring StudioApp::exeDir() const {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    std::wstring d(buf, n);
    return d.substr(0, d.find_last_of(L"\\/"));
}

std::wstring StudioApp::nodeLabel(const ManuscriptNode& n) const {
    return n.title.empty() ? w(label(n.kind)) : w(n.title);
}

std::int64_t StudioApp::nodeTotalWords(const Id& id) const {
    const ManuscriptNode* n = lib_->node(id);
    if (!n) return 0;
    std::int64_t t = n->wordCount;
    for (auto* c : lib_->childrenOf(n->bookId, id)) t += nodeTotalWords(c->id);
    return t;
}

// ------------------------------------------------------------------- editor

void StudioApp::ensureEditor() {
    if (editorStarted_ || !lib_) return;
    editorStarted_ = true;
    editorNote_.clear();
    CreateDirectoryW((stateDir() + L"\\WebView2").c_str(), nullptr);
    const std::wstring objects = w(lib_->folder()) + L"\\objects";
    CreateDirectoryW(objects.c_str(), nullptr);
    if (!editor_.create(hwnd(), exeDir(), stateDir() + L"\\WebView2", objects, L"editor.html",
                        [this](const std::string& m) { editorMessage(m); }))
        editorNote_ = editor_.error();
}

void StudioApp::sendLoad() {
    if (!lib_ || currentNode_.empty()) return;
    const ManuscriptNode* n = lib_->node(currentNode_);
    if (!n) return;
    if (!editor_.ready()) { loadPending_ = true; return; }
    loadPending_ = false;
    const Book* b = lib_->book(n->bookId);
    const char* placeholder = n->kind == NodeKind::Scene ? "Start writing this scene."
                            : n->kind == NodeKind::Part ? "A part can open with a page of its own, such as an epigraph. Leave it empty if it doesn't."
                                                         : "Start writing this chapter.";
    json msg = {{"type", "load"}, {"id", n->id}, {"title", n->title}, {"language", b ? b->language : "en-GB"}, {"dark", pal_.dark},
                {"readOnly", n->deleted != 0}, {"focus", loadFocus_}, {"placeholder", placeholder}};
    loadFocus_ = true;
    msg["doc"] = json::parse(lib_->nodeContent(n->id), nullptr, false);
    if (msg["doc"].is_discarded()) msg["doc"] = json::parse(doc::emptyDoc());
    editor_.send(msg.dump());
    sendComments();
    if (!pendingComment_.empty()) {
        editor_.send(json{{"type", "selectComment"}, {"id", pendingComment_}}.dump());
        pendingComment_.clear();
    }
    liveWords_ = n->wordCount;
    selectionWords_ = 0;
}

void StudioApp::editorMessage(const std::string& text) {
    json m = json::parse(text, nullptr, false);
    if (!m.is_object() || !lib_) return;
    const std::string type = m.value("type", "");
    if (type == "ready") {
        editor_.send(json{{"type", "theme"}, {"dark", pal_.dark}}.dump());
        if (loadPending_ || !currentNode_.empty()) sendLoad();
    } else if (type == "change") {
        const std::string id = m.value("id", "");
        if (!m.contains("doc") || !lib_->node(id)) return;
        editorDirty_ = false;
        saveFailed_ = !lib_->saveNodeContent(id, m["doc"].dump());
        editor_.send(json{{"type", "saved"}, {"id", id}, {"ok", !saveFailed_}}.dump());
        if (saveFailed_) flash(w("Couldn't save the text: " + lib_->lastError()), 15);
    } else if (type == "saved-state") {
        editorDirty_ = m.value("dirty", false);
    } else if (type == "navigate") {
        stepNode(m.value("delta", 0) < 0 ? -1 : 1, true, true);
    } else if (type == "flushed") {
        if (closePending_) { KillTimer(hwnd(), kCloseTimer); closePending_ = false; DestroyWindow(hwnd()); return; }
    } else if (type == "stats") {
        if (m.value("id", "") == currentNode_) {
            liveWords_ = m.value("words", (std::int64_t)0);
            selectionWords_ = m.value("selectionWords", (std::int64_t)0);
        }
    } else if (type == "picture") {
        pictureMessage(text);
        return;
    } else if (type == "comment" || type == "commentFocus") {
        commentMessage(text);
        return;
    } else if (type == "link") {
        cjui::openInBrowser(m.value("href", ""));
    } else if (type == "insertImage") {
        // Same picker as covers; the image is stored once in the library by content.
        IFileOpenDialog* dlg = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
        COMDLG_FILTERSPEC types[] = {{L"Pictures (JPEG, PNG, GIF, WebP)", L"*.jpg;*.jpeg;*.png;*.gif;*.webp"}};
        dlg->SetFileTypes(1, types);
        dlg->SetTitle(L"Insert a picture");
        if (SUCCEEDED(dlg->Show(hwnd()))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    std::ifstream f(path, std::ios::binary);
                    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                    std::wstring file = path;
                    file = file.substr(file.find_last_of(L"\\/") + 1);
                    const std::wstring stem = file.substr(0, file.find_last_of(L'.'));
                    if (bytes.size() > 200u * 1024 * 1024) flash(L"That picture is too large (over 200 MB).", 6);
                    else if (!bytes.empty()) {
                        // Into the artwork library, then the text, at the widest size that prints sharply.
                        const Id art = lib_->addArtwork(bytes, narrow(stem), narrow(file));
                        if (art.empty()) flash(w(lib_->lastError()), 8);
                        else insertArtwork(art);
                    }
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
    } else if (type == "failed") {
        editorNote_ = L"The editor stopped working. Switch to the Library and back to restart it; your text up to the last save is safe.";
        editor_.destroy();
        editorStarted_ = false;
    }
    invalidate();
}

void StudioApp::placeEditor() {
    const bool show = ws_ == Workspace::Writing && lib_ && !currentNode_.empty() && editorNote_.empty();
    if (!show) { editor_.hide(); return; }
    const float s = gfx_.dpiScale();
    RECT rc{(LONG)std::lround(pageRect_.x * s), (LONG)std::lround(pageRect_.y * s), (LONG)std::lround(pageRect_.right() * s),
            (LONG)std::lround(pageRect_.bottom() * s)};
    editor_.place(rc);
}

// --------------------------------------------------------------- navigation

void StudioApp::openWriting(const Id& bookId) {
    if (!lib_ || !lib_->book(bookId)) return;
    commitFields();
    ws_ = Workspace::Writing;
    if (writingBook_ != bookId) { binderScroll_ = 0; currentNode_.clear(); }
    writingBook_ = bookId;
    ensureEditor();
    // Something to write in: the last node, else the first with text, else a new first chapter.
    const ManuscriptNode* cur = currentNode_.empty() ? nullptr : lib_->node(currentNode_);
    if (!cur || cur->deleted || cur->bookId != bookId) {
        currentNode_.clear();
        auto all = lib_->manuscriptOf(bookId);
        for (auto* n : all) if (n->kind != NodeKind::Part) { currentNode_ = n->id; break; }
        if (currentNode_.empty() && !all.empty()) currentNode_ = all[0]->id;
        if (currentNode_.empty()) currentNode_ = lib_->createNode(bookId, {}, NodeKind::Chapter);
    }
    selectNode(currentNode_);
}

void StudioApp::selectNode(const Id& id, bool focusEditor) {
    const ManuscriptNode* n = lib_ ? lib_->node(id) : nullptr;
    if (!n) return;
    commitFields();
    if (id != currentNode_ && editor_.ready()) editor_.send(R"({"type":"flush"})");
    loadFocus_ = focusEditor;
    if (id != currentNode_) { pic_ = {}; crop_ = {}; }
    currentNode_ = id;
    writingBook_ = n->bookId;
    setFocus({Focus::Node, id, {}});
    sendLoad();
    invalidate();
}

void StudioApp::stepNode(int delta, bool chaptersOnly, bool focusEditor) {
    if (!lib_ || writingBook_.empty()) return;
    const auto all = lib_->manuscriptOf(writingBook_);
    long i = -1;
    for (size_t k = 0; k < all.size(); ++k) if (all[k]->id == currentNode_) i = (long)k;
    if (i < 0) return;
    for (long j = i + delta; j >= 0 && j < (long)all.size(); j += delta) {
        // Chapters only: parts are skipped unless they have text of their own.
        if (chaptersOnly && all[j]->kind == NodeKind::Part && all[j]->wordCount == 0) continue;
        selectNode(all[j]->id, focusEditor);
        return;
    }
    flash(delta < 0 ? L"This is the first chapter." : L"This is the last chapter.", 3);
    if (focusEditor && editor_.ready()) editor_.send(R"({"type":"focus"})");
}

// A new scene is a *** divider in the chapter's own text, not a separate page.
void StudioApp::addSceneBreak() {
    if (!lib_ || writingBook_.empty()) return;
    const ManuscriptNode* n = currentNode_.empty() ? nullptr : lib_->node(currentNode_);
    if (!n || n->kind == NodeKind::Part) {
        flash(L"Choose a chapter first: a new scene starts with a *** divider in the chapter's text.", 6);
        return;
    }
    if (!editor_.ready() || !editorNote_.empty()) {
        flash(L"The editor isn't available, so a scene divider can't be added.", 6);
        return;
    }
    commitFields();
    editor_.focus();
    editor_.send(R"({"type":"command","name":"newScene"})");
    editor_.send(R"({"type":"focus"})");
}

void StudioApp::addNode(NodeKind kind) {
    if (!lib_ || writingBook_.empty()) return;
    if (kind == NodeKind::Scene) { addSceneBreak(); return; }
    commitFields();
    const ManuscriptNode* cur = currentNode_.empty() ? nullptr : lib_->node(currentNode_);
    Id parent, after;
    if (kind == NodeKind::Part) {
        // After the part we're in (or at the end).
        const ManuscriptNode* p = cur;
        while (p && p->kind != NodeKind::Part) p = p->parentId.empty() ? nullptr : lib_->node(p->parentId);
        after = p ? p->id : Id{};
    } else if (kind == NodeKind::Chapter) {
        // After the current chapter, in the same part.
        const ManuscriptNode* c = cur;
        while (c && c->kind == NodeKind::Scene && !c->parentId.empty()) c = lib_->node(c->parentId);
        if (c && c->kind == NodeKind::Part) { parent = c->id; }
        else if (c) { parent = c->parentId; after = c->id; }
    } else {
        // A scene: inside the current chapter (after the current scene).
        const ManuscriptNode* c = cur;
        if (c && c->kind == NodeKind::Scene) { parent = c->parentId; after = c->id; }
        else if (c && c->kind == NodeKind::Chapter) { parent = c->id; }
        else if (c && c->kind == NodeKind::Part) { parent = c->id; }
    }
    const char* title = kind == NodeKind::Part ? "New part" : kind == NodeKind::Scene ? "New scene" : "New chapter";
    const Id id = lib_->createNode(writingBook_, parent, kind, title, after);
    if (id.empty()) { flash(w(lib_->lastError()), 6); return; }
    // Ready to be named: the title is selected, so typing replaces it and Enter goes to the text.
    selectNode(id, false);
    renameFocused();
}

void StudioApp::moveNodeBy(const Id& id, int delta) {
    const ManuscriptNode* n = lib_->node(id);
    if (!n) return;
    auto sibs = lib_->childrenOf(n->bookId, n->parentId);
    int idx = 0;
    for (int i = 0; i < (int)sibs.size(); ++i) if (sibs[i]->id == id) idx = i;
    const int to = idx + delta;
    if (to < 0 || to >= (int)sibs.size()) return;
    lib_->moveNode(id, n->parentId, to);
}

void StudioApp::nodeContextMenu(const Id& id) {
    const ManuscriptNode* n = lib_->node(id);
    if (!n) return;
    if (n->deleted) {
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, WRestore, L"&Restore");
        POINT pt;
        GetCursorPos(&pt);
        const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd(), nullptr);
        DestroyMenu(m);
        if (cmd == WRestore && lib_->restore(id)) { selectNode(id); flash(L"Restored “" + nodeLabel(*lib_->node(id)) + L"”.", 4); }
        invalidate();
        return;
    }
    selectNode(id);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, WRename, L"&Rename\tF2");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    if (n->kind == NodeKind::Part) AppendMenuW(m, MF_STRING, WNewSceneInside, L"New &chapter in this part");
    else AppendMenuW(m, MF_STRING, WNewScene, L"New &scene (a *** divider at the end)\tCtrl+Enter in the text");
    AppendMenuW(m, MF_STRING, WNewChapterAfter, L"New chapter &after this");
    AppendMenuW(m, MF_STRING, WNewPart, L"New &part");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    HMENU kinds = CreatePopupMenu();
    const ManuscriptNode* p = n->parentId.empty() ? nullptr : lib_->node(n->parentId);
    for (int k = 0; k < 3; ++k) {
        const NodeKind nk = (NodeKind)k;
        bool ok = Library::canContain(p ? std::optional<NodeKind>(p->kind) : std::nullopt, nk);
        for (auto* c : lib_->childrenOf(n->bookId, n->id)) ok = ok && Library::canContain(nk, c->kind);
        AppendMenuW(kinds, MF_STRING | (n->kind == nk ? MF_CHECKED : 0) | (ok ? 0 : MF_GRAYED), WKindBase + k, w(label(nk)).c_str());
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR)kinds, L"Make it a...");
    HMENU into = CreatePopupMenu();
    std::vector<Id> targets;
    targets.push_back({});
    AppendMenuW(into, MF_STRING | (Library::canContain(std::nullopt, n->kind) ? 0 : MF_GRAYED) | (n->parentId.empty() ? MF_CHECKED : 0),
                WIntoBase, L"The book (top level)");
    for (auto* x : lib_->manuscriptOf(n->bookId)) {
        if (x->id == id || x->kind == NodeKind::Scene || !Library::canContain(x->kind, n->kind)) continue;
        bool inside = false;  // not into its own descendants
        for (const ManuscriptNode* a = x; a; a = a->parentId.empty() ? nullptr : lib_->node(a->parentId)) if (a->id == id) inside = true;
        if (inside || targets.size() > 250) continue;
        AppendMenuW(into, MF_STRING | (n->parentId == x->id ? MF_CHECKED : 0), WIntoBase + (UINT)targets.size(),
                    ((x->kind == NodeKind::Part ? L"" : L"    ") + nodeLabel(*x)).c_str());
        targets.push_back(x->id);
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR)into, L"Move &into");
    AppendMenuW(m, MF_STRING, WMoveUp, L"Move &up\tAlt+Up");
    AppendMenuW(m, MF_STRING, WMoveDown, L"Move &down\tAlt+Down");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, WTrash, L"Move to &Trash\tDel");
    POINT pt;
    GetCursorPos(&pt);
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd(), nullptr);
    DestroyMenu(m);
    if (cmd >= WIntoBase && cmd < WIntoBase + (int)targets.size()) {
        if (!lib_->moveNode(id, targets[cmd - WIntoBase], -1)) flash(w(lib_->lastError()), 6);
    } else if (cmd >= WKindBase && cmd < WKindBase + 3) {
        ManuscriptNode x = *lib_->node(id);
        x.kind = (NodeKind)(cmd - WKindBase);
        if (!lib_->updateNode(x)) flash(w(lib_->lastError()), 6);
        loadedFocus_ = {Focus::None, "-", ""};
    } else if (cmd) {
        writingCommand(cmd);
    }
    invalidate();
}

void StudioApp::writingCommand(int id) {
    if (!lib_) return;
    if (ws_ != Workspace::Writing || !lib_->book(writingBook_)) {
        flash(L"Open a book in Writing first (Ctrl+W, or double-click a book).", 5);
        return;
    }
    const ManuscriptNode* n = currentNode_.empty() ? nullptr : lib_->node(currentNode_);
    switch (id) {
        case WNewChapterAfter: addNode(NodeKind::Chapter); break;
        case WNewSceneInside:
            if (n && n->kind == NodeKind::Part) {
                const Id nid = lib_->createNode(n->bookId, n->id, NodeKind::Chapter, "New chapter");
                if (!nid.empty()) { selectNode(nid, false); renameFocused(); }
            } else addSceneBreak();
            break;
        case WNewPart: addNode(NodeKind::Part); break;
        case WNewChapter: addNode(NodeKind::Chapter); break;
        case WNewScene: addNode(NodeKind::Scene); break;
        case WPrev: stepNode(-1, true, true); break;
        case WNext: stepNode(1, true, true); break;
        case WLibrary:
            commitFields();
            ws_ = Workspace::Library;
            goTo(View::Book, writingBook_);
            break;
        case WRename:
            if (fields_[FName]) { SetFocus(fields_[FName]); SendMessageW(fields_[FName], EM_SETSEL, 0, -1); }
            break;
        case WMoveUp: if (n) moveNodeBy(n->id, -1); break;
        case WMoveDown: if (n) moveNodeBy(n->id, 1); break;
        case WTrash:
            if (n) {
                const std::wstring what = nodeLabel(*n);
                const std::int64_t words = nodeTotalWords(n->id);
                if (words > 0) {
                    const std::wstring q = L"Move “" + what + L"” (" + wordsLabel(words) + L") to the Trash?\n\n"
                                           L"It stays in the Trash at the bottom of the outline, where you can restore it, until you empty the Trash.";
                    if (MessageBoxW(hwnd(), q.c_str(), L"Move to Trash", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) break;
                }
                if (editor_.ready()) editor_.send(R"({"type":"flush"})");
                const Id bookId = n->bookId, trashed = n->id;
                // Select a neighbour first.
                auto all = lib_->manuscriptOf(bookId);
                Id next;
                for (size_t i = 0; i < all.size(); ++i)
                    if (all[i]->id == trashed) { next = i > 0 ? all[i - 1]->id : (i + 1 < all.size() ? all[i + 1]->id : Id{}); }
                lib_->trashNode(trashed);
                // Not one of its own children.
                for (const ManuscriptNode* a = next.empty() ? nullptr : lib_->node(next); a; a = a->parentId.empty() ? nullptr : lib_->node(a->parentId))
                    if (a->id == trashed) next.clear();
                if (next.empty()) {
                    auto rest = lib_->manuscriptOf(bookId);
                    next = rest.empty() ? lib_->createNode(bookId, {}, NodeKind::Chapter) : rest[0]->id;
                }
                currentNode_.clear();
                selectNode(next);
                flash(L"Moved “" + what + L"” to the Trash. Restore it from the Trash at the bottom of the outline.", 6);
            }
            break;
    }
    invalidate();
}

// ------------------------------------------------------------------- binder

void StudioApp::drawBinder(const Rect& r) {
    gfx_.fill(r, pal_.sunken);
    gfx_.line(r.right() - 0.5f, r.y, r.right() - 0.5f, r.bottom(), pal_.rule);
    const Book* b = lib_ ? lib_->book(writingBook_) : nullptr;
    if (!b) return;
    gfx_.pushClip(r);
    setHitClip(r);
    // Same grid as the Library sidebar: rows from x, text from x + 12.
    const float x = r.x + 10, wd = r.w - 20, tx0 = x + 12;
    float y = r.y + 14 - binderScroll_;

    linkText("side:writing:library", tx0, y, L"‹ Library", [this] { writingCommand(WLibrary); });
    y += 30;
    gfx_.text(L"MANUSCRIPT", {tx0, y, wd - 24, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 20;
    y += gfx_.text(w(b->title), {tx0, y, wd - 24, 90}, Font::SerifTitle, pal_.ink) + 2;
    gfx_.text(wordsLabel(lib_->bookWordCount(b->id)), {tx0, y, wd - 24, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
    y += 30;
    float bx = tx0, bw = 0;
    smallButton("side:add:chapter", bx, y, L"+ Chapter", [this] { addNode(NodeKind::Chapter); }, &bw);
    bx += bw + 6;
    smallButton("side:add:scene", bx, y, L"+ Scene", [this] { addNode(NodeKind::Scene); }, &bw);
    bx += bw + 6;
    smallButton("side:add:part", bx, y, L"+ Part", [this] { addNode(NodeKind::Part); }, &bw);
    y += 44;

    // Depth of each node, for indentation.
    std::function<int(const ManuscriptNode*)> depth = [&](const ManuscriptNode* n) {
        int d = 0;
        for (const ManuscriptNode* p = n->parentId.empty() ? nullptr : lib_->node(n->parentId); p;
             p = p->parentId.empty() ? nullptr : lib_->node(p->parentId)) ++d;
        return d;
    };
    const auto all = lib_->manuscriptOf(b->id);
    std::vector<Id> order;
    for (auto* n : all) order.push_back(n->id);
    bool first = true;
    for (const ManuscriptNode* n : all) {
        const int d = depth(n);
        if (n->kind == NodeKind::Part && !first) y += 8;  // a little air above each part
        first = false;
        const float rh = 30.0f;
        const Rect rr{x, y, wd, rh};
        const std::string id = "node:" + n->id;
        addHit(id, rr, [this, nid = n->id] { selectNode(nid); });
        Drop drop{Drop::NodeRow, rr, n->id, n->parentId};
        drop.list = order;
        addDrop(drop);
        const bool sel = n->id == currentNode_;
        const float hv = hoverAmount(id);
        if (sel) {
            const float sy = slideTo("binder#sel", rr.y + binderScroll_) - binderScroll_;
            gfx_.fillRounded({rr.x, sy, rr.w, rh}, 3, pal_.surface);
            gfx_.fill({rr.x, sy + 7, 3, rh - 14}, pal_.ribbon);
        } else if (hv > 0.01f) gfx_.fillRounded(rr, 3, pal_.surface.withAlpha(0.6f * hv));
        // Status dot: outline grey, draft amber, revised blue, final green.
        static const char* dots[] = {"#9A958A", "#C9922E", "#4E7DB3", "#3E8E5E"};
        const float tx = tx0 + d * 16;
        gfx_.circle(tx + 3, rr.y + rh / 2, 3, hexColor(dots[(int)n->status]), true);
        const std::int64_t words = n->kind == NodeKind::Scene ? n->wordCount : nodeTotalWords(n->id);
        const std::wstring count = words ? thousands(words) : L"";
        const float cw = count.empty() ? 0 : gfx_.measureWidth(count, Font::Small) + 8;
        std::wstring title = nodeLabel(*n);
        Font f = sel ? Font::BodyStrong : Font::Body;
        if (n->kind == NodeKind::Part) { CharUpperBuffW(title.data(), (DWORD)title.size()); f = Font::SmallStrong; }
        if (n->kind == NodeKind::Scene) f = sel ? Font::SmallStrong : Font::Small;
        gfx_.text(title, {tx + 14, rr.y, rr.right() - tx - 14 - cw - 8, rh}, f, sel ? pal_.ink : pal_.inkMuted.mix(pal_.ink, 0.35f),
                  {Align::Left, false, true, true});
        if (!count.empty())
            gfx_.text(count, {rr.right() - cw - 4, rr.y, cw, rh}, Font::Small, pal_.inkFaint, {Align::Right, false, false, true});
        if (focused(id)) focusRing(rr);
        y += rh + 2;
    }
    if (all.empty()) {
        y += gfx_.text(L"This book has no chapters yet. Press + Chapter to start one.", {tx0, y, wd - 24, 40}, Font::Small,
                       pal_.inkMuted) + 8;
    }
    // Trash.
    const auto trashed = lib_->trashedNodesOf(b->id);
    if (!trashed.empty()) {
        y += 16;
        const std::wstring t = (showNodeTrash_ ? L"▾ Trash (" : L"▸ Trash (") + std::to_wstring(trashed.size()) + L")";
        linkText("side:nodetrash", tx0, y, t, [this] { showNodeTrash_ = !showNodeTrash_; invalidate(); });
        y += 26;
        if (showNodeTrash_) {
            for (auto* n : trashed) {
                const Rect rr{x, y, wd, 30};
                const std::string id = "node:" + n->id;
                addHit(id, rr, [this, nid = n->id] {
                    if (lib_->restore(nid)) { selectNode(nid); flash(L"Restored “" + nodeLabel(*lib_->node(nid)) + L"”.", 4); }
                });
                const float hv = hoverAmount(id);
                if (hv > 0.01f) gfx_.fillRounded(rr, 3, pal_.surface.withAlpha(0.6f * hv));
                gfx_.text(nodeLabel(*n), {tx0 + 14, rr.y, rr.w - 120, rr.h}, Font::Small, pal_.inkFaint, {Align::Left, false, true, true});
                gfx_.text(L"Restore", {rr.right() - 70, rr.y, 66, rr.h}, Font::Small, pal_.ribbon.withAlpha(hv),
                          {Align::Right, false, false, true});
                y += 32;
            }
            y += gfx_.text(L"Click one to restore it. Edit > Empty Trash removes them for good.", {tx0, y + 4, wd - 24, 60},
                           Font::Small, pal_.inkFaint) + 10;
        }
    }
    binderContent_ = y + binderScroll_ - r.y + 20;
    clearHitClip();
    gfx_.popClip();
}

// ------------------------------------------------------------ details panel

void StudioApp::drawNodePanel(const Rect& r, const Rect& c, float& y) {
    (void)r;
    const ManuscriptNode* n = lib_->node(focus_.id);
    if (!n) return;
    std::wstring kicker = w(label(n->kind));
    CharUpperBuffW(kicker.data(), (DWORD)kicker.size());
    panelHeading(c, y, kicker, true);
    y = panelField(c, y, L"Title", FName);
    panelChoice("panel:nodestatus", c, y, L"Status", w(label(n->status)), [this] {
        HMENU m = CreatePopupMenu();
        const ManuscriptNode* x = lib_->node(focus_.id);
        for (int k = 0; k < 4; ++k)
            AppendMenuW(m, MF_STRING | (x && x->status == (NodeStatus)k ? MF_CHECKED : 0), WStatusBase + k, w(label((NodeStatus)k)).c_str());
        POINT pt;
        GetCursorPos(&pt);
        const int cmd = TrackPopupMenu(m, TPM_RETURNCMD, pt.x, pt.y, 0, hwnd(), nullptr);
        DestroyMenu(m);
        if (cmd >= WStatusBase && cmd < WStatusBase + 4 && lib_->node(focus_.id)) {
            commitFields();
            ManuscriptNode u = *lib_->node(focus_.id);
            u.status = (NodeStatus)(cmd - WStatusBase);
            lib_->updateNode(u);
        }
    });
    panelChoice("panel:nodekind", c, y, L"Kind", w(label(n->kind)), [this] {
        const ManuscriptNode* x = lib_->node(focus_.id);
        if (!x) return;
        HMENU m = CreatePopupMenu();
        const ManuscriptNode* p = x->parentId.empty() ? nullptr : lib_->node(x->parentId);
        for (int k = 0; k < 3; ++k) {
            bool ok = Library::canContain(p ? std::optional<NodeKind>(p->kind) : std::nullopt, (NodeKind)k);
            for (auto* ch : lib_->childrenOf(x->bookId, x->id)) ok = ok && Library::canContain((NodeKind)k, ch->kind);
            AppendMenuW(m, MF_STRING | (x->kind == (NodeKind)k ? MF_CHECKED : 0) | (ok ? 0 : MF_GRAYED), WKindBase + k, w(label((NodeKind)k)).c_str());
        }
        POINT pt;
        GetCursorPos(&pt);
        const int cmd = TrackPopupMenu(m, TPM_RETURNCMD, pt.x, pt.y, 0, hwnd(), nullptr);
        DestroyMenu(m);
        if (cmd >= WKindBase && cmd < WKindBase + 3) {
            commitFields();
            ManuscriptNode u = *lib_->node(focus_.id);
            u.kind = (NodeKind)(cmd - WKindBase);
            if (!lib_->updateNode(u)) flash(w(lib_->lastError()), 6);
            else if (u.id == currentNode_) sendLoad();  // the placeholder text changes with the kind
        }
    });
    y = panelField(c, y, L"Synopsis", FSynopsis, 150);

    // Counts.
    panelHeading(c, y, L"WORDS", false);
    auto row = [&](const std::wstring& k, std::int64_t v) {
        gfx_.text(k, {c.x, y, c.w * 0.6f, 20}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
        gfx_.text(thousands(v), {c.x + c.w * 0.6f, y, c.w * 0.4f, 20}, Font::Small, pal_.ink, {Align::Right, false});
        y += 22;
    };
    const std::int64_t own = n->id == currentNode_ && liveWords_ >= 0 ? liveWords_ : n->wordCount;
    row(std::wstring(L"This ") + (n->kind == NodeKind::Scene ? L"scene" : n->kind == NodeKind::Part ? L"part's opening" : L"chapter's text"), own);
    if (n->kind != NodeKind::Scene && !lib_->childrenOf(n->bookId, n->id).empty()) row(L"Including everything inside", nodeTotalWords(n->id) - n->wordCount + own);
    if (n->kind == NodeKind::Scene && !n->parentId.empty()) row(L"Whole chapter", nodeTotalWords(n->parentId));
    row(L"Whole book", lib_->bookWordCount(n->bookId));
    if (selectionWords_ > 0 && n->id == currentNode_) row(L"Selected", selectionWords_);

    drawNodeComments(c, y, *n);

    // Earlier versions.
    panelHeading(c, y, L"EARLIER VERSIONS", false);
    const auto versions = lib_->nodeVersions(n->id);
    if (versions.empty()) {
        y += gfx_.text(L"While you write, the Studio keeps a copy of this text every ten minutes or so. They'll be listed here.",
                       {c.x, y, c.w, 80}, Font::Small, pal_.inkFaint) + 8;
    }
    int shown = 0;
    for (auto& v : versions) {
        if (++shown > 12) break;
        const std::wstring when = cjui::formatDay((cj::UtcSeconds)v.saved) + L", " + cjui::formatTime((cj::UtcSeconds)v.saved);
        gfx_.text(when, {c.x, y, c.w - 70, 18}, Font::Small, pal_.ink, {Align::Left, false, true});
        gfx_.text(wordsLabel(v.wordCount) + (v.reason == "before restore" ? L" · before a restore" : L""),
                  {c.x, y + 17, c.w - 70, 18}, Font::Small, pal_.inkFaint, {Align::Left, false, true});
        const float lw = gfx_.measureWidth(L"Restore", Font::Small);
        linkText("panel:ver:" + std::to_string(v.id), c.right() - lw, y + 8, L"Restore", [this, vid = v.id, when] {
            if (MessageBoxW(hwnd(), (L"Replace the current text with the version from " + when +
                                     L"?\n\nThe current text is kept as a version too, so you can switch back.").c_str(),
                            L"Restore an earlier version", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
                return;
            if (editor_.ready()) editor_.send(R"({"type":"flush"})");
            if (lib_->restoreNodeVersion(focus_.id, vid)) { sendLoad(); flash(L"Restored the version from " + when + L".", 4); }
            else flash(w(lib_->lastError()), 8);
        });
        gfx_.line(c.x, y + 40.5f, c.right(), y + 40.5f, pal_.rule.withAlpha(0.6f));
        y += 46;
    }
}

// -------------------------------------------------------------- your place

void StudioApp::rememberPlace() {
    const bool writing = ws_ == Workspace::Writing && lib_ && lib_->book(writingBook_);
    settings_.lastInWriting = writing;
    settings_.lastBook = writing ? writingBook_ : view_ == View::Book ? viewId_ : focus_.kind == Focus::Book ? focus_.id : Id{};
    settings_.lastNode = writing ? currentNode_ : Id{};
}

// On start: back to the chapter you were writing, or the book you had open.
void StudioApp::restorePlace() {
    if (!lib_ || settings_.lastBook.empty()) return;
    const Book* b = lib_->book(settings_.lastBook);
    if (!b || b->deleted) return;
    if (settings_.lastInWriting) {
        const ManuscriptNode* n = settings_.lastNode.empty() ? nullptr : lib_->node(settings_.lastNode);
        writingBook_ = b->id;
        currentNode_ = n && !n->deleted && n->bookId == b->id ? n->id : Id{};
        openWriting(b->id);
    } else {
        goTo(View::Book, b->id);
    }
}

// -------------------------------------------------------------------- import

void StudioApp::startImport(const Id& intoBook) {
    if (!lib_ || importBusy_) return;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    COMDLG_FILTERSPEC types[] = {{L"Books and manuscripts (EPUB, Word, OpenDocument, PDF, HTML, Markdown, text)",
                                  L"*.epub;*.docx;*.odt;*.pdf;*.html;*.htm;*.xhtml;*.md;*.markdown;*.txt"},
                                 {L"All files", L"*.*"}};
    dlg->SetFileTypes(2, types);
    dlg->SetTitle(intoBook.empty() ? L"Import a book" : L"Import into this book");
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
    if (!path.empty()) importPath(path, intoBook);
}

void StudioApp::importPath(const std::wstring& path, const Id& intoBook) {
    if (!lib_ || importBusy_ || path.empty()) return;
    commitFields();
    if (ws_ != Workspace::Import) beforeImport_ = ws_;
    ws_ = Workspace::Import;
    if (focus_.kind == Focus::Node) setFocus({});  // the panel isn't about a chapter while previewing
    importTarget_ = intoBook;
    importUseDetails_ = true;
    importPlan_.reset();
    importBusy_ = true;
    importFile_ = path.substr(path.find_last_of(L"\\/") + 1);
    resetScroll();
    // The file is read by a helper copy of the Studio in its own process
    // (see main.cpp), so a file that crashes a reader can't close the app.
    wchar_t exe[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, exe, (DWORD)std::size(exe));
    const std::wstring exePath = exe;
    const std::wstring work = stateDir() + L"\\import-" + std::to_wstring(GetTickCount64());
    HWND target = hwnd();
    std::thread([exePath, path, work, target] {
        using studio::import::Plan;
        auto* plan = new Plan();
        std::wstring cmd = L"\"" + exePath + L"\" --import-worker \"" + path + L"\" \"" + work + L"\"";
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(exePath.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                            nullptr, nullptr, &si, &pi)) {
            plan->error = "The import helper couldn't start (Windows error " + std::to_string(GetLastError()) + ").";
        } else {
            const DWORD waited = WaitForSingleObject(pi.hProcess, 10 * 60 * 1000);
            DWORD code = 0;
            if (waited == WAIT_TIMEOUT) {
                TerminateProcess(pi.hProcess, 1);
                plan->error = "Reading this file took more than ten minutes, so it was stopped.";
            } else {
                GetExitCodeProcess(pi.hProcess, &code);
                if (code >= 0xC0000000u) {
                    char hex[16];
                    std::snprintf(hex, sizeof hex, "%08lX", (unsigned long)code);
                    plan->error = std::string("The importer stopped while reading this file (error 0x") + hex +
                                  "). Nothing was changed. The file may be damaged, or use something the Studio can't read yet; "
                                  "if it opens normally elsewhere, please send it to NorthRay Software so this can be fixed.";
                } else {
                    *plan = studio::import::loadPlan(narrow(work));
                }
            }
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path(work), ec);
        if (!PostMessageW(target, WM_APP_IMPORT, 0, reinterpret_cast<LPARAM>(plan))) delete plan;
    }).detach();
    invalidate();
}

void StudioApp::cancelImport() {
    importPlan_.reset();
    ws_ = beforeImport_;
    resetScroll();
    if (ws_ == Workspace::Writing && !currentNode_.empty()) selectNode(currentNode_, false);  // its details again
    invalidate();
}

void StudioApp::importFinished(studio::import::Plan* plan) {
    importPlan_.reset(plan);
    importBusy_ = false;
    invalidate();
}

void StudioApp::doImport() {
    if (!lib_ || !importPlan_ || !importPlan_->ok) return;
    Id book = importTarget_;
    if (book.empty() || !lib_->book(book)) {
        book = lib_->createBook(importPlan_->meta.title.empty() ? narrow(importFile_) : importPlan_->meta.title, {});
        if (book.empty()) { flash(w(lib_->lastError()), 10); return; }
    }
    studio::import::ApplyOptions opt;
    opt.setMetadata = importUseDetails_;
    opt.setCover = importUseDetails_;
    std::vector<Id> ids;
    try {
        ids = studio::import::apply(*lib_, book, *importPlan_, opt);
    } catch (const std::exception& e) {
        flash(w(std::string("Import failed: ") + e.what()), 15);
        return;
    }
    if (ids.empty() && importPlan_->includedCount() > 0) { flash(w("Import failed: " + lib_->lastError()), 15); return; }
    const std::int64_t words = importPlan_->includedWords();
    const int count = importPlan_->includedCount();
    importPlan_.reset();
    // Open the first new chapter or scene (not a part) in the editor.
    currentNode_.clear();
    std::set<Id> fresh(ids.begin(), ids.end());
    for (auto* n : lib_->manuscriptOf(book)) {
        if (!fresh.count(n->id) && !(n->parentId.size() && fresh.count(n->parentId))) continue;
        fresh.insert(n->id);
        if (n->kind != NodeKind::Part) { currentNode_ = n->id; break; }
    }
    writingBook_ = book;
    openWriting(book);
    flash(L"Imported " + std::to_wstring(count) + (count == 1 ? L" section, " : L" sections, ") + wordsLabel(words) + L".", 8);
}

float StudioApp::pageImport(const Rect& r0) {
    // One column, the same width as the notes, so nothing drifts to the far edge on a wide window.
    const Rect r{r0.x, r0.y, std::min(r0.w, 820.0f), 0};
    float y = r.y;
    using studio::import::Role;
    if (importBusy_) {
        y += pageHeader({r.x, y, r.w, 0}, L"IMPORT", L"Reading " + importFile_ + L"...",
                        L"Finding chapters, styles and pictures. Large PDFs can take a little while.");
        return y - r.y;
    }
    if (!importPlan_) return 0;
    const auto& p = *importPlan_;
    auto cancel = [this] { cancelImport(); };
    if (!p.ok) {
        y += pageHeader({r.x, y, r.w, 0}, L"IMPORT", L"This file can't be imported", w(p.error));
        const std::wstring other = L"Choose another file...";
        const float ow = gfx_.measureWidth(other, Font::BodyStrong) + 36;
        button("imp:other", {r.x, y, ow, 34}, other, [this] { startImport(importTarget_); }, true);
        button("imp:cancel", {r.x + ow + 10, y, 100, 34}, L"Cancel", cancel);
        return y + 50 - r.y;
    }
    const Book* into = importTarget_.empty() ? nullptr : lib_->book(importTarget_);
    std::wstring sub = std::wstring(w(label(p.format))) + L" · " + importFile_ +
                       (into ? L" · adding to “" + w(into->title) + L"”" : L" · as a new book");
    if (!p.meta.author.empty()) sub = L"by " + w(p.meta.author) + L"\n" + sub;
    y += pageHeader({r.x, y, r.w, 0}, L"IMPORT", w(p.meta.title.empty() ? narrow(importFile_) : p.meta.title), sub);
    if (p.approximate) {
        const std::wstring t = p.format == studio::import::Format::Pdf
            ? L"A PDF is a picture of finished pages, not a manuscript, so its chapters and paragraphs have been worked out from the "
              L"layout. Check the list below, and read through the result before relying on it."
            : L"This file has no formal chapter structure, so chapters were found from headings like “Chapter 1”. Check the list below.";
        const float th = gfx_.measureHeight(t, r.w - 32, Font::Small);
        gfx_.fillRounded({r.x, y, r.w, th + 22}, 3, pal_.sunken);
        gfx_.fill({r.x, y, 3, th + 22}, pal_.ribbon);
        gfx_.text(t, {r.x + 16, y + 11, r.w - 32, th + 4}, Font::Small, pal_.ink);
        y += th + 38;
    }
    // Actions.
    const int count = p.includedCount();
    const std::wstring go = L"Import " + std::to_wstring(count) + (count == 1 ? L" section" : L" sections") + L" (" + wordsLabel(p.includedWords()) + L")";
    const float gw = gfx_.measureWidth(go, Font::BodyStrong) + 40;
    if (count > 0) button("imp:go", {r.x, y, gw, 34}, go, [this] { doImport(); }, true);
    else disabledButton({r.x, y, gw, 34}, go);
    button("imp:cancel", {r.x + gw + 10, y, 100, 34}, L"Cancel", cancel);
    y += 52;
    // Book details.
    {
        const Rect cb{r.x, y + 3, 18, 18};
        const std::string id = "imp:details";
        const Rect hit{r.x, y, r.w, 24};
        addHit(id, hit, [this] { importUseDetails_ = !importUseDetails_; invalidate(); });
        gfx_.strokeRounded(cb, 3, pal_.inkMuted.mix(pal_.ink, hoverAmount(id)), 1.5f);
        const float on = anim(id + "#on", importUseDetails_ ? 1.0f : 0.0f, 24.0f);
        if (on > 0.01f) gfx_.fillRounded(cb.inset(4 + 3 * (1 - on)), 2, pal_.ribbon.withAlpha(on));
        std::wstring t = into ? L"Fill in the book's empty details (author, description, ISBN, cover) from the file"
                              : L"Use the file's details for the new book (title, author, description, ISBN, cover)";
        if (!p.meta.coverBytes.empty()) t += L". A cover was found.";
        gfx_.text(t, {r.x + 28, y, r.w - 28, 24}, Font::Small, pal_.ink, {Align::Left, false, true, true});
        if (focused(id)) focusRing(hit);
        y += 42;
    }
    // Sections.
    gfx_.text(L"WHAT WAS FOUND", {r.x, y, r.w, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    {
        // All · None · Suggested, right-aligned with the word counts below.
        const float ws = gfx_.measureWidth(L"Suggested", Font::Small), wn = gfx_.measureWidth(L"None", Font::Small),
                    wa = gfx_.measureWidth(L"All", Font::Small);
        float lx = r.right() - 10 - ws;
        linkText("imp:default", lx, y - 2, L"Suggested", [this] {
            for (auto& s : importPlan_->sections) s.include = studio::import::includedByDefault(s.role);
        });
        lx -= 16 + wn;
        linkText("imp:none", lx, y - 2, L"None", [this] { for (auto& s : importPlan_->sections) s.include = false; });
        lx -= 16 + wa;
        linkText("imp:all", lx, y - 2, L"All", [this] { for (auto& s : importPlan_->sections) s.include = true; });
    }
    y += 26;
    gfx_.line(r.x, y - 0.5f, r.right(), y - 0.5f, pal_.rule);
    for (size_t i = 0; i < p.sections.size(); ++i) {
        const auto& s = p.sections[i];
        const Rect row{r.x, y, r.w, 38};
        const std::string id = "imp:sec:" + std::to_string(i);
        addHit(id, row, [this, i] { importPlan_->sections[i].include = !importPlan_->sections[i].include; invalidate(); });
        const float hv = hoverAmount(id);
        if (hv > 0.01f) gfx_.fill(row, pal_.surface.withAlpha(0.7f * hv));
        gfx_.line(row.x, row.bottom() - 0.5f, row.right(), row.bottom() - 0.5f, pal_.rule);
        const float ix = row.x + 10 + std::min(s.level, 3) * 24;
        const Rect cb{ix, row.y + 10, 18, 18};
        gfx_.strokeRounded(cb, 3, pal_.inkMuted, 1.5f);
        const float on = anim(id + "#on", s.include ? 1.0f : 0.0f, 24.0f);
        if (on > 0.01f) gfx_.fillRounded(cb.inset(4 + 3 * (1 - on)), 2, pal_.ribbon.withAlpha(on));
        const bool part = s.role == Role::Part;
        // Right-hand columns: role tag (if not a chapter), then the word count.
        const float countW = 100, tagCol = 120;
        const float tx = ix + 30;
        const float titleRight = row.right() - countW - (s.role != Role::Chapter ? tagCol : 0) - 16;
        gfx_.text(w(s.title), {tx, row.y, std::max(40.0f, titleRight - tx), row.h}, part ? Font::BodyStrong : Font::Body,
                  s.include ? pal_.ink : pal_.inkFaint, {Align::Left, false, true, true});
        if (s.role != Role::Chapter) {
            const std::wstring role = w(studio::import::label(s.role));
            const float tw = std::min(gfx_.measureWidth(role, Font::Label) + 16, tagCol - 8);
            tag(row.right() - countW - 10 - tw, row.y + 10, role, part ? pal_.ribbon : pal_.inkMuted);
        }
        const std::wstring words = part && s.words <= studio::import::kPartPageWords ? L"title only" : wordsLabel(s.words);
        gfx_.text(words, {row.right() - countW - 10, row.y, countW, row.h}, Font::Small, pal_.inkMuted, {Align::Right, false, false, true});
        if (focused(id)) focusRing(row);
        y += 38;
    }
    if (!p.warnings.empty()) {
        y += 22;
        gfx_.text(L"NOTES", {r.x, y, r.w, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
        y += 24;
        for (auto& wn : p.warnings) y += gfx_.text(L"•  " + w(wn), {r.x, y, r.w, 80}, Font::Small, pal_.ink) + 6;
    }
    y += 16;
    y += gfx_.text(L"Nothing has been added yet. Unticked sections are left out; parts keep the chapters ticked inside them. "
                   L"Images are stored once in your library, and the original file is never changed.",
                   {r.x, y, r.w, 80}, Font::Small, pal_.inkFaint) + 10;
    return y - r.y + 20;
}

}  // namespace nrs
