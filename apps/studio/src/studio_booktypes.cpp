// NorthRay Writing Studio: book types and a book's place in a series (0.73).
//
// A book has a type (novel, novella, novelette, short story, flash fiction, collection or anthology, serial or
// episode, or the author's own); the Studio suggests one from the words until one is chosen. In each series a book
// is a main book or a sub-book: set between two books (placed by reading order, numbered like 2.5) or set during one
// (it follows that book in reading order).

#include "studio_app.hpp"

#include <algorithm>

namespace nrs {

using namespace studio;
using cjui::Align;
using cjui::Font;
using cjui::Rect;

static std::wstring w(const std::string& s) { return widen(s); }

namespace {
std::wstring grouped(std::int64_t n) {
    std::wstring d = std::to_wstring(n), out;
    for (size_t i = 0; i < d.size(); ++i) {
        if (i && (d.size() - i) % 3 == 0) out += L',';
        out += d[i];
    }
    return out;
}
std::wstring rangeText(const BookType& t) {
    if (!t.hasRange()) return L"any length";
    if (!t.maxWords) return grouped(t.minWords) + L"+ words";
    if (t.minWords <= 1) return L"under " + grouped(t.maxWords + 1) + L" words";
    return grouped(t.minWords) + L"–" + grouped(t.maxWords) + L" words";
}
}  // namespace

std::wstring StudioApp::bookTypeText(const Book& b) const {
    if (const auto t = lib_->bookType(b.type)) return w(t->name);
    return {};
}

std::wstring StudioApp::bookTypeValue(const Book& b) const {
    if (const auto t = lib_->bookType(b.type)) return w(t->name);
    if (const auto s = lib_->bookType(lib_->suggestedBookType(b.wordCount)))
        return L"Not chosen (" + w(s->name) + L" by length)";
    return L"Not chosen";
}

void StudioApp::chooseBookType(const Id& bookId) {
    const Book* b = lib_->book(bookId);
    if (!b) return;
    commitFields();
    const std::string suggested = lib_->suggestedBookType(b->wordCount);
    std::vector<ToolItem> items;
    auto set = [this, bookId](const std::string& type) {
        if (const Book* x = lib_->book(bookId)) {
            Book n = *x;
            n.type = type;
            lib_->updateBook(n);
            loadFields();
            invalidate();
        }
    };
    if (!suggested.empty() && b->type.empty())
        items.push_back(ToolItem::heading(L"Suggested for " + wordsText(b->wordCount)));
    bool ownHead = false;
    for (const BookType& t : lib_->bookTypes()) {
        if (!t.builtIn && !ownHead) { items.push_back(ToolItem::heading(L"Your own")); ownHead = true; }
        std::wstring l = w(t.name);
        if (t.id == suggested && b->type != t.id) l += L"  · fits its length";
        items.push_back(ToolItem::item(l, [set, id = t.id] { set(id); }, b->type == t.id));
    }
    items.push_back(ToolItem::line());
    items.push_back(ToolItem::item(L"Not chosen", [set] { set({}); }, b->type.empty() || !lib_->bookType(b->type)));
    items.push_back(ToolItem::item(L"Your own book types…", [this] { goTo(View::Settings); invalidate(); }));
    showTools(items, mouseX(), mouseY());
}

std::wstring StudioApp::seriesRoleText(const Membership& m) const {
    if (m.role == SeriesRole::During) {
        const Book* host = lib_->book(m.during);
        return host ? L"Set during " + w(host->title) : L"Set during a book";
    }
    return w(label(m.role));
}

void StudioApp::chooseSeriesRole(const Id& seriesId, const Id& bookId) {
    const Membership* m = lib_->membership(seriesId, bookId);
    if (!m) return;
    commitFields();
    auto set = [this, seriesId, bookId](SeriesRole role, const Id& during) {
        if (!lib_->setSeriesRole(seriesId, bookId, role, during)) flash(w(lib_->lastError()));
        loadFields();
        invalidate();
    };
    std::vector<ToolItem> items;
    items.push_back(ToolItem::item(L"Main book", [set] { set(SeriesRole::Main, {}); }, m->role == SeriesRole::Main));
    items.push_back(ToolItem::item(L"Set between books (placed by reading order)", [set] { set(SeriesRole::Between, {}); },
                                   m->role == SeriesRole::Between));
    std::vector<ToolItem> hosts;
    for (const Book* b : lib_->booksInSeries(seriesId)) {
        if (b->id == bookId) continue;
        const Membership* hm = lib_->membership(seriesId, b->id);
        std::wstring l = (hm && !hm->number.empty() ? L"Book " + w(hm->number) + L"  " : L"") + w(b->title);
        hosts.push_back(ToolItem::item(l, [set, id = b->id] { set(SeriesRole::During, id); },
                                       m->role == SeriesRole::During && m->during == b->id));
    }
    if (!hosts.empty()) items.push_back(ToolItem::menu(L"Set during", hosts));
    showTools(items, mouseX(), mouseY());
}

// ------------------------------------------------------------------ Settings › Book types

float StudioApp::drawBookTypeSettings(const Rect& r, float y) {
    // The Settings page has no panel to load the fields, so the book type being edited loads them here.
    if (focus_.kind == Focus::BookType && !(focus_ == loadedFocus_)) loadFields();
    y += gfx_.text(L"What sort of book each one is (on its page, under Book settings). Until you choose, the Studio "
                   L"suggests one from the book's length.", {r.x, y, r.w, 44}, Font::Body, pal_.inkMuted) + 10;
    for (const BookType& t : builtInBookTypes()) {
        gfx_.text(w(t.name), {r.x, y, 220, 24}, Font::Body, pal_.ink, {Align::Left, false, true, true});
        gfx_.text(rangeText(t), {r.x + 220, y, r.w - 220, 24}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
        y += 26;
    }
    y += 12;
    gfx_.text(L"Your own", {r.x, y, r.w, 20}, Font::SmallStrong, pal_.ink, {Align::Left, false});
    y += 26;
    const auto own = lib_->customBookTypes();
    if (own.empty()) {
        y += gfx_.text(L"None yet. Give one a range in words to have it suggested by length.", {r.x, y, r.w, 22}, Font::Small,
                       pal_.inkMuted) + 8;
    }
    for (const BookType& t : own) {
        const bool open = focus_.kind == Focus::BookType && focus_.id == t.id;
        if (!open) {
            gfx_.text(w(t.name), {r.x, y, 220, 26}, Font::BodyStrong, pal_.ink, {Align::Left, false, true, true});
            gfx_.text(rangeText(t), {r.x + 220, y, r.w - 360, 26}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
            linkText("set:bt:" + t.id, r.right() - gfx_.measureWidth(L"Change", Font::Small) - 8, y + 2, L"Change",
                     [this, id = t.id] { setFocus({Focus::BookType, id, {}}); invalidate(); });
            y += 34;
            continue;
        }
        const Rect c{r.x, y, r.w, 0};
        gfx_.fillRounded({c.x - 10, y - 6, c.w + 20, 196}, 8, pal_.sunken);
        y = pageField(c, y, L"Name", FName);
        const float hw = (c.w - 12) / 2, y0 = y;
        y = pageField({c.x, 0, hw, 0}, y0, L"At least (words, blank for none)", FTarget);
        pageField({c.x + hw + 12, 0, hw, 0}, y0, L"At most (words, blank for no limit)", FStoryDay);
        linkText("set:bt:done", c.x - 4, y - 6, L"Done", [this] { commitFields(); setFocus({}); invalidate(); });
        linkText("set:bt:remove", c.x + 70, y - 6, L"Remove this type", [this, id = t.id] {
            commitFields();
            const auto before = lib_->customBookTypes();
            auto next = before;
            next.erase(std::remove_if(next.begin(), next.end(), [&](const BookType& x) { return x.id == id; }), next.end());
            lib_->setCustomBookTypes(next);
            setFocus({});
            flashUndo(L"Removed the book type.", [this, before] { lib_->setCustomBookTypes(before); invalidate(); });
        });
        y += 40;
    }
    button("set:bt:new", {r.x, y, 190, 32}, L"+ New book type", [this] { newBookType(); });
    return y + 46;
}

void StudioApp::newBookType() {
    commitFields();
    auto types = lib_->customBookTypes();
    BookType t;
    t.name = "New type";
    types.push_back(t);
    lib_->setCustomBookTypes(types);
    const auto now = lib_->customBookTypes();
    if (now.empty()) return;
    setFocus({Focus::BookType, now.back().id, {}});
    focusFieldSoon_ = FName;
    invalidate();
}

void StudioApp::loadBookTypeFields() {
    for (const BookType& t : lib_->customBookTypes())
        if (t.id == focus_.id) {
            setFieldText(FName, t.name);
            setFieldText(FTarget, t.minWords > 0 ? std::to_string(t.minWords) : std::string());
            setFieldText(FStoryDay, t.maxWords > 0 ? std::to_string(t.maxWords) : std::string());
        }
}

bool StudioApp::commitBookTypeFields(const Id& id) {
    auto types = lib_->customBookTypes();
    for (BookType& t : types)
        if (t.id == id) {
            const std::string nm = fieldText(FName);
            if (!nm.empty()) t.name = nm;
            const std::string lo = fieldText(FTarget), hi = fieldText(FStoryDay);
            t.minWords = lo.empty() ? 0 : std::max<std::int64_t>(0, std::strtoll(lo.c_str(), nullptr, 10));
            t.maxWords = hi.empty() ? 0 : std::max<std::int64_t>(0, std::strtoll(hi.c_str(), nullptr, 10));
        }
    return lib_->setCustomBookTypes(types);
}

}  // namespace nrs
