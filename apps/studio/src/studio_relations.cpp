// Worldbuilding › Relationships (0.38.2): the world's own kinds of relationship.
//
// Besides the built-in relationships (Friend of, Lives in, Member of...), a world can have its own: "Admires", read
// from the other side as "Admired by". They come from pasted pages (any Relationships line Writing Studio doesn't know
// becomes one) or are made here, and they're offered in "Add a relationship". The page lists them with how many links
// use each; the panel renames one (its links follow) or changes how it reads from the other side.
#include "studio_app.hpp"
#include "cjui/dialog.hpp"

#include <algorithm>
#include <set>

namespace nrs {

using namespace studio;
using cjui::Align;
using cjui::Font;
using cjui::Rect;

static std::wstring w(const std::string& s) { return widen(s); }

void StudioApp::openRelations(const std::string& select) {
    openEntry({});
    wb_.relationsPage = true;
    wb_.scroll = 0;
    const auto list = lib_->customRelations(wb_.world, wb_.world.empty() ? wb_.book : Id{});
    std::string pick = select;
    if (pick.empty() && !list.empty()) pick = list[0].label;
    setFocus(pick.empty() ? Focus{} : Focus{Focus::Relation, pick, {}});
    if (!select.empty()) focusFieldSoon_ = FName;
    invalidate();
}

void StudioApp::newRelation() {
    commitFields();
    const Id book = wb_.world.empty() ? wb_.book : Id{};
    auto list = lib_->customRelations(wb_.world, book);
    std::string label = "New relationship";
    for (int n = 2; std::any_of(list.begin(), list.end(), [&](const CustomRelation& c) { return c.label == label; }); ++n)
        label = "New relationship " + std::to_string(n);
    list.push_back({label, label + " (other side)"});
    lib_->setCustomRelations(wb_.world, book, list);
    openRelations(label);
    flash(L"Name it, and say how it reads from the other side.", 6);
}

void StudioApp::drawRelations(const Rect& r) {
    const Id book = wb_.world.empty() ? wb_.book : Id{};
    const auto list = lib_->customRelations(wb_.world, book);
    gfx_.pushClip(r);
    setHitClip(r);
    const float pad = r.w > 900 ? 40 : 28;
    const float cw = std::min(r.w - pad * 2, 820.0f);
    const Rect m{r.x + pad, r.y + 26 - wb_.scroll, cw, 0};
    float y = m.y;
    y += pageHeader(m, L"WORLDBUILDING", L"Relationships",
                    L"Your world's own kinds of relationship, each with the way it reads from the other side.");
    {
        float bw = 0;
        smallButton("rel:new", m.x, y, L"New kind of relationship", [this] { newRelation(); }, &bw);
        y += 48;
    }
    gfx_.text(L"YOUR WORLD'S OWN", {m.x, y, m.w, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 26;
    if (list.empty())
        y += gfx_.text(L"None yet. Make one here, or paste a page whose Relationships use a wording Writing Studio doesn't know "
                       L"(“Admires: Jim”): it becomes one of these.",
                       {m.x, y, m.w, 60}, Font::Small, pal_.inkMuted) + 16;
    for (const auto& c : list) {
        const Rect row{m.x, y, m.w, 46};
        const std::string id = "rel:row:" + c.label;
        const bool sel = focus_.kind == Focus::Relation && focus_.id == c.label;
        addHit(id, row, [this, l = c.label] { commitFields(); setFocus({Focus::Relation, l, {}}); invalidate(); });
        const float hv = hoverAmount(id);
        if (sel) gfx_.fillRounded(row, 6, pal_.surface);
        else if (hv > 0.01f) gfx_.fillRounded(row, 6, pal_.surface.withAlpha(0.6f * hv));
        if (sel) gfx_.fill({row.x, row.y + 8, 3, row.h - 16}, pal_.ribbon);
        gfx_.text(w(c.label), {row.x + 14, row.y, row.w * 0.36f, row.h}, Font::BodyStrong, pal_.ink, {Align::Left, false, true, true});
        {
            // A two-way arrow, drawn (not every font has the character).
            const float ax = row.x + row.w * 0.38f, ay = row.y + row.h / 2;
            gfx_.line(ax, ay - 3, ax + 18, ay - 3, pal_.inkFaint, 1.3f);
            gfx_.line(ax + 18, ay - 3, ax + 14, ay - 7, pal_.inkFaint, 1.3f);
            gfx_.line(ax, ay + 3, ax + 18, ay + 3, pal_.inkFaint, 1.3f);
            gfx_.line(ax, ay + 3, ax + 4, ay + 7, pal_.inkFaint, 1.3f);
        }
        gfx_.text(w(c.reverse), {row.x + row.w * 0.38f + 30, row.y, row.w * 0.34f, row.h}, Font::Body, pal_.inkMuted, {Align::Left, false, true, true});
        const int uses = lib_->relationUses(wb_.world, book, c.label);
        gfx_.text(uses == 1 ? L"1 link" : std::to_wstring(uses) + L" links", {row.right() - 100, row.y, 86, row.h}, Font::Small, pal_.inkFaint,
                  {Align::Right, false, false, true});
        if (focused(id)) focusRing(row);
        gfx_.line(row.x, row.bottom(), row.right(), row.bottom(), pal_.rule);
        y += 48;
    }
    y += 24;
    gfx_.text(L"BUILT IN", {m.x, y, m.w, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 24;
    {
        std::wstring all;
        int n = 0;
        for (auto& rr : relations()) {
            if (n++ % 2) continue;   // listed in pairs: each and its other side
            all += (all.empty() ? L"" : L"   ·   ") + w(rr.label) + L" / " + w(rr.reverse);
        }
        y += gfx_.text(all, {m.x, y, m.w, 400}, Font::Small, pal_.inkMuted) + 20;
    }
    wb_.content = y + wb_.scroll - r.y + 40;
    wb_.scroll = std::clamp(wb_.scroll, 0.0f, std::max(0.0f, wb_.content - r.h));
    clearHitClip();
    gfx_.popClip();
}

void StudioApp::drawRelationsPanel(const Rect& r) {
    if (!(focus_ == loadedFocus_)) loadFields();
    gfx_.pushClip(r);
    setHitClip(r);
    const Rect c{r.x + 20, r.y, r.w - 40, 0};
    float y = r.y + 18 - wb_.panelScroll;
    if (focus_.kind == Focus::Relation) {
        panelHeading(c, y, L"RELATIONSHIP", true);
        y = panelField(c, y, L"Name (as it reads on the entry: Admires)", FName);
        y = panelField(c, y, L"From the other side (Admired by)", FAlt);
        y += gfx_.text(L"Renaming it renames every link that uses it.", {c.x, y, c.w, 40}, Font::Small, pal_.inkFaint) + 10;
        linkText("rel:remove", c.x, y, L"Remove this kind of relationship", [this] {
            const Id book = wb_.world.empty() ? wb_.book : Id{};
            const std::string label = focus_.id;
            const int uses = lib_->relationUses(wb_.world, book, label);
            if (uses > 0) {
                const std::wstring q = L"Remove “" + w(label) + L"”?\n\n" + std::to_wstring(uses) +
                                       (uses == 1 ? L" link uses it and will be taken away too." : L" links use it and will be taken away too.");
                if (cjui::messageBox(hwnd(), q.c_str(), L"Relationships", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) return;
            }
            loadedFocus_ = focus_ = Focus{};   // nothing to save
            std::set<Id> mine;
            for (const Entry* e : lib_->entriesOf(wb_.world, book)) mine.insert(e->id);
            for (const Id& id : mine)
                for (auto& l : lib_->linksOf(id))
                    if (l.from == id && l.label == label) lib_->removeLink(l.from, l.to, l.label);
            auto list = lib_->customRelations(wb_.world, book);
            list.erase(std::remove_if(list.begin(), list.end(), [&](const CustomRelation& x) { return x.label == label; }), list.end());
            lib_->setCustomRelations(wb_.world, book, list);
            openRelations({});
        });
        y += 34;
    } else {
        panelHeading(c, y, L"RELATIONSHIPS", true);
        y += gfx_.text(L"Pick one of your world's relationships to rename it or change how it reads from the other side.", {c.x, y, c.w, 60},
                       Font::Small, pal_.inkMuted) + 12;
    }
    wb_.panelContent = y + wb_.panelScroll - r.y + 20;
    clearHitClip();
    gfx_.popClip();
}

void StudioApp::loadRelationFields() {
    for (auto& c : lib_->customRelations(wb_.world, wb_.world.empty() ? wb_.book : Id{}))
        if (c.label == focus_.id) {
            setFieldText(FName, c.label);
            setFieldText(FAlt, c.reverse);
        }
}

void StudioApp::commitRelationFields(const Focus& f) {
    const Id book = wb_.world.empty() ? wb_.book : Id{};
    std::string label = fieldText(FName), reverse = fieldText(FAlt);
    for (auto& c : lib_->customRelations(wb_.world, book))
        if (c.label == f.id) {
            if (label.empty()) label = c.label;
            if (label == c.label && reverse == c.reverse) return;
            if (!lib_->renameRelation(wb_.world, book, c.label, {label, reverse})) { flash(w(lib_->lastError()), 6); return; }
            if (focus_.kind == Focus::Relation && focus_.id == f.id) {
                // The name as kept (with its first letter in capitals): the panel stays on it.
                std::string kept = label;
                kept[0] = (char)std::toupper((unsigned char)kept[0]);
                focus_.id = loadedFocus_.id = kept;
            }
            return;
        }
}

}  // namespace nrs
