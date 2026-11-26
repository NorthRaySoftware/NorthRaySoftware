// Writing Studio: the workspaces' toolbars (phase 3 of the director's combined request, 0.100).
// One row of tools under the page's tabs, built from the same list as the workspace's tools menu. What doesn't fit goes
// to icons, then under More; rare and destructive tools are always under More. Right-click a tool to pin, hide, move it
// or change its group; the bar can be hidden (its menu button comes back), and Settings can show every bar as menus.
#include "studio_app.hpp"
#include "cjui/format.hpp"

#include <algorithm>
#include <set>

namespace nrs {

using cjui::Align;
using cjui::Font;
using cjui::Rect;
namespace tb = studio::toolbar;
static std::wstring w(const std::string& s) { return widen(s); }

namespace {
constexpr float kBarH = 34, kGap = 4, kGroupGap = 13;

// Drops headings with nothing under them and doubled, leading or trailing lines.
std::vector<StudioApp::ToolItem> tidyMenu(std::vector<StudioApp::ToolItem> in) {
    std::vector<StudioApp::ToolItem> out;
    for (size_t i = 0; i < in.size(); ++i) {
        const auto& t = in[i];
        if (t.kind == 2) {   // a heading: only if an item follows before the next line or heading
            bool any = false;
            for (size_t k = i + 1; k < in.size() && in[k].kind == 0; ++k) any = true;
            if (!any) continue;
        }
        if (t.kind == 1 && (out.empty() || out.back().kind == 1)) continue;
        out.push_back(t);
    }
    while (!out.empty() && out.back().kind != 0) out.pop_back();
    return out;
}
}  // namespace

bool StudioApp::toolBarOn(const std::string& bar) const {
    if (settings_.toolsAsMenus) return false;
    const auto it = settings_.toolBars.find(bar);
    return it == settings_.toolBars.end() || !it->second.off;
}

void StudioApp::changeBar(const std::string& bar, const std::function<void(tb::Custom&)>& f) {
    f(settings_.toolBars[bar]);
    saveSettings();
    invalidate();
}

std::vector<StudioApp::ToolItem> StudioApp::withBarItem(const std::string& bar, std::vector<ToolItem> items) {
    if (!settings_.toolsAsMenus) {   // the bar was hidden: bring it back from its menu
        items.push_back(ToolItem::line());
        items.push_back(ToolItem::item(L"Show the toolbar", [this, bar] { changeBar(bar, [](tb::Custom& c) { c.off = false; }); }));
    }
    return items;
}

std::vector<StudioApp::ToolItem> StudioApp::toolBarMenu(const std::string& bar, const std::string& toolId) {
    std::vector<ToolItem> t;
    const BarState& st = bars_[bar];
    const tb::Custom& c = settings_.toolBars[bar];
    const auto tools = st.tools;
    const auto known = std::find_if(tools.begin(), tools.end(), [&](const tb::Tool& x) { return x.id == toolId; });
    if (known != tools.end()) {
        const std::wstring name = st.labels.count(toolId) ? st.labels.at(toolId) : L"this tool";
        const bool pinned = c.pinned.count(toolId) > 0;
        t.push_back(ToolItem::heading(name));
        t.push_back(ToolItem::item(pinned ? L"Unpin" : L"Pin (keeps its words, and stays on the bar longest)",
                                   [this, bar, toolId] { changeBar(bar, [&](tb::Custom& cc) { tb::togglePinned(cc, toolId); }); }, pinned));
        t.push_back(ToolItem::item(L"Move left", [this, bar, toolId, tools] { changeBar(bar, [&](tb::Custom& cc) { tb::move(tools, cc, toolId, -1); }); }));
        t.push_back(ToolItem::item(L"Move right", [this, bar, toolId, tools] { changeBar(bar, [&](tb::Custom& cc) { tb::move(tools, cc, toolId, 1); }); }));
        std::vector<ToolItem> groups;
        const int now = tb::groupOf(*known, c);
        for (int g = tb::Create; g < tb::kGroups; ++g)
            groups.push_back(ToolItem::item(w(tb::groupName(g)), [this, bar, toolId, g, tools] {
                changeBar(bar, [&](tb::Custom& cc) { tb::setGroup(cc, toolId, g, tools); });
            }, g == now));
        t.push_back(ToolItem::menu(L"Move to group", groups));
        t.push_back(ToolItem::item(L"Hide from the bar (it stays under More)", [this, bar, toolId] {
            changeBar(bar, [&](tb::Custom& cc) { tb::setHidden(cc, toolId, true); });
        }));
        t.push_back(ToolItem::line());
    }
    if (!c.hidden.empty()) {
        std::vector<ToolItem> hidden;
        for (const std::string& id : c.hidden) {
            if (!st.labels.count(id)) continue;   // not a tool of this bar any more
            hidden.push_back(ToolItem::item(st.labels.at(id), [this, bar, id] { changeBar(bar, [&](tb::Custom& cc) { tb::setHidden(cc, id, false); }); }));
        }
        if (!hidden.empty()) t.push_back(ToolItem::menu(L"Show a hidden tool", hidden));
    }
    t.push_back(ToolItem::item(L"Reset this toolbar", [this, bar] {
        changeBar(bar, [](tb::Custom& cc) { cc = {}; });
        flash(L"The toolbar is back as it came: its groups, order and tools.");
    }));
    t.push_back(ToolItem::item(L"Hide the toolbar", [this, bar] {
        changeBar(bar, [](tb::Custom& cc) { cc.off = true; });
        flash(L"The tools are in the tools menu now. Its last item brings the toolbar back.");
    }));
    t.push_back(ToolItem::item(L"Show tools as menus everywhere…", [this] { openSettings(); }));
    return t;
}

float StudioApp::toolBar(const std::string& bar, const Rect& r, const std::vector<ToolItem>& items) {
    if (!toolBarOn(bar)) return 0;
    const tb::Custom& c = settings_.toolBars[bar];
    // The tools: every item with an id (bar tools, and rare ones kept for More).
    BarState st;
    std::vector<const ToolItem*> byIndex;
    for (const ToolItem& t : items)
        if (t.kind == 0 && !t.id.empty()) {
            st.tools.push_back({t.id, t.group, t.rare});
            st.labels[t.id] = t.label;
            byIndex.push_back(&t);
        }
    const std::vector<size_t> order = tb::arrange(st.tools, c);
    auto plain = [](std::wstring s) {   // "Languages  (3)" is the tool's name on the bar too, but not "…"
        if (s.size() > 1 && s.back() == L'…') s.pop_back();
        return s;
    };
    std::vector<tb::Measure> ms;
    for (size_t i : order) {
        const ToolItem& t = *byIndex[i];
        const bool sub = !t.sub.empty();
        tb::Measure m;
        m.withLabel = gfx_.measureWidth(plain(t.label), Font::Body) + 22 + (t.icon != cjui::Icon::None ? 22 : 0) + (sub ? 14 : 0);
        m.iconOnly = t.icon != cjui::Icon::None ? 34 + (sub ? 10 : 0) : 0;
        m.pinned = c.pinned.count(t.id) > 0;
        m.group = tb::groupOf(st.tools[i], c);
        ms.push_back(m);
    }
    const float hideW = 30, moreW = gfx_.measureWidth(L"More", Font::Body) + 40;
    // Anything not on the bar goes in More: rare and menu-only tools, hidden ones, and what didn't fit.
    bool menuOnly = false;
    for (const ToolItem& t : items)
        if (t.kind == 0 && (t.id.empty() || t.rare || t.group == 0 || c.hidden.count(t.id))) menuOnly = true;
    const tb::Fit f = tb::fit(ms, r.w - hideW - 8, kGap, kGroupGap, moreW, menuOnly);
    std::set<std::string> onBar;
    const float y = r.y + (r.h - kBarH) / 2;
    int lastGroup = -1;
    for (const tb::Placed& p : f.shown) {
        const ToolItem& t = *byIndex[order[p.index]];
        onBar.insert(t.id);
        const Rect b{r.x + p.x, y, p.w, kBarH};
        const int g = ms[p.index].group;
        if (lastGroup >= 0 && g != lastGroup)   // a quiet rule between groups
            gfx_.line(b.x - kGroupGap / 2 - kGap / 2, y + 8, b.x - kGroupGap / 2 - kGap / 2, y + kBarH - 8, pal_.rule);
        lastGroup = g;
        const std::string hid = "tbar:" + bar + ":" + t.id;
        const bool sub = !t.sub.empty();
        auto act = t.act;
        auto subItems = t.sub;
        addHit(hid, b, [this, act, subItems, b] {
            if (!subItems.empty()) { showTools(subItems, b.x, b.bottom() + 4); return; }
            if (act) act();
            invalidate();
        });
        tooltip(hid, p.label ? (sub ? plain(t.label) + L" ▾" : std::wstring()) : plain(t.label));
        const float hv = t.enabled ? hoverAmount(hid) : 0.0f;
        const bool on = t.checked;
        if (on) gfx_.fillRounded(b, 8, pal_.ribbon.withAlpha(pal_.dark ? 0.20f : 0.12f));
        if (hv > 0.01f) gfx_.fillRounded(b, 8, pal_.ink.withAlpha(0.06f * hv));
        const cjui::Color ink = !t.enabled ? pal_.inkFaint : on ? pal_.ink : pal_.inkMuted.mix(pal_.ink, std::max(hv, 0.25f));
        float tx = b.x + 11;
        if (t.icon != cjui::Icon::None) {
            cjui::drawIcon(gfx_, t.icon, {p.label ? tx : b.x + 9, b.y + (b.h - 16) / 2, 16, 16}, ink, 1.4f);
            tx += 22;
        }
        if (p.label) gfx_.text(plain(t.label), {tx, b.y, b.right() - tx, b.h}, Font::Body, ink, {Align::Left, false, true, true});
        if (sub) {
            const float cx = b.right() - 11, cy = b.y + b.h / 2;
            gfx_.line(cx - 3, cy - 1.5f, cx, cy + 1.5f, ink, 1.3f);
            gfx_.line(cx, cy + 1.5f, cx + 3, cy - 1.5f, ink, 1.3f);
        }
        if (c.pinned.count(t.id)) gfx_.circle(b.right() - 4, b.y + 4, 2, pal_.ribbon, true);   // pinned: a small dot
        if (focused(hid)) focusRing(b);
    }
    if (f.more) {
        std::vector<ToolItem> rest;
        for (const ToolItem& t : items)
            if (t.kind != 0 || t.id.empty() || !onBar.count(t.id)) rest.push_back(t);
        rest = tidyMenu(std::move(rest));
        const Rect b{r.x + f.moreX, y, moreW, kBarH};
        if (!f.shown.empty()) gfx_.line(b.x - kGroupGap / 2 - kGap / 2, y + 8, b.x - kGroupGap / 2 - kGap / 2, y + kBarH - 8, pal_.rule);
        const std::string mid = "tbar:" + bar + ":#more";
        addHit(mid, b, [this, rest, b] { showTools(rest, b.x, b.bottom() + 4); });
        tooltip(mid, L"The rest of the tools");
        const float hv = hoverAmount(mid);
        if (hv > 0.01f) gfx_.fillRounded(b, 8, pal_.ink.withAlpha(0.06f * hv));
        const cjui::Color ink = pal_.inkMuted.mix(pal_.ink, std::max(hv, 0.25f));
        gfx_.text(L"More", {b.x + 12, b.y, b.w, b.h}, Font::Body, ink, {Align::Left, false, false, true});
        const float cx = b.x + 12 + gfx_.measureWidth(L"More", Font::Body) + 10, cy = b.y + b.h / 2;
        gfx_.line(cx - 3.5f, cy - 1.5f, cx, cy + 2, ink, 1.4f);
        gfx_.line(cx, cy + 2, cx + 3.5f, cy - 1.5f, ink, 1.4f);
        if (focused(mid)) focusRing(b);
    }
    {   // Hide the bar: a small chevron at its end (the tools menu comes back in its place).
        const Rect hb{r.right() - hideW, y + 2, hideW, kBarH - 4};
        const std::string hid = "tbar:" + bar + ":#hide";
        addHit(hid, hb, [this, bar] {
            changeBar(bar, [](tb::Custom& cc) { cc.off = true; });
            flash(L"The tools are in the tools menu now. Its last item brings the toolbar back.");
        });
        tooltip(hid, L"Hide the toolbar (right-click a tool to change the bar)");
        const float hv = hoverAmount(hid);
        if (hv > 0.01f) gfx_.fillRounded(hb, 7, pal_.ink.withAlpha(0.06f * hv));
        cjui::drawIcon(gfx_, cjui::Icon::Up, {hb.x + (hb.w - 14) / 2, hb.y + (hb.h - 14) / 2, 14, 14}, pal_.inkFaint.mix(pal_.ink, hv), 1.4f);
        if (focused(hid)) focusRing(hb);
    }
    bars_[bar] = std::move(st);
    return r.h;
}

}  // namespace nrs
