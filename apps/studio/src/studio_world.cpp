// The Worldbuilding workspace: a world's (or an independent book's) bible of
// characters, places, groups, things, lore and events. The sidebar filters by
// kind; the middle shows the entries as cards, or one entry with its details,
// relationships and where it appears in the manuscripts; the right-hand panel
// edits it.
#include "studio_app.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace nrs {

using namespace studio;

static std::wstring w(const std::string& s) { return widen(s); }

static int popupAt(HWND owner, const std::vector<std::wstring>& items, int checked = -1) {
    HMENU m = CreatePopupMenu();
    for (size_t i = 0; i < items.size(); ++i)
        AppendMenuW(m, MF_STRING | ((int)i == checked ? MF_CHECKED : 0), 1 + (UINT)i, items[i].c_str());
    POINT pt;
    GetCursorPos(&pt);
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD, pt.x, pt.y, 0, owner, nullptr);
    DestroyMenu(m);
    return cmd - 1;
}

static std::wstring upper(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towupper(c);
    return s;
}

static std::wstring initials(const std::string& name) {
    std::wstring out;
    bool start = true;
    for (wchar_t c : w(name)) {
        if (iswspace(c)) { start = true; continue; }
        if (start && iswalnum(c) && out.size() < 2) out += (wchar_t)towupper(c);
        start = false;
    }
    return out.empty() ? L"?" : out;
}

// ------------------------------------------------------------- opening

// The world (or independent book) the user is working in, from wherever they are.
bool StudioApp::worldbuildingScope(Id& worldId, Id& bookId) const {
    worldId.clear();
    bookId.clear();
    auto fromBook = [&](const Id& b) {
        const studio::Book* bk = lib_->book(b);
        if (!bk) return false;
        if (!bk->worldId.empty()) worldId = bk->worldId; else bookId = bk->id;
        return true;
    };
    if (ws_ == Workspace::Worldbuilding && (!wb_.world.empty() || !wb_.book.empty())) { worldId = wb_.world; bookId = wb_.book; return true; }
    if (ws_ == Workspace::Writing && fromBook(writingBook_)) return true;
    if (ws_ == Workspace::Typesetting && fromBook(ts_.book)) return true;
    if (ws_ == Workspace::Preview && fromBook(pv_.book)) return true;
    if (ws_ == Workspace::Publishing && fromBook(pb_.book)) return true;
    if (focus_.kind == Focus::World && lib_->world(focus_.id)) { worldId = focus_.id; return true; }
    if (focus_.kind == Focus::Series)
        if (const Series* s = lib_->seriesById(focus_.id); s && !s->worldId.empty()) { worldId = s->worldId; return true; }
    if (focus_.kind == Focus::Book && fromBook(focus_.id)) return true;
    if (view_ == View::World && lib_->world(viewId_)) { worldId = viewId_; return true; }
    if (view_ == View::Book && fromBook(viewId_)) return true;
    if (!wb_.world.empty() && lib_->world(wb_.world)) { worldId = wb_.world; return true; }
    if (!wb_.book.empty() && lib_->book(wb_.book)) { bookId = wb_.book; return true; }
    // Only one world: that one.
    Id only;
    int n = 0;
    for (auto& x : lib_->worlds()) if (!x.deleted) { only = x.id; ++n; }
    if (n == 1) { worldId = only; return true; }
    return false;
}

void StudioApp::openWorldbuilding(const Id& worldId, const Id& bookId) {
    if (!lib_) return;
    if (worldId.empty() ? !lib_->book(bookId) : !lib_->world(worldId)) return;
    commitFields();
    if (ws_ == Workspace::Writing && editor_.ready()) editor_.send(R"({"type":"flush"})");
    if (wb_.world != worldId || wb_.book != bookId) {
        wb_ = {};
        wb_.world = worldId;
        wb_.book = bookId;
    }
    ws_ = Workspace::Worldbuilding;
    if (const Entry* e = lib_->entry(wb_.entry); e && !e->deleted) openEntry(e->id);
    else {
        wb_.entry.clear();
        setFocus({});
    }
    invalidate();
}

void StudioApp::openEntry(const Id& id) {
    commitFields();
    wb_.entry = id;
    wb_.scroll = 0;
    wb_.mentionsFor.clear();   // looked up again
    setFocus(id.empty() ? Focus{} : Focus{Focus::Entry, id, {}});
    invalidate();
}

