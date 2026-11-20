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

Library::FamilyLayout Library::familyLayout(const Id& root, int limit, bool bloodline) const {
    FamilyLayout out;
    const Entry* r = entry(root);
    if (!r || r->deleted) return out;
    std::map<Id, int> gen;
    std::vector<Id> order;
    std::map<Id, Family> fam;
    std::set<Id> inLaws;
    auto famOf = [&](const Id& id) -> const Family& {
        auto it = fam.find(id);
        if (it == fam.end()) it = fam.emplace(id, familyOf(id)).first;
        return it->second;
    };
    auto add = [&](const Id& id, int g) {
        if (gen.count(id)) return false;
        if ((int)gen.size() >= limit) { out.cut = true; return false; }
        gen[id] = g;
        order.push_back(id);
        return true;
    };
    // Descendants of the people in `from` (breadth first), with each one's partners beside them as in-laws.
    auto descend = [&](std::deque<Id> queue) {
        while (!queue.empty()) {
            const Id id = queue.front();
            queue.pop_front();
            for (const Id& c : famOf(id).children) {
                if (add(c, gen[id] + 1)) queue.push_back(c);
                // Brothers and sisters linked as such (not through a parent here) belong too.
                for (const Id& sib : famOf(c).siblings)
                    if (gen.count(c) && add(sib, gen[c])) queue.push_back(sib);
            }
        }
    };
    auto partnersIn = [&](const std::vector<Id>& members) {
        for (const Id& m : members) {
            if (inLaws.count(m)) continue;
            for (const Id& p : famOf(m).partners)
                if (add(p, gen[m])) inLaws.insert(p);
            // The other parent of a child, partnered or not.
            for (const Id& c : famOf(m).children)
                if (gen.count(c))
                    for (const Id& op : famOf(c).parents)
                        if (op != m && add(op, gen[m])) inLaws.insert(op);
        }
    };
    add(root, 0);
    if (bloodline) {
        // The founder, their brothers and sisters, and the founders they had children with.
        for (const Id& s : famOf(root).siblings) add(s, 0);
        for (const Id& p : famOf(root).partners)
            if (famOf(p).parents.empty()) add(p, 0);
        // The founder's children's other parents, partnered or not, when they have no parents either: founders too.
        for (const Id& c : famOf(root).children)
            for (const Id& op : famOf(c).parents)
                if (op != root && famOf(op).parents.empty()) add(op, 0);
        std::deque<Id> q(order.begin(), order.end());
        descend(q);
    } else {
        // Ancestors, straight up every line.
        std::deque<Id> up{root};
        while (!up.empty()) {
            const Id id = up.front();
            up.pop_front();
            for (const Id& p : famOf(id).parents)
                if (add(p, gen[id] - 1)) up.push_back(p);
        }
        // Brothers and sisters, then everyone's descendants from the person's generation down.
        for (const Id& s : famOf(root).siblings) add(s, 0);
        std::deque<Id> q;
        for (const Id& id : order) if (gen[id] == 0) q.push_back(id);
        descend(q);
    }
    {
        const std::vector<Id> members = order;
        partnersIn(members);
    }
    std::set<std::pair<Id, Id>> couples, pc;
    for (const Id& id : order) {
        for (const Id& p : famOf(id).partners)
            if (gen.count(p)) couples.insert(id < p ? std::make_pair(id, p) : std::make_pair(p, id));
        for (const Id& c : famOf(id).children)
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
            for (const Id& p : famOf(u[k]).partners)
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
                for (const Id& p : famOf(m).parents)
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
                for (const Id& m : u) for (const Id& c : famOf(m).children) if (x.count(c) && gen[c] == g + 1) { sum += x[c]; ++n; }
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
    for (const Id& id : order) out.people.push_back({id, gen[id], x[id] - minX, inLaws.count(id) > 0});
    out.width = order.empty() ? 0 : maxX - minX + 1;
    return out;
}

std::vector<Library::FamilyGroup> Library::families(const Id& worldId, const Id& bookId) const {
    std::vector<FamilyGroup> out;
    std::set<Id> own;   // people already in a family as one of its own (not married in)
    std::vector<const Entry*> chars;
    for (const Entry* e : entriesOf(worldId, bookId)) if (e->kind == "character") chars.push_back(e);
    auto addGroup = [&](const Id& founder) {
        const FamilyLayout lay = familyLayout(founder, 400, true);
        std::map<std::string, int> names;
        int count = 0;
        for (const auto& p : lay.people) {
            if (p.inLaw) continue;
            own.insert(p.id);
            ++count;
            if (const Entry* m = entry(p.id)) if (const std::string s = surnameOf(m->name); !s.empty()) ++names[s];
        }
        if (lay.people.size() < 2) return;
        std::string surname;
        int best = 1;
        for (auto& [s, n] : names) if (n > best) { best = n; surname = s; }
        out.push_back({founder, surname, (int)lay.people.size()});
    };
    // Founders: no parents, with children whose other parents have no parents either (else they married in).
    for (const Entry* e : chars) {
        if (own.count(e->id)) continue;
        const Family f = familyOf(e->id);
        if (!f.parents.empty() || f.children.empty()) continue;
        bool marriedIn = false;
        for (const Id& c : f.children)
            for (const Id& op : familyOf(c).parents)
                if (op != e->id && !familyOf(op).parents.empty()) marriedIn = true;
        if (!marriedIn) addGroup(e->id);
    }
    // Brothers and sisters with no parents (and no family yet).
    for (const Entry* e : chars) {
        if (own.count(e->id)) continue;
        const Family f = familyOf(e->id);
        if (f.parents.empty() && !f.siblings.empty()) addGroup(e->id);
    }
    std::stable_sort(out.begin(), out.end(), [](const FamilyGroup& a, const FamilyGroup& b) { return a.people > b.people; });
    return out;
}

}  // namespace studio
