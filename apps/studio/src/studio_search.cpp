// The search box in the top bar (UI pass, 0.64): one place to find chapters and scenes, the bible, books, worlds and
// commands. Typing lists what matches below it; Up and Down choose, Enter opens, Esc leaves it. Ctrl+K goes to it.
#include "studio_app.hpp"

#include "cjui/icons.hpp"

#include <algorithm>
#include <cwctype>

namespace nrs {

using namespace studio;
using cjui::Align;
using cjui::Font;
using cjui::Rect;

namespace {
std::wstring w(const std::string& s) { return cjui::widen(s); }
std::wstring lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)std::towlower(c);
    return s;
}
// Every word typed is somewhere in the text (in any order).
bool matches(const std::wstring& text, const std::vector<std::wstring>& words) {
    const std::wstring t = lower(text);
    for (auto& wd : words)
        if (t.find(wd) == std::wstring::npos) return false;
    return true;
}
}  // namespace

void StudioApp::focusTopSearch() {
    if (!fields_[FTopSearch]) return;
    SetFocus(fields_[FTopSearch]);
    SendMessageW(fields_[FTopSearch], EM_SETSEL, 0, -1);
    topSel_ = 0;
    invalidate();
}

std::vector<StudioApp::SearchHit> StudioApp::searchHits() const {
    std::vector<SearchHit> hits;
    const std::wstring q = lower(w(fieldText(FTopSearch)));
    std::vector<std::wstring> words;
    for (size_t i = 0; i < q.size();) {
        while (i < q.size() && q[i] == L' ') ++i;
        size_t j = i;
        while (j < q.size() && q[j] != L' ') ++j;
        if (j > i) words.push_back(q.substr(i, j - i));
        i = j;
    }
    if (words.empty()) return hits;
    const size_t kMost = 12;
    auto add = [&](SearchHit h) {
        if (hits.size() < kMost && matches(h.label + L" " + h.right, words)) hits.push_back(std::move(h));
    };
    // Commands first when their words match: they're few, and what you type for them is short.
    struct C {
        const wchar_t* label;
        const wchar_t* right;
        int cmd;
        cjui::Icon icon;
    };
    static const C commands[] = {
        {L"New book", L"command  Ctrl+N", 102, cjui::Icon::Plus},
        {L"New series", L"command  Ctrl+Shift+N", 101, cjui::Icon::Plus},
        {L"New world", L"command  Ctrl+Shift+W", 100, cjui::Icon::Plus},
        {L"Import a book", L"command  Ctrl+Shift+I", -1, cjui::Icon::Import},
        {L"Export the library", L"command  Ctrl+E", 105, cjui::Icon::Export},
        {L"Back up now", L"command", 106, cjui::Icon::Archive},
        {L"Open another library", L"command  Ctrl+O", 103, cjui::Icon::Folder},
        {L"Show the library's folder", L"command", 104, cjui::Icon::Folder},
        {L"Settings", L"command  Ctrl+,", -2, cjui::Icon::Settings},
        {L"Keyboard shortcuts", L"command  Ctrl+/", -3, cjui::Icon::More},
        {L"Show or hide the left side", L"command  F3", -4, cjui::Icon::Sidebar},
        {L"Show or hide the details", L"command  F4", -5, cjui::Icon::Panel},
        {L"Light theme", L"command", -6, cjui::Icon::Eye},
        {L"Dark theme", L"command", -7, cjui::Icon::Eye},
        {L"Theme as Windows has it", L"command", -8, cjui::Icon::Eye},
    };
    for (auto& c : commands) add({c.label, c.right, 3, {}, c.cmd, c.icon});
    static const wchar_t* places[] = {L"Library", L"Writing", L"Planning", L"Worldbuilding", L"Review", L"Typesetting", L"Preview", L"Publishing"};
    static const cjui::Icon placeIcons[] = {cjui::Icon::Library, cjui::Icon::Writing, cjui::Icon::Planning, cjui::Icon::World,
                                            cjui::Icon::Review, cjui::Icon::Typeset, cjui::Icon::Preview, cjui::Icon::Publish};
    for (int i = 0; i < 8; ++i) add({places[i], L"go to  Ctrl+" + std::to_wstring(i + 1), 4, {}, i, placeIcons[i]});
    if (!lib_) return hits;
    // The book in hand: its chapters and scenes, and its bible.
    const Id bookId = ws_ == Workspace::Writing && lib_->book(writingBook_) ? writingBook_ : focus_.kind == Focus::Book ? focus_.id
                    : view_ == View::Book ? viewId_ : settings_.lastBook;
    const Book* book = lib_->book(bookId);
    if (book && book->deleted) book = nullptr;
    if (book) {
        for (const ManuscriptNode* n : lib_->manuscriptOf(book->id))
            add({nodeLabel(*n), n->kind == NodeKind::Scene ? L"scene" : n->kind == NodeKind::Part ? L"part" : L"chapter", 0, n->id, 0,
                 cjui::Icon::Writing});
        for (const Entry* e : lib_->entriesOf(book->worldId, book->worldId.empty() ? book->id : Id{})) {
            std::wstring right = w(e->kind);
            add({w(e->name), right, 1, e->id, 0, cjui::Icon::World});
            for (const std::string& o : e->aliases) add({w(o), right + L"  (" + w(e->name) + L")", 1, e->id, 0, cjui::Icon::World});
        }
    }
    if (!book)   // no book in hand: every world's bible
        for (const World* wd : lib_->liveWorlds())
            for (const Entry* e : lib_->entriesOf(wd->id, Id{})) add({w(e->name), w(e->kind) + L" in " + w(wd->name), 1, e->id, 0, cjui::Icon::World});
    for (const Book* b : lib_->liveBooks()) add({w(b->title), L"book", 2, b->id, 0, cjui::Icon::Book});
    for (const World* wd : lib_->liveWorlds()) add({w(wd->name), L"world", 5, wd->id, 0, cjui::Icon::World});
    return hits;
}

