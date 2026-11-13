// The Review workspace (Milestone 9): a book's changes against earlier
// versions and snapshots, its snapshots, a check of its names against the
// world's bible, and daily writing goals.
#include "studio_app.hpp"

#include "cjui/format.hpp"

#include "studio/document.hpp"
#include "studio/review.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <nlohmann/json.hpp>
#include <set>

namespace nrs {

using nlohmann::json;
using namespace studio;
namespace rv = studio::review;

static std::wstring w(const std::string& s) { return widen(s); }

static int popupMenu(HWND owner, const std::vector<std::wstring>& items, int checked = -1) {
    HMENU m = CreatePopupMenu();
    for (size_t i = 0; i < items.size(); ++i)
        AppendMenuW(m, MF_STRING | ((int)i == checked ? MF_CHECKED : 0), 1 + (UINT)i, items[i].c_str());
    POINT pt;
    GetCursorPos(&pt);
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD, pt.x, pt.y, 0, owner, nullptr);
    DestroyMenu(m);
    return cmd - 1;
}

static std::wstring num(std::int64_t n) {
    std::wstring s = std::to_wstring(n < 0 ? -n : n), out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i && (s.size() - i) % 3 == 0) out += L',';
        out += s[i];
    }
    return (n < 0 ? L"-" : L"") + out;
}

static std::wstring when(Time t) { return cjui::formatDay((cj::UtcSeconds)t) + L", " + cjui::formatTime((cj::UtcSeconds)t); }

static std::string dayBefore(const std::string& day, int n) {
    std::tm t{};
    t.tm_year = std::stoi(day.substr(0, 4)) - 1900;
    t.tm_mon = std::stoi(day.substr(5, 2)) - 1;
    t.tm_mday = std::stoi(day.substr(8, 2)) - n;
    t.tm_hour = 12;
    std::mktime(&t);
    char b[16];
    std::strftime(b, sizeof b, "%Y-%m-%d", &t);
    return b;
}

std::int64_t StudioApp::dailyGoal() const {
    const std::string s = lib_ ? lib_->setting("goal:daily") : std::string();
    return s.empty() ? 0 : std::max<std::int64_t>(0, std::strtoll(s.c_str(), nullptr, 10));
}

// ------------------------------------------------------------- opening

void StudioApp::openReview(const Id& bookId, int section) {
    if (!lib_ || !lib_->book(bookId)) return;
    commitFields();
    if (ws_ == Workspace::Writing && editor_.ready()) editor_.send(R"({"type":"flush"})");
    if (rv_.book != bookId) {
        rv_ = {};
        rv_.book = bookId;
        // Compare the chapter being written, if it's this book's.
        if (const ManuscriptNode* n = lib_->node(currentNode_); n && n->bookId == bookId) rv_.node = n->id;
    }
    if (section >= 0) rv_.section = section;
    ws_ = Workspace::Review;
    rv_.diffKey.clear();       // compared again: the text may have changed
    rv_.namesDone = false;
    rv_.bookCompareFor.clear();
    reviewFocus();
    invalidate();
}

void StudioApp::reviewFocus() {
    if (rv_.section == 1 && !rv_.snapshot.empty()) setFocus({Focus::Snapshot, rv_.snapshot, {}});
    else if (rv_.section == 3) setFocus({Focus::Goal, rv_.book, {}});
    else setFocus({Focus::Book, rv_.book, {}});
}

void StudioApp::showReviewSection(int s) {
    commitFields();
    rv_.section = s;
    rv_.scroll = 0;
    reviewFocus();
    invalidate();
}

// What the chosen chapter is compared with: (label, document JSON).
bool StudioApp::reviewAgainst(std::wstring& label, std::string& content) const {
    if (rv_.against.rfind("v:", 0) == 0) {
        const std::int64_t vid = std::strtoll(rv_.against.c_str() + 2, nullptr, 10);
        for (const NodeVersion& v : lib_->nodeVersions(rv_.node))
            if (v.id == vid) {
                label = L"the version saved " + when(v.saved);
                content = lib_->nodeVersionContent(vid);
                return true;
            }
    } else if (rv_.against.rfind("s:", 0) == 0) {
        const Id sid = rv_.against.substr(2);
        for (const Snapshot& s : lib_->snapshotsOf(rv_.book))
            if (s.id == sid)
                for (const SnapshotNode& n : lib_->snapshotNodes(sid))
                    if (n.nodeId == rv_.node) {
                        label = L"the snapshot “" + w(s.name) + L"”";
                        content = n.content;
                        return true;
                    }
    }
    return false;
}

void StudioApp::compareWith(const Id& nodeId, const std::string& against) {
    rv_.node = nodeId;
    rv_.against = against;
    rv_.diffKey.clear();
    showReviewSection(0);
}

