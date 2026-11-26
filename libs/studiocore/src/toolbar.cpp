#include "studio/toolbar.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace studio::toolbar {

const char* groupName(int group) {
    switch (group) {
        case Create: return "Create";
        case ImportExport: return "Import & export";
        case Organise: return "Organise";
        case Check: return "Check";
        case Settings: return "Settings";
        default: return "";
    }
}

std::string Custom::toJson() const {
    nlohmann::json j = nlohmann::json::object();
    if (!order.empty()) j["order"] = order;
    if (!groups.empty()) j["groups"] = groups;
    if (!pinned.empty()) j["pinned"] = std::vector<std::string>(pinned.begin(), pinned.end());
    if (!hidden.empty()) j["hidden"] = std::vector<std::string>(hidden.begin(), hidden.end());
    if (off) j["off"] = true;
    return j.dump();
}

Custom Custom::fromJson(const std::string& text) {
    Custom c;
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object()) return c;
    auto strings = [&](const char* key) {
        std::vector<std::string> out;
        if (j.contains(key) && j[key].is_array())
            for (auto& v : j[key]) if (v.is_string() && !v.get<std::string>().empty() && v.get<std::string>().size() < 200) out.push_back(v.get<std::string>());
        return out;
    };
    for (auto& s : strings("order")) if (std::find(c.order.begin(), c.order.end(), s) == c.order.end()) c.order.push_back(s);
    for (auto& s : strings("pinned")) c.pinned.insert(s);
    for (auto& s : strings("hidden")) c.hidden.insert(s);
    if (j.contains("groups") && j["groups"].is_object())
        for (auto it = j["groups"].begin(); it != j["groups"].end(); ++it)
            if (it.value().is_number_integer()) {
                const int g = it.value().get<int>();
                if (g > None && g < kGroups) c.groups[it.key()] = g;
            }
    c.off = j.value("off", false);
    if (c.order.size() > 500) c.order.resize(500);
    return c;
}

int groupOf(const Tool& t, const Custom& c) {
    if (t.rare || t.group <= None || t.group >= kGroups) return None;   // never on the bar, whatever was saved
    const auto it = c.groups.find(t.id);
    return it != c.groups.end() ? it->second : t.group;
}

std::vector<size_t> arrange(const std::vector<Tool>& tools, const Custom& c) {
    std::vector<size_t> out;
    for (size_t i = 0; i < tools.size(); ++i)
        if (groupOf(tools[i], c) != None && !c.hidden.count(tools[i].id)) out.push_back(i);
    auto rank = [&](size_t i) {
        const auto it = std::find(c.order.begin(), c.order.end(), tools[i].id);
        // Ordered tools by their place; a tool the writer has never placed (new since) after them, in its default place.
        return it != c.order.end() ? (long)(it - c.order.begin()) : (long)(c.order.size() + i);
    };
    std::stable_sort(out.begin(), out.end(), [&](size_t a, size_t b) {
        const int ga = groupOf(tools[a], c), gb = groupOf(tools[b], c);
        if (ga != gb) return ga < gb;
        return rank(a) < rank(b);
    });
    return out;
}

void move(const std::vector<Tool>& tools, Custom& c, const std::string& id, int delta) {
    const std::vector<size_t> bar = arrange(tools, c);
    std::vector<std::string> ids;
    for (size_t i : bar) ids.push_back(tools[i].id);
    const auto it = std::find(ids.begin(), ids.end(), id);
    if (it == ids.end() || delta == 0) return;
    const long at = it - ids.begin(), to = at + (delta < 0 ? -1 : 1);
    if (to < 0 || to >= (long)ids.size()) return;
    const int mine = groupOf(tools[bar[(size_t)at]], c), theirs = groupOf(tools[bar[(size_t)to]], c);
    if (mine == theirs) std::swap(ids[(size_t)at], ids[(size_t)to]);
    else setGroup(c, id, theirs, tools);   // across a group's edge: it joins that group, at that end
    // Keep the places of tools not on the bar now (hidden ones), after the bar's.
    for (const std::string& o : c.order)
        if (std::find(ids.begin(), ids.end(), o) == ids.end()) ids.push_back(o);
    c.order = ids;
}

void setGroup(Custom& c, const std::string& id, int group, const std::vector<Tool>& tools) {
    if (group <= None || group >= kGroups) return;
    for (const Tool& t : tools)
        if (t.id == id) {
            if (t.group == group) c.groups.erase(id);
            else c.groups[id] = group;
            return;
        }
}

void togglePinned(Custom& c, const std::string& id) {
    if (!c.pinned.erase(id)) c.pinned.insert(id);
}

void setHidden(Custom& c, const std::string& id, bool hidden) {
    if (hidden) { c.hidden.insert(id); c.pinned.erase(id); }
    else c.hidden.erase(id);
}

Fit fit(const std::vector<Measure>& m, float avail, float gap, float groupGap, float moreW, bool moreAlways) {
    const size_t n = m.size();
    std::vector<bool> in(n, true), label(n, true);
    auto widthOf = [&](size_t i) { return label[i] || m[i].iconOnly <= 0 ? m[i].withLabel : m[i].iconOnly; };
    auto total = [&](bool more) {
        float w = 0;
        int lastGroup = -1;
        bool any = false;
        for (size_t i = 0; i < n; ++i) {
            if (!in[i]) continue;
            if (any) w += gap + (m[i].group != lastGroup ? groupGap : 0);
            w += widthOf(i);
            lastGroup = m[i].group;
            any = true;
        }
        if (more) w += (any ? gap + groupGap : 0) + moreW;
        return w;
    };
    auto anyOut = [&] { return std::find(in.begin(), in.end(), false) != in.end(); };
    auto fits = [&] { return total(moreAlways || anyOut()) <= avail; };
    bool ok = fits();
    // 1. The unpinned tools to their icons, then 2. the pinned ones too.
    for (int pass = 0; pass < 2 && !ok; ++pass) {
        for (size_t i = 0; i < n; ++i)
            if ((pass == 0 ? !m[i].pinned : m[i].pinned) && m[i].iconOnly > 0) label[i] = false;
        ok = fits();
    }
    // 3. Into More from the end: unpinned first, then pinned.
    for (int pass = 0; pass < 2 && !ok; ++pass)
        for (size_t k = n; k-- > 0 && !ok;) {
            if (!in[k] || (pass == 0 ? m[k].pinned : !m[k].pinned)) continue;
            in[k] = false;
            ok = fits();
        }
    Fit f;
    float x = 0;
    int lastGroup = -1;
    for (size_t i = 0; i < n; ++i) {
        if (!in[i]) { f.overflow.push_back(i); continue; }
        if (!f.shown.empty()) x += gap + (m[i].group != lastGroup ? groupGap : 0);
        const float w = widthOf(i);
        f.shown.push_back({i, label[i] || m[i].iconOnly <= 0, x, w});
        x += w;
        lastGroup = m[i].group;
    }
    f.more = moreAlways || !f.overflow.empty();
    f.moreX = f.shown.empty() ? 0 : x + gap + groupGap;
    return f;
}

}  // namespace studio::toolbar