void StudioApp::runSearchHit(const SearchHit& h) {
    setFieldText(FTopSearch, "");
    SetFocus(hwnd());
    topSel_ = 0;
    if (h.kind == 3) {
        if (h.cmd >= 100) command(h.cmd);
        else if (h.cmd == -1) startImport({});
        else if (h.cmd == -2) openSettings();
        else if (h.cmd == -3) showShortcuts();
        else if (h.cmd == -4) { settings_.showSide = !settings_.showSide; saveSettings(); }
        else if (h.cmd == -5) { settings_.showPanel = !settings_.showPanel; saveSettings(); }
        else if (h.cmd <= -6) {
            settings_.theme = h.cmd == -6 ? 1 : h.cmd == -7 ? 2 : 0;
            saveSettings();
            applyTheme();
        }
    } else if (h.kind == 4) {
        openTab(h.cmd);
    } else if (h.kind == 0) {
        const ManuscriptNode* n = lib_ ? lib_->node(h.id) : nullptr;
        if (!n) return;
        if (ws_ != Workspace::Writing || writingBook_ != n->bookId) {
            currentNode_ = n->id;
            writingBook_ = n->bookId;
            openWriting(n->bookId);
        }
        selectNode(n->id, true);
    } else if (h.kind == 1) {
        if (const Entry* e = lib_ ? lib_->entry(h.id) : nullptr; e && !e->deleted) {
            const Id id = e->id;
            openWorldbuilding(e->worldId, e->worldId.empty() ? e->bookId : Id{});
            openEntry(id);
        }
    } else if (h.kind == 2) {
        ws_ = Workspace::Library;
        goTo(View::Book, h.id);
    } else if (h.kind == 5) {
        ws_ = Workspace::Library;
        goTo(View::World, h.id);
    }
    invalidate();
}

bool StudioApp::searchKey(WPARAM vk) {
    const auto hits = searchHits();
    if (vk == VK_DOWN) topSel_ = hits.empty() ? 0 : (topSel_ + 1) % (int)hits.size();
    else if (vk == VK_UP) topSel_ = hits.empty() ? 0 : (topSel_ + (int)hits.size() - 1) % (int)hits.size();
    else if (vk == VK_RETURN) {
        if (!hits.empty()) runSearchHit(hits[(size_t)std::clamp(topSel_, 0, (int)hits.size() - 1)]);
        return true;
    } else if (vk == VK_ESCAPE) {
        setFieldText(FTopSearch, "");
        SetFocus(hwnd());
    } else {
        return false;
    }
    invalidate();
    return true;
}

// The results, in a card under the box, over whatever is there.
void StudioApp::drawSearchResults() {
    lastSearchHits_.clear();
    if (GetFocus() != fields_[FTopSearch] || fieldText(FTopSearch).empty()) return;
    const auto hits = searchHits();
    lastSearchHits_ = hits;
    const Rect sr = topSearchRect_;
    const float rowH = 34;
    const float h = hits.empty() ? 52 : 12 + rowH * hits.size();
    const float a = anim("search#open", 1.0f, 22.0f);
    const Rect card{sr.x - 8, sr.bottom() + 6 - 8 * (1 - a), sr.w + 16, h};
    softShadow(card, 12, a);
    gfx_.fillRounded(card, 12, pal_.surface);
    gfx_.strokeRounded(card, 12, pal_.rule);
    if (hits.empty()) {
        gfx_.text(L"Nothing matches. Try fewer letters, or another word.", card.inset(18, 0), Font::Small, pal_.inkMuted,
                  {Align::Left, false, true, true});
        return;
    }
    topSel_ = std::clamp(topSel_, 0, (int)hits.size() - 1);
    const float selY = slideTo("search#sel", card.y + 6 + rowH * topSel_);
    gfx_.fillRounded({card.x + 6, selY, card.w - 12, rowH}, 8, pal_.ribbon.withAlpha(pal_.dark ? 0.18f : 0.12f));
    for (size_t i = 0; i < hits.size(); ++i) {
        const Rect r{card.x + 6, card.y + 6 + rowH * i, card.w - 12, rowH};
        const std::string id = "search:" + std::to_string(i);
        const SearchHit hit = hits[i];
        addHit(id, r, [this, hit] { runSearchHit(hit); }, false);
        if (hovered(id) && topSel_ != (int)i) gfx_.fillRounded(r, 8, pal_.ink.withAlpha(0.04f));
        cjui::drawIcon(gfx_, hit.icon, {r.x + 10, r.y + (rowH - 16) / 2, 16, 16}, (int)i == topSel_ ? pal_.ink : pal_.inkMuted, 1.4f);
        const float rw = std::min(gfx_.measureWidth(hit.right, Font::Small) + 8, r.w * 0.45f);
        gfx_.text(hit.label, {r.x + 36, r.y, r.w - 48 - rw, rowH}, (int)i == topSel_ ? Font::BodyStrong : Font::Body, pal_.ink,
                  {Align::Left, false, true, true});
        gfx_.text(hit.right, {r.right() - rw - 8, r.y, rw, rowH}, Font::Small, pal_.inkFaint, {Align::Right, false, true, true});
    }
}

}  // namespace nrs