void StudioApp::newEntry(const std::string& kindIn) {
    std::string kind = kindIn;
    if (kind.empty()) {
        std::vector<std::wstring> items;
        for (auto& k : entryKinds()) items.push_back(w(entryKindLabel(k)));
        const int i = popupAt(hwnd(), items);
        if (i < 0) return;
        kind = entryKinds()[i];
    }
    const Id id = lib_->createEntry(wb_.world, wb_.book, kind, std::string("New ") + [&] {
        std::string l = entryKindLabel(kind);
        for (auto& c : l) c = (char)std::tolower((unsigned char)c);
        return l;
    }());
    if (id.empty()) { flash(w("Couldn't create the entry: " + lib_->lastError()), 8); return; }
    openEntry(id);
    // Straight into naming it.
    loadFields();
    if (fields_[FName]) { SetFocus(fields_[FName]); SendMessageW(fields_[FName], EM_SETSEL, 0, -1); }
}

void StudioApp::addRelationship() {
    const Entry* e = lib_->entry(wb_.entry);
    if (!e) return;
    std::vector<const Entry*> others;
    for (const Entry* x : lib_->entriesOf(wb_.world, wb_.book)) if (x->id != e->id) others.push_back(x);
    if (others.empty()) { flash(L"Add another entry first, then link the two.", 5); return; }
    std::vector<std::wstring> labels;
    for (auto& r : relations()) labels.push_back(w(r.label));
    const int li = popupAt(hwnd(), labels);
    if (li < 0) return;
    std::vector<std::wstring> names;
    for (const Entry* x : others) names.push_back(w(x->name) + L"   (" + w(entryKindLabel(x->kind)) + L")");
    const int oi = popupAt(hwnd(), names);
    if (oi < 0) return;
    if (!lib_->addLink(e->id, others[oi]->id, relations()[li].label)) flash(w("Couldn't link them: " + lib_->lastError()), 6);
    invalidate();
}

// ---------------------------------------------------------------- drawing