// ---------------------------------------------------------------- sidebar

void StudioApp::drawReviewSidebar(const Rect& r) {
    gfx_.fill(r, pal_.sunken);
    gfx_.line(r.right() - 0.5f, r.y, r.right() - 0.5f, r.bottom(), pal_.rule);
    const studio::Book* b = lib_->book(rv_.book);
    if (!b) return;
    gfx_.pushClip(r);
    setHitClip(r);
    const float x = r.x + 10, wd = r.w - 20, tx0 = x + 12, cw = wd - 24;
    float y = r.y + 14 - rv_.sideScroll;
    linkText("side:rv:library", tx0, y, L"‹ Library", [this] { ws_ = Workspace::Library; goTo(View::Book, rv_.book); });
    y += 30;
    gfx_.text(L"REVIEW", {tx0, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 20;
    y += gfx_.text(w(b->title), {tx0, y, cw, 90}, Font::SerifTitle, pal_.ink) + 22;
    struct S { const wchar_t* name; const wchar_t* note; };
    const S sections[] = {{L"Changes", L"Compare with an earlier version"},
                          {L"Snapshots", L"The whole book, saved to compare"},
                          {L"Names", L"Spellings and names not in the bible"},
                          {L"Goals", L"Words written each day"},
                          {L"Comments", L"Notes left on the text"}};
    for (int i : {0, 1, 2, 4, 3}) {
        const std::string id = "side:rv:s" + std::to_string(i);
        const Rect rr{x, y, wd, 48};
        addHit(id, rr, [this, i] { showReviewSection(i); });
        const bool sel = rv_.section == i;
        const float hv = hoverAmount(id);
        if (sel) { gfx_.fillRounded(rr, 3, pal_.surface); gfx_.fill({rr.x, rr.y + 8, 3, rr.h - 16}, pal_.ribbon); }
        else if (hv > 0.01f) gfx_.fillRounded(rr, 3, pal_.surface.withAlpha(0.6f * hv));
        gfx_.text(sections[i].name, {tx0, rr.y + 6, cw, 20}, sel ? Font::BodyStrong : Font::Body, pal_.ink, {Align::Left, false});
        gfx_.text(sections[i].note, {tx0, rr.y + 26, cw, 18}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
        if (focused(id)) focusRing(rr);
        y += 52;
    }
    // Today, at a glance.
    y += 16;
    const std::string today = Library::localDay(now());
    const auto days = lib_->wordsWritten({}, today);
    const std::int64_t written = days.count(today) ? days.at(today) : 0;
    const std::int64_t goal = dailyGoal();
    gfx_.text(L"TODAY", {tx0, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 22;
    gfx_.text(num(written) + (goal ? L" of " + num(goal) + L" words" : L" words written"), {tx0, y, cw, 20}, Font::Body, pal_.ink,
              {Align::Left, false});
    y += 24;
    if (goal) {
        const float f = (float)std::min(1.0, (double)written / goal);
        gfx_.fillRounded({tx0, y, cw, 6}, 3, pal_.rule);
        if (f > 0) gfx_.fillRounded({tx0, y, std::max(6.0f, cw * f), 6}, 3, written >= goal ? pal_.ink : pal_.ribbon);
        y += 14;
    }
    rv_.sideContent = y + rv_.sideScroll - r.y + 20;
    clearHitClip();
    gfx_.popClip();
}

// ------------------------------------------------------------- the page

void StudioApp::drawReviewPage(const Rect& r) {
    const studio::Book* b = lib_->book(rv_.book);
    if (!b) return;
    gfx_.pushClip(r);
    setHitClip(r);
    const float pad = r.w > 900 ? 40 : 28;
    const float cw = std::min(r.w - pad * 2, 760.0f);
    const Rect m{r.x + pad, r.y + 26 - rv_.scroll, cw, 0};
    float y = m.y;
    const auto nodes = lib_->manuscriptOf(rv_.book);
    auto nodeName = [&](const ManuscriptNode* n) { return w(n->title.empty() ? std::string(label(n->kind)) : n->title); };

    if (rv_.section == 0) {
        // ---- Changes.
        y += pageHeader(m, L"REVIEW", L"Changes", L"Compare a chapter or scene with a version saved while you wrote, or with a snapshot.");
        const ManuscriptNode* n = lib_->node(rv_.node);
        if (n && (n->deleted || n->bookId != rv_.book)) n = nullptr;
        if (!n) for (auto* x : nodes) if (x->kind != NodeKind::Part || x->wordCount) { n = x; rv_.node = x->id; break; }
        if (!n) {
            y += gfx_.text(L"The book has no chapters yet.", {m.x, y, cw, 24}, Font::Body, pal_.inkMuted) + 20;
        } else {
            // What to compare.
            const float half = (cw - 16) / 2;
            gfx_.text(L"Chapter or scene", {m.x, y, half, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
            gfx_.text(L"Compared with", {m.x + half + 16, y, half, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
            y += 20;
            const Rect nb{m.x, y, half, 30};
            button("rv:node", nb, nodeName(n), [this, nodes] {
                std::vector<std::wstring> items;
                int cur = -1;
                for (size_t i = 0; i < nodes.size(); ++i) {
                    const ManuscriptNode* x = nodes[i];
                    items.push_back(std::wstring(x->kind == NodeKind::Scene ? L"      " : L"") + w(x->title.empty() ? std::string(label(x->kind)) : x->title));
                    if (x->id == rv_.node) cur = (int)i;
                }
                const int i = popupMenu(hwnd(), items, cur);
                if (i < 0) return;
                rv_.node = nodes[i]->id;
                rv_.against.clear();
                rv_.diffKey.clear();
                invalidate();
            });
            gfx_.fillTriangle(nb.right() - 20, nb.y + 13, nb.right() - 10, nb.y + 13, nb.right() - 15, nb.y + 18, pal_.inkMuted);
            // Versions saved while writing, and snapshots that have this chapter.
            std::vector<std::pair<std::string, std::wstring>> choices;
            for (const Snapshot& s : lib_->snapshotsOf(rv_.book))
                for (const SnapshotNode& sn : lib_->snapshotNodes(s.id))
                    if (sn.nodeId == n->id) { choices.push_back({"s:" + s.id, L"Snapshot: " + w(s.name) + L" (" + cjui::formatDay((cj::UtcSeconds)s.taken) + L")"}); break; }
            for (const NodeVersion& v : lib_->nodeVersions(n->id))
                choices.push_back({"v:" + std::to_string(v.id), when(v.saved) + L" · " + num(v.wordCount) + L" words" +
                                                                    (v.reason == "autosave" ? L"" : L" · " + w(v.reason))});
            if (rv_.against.empty() && !choices.empty()) rv_.against = choices.front().first;
            std::wstring againstLabel = L"Nothing saved yet";
            for (auto& c : choices) if (c.first == rv_.against) againstLabel = c.second;
            const Rect ab{m.x + half + 16, y, half, 30};
            if (choices.empty()) disabledButton(ab, againstLabel);
            else {
                button("rv:against", ab, againstLabel, [this, choices] {
                    std::vector<std::wstring> items;
                    int cur = -1;
                    for (size_t i = 0; i < choices.size(); ++i) {
                        items.push_back(choices[i].second);
                        if (choices[i].first == rv_.against) cur = (int)i;
                    }
                    const int i = popupMenu(hwnd(), items, cur);
                    if (i < 0) return;
                    rv_.against = choices[i].first;
                    rv_.diffKey.clear();
                    invalidate();
                });
                gfx_.fillTriangle(ab.right() - 20, ab.y + 13, ab.right() - 10, ab.y + 13, ab.right() - 15, ab.y + 18, pal_.inkMuted);
            }
            y += 46;
            std::wstring label;
            std::string before;
            if (choices.empty()) {
                y += gfx_.text(L"No earlier versions yet. The Studio keeps one every ten minutes or so while you write, and a snapshot "
                               L"keeps the whole book whenever you take one.",
                               {m.x, y, cw, 60}, Font::Small, pal_.inkFaint) + 16;
            } else if (reviewAgainst(label, before)) {
                // Compare (once per choice and text).
                const std::string now = lib_->nodeContent(n->id);
                const std::string key = n->id + "|" + rv_.against + "|" + std::to_string(std::hash<std::string>{}(now));
                if (rv_.diffKey != key) {
                    rv_.diff = rv::diffText(doc::plainText(before), doc::plainText(now));
                    rv_.diffStats = rv::stats(rv_.diff);
                    rv_.diffKey = key;
                    rv_.diffLayoutW = -1;
                }
                const auto& st = rv_.diffStats;
                std::wstring summary = st.changed() ? L"Since " + label + L": " + num(st.added) + L" words added, " + num(st.removed) + L" removed."
                                                    : L"No changes since " + label + L".";
                gfx_.text(summary, {m.x, y, cw - 150, 40}, Font::Body, pal_.ink, {Align::Left, true});
                if (st.changed())
                    button("rv:restore", {m.right() - 140, y - 4, 140, 30}, L"Restore it...", [this, label, before] {
                        const std::wstring msg = L"Replace the text of this chapter with " + label +
                                                 L"? The text as it is now is kept as an earlier version, so this can be undone.";
                        if (MessageBoxW(hwnd(), msg.c_str(), L"Restore", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
                        if (lib_->restoreNodeText(rv_.node, before)) { rv_.diffKey.clear(); rv_.against.clear(); flash(L"Restored.", 4); }
                        else flash(w(lib_->lastError()), 8);
                        invalidate();
                    });
                y += 34;
                if (!st.changed()) { y += 10; goto changesDone; }
                // Key.
                {
                    float kx = m.x;
                    gfx_.fillRounded({kx, y + 3, 14, 14}, 2, pal_.ribbon.withAlpha(0.22f));
                    gfx_.text(L"Added", {kx + 20, y, 80, 20}, Font::Small, pal_.inkMuted, {Align::Left, false});
                    kx += 80;
                    gfx_.text(L"Removed", {kx + 20, y, 80, 20}, Font::Small, pal_.live, {Align::Left, false, false, false, true});
                    gfx_.line(kx, y + 10, kx + 14, y + 10, pal_.live, 1.5f);
                    y += 30;
                }
                // The text, with the changes marked: laid out word by word once per width.
                if (rv_.diffLayoutW != cw) {
                    rv_.words.clear();
                    const float lineH = 25, spaceW = gfx_.measureWidth(L"x x", Font::Serif) - gfx_.measureWidth(L"xx", Font::Serif);
                    float x = 0, ly = 0;
                    for (const auto& p : rv_.diff) {
                        const std::wstring t = w(p.text);
                        size_t i = 0;
                        while (i < t.size()) {
                            if (t[i] == L'\n') {
                                // A paragraph break ("\n\n"): a new line with a little space.
                                size_t j = i;
                                while (j < t.size() && t[j] == L'\n') ++j;
                                if (j - i >= 2 || x > 0) { x = 0; ly += lineH + (j - i >= 2 ? 10 : 0); }
                                i = j;
                                continue;
                            }
                            if (t[i] == L' ') { ++i; continue; }
                            size_t j = i;
                            while (j < t.size() && t[j] != L' ' && t[j] != L'\n') ++j;
                            const std::wstring word = t.substr(i, j - i);
                            const float ww = gfx_.measureWidth(word, Font::Serif);
                            if (x > 0 && x + ww > cw) { x = 0; ly += lineH; }
                            rv_.words.push_back({x, ly, ww, word, (int)p.kind});
                            x += ww + spaceW;
                            i = j;
                        }
                    }
                    rv_.diffHeight = ly + lineH;
                    rv_.diffLayoutW = cw;
                }
                const float top = y;
                for (size_t wi = 0; wi < rv_.words.size(); ++wi) {
                    const auto& wd = rv_.words[wi];
                    const float wy = top + wd.y;
                    if (wy + 25 < r.y || wy > r.bottom()) continue;
                    if (wd.kind == rv::DiffPart::Added) {
                        // One shade for a run of added words on a line.
                        const bool runOn = wi + 1 < rv_.words.size() && rv_.words[wi + 1].kind == wd.kind && rv_.words[wi + 1].y == wd.y;
                        const float right = runOn ? rv_.words[wi + 1].x : wd.x + wd.w;
                        gfx_.fill({m.x + wd.x - 2, wy + 1, right - wd.x + (runOn ? 0 : 4), 22}, pal_.ribbon.withAlpha(0.18f));
                        gfx_.text(wd.text, {m.x + wd.x, wy, wd.w + 4, 24}, Font::Serif, pal_.ink, {Align::Left, false});
                    } else if (wd.kind == rv::DiffPart::Removed) {
                        gfx_.text(wd.text, {m.x + wd.x, wy, wd.w + 4, 24}, Font::Serif, pal_.live, {Align::Left, false, false, false, true});
                    } else {
                        gfx_.text(wd.text, {m.x + wd.x, wy, wd.w + 4, 24}, Font::Serif, pal_.inkMuted, {Align::Left, false});
                    }
                }
                y = top + rv_.diffHeight + 20;
            }
        changesDone:;
        }
    } else if (rv_.section == 1) {
        // ---- Snapshots.
        y += pageHeader(m, L"REVIEW", L"Snapshots",
                        L"A snapshot saves the whole book as it is now, to compare with later: before a revision, before sending it to an "
                        L"editor, at the end of a draft.");
        button("rv:take", {m.x, y, 190, 34}, L"Take a snapshot", [this] {
            commitFields();
            wchar_t name[64];
            const std::time_t t = std::time(nullptr);
            std::tm lt{};
            localtime_s(&lt, &t);
            wcsftime(name, 64, L"Snapshot of %d %B %Y, %H:%M", &lt);
            const Id id = lib_->takeSnapshot(rv_.book, narrow(name));
            if (id.empty()) { flash(w("Couldn't take the snapshot: " + lib_->lastError()), 8); return; }
            rv_.snapshot = id;
            rv_.bookCompareFor.clear();
            reviewFocus();
            flash(L"Snapshot taken. Give it a name in the panel on the right.", 6);
            if (fields_[FName]) { loadFields(); SetFocus(fields_[FName]); SendMessageW(fields_[FName], EM_SETSEL, 0, -1); }
        }, true);
        y += 54;
        const auto snaps = lib_->snapshotsOf(rv_.book);
        if (snaps.empty()) y += gfx_.text(L"No snapshots yet.", {m.x, y, cw, 24}, Font::Body, pal_.inkMuted) + 12;
        for (const Snapshot& s : snaps) {
            const std::string id = "rv:snap:" + s.id;
            const Rect rr{m.x - 8, y, cw + 16, 52};
            addHit(id, rr, [this, sid = s.id] {
                commitFields();
                rv_.snapshot = sid;
                rv_.bookCompareFor.clear();
                reviewFocus();
                invalidate();
            });
            const bool sel = rv_.snapshot == s.id;
            const float hv = hoverAmount(id);
            if (sel) { gfx_.fillRounded(rr, 3, pal_.surface); gfx_.fill({rr.x, rr.y + 8, 3, rr.h - 16}, pal_.ribbon); }
            else if (hv > 0.01f) gfx_.fillRounded(rr, 3, pal_.surface.withAlpha(0.7f * hv));
            gfx_.text(w(s.name.empty() ? std::string("Snapshot") : s.name), {m.x + 6, y + 7, cw - 200, 20}, Font::BodyStrong, pal_.ink,
                      {Align::Left, false, true});
            gfx_.text(when(s.taken), {m.x + 6, y + 28, cw - 200, 18}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
            gfx_.text(num(s.words) + L" words", {m.right() - 190, y + 7, 190, 20}, Font::Small, pal_.inkMuted, {Align::Right, false});
            if (focused(id)) focusRing(rr);
            y += 56;
        }
        // The selected snapshot against the book now, chapter by chapter.
        const Snapshot* sel = nullptr;
        for (const Snapshot& s : snaps) if (s.id == rv_.snapshot) sel = &s;
        if (sel) {
            y += 18;
            gfx_.text(L"SINCE “" + w(sel->name) + L"”", {m.x, y, cw, 18}, Font::Label, pal_.inkFaint, {Align::Left, false, true});
            y += 28;
            if (rv_.bookCompareFor != sel->id) {
                rv_.bookCompare.clear();
                const auto then = lib_->snapshotNodes(sel->id);
                std::map<Id, const SnapshotNode*> byId;
                for (auto& sn : then) byId[sn.nodeId] = &sn;
                for (const ManuscriptNode* nd : nodes) {
                    ReviewRow row;
                    row.node = nd->id;
                    row.title = nodeName(nd);
                    auto it = byId.find(nd->id);
                    const std::string nowText = doc::plainText(lib_->nodeContent(nd->id));
                    if (it == byId.end()) { row.isNew = true; row.st.added = (std::int64_t)nd->wordCount; }
                    else row.st = rv::stats(rv::diffText(doc::plainText(it->second->content), nowText));
                    if (row.isNew || row.st.changed()) rv_.bookCompare.push_back(row);
                    if (it != byId.end()) byId.erase(it);
                }
                for (auto& [nid, sn] : byId) {
                    ReviewRow row;
                    row.node = nid;
                    row.title = w(sn->title);
                    row.gone = true;
                    row.st.removed = sn->words;
                    rv_.bookCompare.push_back(row);
                }
                rv_.bookCompareFor = sel->id;
            }
            if (rv_.bookCompare.empty()) y += gfx_.text(L"Nothing has changed since this snapshot.", {m.x, y, cw, 24}, Font::Body, pal_.inkMuted) + 12;
            std::int64_t ta = 0, tr = 0;
            for (auto& row : rv_.bookCompare) { ta += row.st.added; tr += row.st.removed; }
            if (!rv_.bookCompare.empty())
                y += gfx_.text(std::to_wstring(rv_.bookCompare.size()) + (rv_.bookCompare.size() == 1 ? L" chapter or scene changed: " : L" chapters and scenes changed: ") +
                                   num(ta) + L" words added, " + num(tr) + L" removed.",
                               {m.x, y, cw, 40}, Font::Small, pal_.inkMuted) + 10;
            for (auto& row : rv_.bookCompare) {
                const std::string id = "rv:row:" + row.node;
                const Rect rr{m.x - 8, y, cw + 16, 32};
                if (!row.gone) addHit(id, rr, [this, nid = row.node, sid = sel->id] { compareWith(nid, row_is_new_guard(nid, sid)); });
                const float hv = row.gone ? 0 : hoverAmount(id);
                if (hv > 0.01f) gfx_.fillRounded(rr, 3, pal_.surface.withAlpha(0.8f * hv));
                gfx_.text(row.title, {m.x, y, cw - 260, 32}, Font::Body, row.gone ? pal_.inkFaint : pal_.ink, {Align::Left, false, true, true});
                std::wstring what = row.isNew ? L"New since then" : row.gone ? L"Removed or moved to the Trash" : L"";
                if (!row.isNew && !row.gone) what = L"+" + num(row.st.added) + L"   −" + num(row.st.removed);
                gfx_.text(what, {m.right() - 250, y, 250, 32}, Font::Small, pal_.inkMuted, {Align::Right, false, false, true});
                gfx_.line(m.x, y + 31.5f, m.right(), y + 31.5f, pal_.rule.withAlpha(0.5f));
                y += 34;
            }
        }
    } else if (rv_.section == 2) {
        // ---- Names.
        y += pageHeader(m, L"REVIEW", L"Names",
                        L"Names in the manuscript checked against the world's bible: spellings close to a known name, and names used "
                        L"often that aren't in it yet.");
        if (!rv_.namesDone) {
            rv_.names = rv::nameChecks(*lib_, rv_.book);
            rv_.namesDone = true;
        }
        // Names the author has said are fine.
        std::set<std::string> ignored;
        {
            const json j = json::parse(lib_->setting("names-ok:" + rv_.book), nullptr, false);
            if (j.is_array()) for (auto& x : j) if (x.is_string()) ignored.insert(x.get<std::string>());
        }
        int shown = 0;
        for (const auto& is : rv_.names) {
            if (ignored.count(is.name)) continue;
            ++shown;
            const bool variant = is.kind == rv::NameIssue::Variant;
            gfx_.circle(m.x + 7, y + 13, 6, variant ? pal_.demo : pal_.inkFaint, !variant, 1.6f);
            gfx_.text(w(is.name), {m.x + 24, y + 2, cw * 0.5f, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
            std::wstring note = variant ? L"looks like " + w(is.like) : L"not in the bible";
            note += L" · " + std::to_wstring(is.count) + (is.count == 1 ? L" time" : L" times") + L" in " + std::to_wstring(is.nodes.size()) +
                    (is.nodes.size() == 1 ? L" chapter" : L" chapters");
            gfx_.text(note, {m.x + 24, y + 24, cw - 24, 18}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
            // Actions.
            float ax = m.x + 24;
            const float ay = y + 46;
            auto act = [&](const std::string& id, const std::wstring& t, std::function<void()> f) {
                linkText(id, ax, ay, t, std::move(f));
                ax += gfx_.measureWidth(t, Font::Small) + 18;
            };
            if (!is.nodes.empty())
                act("rv:nm:open:" + is.name, L"Show it", [this, nid = is.nodes.front()] {
                    writingBook_ = rv_.book;
                    currentNode_ = nid;
                    openWriting(rv_.book);
                });
            if (variant)
                act("rv:nm:alias:" + is.name, L"It's another name for " + w(is.like), [this, eid = is.entryId, nm = is.name] {
                    if (const Entry* e = lib_->entry(eid)) {
                        Entry u = *e;
                        u.aliases.push_back(nm);
                        lib_->updateEntry(u);
                        rv_.namesDone = false;
                        flash(L"Added “" + w(nm) + L"” as another name for " + w(e->name) + L".", 5);
                    }
                });
            act("rv:nm:add:" + is.name, L"Add to the bible", [this, nm = is.name] {
                const studio::Book* bk = lib_->book(rv_.book);
                if (!bk) return;
                const Id id = lib_->createEntry(bk->worldId, bk->worldId.empty() ? bk->id : Id{}, "character", nm);
                if (id.empty()) { flash(w(lib_->lastError()), 6); return; }
                rv_.namesDone = false;
                flash(L"Added “" + w(nm) + L"” to the bible as a character. Change its kind in Worldbuilding if it isn't one.", 6);
            });
            act("rv:nm:ok:" + is.name, L"It's fine", [this, nm = is.name] {
                json j = json::parse(lib_->setting("names-ok:" + rv_.book), nullptr, false);
                if (!j.is_array()) j = json::array();
                j.push_back(nm);
                lib_->setSetting("names-ok:" + rv_.book, j.dump());
                invalidate();
            });
            y += 74;
            gfx_.line(m.x, y - 6.5f, m.right(), y - 6.5f, pal_.rule.withAlpha(0.5f));
        }
        if (!shown)
            y += gfx_.text(rv_.names.empty() ? L"Nothing to check: every name used often is in the bible, and none looks like a misspelling of one."
                                             : L"Everything here has been checked.",
                           {m.x, y, cw, 40}, Font::Body, pal_.inkMuted) + 12;
        if (!ignored.empty()) {
            y += 10;
            linkText("rv:nm:reset", m.x, y, L"Check the names marked as fine again (" + std::to_wstring(ignored.size()) + L")", [this] {
                lib_->setSetting("names-ok:" + rv_.book, "[]");
                invalidate();
            });
            y += 30;
        }
    } else if (rv_.section == 4) {
        y = drawReviewComments(m, y);
    } else {
        // ---- Goals.
        y += pageHeader(m, L"REVIEW", L"Goals", L"Words you add while writing, day by day, across every book. Imported text doesn't count.");
        const std::string today = Library::localDay(now());
        const std::string from = dayBefore(today, 27);
        const auto all = lib_->wordsWritten({}, from);
        const auto mine = lib_->wordsWritten(rv_.book, from);
        const std::int64_t goal = dailyGoal();
        // Streak: days in a row (to today, or to yesterday if today isn't done yet) meeting the goal, or with any words.
        int streak = 0;
        {
            const auto longer = lib_->wordsWritten({}, dayBefore(today, 400));
            auto met = [&](const std::string& d) { auto it = longer.find(d); return it != longer.end() && it->second >= std::max<std::int64_t>(1, goal); };
            std::string d = met(today) ? today : dayBefore(today, 1);
            while (met(d) && streak < 400) { ++streak; d = dayBefore(d, 1); }
        }
        std::int64_t week = 0, month = 0;
        for (int i = 0; i < 28; ++i) {
            const std::string d = dayBefore(today, i);
            const std::int64_t v = all.count(d) ? all.at(d) : 0;
            month += v;
            if (i < 7) week += v;
        }
        // Figures.
        const float fw = cw / 4;
        auto figure = [&](int i, const std::wstring& big, const std::wstring& small) {
            gfx_.text(big, {m.x + fw * i, y, fw - 10, 34}, Font::Title, pal_.ink, {Align::Left, false});
            gfx_.text(small, {m.x + fw * i, y + 36, fw - 10, 18}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
        };
        const std::int64_t todayWords = all.count(today) ? all.at(today) : 0;
        figure(0, num(todayWords), goal ? L"today, of " + num(goal) : L"today");
        figure(1, num(week), L"in the last 7 days");
        figure(2, num(month), L"in the last 28 days");
        figure(3, std::to_wstring(streak), streak == 1 ? L"day in a row" : L"days in a row");
        y += 80;
        // The last 28 days.
        const float ch = 150;
        std::int64_t top = std::max<std::int64_t>(goal, 1);
        for (auto& [d, v] : all) top = std::max(top, v);
        const float bw = cw / 28;
        gfx_.line(m.x, y + ch + 0.5f, m.right(), y + ch + 0.5f, pal_.rule);
        if (goal) {
            const float gy = y + ch - ch * (float)goal / top;
            for (float gx = m.x; gx < m.right(); gx += 8) gfx_.line(gx, gy, std::min(gx + 4, m.right()), gy, pal_.inkFaint);
            gfx_.text(L"Goal", {m.right() - 60, gy - 18, 60, 16}, Font::Small, pal_.inkFaint, {Align::Right, false});
        }
        for (int i = 27; i >= 0; --i) {
            const std::string d = dayBefore(today, i);
            const std::int64_t v = all.count(d) ? all.at(d) : 0, vb = mine.count(d) ? mine.at(d) : 0;
            const float x0 = m.x + (27 - i) * bw;
            const float hAll = ch * (float)v / top, hMine = ch * (float)vb / top;
            if (v > 0) gfx_.fillRounded({x0 + 2, y + ch - hAll, bw - 4, hAll}, 2, pal_.ribbon.withAlpha(0.3f));
            if (vb > 0) gfx_.fillRounded({x0 + 2, y + ch - hMine, bw - 4, hMine}, 2, pal_.ribbon);
            if (i % 7 == 0)
                gfx_.text(w(d.substr(8, 2) + "/" + d.substr(5, 2)), {x0 - 10, y + ch + 6, bw + 20, 16}, Font::Small, pal_.inkFaint, {Align::Center, false});
        }
        y += ch + 32;
        gfx_.fillRounded({m.x, y + 3, 12, 12}, 2, pal_.ribbon);
        gfx_.text(w(b->title), {m.x + 18, y, 260, 18}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
        gfx_.fillRounded({m.x + 290, y + 3, 12, 12}, 2, pal_.ribbon.withAlpha(0.3f));
        gfx_.text(L"Other books", {m.x + 308, y, 200, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
        y += 34;
    }
    rv_.content = y + rv_.scroll - r.y + 40;
    rv_.scroll = std::clamp(rv_.scroll, 0.0f, std::max(0.0f, rv_.content - r.h));
    clearHitClip();
    gfx_.popClip();
}

std::string StudioApp::row_is_new_guard(const Id& nodeId, const Id& snapshotId) const {
    // A chapter new since the snapshot has nothing there to compare with: its own versions instead.
    for (const SnapshotNode& sn : lib_->snapshotNodes(snapshotId)) if (sn.nodeId == nodeId) return "s:" + snapshotId;
    return {};
}

void StudioApp::drawReviewPanel(const Rect& r) {
    gfx_.fill(r, pal_.paper.mix(pal_.surface, 0.5f));
    gfx_.line(r.x + 0.5f, r.y, r.x + 0.5f, r.bottom(), pal_.rule);
    if (!(focus_ == loadedFocus_)) loadFields();
    gfx_.pushClip(r);
    setHitClip(r);
    const Rect c{r.x + 20, r.y, r.w - 40, 0};
    float y = r.y + 18;
    if (rv_.section == 0) {
        panelHeading(c, y, L"CHANGES", true);
        y += gfx_.text(L"Words added are shaded; words removed are struck through in red.\n\nRestoring keeps the current text as an "
                       L"earlier version, so it can be undone.",
                       {c.x, y, c.w, 200}, Font::Small, pal_.inkMuted) + 14;
    } else if (rv_.section == 1) {
        bool any = false;
        for (const Snapshot& s : lib_->snapshotsOf(rv_.book))
            if (s.id == rv_.snapshot) {
                any = true;
                panelHeading(c, y, L"SNAPSHOT", true);
                y = panelField(c, y, L"Name", FName);
                gfx_.text(L"Taken " + when(s.taken), {c.x, y, c.w, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
                y += 22;
                gfx_.text(num(s.words) + L" words in " + std::to_wstring(s.parts) + L" chapters, scenes and parts", {c.x, y, c.w, 18}, Font::Small,
                          pal_.inkMuted, {Align::Left, false, true});
                y += 34;
                linkText("rv:snap:remove", c.x, y, L"Remove this snapshot", [this, sid = s.id, name = s.name] {
                    const std::wstring msg = L"Remove the snapshot “" + w(name) + L"”? The book itself isn't changed.";
                    if (MessageBoxW(hwnd(), msg.c_str(), L"Remove snapshot", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
                    focus_ = {};
                    loadedFocus_ = {Focus::None, "-", ""};
                    lib_->removeSnapshot(sid);
                    rv_.snapshot.clear();
                    reviewFocus();
                    invalidate();
                });
                y += 34;
            }
        if (!any) {
            panelHeading(c, y, L"SNAPSHOTS", true);
            y += gfx_.text(L"Select a snapshot to see what has changed since, chapter by chapter. Select a chapter there to compare its "
                           L"text word by word.\n\nSnapshots are kept until you remove them.",
                           {c.x, y, c.w, 200}, Font::Small, pal_.inkMuted) + 14;
        }
    } else if (rv_.section == 2) {
        panelHeading(c, y, L"NAMES", true);
        y += gfx_.text(L"A name is a word capitalised in the middle of a sentence that's never written in lower case. Names are "
                       L"compared with every entry's name and other names in the world's bible.\n\nA spelling one or two letters from a "
                       L"known name is shown first. Names used three times or more that aren't in the bible come next.",
                       {c.x, y, c.w, 300}, Font::Small, pal_.inkMuted) + 14;
    } else if (rv_.section == 4) {
        panelHeading(c, y, L"COMMENTS", true);
        y += gfx_.text(L"A comment is a note on some words of the text, for you or a reader helping you. Make one in Writing: select the "
                       L"words, then choose Comment (✎) in the toolbar or press Ctrl+Alt+M.\n\nComments are kept in the library, never in what "
                       L"you publish. Resolve one once it's dealt with; it's kept and can be reopened.",
                       {c.x, y, c.w, 300}, Font::Small, pal_.inkMuted) + 14;
    } else {
        panelHeading(c, y, L"YOUR GOAL", true);
        y = panelField(c, y, L"Words a day (blank: none)", FTarget);
        y += gfx_.text(L"Counted across every book. A streak is the days in a row you've reached it (or written anything, with no goal).",
                       {c.x, y, c.w, 100}, Font::Small, pal_.inkMuted) + 14;
    }
    clearHitClip();
    gfx_.popClip();
}

}  // namespace nrs
