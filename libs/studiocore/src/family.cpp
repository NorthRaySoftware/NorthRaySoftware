// Family trees from the bible's relationships: who a character's parents, partners, siblings and
// children are, and everyone they're joined to laid out by generation for drawing.
#include "studio/library.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <set>

namespace studio {

namespace {

bool isParentLink(const std::string& l) { return l == "Parent of"; }
bool isChildLink(const std::string& l) { return l == "Child of"; }
bool isPartnerLink(const std::string& l) { return l == "Partner of" || l == "Married to"; }
bool isSiblingLink(const std::string& l) { return l == "Sibling of"; }

std::string surnameOf(const std::string& name) {
    size_t end = name.size();
    while (end && name[end - 1] == ' ') --end;
    const size_t sp = name.rfind(' ', end ? end - 1 : 0);
    if (sp == std::string::npos || sp + 1 >= end) return {};
    return name.substr(sp + 1, end - sp - 1);
}

}  // namespace

Library::Family Library::familyOf(const Id& id) const { return familyOf(id, true); }

Library::Family Library::familyOf(const Id& id, bool sharedParents) const {
    Family f;
    std::set<Id> parents, partners, siblings, children;
    auto live = [&](const Id& x) { const Entry* e = entry(x); return e && !e->deleted && x != id; };
    for (const EntryLink& l : linksOf(id)) {
        const bool out = l.from == id;
        const Id other = out ? l.to : l.from;
        if (!live(other)) continue;
        if ((out && isChildLink(l.label)) || (!out && isParentLink(l.label))) parents.insert(other);
        else if ((out && isParentLink(l.label)) || (!out && isChildLink(l.label))) children.insert(other);
        else if (isPartnerLink(l.label)) partners.insert(other);
        else if (isSiblingLink(l.label)) siblings.insert(other);
    }
    // Anyone else sharing a parent is a sibling.
    if (sharedParents)
        for (const Id& p : parents)
            for (const Id& c : familyOf(p, false).children) if (c != id) siblings.insert(c);
    auto sorted = [&](const std::set<Id>& s) {
        std::vector<Id> v(s.begin(), s.end());
        std::sort(v.begin(), v.end(), [&](const Id& a, const Id& b) { return entry(a)->name < entry(b)->name; });
        return v;
    };
    f.parents = sorted(parents);
    f.partners = sorted(partners);
    f.siblings = sorted(siblings);
    f.children = sorted(children);
    return f;
}

Library::FamilyLayout Library::familyLayout(const Id& root, int limit) const {
    FamilyLayout out;
    const Entry* r = entry(root);
    if (!r || r->deleted) return out;
    // Everyone joined by family, each with a generation (breadth first from the root).
    std::map<Id, int> gen;
    std::vector<Id> order;
    std::map<Id, Family> fam;
    std::deque<Id> queue{root};
    gen[root] = 0;
    while (!queue.empty()) {
        const Id id = queue.front();
        queue.pop_front();
        order.push_back(id);
        Family f = familyOf(id);
        auto visit = [&](const std::vector<Id>& ids, int dg) {
            for (const Id& o : ids) {
                if (gen.count(o)) continue;
                if ((int)gen.size() >= limit) { out.cut = true; continue; }
                gen[o] = gen[id] + dg;
                queue.push_back(o);
            }
        };
        visit(f.partners, 0);
        visit(f.parents, -1);
        visit(f.children, 1);
        visit(f.siblings, 0);
        fam[id] = std::move(f);
    }
    std::set<std::pair<Id, Id>> couples, pc;
    for (const Id& id : order) {
        for (const Id& p : fam[id].partners)
            if (gen.count(p)) couples.insert(id < p ? std::make_pair(id, p) : std::make_pair(p, id));
        for (const Id& c : fam[id].children)
            if (gen.count(c)) pc.insert({id, c});
    }
    out.couples.assign(couples.begin(), couples.end());
    out.parentChild.assign(pc.begin(), pc.end());
    int lo = 0, hi = 0;
    for (auto& [id, g] : gen) { lo = std::min(lo, g); hi = std::max(hi, g); }
    out.minGeneration = lo;
    out.maxGeneration = hi;

    // Units: a person and their partners in the same generation, side by side.
    std::map<Id, int> unitOf;
    std::vector<std::vector<Id>> units;
    for (const Id& id : order) {
        if (unitOf.count(id)) continue;
        std::vector<Id> u{id};
        unitOf[id] = (int)units.size();
        for (size_t k = 0; k < u.size(); ++k)
            for (const Id& p : fam[u[k]].partners)
                if (gen.count(p) && gen[p] == gen[id] && !unitOf.count(p)) { unitOf[p] = (int)units.size(); u.push_back(p); }
        units.push_back(u);
    }
    std::map<Id, float> x;
    // Generation by generation from the eldest: each unit placed under the middle of its people's parents,
    // in that order, then pushed right so none overlap.
    std::map<int, std::vector<int>> byGen;
    for (size_t i = 0; i < units.size(); ++i) byGen[gen[units[i][0]]].push_back((int)i);
    std::map<int, float> wish;   // a unit's wanted centre
    for (int g = lo; g <= hi; ++g) {
        auto& us = byGen[g];
        for (int ui : us) {
            float sum = 0;
            int n = 0;
            for (const Id& m : units[(size_t)ui])
                for (const Id& p : fam[m].parents)
                    if (x.count(p)) { sum += x[p]; ++n; }
            wish[ui] = n ? sum / n : 1e9f;   // no parents drawn: after the others, in the order met
        }
        std::stable_sort(us.begin(), us.end(), [&](int a, int b) { return wish[a] < wish[b]; });
        float next = 0;
        for (int ui : us) {
            const auto& u = units[(size_t)ui];
            const float w = (float)u.size();
            float left = wish[ui] < 1e8f ? wish[ui] - (w - 1) / 2 : next;
            left = std::max(left, next);
            for (size_t k = 0; k < u.size(); ++k) x[u[k]] = left + (float)k;
            next = left + w + 0.25f;   // a little gap between families
        }
    }
    // Parents over their children: move each unit (eldest last) towards the middle of its children,
    // as far as its neighbours allow.
    for (int pass = 0; pass < 2; ++pass)
        for (int g = hi - 1; g >= lo; --g) {
            auto& us = byGen[g];
            for (size_t i = 0; i < us.size(); ++i) {
                const auto& u = units[(size_t)us[i]];
                float sum = 0;
                int n = 0;
                for (const Id& m : u) for (const Id& c : fam[m].children) if (x.count(c) && gen[c] == g + 1) { sum += x[c]; ++n; }
                if (!n) continue;
                const float w = (float)u.size();
                const float want = sum / n - (w - 1) / 2;
                const float minL = i ? x[units[(size_t)us[i - 1]].back()] + 1.25f : -1e9f;
                const float maxL = i + 1 < us.size() ? x[units[(size_t)us[i + 1]].front()] - 0.25f - w : 1e9f;
                const float left = std::clamp(want, minL, std::max(minL, maxL));
                for (size_t k = 0; k < u.size(); ++k) x[u[k]] = left + (float)k;
            }
        }
    float minX = 1e9f, maxX = -1e9f;
    for (auto& [id, v] : x) { minX = std::min(minX, v); maxX = std::max(maxX, v); }
    for (const Id& id : order) out.people.push_back({id, gen[id], x[id] - minX});
    out.width = order.empty() ? 0 : maxX - minX + 1;
    return out;
}

std::vector<Library::FamilyGroup> Library::families(const Id& worldId, const Id& bookId) const {
    std::vector<FamilyGroup> out;
    std::set<Id> seen;
    for (const Entry* e : entriesOf(worldId, bookId)) {
        if (e->kind != "character" || seen.count(e->id)) continue;
        const Family f = familyOf(e->id);
        if (f.parents.empty() && f.partners.empty() && f.siblings.empty() && f.children.empty()) continue;
        const FamilyLayout lay = familyLayout(e->id, 400);
        std::map<std::string, int> names;
        Id root;
        int rootGen = 1 << 30;
        for (const auto& p : lay.people) {
            seen.insert(p.id);
            if (const Entry* m = entry(p.id)) {
                if (const std::string s = surnameOf(m->name); !s.empty()) ++names[s];
                if (p.generation < rootGen) { rootGen = p.generation; root = p.id; }
            }
        }
        std::string surname;
        int best = 1;
        for (auto& [s, n] : names) if (n > best) { best = n; surname = s; }
        out.push_back({root, surname, (int)lay.people.size()});
    }
    std::stable_sort(out.begin(), out.end(), [](const FamilyGroup& a, const FamilyGroup& b) { return a.people > b.people; });
    return out;
}

}  // namespace studio