void StudioApp::drawWorldSidebar(const Rect& r) {
    gfx_.fill(r, pal_.sunken);
    gfx_.line(r.right() - 0.5f, r.y, r.right() - 0.5f, r.bottom(), pal_.rule);
    gfx_.pushClip(r);
    setHitClip(r);
    const float x = r.x + 10, wd = r.w - 20, tx0 = x + 12, cw = wd - 24;
    float y = r.y + 14 - wb_.sideScroll;
    linkText("side:wb:library", tx0, y, L"‹ Library", [this] {
        ws_ = Workspace::Library;
        if (!wb_.world.empty()) goTo(View::World, wb_.world); else goTo(View::Book, wb_.book);
    });
    y += 30;
    gfx_.text(L"WORLDBUILDING", {tx0, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 20;
    std::wstring name;
    if (const World* wo = lib_->world(wb_.world)) name = w(wo->name);
    else if (const studio::Book* b = lib_->book(wb_.book)) name = w(b->title);
    y += gfx_.text(name, {tx0, y, cw, 90}, Font::SerifTitle, pal_.ink) + 2;
    const auto all = lib_->entriesOf(wb_.world, wb_.book);
    gfx_.text(wb_.world.empty() ? L"An independent book" : L"A world", {tx0, y, cw, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
    y += 30;
    button("side:wb:new", {tx0, y, cw, 32}, wb_.kind.empty() ? L"+ New entry" : L"+ New " + [&] {
        std::wstring l = w(entryKindLabel(wb_.kind));
        for (auto& c : l) c = (wchar_t)towlower(c);
        return l;
    }(), [this] { newEntry(wb_.kind); }, true);
    y += 48;
    // Kinds.
    auto row = [&](const std::string& kind, const std::wstring& label, int count) {
        const std::string id = "side:wb:kind:" + kind;
        const Rect rr{x, y, wd, 30};
        addHit(id, rr, [this, kind] {
            wb_.kind = kind;
            wb_.scroll = 0;
            openEntry({});
        });
        const bool sel = wb_.kind == kind && wb_.entry.empty();
        const float hv = hoverAmount(id);
        if (sel) { gfx_.fillRounded(rr, 3, pal_.surface); gfx_.fill({rr.x, rr.y + 6, 3, rr.h - 12}, pal_.ribbon); }
        else if (hv > 0.01f) gfx_.fillRounded(rr, 3, pal_.surface.withAlpha(0.6f * hv));
        gfx_.text(label, {tx0, rr.y, cw - 30, rr.h}, sel ? Font::BodyStrong : Font::Body, pal_.ink, {Align::Left, false, true, true});
        gfx_.text(std::to_wstring(count), {tx0, rr.y, cw, rr.h}, Font::Small, pal_.inkFaint, {Align::Right, false, false, true});
        if (focused(id)) focusRing(rr);
        y += 32;
    };
    row("", L"Everything", (int)all.size());
    for (auto& k : entryKinds()) {
        int n = 0;
        for (auto* e : all) if (e->kind == k) ++n;
        row(k, w(entryKindPlural(k)), n);
    }
    y += 20;
    // The books it covers.
    gfx_.text(L"BOOKS", {tx0, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 22;
    std::vector<const studio::Book*> books;
    if (wb_.world.empty()) { if (const studio::Book* b = lib_->book(wb_.book)) books.push_back(b); }
    else for (const studio::Book* b : lib_->liveBooks()) if (b->worldId == wb_.world) books.push_back(b);
    for (const studio::Book* b : books) {
        linkText("side:wb:book:" + b->id, tx0, y, w(b->title), [this, id = b->id] { openWriting(id); });
        y += 24;
    }
    if (books.empty()) { gfx_.text(L"No books yet.", {tx0, y, cw, 18}, Font::Small, pal_.inkFaint, {Align::Left, false}); y += 24; }
    wb_.sideContent = y + wb_.sideScroll - r.y + 20;
    clearHitClip();
    gfx_.popClip();
}

void StudioApp::drawWorldPage(const Rect& r) {
    gfx_.pushClip(r);
    setHitClip(r);
    const float pad = r.w > 900 ? 40 : 28;
    const float cw = std::min(r.w - pad * 2, 900.0f);
    const Rect m{r.x + pad, r.y + 26 - wb_.scroll, cw, 0};
    float y = m.y;
    const Entry* e = wb_.entry.empty() ? nullptr : lib_->entry(wb_.entry);
    if (e && e->deleted) { e = nullptr; wb_.entry.clear(); }

    if (!e) {
        // ---- All entries (or one kind), as cards.
        std::wstring scopeName;
        if (const World* wo = lib_->world(wb_.world)) scopeName = w(wo->name);
        else if (const studio::Book* b = lib_->book(wb_.book)) scopeName = w(b->title);
        std::vector<const Entry*> list;
        for (const Entry* x : lib_->entriesOf(wb_.world, wb_.book)) if (wb_.kind.empty() || x->kind == wb_.kind) list.push_back(x);
        const std::wstring title = wb_.kind.empty() ? scopeName : w(entryKindPlural(wb_.kind));
        std::wstring sub = std::to_wstring(list.size()) + (list.size() == 1 ? L" entry" : L" entries");
        if (!wb_.kind.empty()) sub += L" in " + scopeName;
        y += pageHeader(m, L"WORLDBUILDING", title, sub);
        if (list.empty()) {
            const std::wstring hint = wb_.kind.empty()
                ? L"Keep track of who and what is in your books: characters, places, groups, things, lore and events. Each entry has "
                  L"its details, notes and relationships, and lists the chapters it appears in."
                : L"None yet.";
            y += gfx_.text(hint, {m.x, y, std::min(cw, 640.0f), 80}, Font::Body, pal_.inkMuted) + 16;
            button("wb:first", {m.x, y, 180, 34}, wb_.kind.empty() ? L"Add the first entry" : L"Add one", [this] { newEntry(wb_.kind); }, true);
            y += 50;
        } else {
            const float gap = 16;
            const int cols = std::max(1, (int)((cw + gap) / (240 + gap)));
            const float cardW = (cw - gap * (cols - 1)) / cols, cardH = 96;
            for (size_t i = 0; i < list.size(); ++i) {
                const Entry* x = list[i];
                const int col = (int)i % cols;
                const Rect cr{m.x + col * (cardW + gap), y, cardW, cardH};
                const std::string id = "wb:card:" + x->id;
                addHit(id, cr, [this, eid = x->id] { openEntry(eid); });
                const float hv = hoverAmount(id);
                softShadow(cr, 4, hv);
                gfx_.fillRounded(cr, 4, pal_.surface);
                gfx_.strokeRounded(cr, 4, pal_.rule.mix(pal_.inkFaint, hv), 1);
                const Rect pic{cr.x + 12, cr.y + 12, 72, 72};
                if (x->imageBlob.empty() || !gfx_.drawImage(w(lib_->blobPath(x->imageBlob)), pic, true)) {
                    gfx_.fillRounded(pic, 36, accentOfWorld(wb_.world).withAlpha(0.18f));
                    gfx_.text(initials(x->name), pic, Font::Heading, pal_.ink, {Align::Center, false, false, true});
                }
                const float tx = pic.right() + 14, tw = cr.right() - 12 - tx;
                gfx_.text(upper(w(entryKindLabel(x->kind))), {tx, cr.y + 13, tw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false, true});
                gfx_.text(w(x->name), {tx, cr.y + 30, tw, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
                if (!x->summary.empty()) gfx_.text(w(x->summary), {tx, cr.y + 52, tw, 34}, Font::Small, pal_.inkMuted);
                if (focused(id)) focusRing(cr);
                if (col == cols - 1 || i + 1 == list.size()) y += cardH + gap;
            }
        }
    } else {
        // ---- One entry.
        const std::wstring back = wb_.kind.empty() ? L"‹ Everything" : L"‹ " + w(entryKindPlural(wb_.kind));
        linkText("wb:back", m.x, y, back, [this] { openEntry({}); });
        y += 34;
        const bool hasPic = !e->imageBlob.empty();
        const float picW = hasPic ? std::min(220.0f, cw * 0.3f) : 0;
        float ty = y;
        const float tx = m.x + (hasPic ? picW + 28 : 0), tw = m.right() - tx;
        if (hasPic) {
            const auto [iw, ih] = gfx_.imageSize(w(lib_->blobPath(e->imageBlob)));
            const float ph = iw > 0 ? std::min(picW * ih / (float)iw, picW * 1.5f) : picW;
            gfx_.drawImage(w(lib_->blobPath(e->imageBlob)), {m.x, y, picW, ph}, true);
            gfx_.stroke({m.x, y, picW, ph}, pal_.rule);
            ty = y;
            y += ph;
        }
        gfx_.text(upper(w(entryKindLabel(e->kind))), {tx, ty, tw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
        ty += 20;
        ty += gfx_.text(w(e->name), {tx, ty, tw, 90}, Font::SerifTitle, pal_.ink) + 4;
        if (!e->aliases.empty()) {
            std::wstring a = L"Also ";
            for (size_t i = 0; i < e->aliases.size(); ++i) a += (i ? L", " : L"") + std::wstring(L"“") + w(e->aliases[i]) + L"”";
            ty += gfx_.text(a, {tx, ty, tw, 60}, Font::Small, pal_.inkMuted) + 8;
        }
        if (!e->summary.empty()) ty += gfx_.text(w(e->summary), {tx, ty, std::min(tw, 640.0f), 200}, Font::Body, pal_.ink) + 12;
        // Details: "Label: value" lines as a table.
        if (!e->details.empty()) {
            ty += 8;
            size_t at = 0;
            while (at <= e->details.size()) {
                size_t nl = e->details.find('\n', at);
                if (nl == std::string::npos) nl = e->details.size();
                std::string line = e->details.substr(at, nl - at);
                at = nl + 1;
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                if (line.empty()) continue;
                const size_t colon = line.find(':');
                const float lw = std::min(160.0f, tw * 0.35f);
                if (colon != std::string::npos && colon > 0 && colon < 40) {
                    std::string val = line.substr(colon + 1);
                    while (!val.empty() && val.front() == ' ') val.erase(0, 1);
                    gfx_.text(w(line.substr(0, colon)), {tx, ty, lw - 10, 20}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
                    ty += std::max(20.0f, gfx_.text(w(val), {tx + lw, ty, std::min(tw - lw, 480.0f), 80}, Font::Small, pal_.ink)) + 4;
                } else {
                    ty += gfx_.text(w(line), {tx, ty, std::min(tw, 640.0f), 80}, Font::Small, pal_.ink) + 4;
                }
                gfx_.line(tx, ty - 1.5f, tx + std::min(tw, 640.0f), ty - 1.5f, pal_.rule.withAlpha(0.5f));
                ty += 2;
            }
        }
        y = std::max(y, ty) + 26;

        // Relationships.
        gfx_.text(L"RELATIONSHIPS", {m.x, y, cw, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
        linkText("wb:addrel", m.right() - gfx_.measureWidth(L"+ Add a relationship", Font::Small), y - 2, L"+ Add a relationship",
                 [this] { addRelationship(); });
        y += 28;
        const auto links = lib_->linksOf(e->id);
        if (links.empty()) {
            y += gfx_.text(L"None yet: link this entry to the people, places and things around it.", {m.x, y, cw, 20}, Font::Small,
                           pal_.inkFaint) + 12;
        }
        for (const EntryLink& l : links) {
            const bool mine = l.from == e->id;
            const Entry* other = lib_->entry(mine ? l.to : l.from);
            if (!other) continue;
            const std::wstring label = w(mine ? l.label : reverseRelation(l.label));
            gfx_.text(label, {m.x, y, 160, 24}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
            linkText("wb:rel:" + other->id + l.label, m.x + 170, y + 4, w(other->name), [this, oid = other->id] { openEntry(oid); }, Font::Body);
            gfx_.text(w(entryKindLabel(other->kind)), {m.x + 170, y, cw - 170 - 80, 24}, Font::Small, pal_.inkFaint,
                      {Align::Right, false, true, true});
            linkText("wb:unrel:" + other->id + l.label, m.right() - gfx_.measureWidth(L"Remove", Font::Small), y + 4, L"Remove",
                     [this, l] { lib_->removeLink(l.from, l.to, l.label); invalidate(); });
            y += 28;
            gfx_.line(m.x, y - 2.5f, m.right(), y - 2.5f, pal_.rule.withAlpha(0.6f));
        }
        y += 22;

        // Notes.
        if (!e->notes.empty()) {
            gfx_.text(L"NOTES", {m.x, y, cw, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
            y += 26;
            y += gfx_.text(w(e->notes), {m.x, y, std::min(cw, 680.0f), 4000}, Font::Body, pal_.ink) + 26;
        }

        // Where it appears: looked up when the entry or its names change.
        std::string key = e->name;
        for (auto& a : e->aliases) key += "\n" + a;
        if (wb_.mentionsFor != e->id || wb_.mentionsKey != key) {
            wb_.mentions = lib_->mentionsOf(*e);
            wb_.mentionsFor = e->id;
            wb_.mentionsKey = key;
        }
        gfx_.text(L"APPEARS IN", {m.x, y, cw, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
        y += 28;
        if (wb_.mentions.empty()) {
            y += gfx_.text(L"Its name doesn't appear in the manuscripts yet. Other names it goes by are searched too.", {m.x, y, cw, 40},
                           Font::Small, pal_.inkFaint) + 12;
        }
        int total = 0;
        for (auto& mn : wb_.mentions) total += mn.count;
        if (!wb_.mentions.empty())
            y += gfx_.text(std::to_wstring(total) + (total == 1 ? L" mention in " : L" mentions in ") + std::to_wstring(wb_.mentions.size()) +
                               (wb_.mentions.size() == 1 ? L" chapter or scene." : L" chapters and scenes."),
                           {m.x, y, cw, 20}, Font::Small, pal_.inkMuted) + 10;
        for (const Mention& mn : wb_.mentions) {
            const ManuscriptNode* n = lib_->node(mn.nodeId);
            const studio::Book* b = lib_->book(mn.bookId);
            if (!n || !b) continue;
            const std::string id = "wb:mention:" + mn.nodeId;
            const float sh = gfx_.measureHeight(w(mn.snippet), cw - 24, Font::Small);
            const Rect rr{m.x - 8, y, cw + 16, 30 + sh + 10};
            addHit(id, rr, [this, bid = b->id, nid = n->id] {
                writingBook_ = bid;
                currentNode_ = nid;
                openWriting(bid);
            });
            const float hv = hoverAmount(id);
            if (hv > 0.01f) gfx_.fillRounded(rr, 3, pal_.surface.withAlpha(0.8f * hv));
            const std::wstring where = (wb_.world.empty() ? std::wstring() : w(b->title) + L"  ›  ") +
                                       w(n->title.empty() ? std::string(label(n->kind)) : n->title);
            gfx_.text(where, {m.x, y + 6, cw - 90, 20}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
            gfx_.text(std::to_wstring(mn.count) + (mn.count == 1 ? L" time" : L" times"), {m.right() - 90, y + 6, 90, 20}, Font::Small,
                      pal_.inkFaint, {Align::Right, false});
            gfx_.text(w(mn.snippet), {m.x, y + 28, cw - 24, sh + 4}, Font::Small, pal_.inkMuted);
            if (focused(id)) focusRing(rr);
            y += rr.h + 4;
        }
    }
    wb_.content = y + wb_.scroll - r.y + 40;
    wb_.scroll = std::clamp(wb_.scroll, 0.0f, std::max(0.0f, wb_.content - r.h));
    clearHitClip();
    gfx_.popClip();
}

void StudioApp::drawWorldPanel(const Rect& r) {
    gfx_.fill(r, pal_.paper.mix(pal_.surface, 0.5f));
    gfx_.line(r.x + 0.5f, r.y, r.x + 0.5f, r.bottom(), pal_.rule);
    if (!(focus_ == loadedFocus_)) loadFields();
    gfx_.pushClip(r);
    setHitClip(r);
    const Rect c{r.x + 20, r.y, r.w - 40, 0};
    float y = r.y + 18 - wb_.panelScroll;
    const Entry* e = wb_.entry.empty() ? nullptr : lib_->entry(wb_.entry);
    if (e && !e->deleted) {
        panelHeading(c, y, L"ENTRY", true);
        y = panelField(c, y, L"Name", FName);
        panelChoice("wb:kind", c, y, L"Kind", w(entryKindLabel(e->kind)), [this] {
            const Entry* x = lib_->entry(wb_.entry);
            if (!x) return;
            std::vector<std::wstring> items;
            int cur = 0;
            for (size_t i = 0; i < entryKinds().size(); ++i) {
                items.push_back(w(entryKindLabel(entryKinds()[i])));
                if (entryKinds()[i] == x->kind) cur = (int)i;
            }
            const int i = popupAt(hwnd(), items, cur);
            if (i < 0 || i == cur) return;
            commitFields();
            Entry u = *lib_->entry(wb_.entry);
            u.kind = entryKinds()[i];
            lib_->updateEntry(u);
            invalidate();
        });
        y = panelField(c, y, L"Other names (separate with commas)", FTags);
        y = panelField(c, y, L"Summary", FDescription, 70);
        y = panelImage(c, y, L"Picture", e->imageBlob, [this] { chooseImage(Focus::Entry, wb_.entry, 0); }, [this] {
            if (const Entry* x = lib_->entry(wb_.entry)) { Entry u = *x; u.imageBlob.clear(); lib_->updateEntry(u); }
        }, false);
        y = panelField(c, y, L"Details (one per line, like Age: 34)", FSynopsis, 120);
        y = panelField(c, y, L"Notes", FNotes, 180);
        linkText("wb:trash", c.x, y, L"Move to Trash", [this] {
            const Entry* x = lib_->entry(wb_.entry);
            if (!x) return;
            const std::wstring name = w(x->name);
            commitFields();
            if (lib_->trashEntry(wb_.entry)) {
                focus_ = {};
                loadedFocus_ = {Focus::None, "-", ""};
                openEntry({});
                flash(L"Moved “" + name + L"” to the Trash.", 5);
            }
        });
        y += 34;
    } else {
        panelHeading(c, y, L"WORLDBUILDING", true);
        y += gfx_.text(L"Entries belong to a world, so every book in it shares them. An independent book has its own.\n\n"
                       L"“Appears in” searches the manuscripts for an entry's name and its other names, as whole words.",
                       {c.x, y, c.w, 240}, Font::Small, pal_.inkMuted) + 14;
        panelHeading(c, y, L"KINDS", false);
        for (auto& k : entryKinds()) {
            static const std::map<std::string, const wchar_t*> what = {
                {"character", L"People and creatures in the story."}, {"place", L"Countries, towns, buildings, rooms."},
                {"group", L"Families, guilds, crews, armies, nations."}, {"thing", L"Objects, ships, weapons, heirlooms."},
                {"lore", L"History, magic, beliefs, languages, rules of the world."}, {"event", L"Battles, festivals, disasters, turning points."}};
            gfx_.text(w(entryKindPlural(k)), {c.x, y, c.w, 20}, Font::SmallStrong, pal_.ink, {Align::Left, false});
            y += 20;
            y += gfx_.text(what.at(k), {c.x, y, c.w, 40}, Font::Small, pal_.inkMuted) + 8;
        }
    }
    wb_.panelContent = y + wb_.panelScroll - r.y + 20;
    clearHitClip();
    gfx_.popClip();
}

}  // namespace nrs
