// Comments on the manuscript (Milestone 10): notes pinned to words in the text.
//
// The editor marks the words (a "comment" mark holding the comment's id); the
// library keeps what the comment says, and whether it's resolved. A resolved
// comment keeps its place in the text, so it can be reopened, but isn't shown.
#include "studio_app.hpp"
#include "cjui/dialog.hpp"

#include "cjui/format.hpp"
#include "studio/review.hpp"

#include <fstream>
#include <shobjidl.h>
#include <sstream>

#include <algorithm>
#include <nlohmann/json.hpp>

namespace nrs {

using nlohmann::json;
using namespace studio;

static std::wstring w(const std::string& s) { return widen(s); }
static std::wstring when(Time t) { return cjui::formatDay((cj::UtcSeconds)t) + L", " + cjui::formatTime((cj::UtcSeconds)t); }

// The first line or so of a comment, for lists.
static std::wstring excerpt(const std::string& s, size_t n = 140) {
    std::wstring t = w(s);
    for (auto& ch : t) if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
    if (t.size() > n) t = t.substr(0, n - 1) + L"…";
    return t;
}

// Which comments of the open chapter are resolved, so the editor hides them.
void StudioApp::sendComments() {
    if (!lib_ || currentNode_.empty() || !editor_.ready()) return;
    json ids = json::array();
    for (const Comment* c : lib_->commentsOf(currentNode_)) if (c->resolved) ids.push_back(c->id);
    editor_.send(json{{"type", "comments"}, {"resolved", ids}}.dump());
}

void StudioApp::commentMessage(const std::string& text) {
    const json m = json::parse(text, nullptr, false);
    if (!m.is_object() || !lib_) return;
    const std::string type = m.value("type", "");
    const std::string node = m.value("node", "");
    if (type == "comment") {
        const std::string id = m.value("id", "");
        if (!lib_->addComment(node, id, m.value("quote", ""))) { flash(w("Couldn't add the comment: " + lib_->lastError()), 8); return; }
        setFocus({Focus::Comment, id, {}});
        focusCommentField_ = true;
    } else if (type == "commentFocus") {
        if (node != currentNode_) return;
        const std::string id = m.contains("id") && m["id"].is_string() ? m["id"].get<std::string>() : std::string();
        if (!id.empty() && lib_->comment(id)) {
            setFocus({Focus::Comment, id, {}});
        } else if (focus_.kind == Focus::Comment && GetFocus() != fields_[FDescription]) {
            // Out of the comment: back to the chapter's details (not while its words are being typed).
            const Comment* c = lib_->comment(focus_.id);
            if (!c || c->nodeId == currentNode_) setFocus({Focus::Node, currentNode_, {}});
        }
    }
    invalidate();
}

// Shows a comment: its chapter in Writing, its words selected, and it in the panel.
void StudioApp::showComment(const Id& id) {
    const Comment* c = lib_ ? lib_->comment(id) : nullptr;
    if (!c) return;
    const ManuscriptNode* n = lib_->node(c->nodeId);
    if (!n || n->deleted) { flash(L"That comment's chapter is in the Trash.", 5); return; }
    commitFields();
    if (ws_ != Workspace::Writing || currentNode_ != n->id) {
        currentNode_ = n->id;
        writingBook_ = n->bookId;
        openWriting(n->bookId);
    }
    if (editor_.ready()) editor_.send(json{{"type", "selectComment"}, {"id", id}}.dump());
    else pendingComment_ = id;
    setFocus({Focus::Comment, id, {}});
}

void StudioApp::setCommentResolved(const Id& id, bool resolved) {
    commitFields();
    if (!lib_->resolveComment(id, resolved, false)) { flash(w(lib_->lastError()), 6); return; }
    sendComments();
    invalidate();
}

void StudioApp::deleteComment(const Id& id) {
    const Comment* c = lib_ ? lib_->comment(id) : nullptr;
    if (!c) return;
    if (!c->body.empty() || GetWindowTextLengthW(fields_[FDescription]) > 0) {
        if (cjui::messageBox(hwnd(), L"Delete this comment? The words it's on aren't changed.", L"Delete comment",
                        MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
            return;
    }
    // Nothing to save for a comment that's going.
    dirty_ = false;
    KillTimer(hwnd(), 1);   // the fields' save timer
    const Id node = c->nodeId;
    // In the open chapter the editor takes the mark out (it holds the text); elsewhere the library does.
    const bool open = node == currentNode_ && editor_.ready();
    if (open) editor_.send(json{{"type", "stripComment"}, {"id", id}}.dump());
    if (!lib_->removeComment(id, !open)) { flash(w(lib_->lastError()), 6); return; }
    if (focus_.kind == Focus::Comment && focus_.id == id)
        setFocus(node == currentNode_ ? Focus{Focus::Node, node, {}} : Focus{});
    invalidate();
}

// The panel for one comment.
void StudioApp::drawCommentPanel(const Rect& c, float& y) {
    const Comment* cm = lib_->comment(focus_.id);
    if (!cm) { setFocus(currentNode_.empty() ? Focus{} : Focus{Focus::Node, currentNode_, {}}); return; }
    const Id id = cm->id;
    linkText("panel:cm:back", c.x, y, L"‹ This chapter's details", [this, node = cm->nodeId] { setFocus({Focus::Node, node, {}}); });
    y += 30;
    panelHeading(c, y, cm->resolved ? L"COMMENT · RESOLVED" : L"COMMENT", true);
    // The words it's on, as they were.
    if (!cm->quote.empty()) {
        const float qh = gfx_.text(L"“" + excerpt(cm->quote, 400) + L"”", {c.x + 12, y, c.w - 12, 120}, Font::Small, pal_.inkMuted);
        gfx_.fill({c.x, y + 1, 3, qh - 2}, pal_.demo.withAlpha(0.8f));
        y += qh + 14;
    }
    y = panelField(c, y, L"What it says", FDescription, 130);
    if (focusCommentField_) {
        focusCommentField_ = false;
        SetFocus(fields_[FDescription]);
    }
    gfx_.text(L"Made " + when(cm->created) + (cm->modified > cm->created + 60 ? L" · changed " + when(cm->modified) : std::wstring()),
              {c.x, y, c.w, 36}, Font::Small, pal_.inkFaint);
    y += 40;
    if (!lib_->commentAnchored(*cm) && cm->nodeId == currentNode_ && !editorDirty_) {
        y += gfx_.text(L"The words this comment was on have been deleted from the text. It's kept until you delete it.",
                       {c.x, y, c.w, 60}, Font::Small, pal_.demo) + 12;
    }
    float bw = 0;
    smallButton("panel:cm:resolve", c.x, y, cm->resolved ? L"Reopen" : L"Resolve", [this, id, r = cm->resolved != 0] { setCommentResolved(id, !r); }, &bw);
    linkText("panel:cm:delete", c.x + bw + 16, y + 6, L"Delete", [this, id] { deleteComment(id); });
    y += 46;
    y += gfx_.text(cm->resolved ? L"Resolved comments stay with their words but aren't highlighted. Reopen it to show it again."
                                : L"Resolve a comment once it's dealt with. It's kept, and can be reopened.",
                   {c.x, y, c.w, 60}, Font::Small, pal_.inkFaint) + 12;
}

// The comments section of a chapter's or scene's details.
void StudioApp::drawNodeComments(const Rect& c, float& y, const ManuscriptNode& n) {
    const auto all = lib_->commentsOf(n.id);
    int open = 0;
    for (const Comment* cm : all) if (!cm->resolved) ++open;
    panelHeading(c, y, open ? L"COMMENTS (" + std::to_wstring(open) + L")" : L"COMMENTS", false);
    if (all.empty()) {
        y += gfx_.text(L"None yet. Select some words and press Ctrl+Alt+M.",
                       {c.x, y, c.w, 60}, Font::Small, pal_.inkFaint) + 10;
        return;
    }
    for (const Comment* cm : all) {
        if (cm->resolved && !showResolvedComments_) continue;
        const std::string hid = "panel:cm:" + cm->id;
        const std::wstring body = cm->body.empty() ? L"(Nothing written yet)" : excerpt(cm->body);
        const float bh = gfx_.measureHeight(body, c.w - 20, Font::Small);
        const Rect rr{c.x, y, c.w, 30 + bh};
        addHit(hid, rr, [this, id = cm->id] { showComment(id); });
        const float hv = hoverAmount(hid);
        gfx_.fillRounded(rr, 3, pal_.surface.mix(pal_.sunken, 0.3f * hv));
        gfx_.fill({rr.x, rr.y + 6, 3, rr.h - 12}, cm->resolved ? pal_.rule : pal_.demo);
        gfx_.text(cm->quote.empty() ? std::wstring(L"On the whole chapter") : L"“" + excerpt(cm->quote, 60) + L"”", {rr.x + 12, rr.y + 6, rr.w - 20, 18}, Font::Small, pal_.inkFaint,
                  {Align::Left, false, true});
        gfx_.text(body, {rr.x + 12, rr.y + 24, rr.w - 20, bh + 4}, Font::Small, cm->resolved ? pal_.inkFaint : pal_.ink);
        if (focused(hid)) focusRing(rr);
        y += rr.h + 6;
    }
    const int resolved = (int)all.size() - open;
    if (resolved) {
        linkText("panel:cm:showres", c.x, y + 2,
                 showResolvedComments_ ? L"Hide resolved" : L"Show " + std::to_wstring(resolved) + L" resolved",
                 [this] { showResolvedComments_ = !showResolvedComments_; invalidate(); });
        y += 28;
    }
    y += 4;
}

static bool readAll(const std::wstring& path, std::string& out) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// A review copy: the whole book as one web page a beta reader opens in any browser, selects words and
// comments on, then sends back the small comments file it makes.
void StudioApp::makeReviewCopy() {
    const Book* b = lib_->book(rv_.book);
    if (!b) return;
    std::string tmpl;
    if (!readAll(exeDir() + L"\\web\\review-copy.html", tmpl)) { flash(L"The review copy page is missing from the install. Reinstall Writing Studio.", 8); return; }
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    std::string err;
    const std::string html = review::makeReviewCopy(*lib_, rv_.book, tmpl, err);
    if (html.empty()) { cjui::messageBox(hwnd(), w(err), L"Review copy", MB_OK | MB_ICONWARNING); return; }
    const std::wstring path = askSavePath(L"Web page", L"*.html", L"html", w(b->title) + L" - review copy", L"Save a review copy");
    if (path.empty()) return;
    std::ofstream out(path.c_str(), std::ios::binary);
    out << html;
    if (!out) { cjui::messageBox(hwnd(), L"Couldn't save the review copy there.", L"Review copy", MB_OK | MB_ICONWARNING); return; }
    out.close();
    cjui::messageBox(hwnd(),
                     L"Saved " + path.substr(path.find_last_of(L"\\/") + 1) +
                         L".\n\nSend it to your reader. They open it in any web browser, select words to comment on them, then "
                         L"choose \u201CSend my comments\u201D and send you the file it saves. Bring that back here with "
                         L"\u201CImport a reader's comments\u201D.",
                     L"Review copy", MB_OK | MB_ICONINFORMATION);
}

void StudioApp::importReaderComments() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    COMDLG_FILTERSPEC spec[] = {{L"Reader's comments", L"*.nrcomments;*.json"}};
    dlg->SetFileTypes(1, spec);
    dlg->SetTitle(L"Import a reader's comments");
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
    std::string text;
    if (!readAll(path, text)) { cjui::messageBox(hwnd(), L"Couldn't read that file.", L"Import comments", MB_OK | MB_ICONWARNING); return; }
    const review::ImportReport r = review::importReviewComments(*lib_, rv_.book, text);
    if (!r.error.empty()) { cjui::messageBox(hwnd(), w(r.error), L"Import comments", MB_OK | MB_ICONWARNING); return; }
    std::wstring msg = std::to_wstring(r.placed + r.unplaced) + L" comment" + (r.placed + r.unplaced == 1 ? L"" : L"s") + L" from " + w(r.reader) + L".";
    if (r.placed) msg += L"\n\n" + std::to_wstring(r.placed) + L" placed on the words they were about.";
    if (r.unplaced) msg += L"\n" + std::to_wstring(r.unplaced) + L" on whole chapters, or on words that have changed since; they show with the chapter's comments.";
    if (r.skipped) msg += L"\n" + std::to_wstring(r.skipped) + L" skipped (their chapters are gone).";
    if (std::find(r.nodes.begin(), r.nodes.end(), currentNode_) != r.nodes.end()) sendLoad();
    rv_.allComments = false;
    invalidate();
    cjui::messageBox(hwnd(), msg, L"Comments imported", MB_OK | MB_ICONINFORMATION);
}

// Review: every comment in the book, chapter by chapter.
float StudioApp::drawReviewComments(const Rect& m, float y) {
    const float cw = m.w;
    y += reviewHeader(m, L"Every comment in the book, in the order of the chapters. Choose one to go to its words.");
    const auto all = lib_->commentsInBook(rv_.book);
    int open = 0;
    for (const Comment* c : all) if (!c->resolved) ++open;
    {
        float lx = m.x;
        const std::wstring a = L"Open (" + std::to_wstring(open) + L")", b = L"All (" + std::to_wstring(all.size()) + L")";
        auto tab = [&](const std::string& id, const std::wstring& t, bool on, std::function<void()> act) {
            const float tw = gfx_.measureWidth(t, Font::Small) + 24;
            const Rect r{lx, y, tw, 28};
            addHit(id, r, std::move(act));
            gfx_.fillRounded(r, 3, on ? pal_.surface : pal_.sunken.withAlpha(0.5f));
            if (on) gfx_.strokeRounded(r, 3, pal_.rule);
            gfx_.text(t, r, on ? Font::SmallStrong : Font::Small, on ? pal_.ink : pal_.inkMuted, {Align::Center, false, false, true});
            if (focused(id)) focusRing(r);
            lx += tw + 6;
        };
        tab("rv:cm:open", a, !rv_.allComments, [this] { rv_.allComments = false; invalidate(); });
        tab("rv:cm:all", b, rv_.allComments, [this] { rv_.allComments = true; invalidate(); });
        // Beta readers: a copy of the book they read and comment on in any browser, and their comments brought back.
        const std::wstring imp = L"Import a reader's comments\u2026", mk = L"Make a review copy\u2026";
        const float iw = gfx_.measureWidth(imp, Font::Small), mw = gfx_.measureWidth(mk, Font::Small);
        linkText("rv:cm:import", m.right() - iw - 8, y + 7, imp, [this] { importReaderComments(); });
        linkText("rv:cm:make", m.right() - iw - mw - 44, y + 7, mk, [this] { makeReviewCopy(); });
        y += 44;
    }
    Id lastNode;
    int shown = 0;
    for (const Comment* c : all) {
        if (c->resolved && !rv_.allComments) continue;
        const ManuscriptNode* n = lib_->node(c->nodeId);
        if (!n) continue;
        if (c->nodeId != lastNode) {
            lastNode = c->nodeId;
            gfx_.text(w(n->title.empty() ? std::string(label(n->kind)) : n->title), {m.x, y + 6, cw, 20}, Font::BodyStrong, pal_.ink,
                      {Align::Left, false, true});
            y += 32;
        }
        ++shown;
        const std::wstring body = c->body.empty() ? L"(Nothing written yet)" : w(c->body);
        const float bh = std::min(120.0f, gfx_.measureHeight(body, cw - 40, Font::Body));
        const Rect rr{m.x, y, cw, 58 + bh};
        const std::string hid = "rv:cm:" + c->id;
        addHit(hid, {rr.x, rr.y, rr.w - 120, rr.h}, [this, id = c->id] { showComment(id); });
        const float hv = hoverAmount(hid);
        gfx_.fillRounded(rr, 4, pal_.surface.mix(pal_.sunken, 0.25f * hv));
        gfx_.strokeRounded(rr, 4, pal_.rule);
        gfx_.fill({rr.x, rr.y + 8, 3, rr.h - 16}, c->resolved ? pal_.rule : pal_.demo);
        gfx_.text(c->quote.empty() ? std::wstring(L"On the whole chapter") : L"“" + excerpt(c->quote, 110) + L"”", {rr.x + 18, rr.y + 10, rr.w - 160, 18}, Font::Small, pal_.inkMuted,
                  {Align::Left, false, true});
        gfx_.text(body, {rr.x + 18, rr.y + 32, rr.w - 40, bh + 2}, Font::Body, c->resolved ? pal_.inkFaint : pal_.ink);
        gfx_.text((c->resolved ? L"Resolved · " : L"") + when(c->created), {rr.x + 18, rr.bottom() - 24, rr.w - 160, 18}, Font::Small,
                  pal_.inkFaint, {Align::Left, false, true});
        linkText("rv:cm:res:" + c->id, rr.right() - 100, rr.y + 10, c->resolved ? L"Reopen" : L"Resolve",
                 [this, id = c->id, r = c->resolved != 0] { setCommentResolved(id, !r); });
        if (focused(hid)) focusRing(rr);
        y += rr.h + 10;
    }
    if (!shown)
        y += gfx_.text(all.empty() ? L"No comments yet. In Writing, select some words and choose Comment (✎) in the toolbar (or press Ctrl+Alt+M) to leave one."
                                   : L"Every comment is resolved.",
                       {m.x, y, cw, 50}, Font::Body, pal_.inkMuted) + 12;
    return y;
}

}  // namespace nrs
