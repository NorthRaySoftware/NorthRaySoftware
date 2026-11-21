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

// The kinds of parent, child and sibling links (0.46). Each returns whether the label is one, and its kind.
bool parentLink(const std::string& l, std::string& how) {
    if (l == "Parent of") how = "";
    else if (l == "Adoptive parent of") how = "adoptive";
    else if (l == "Step-parent of") how = "step";
    else if (l == "Foster parent of") how = "foster";
    else return false;
    return true;
}
bool childLink(const std::string& l, std::string& how) {
    if (l == "Child of") how = "";
    else if (l == "Adopted child of") how = "adoptive";
    else if (l == "Stepchild of") how = "step";
    else if (l == "Foster child of") how = "foster";
    else return false;
    return true;
}
bool isPartnerLink(const std::string& l) { return l == "Partner of" || l == "Married to" || l == "Engaged to"; }
bool siblingLink(const std::string& l, std::string& how) {
    if (l == "Sibling of") how = "";
    else if (l == "Twin of") how = "twin";
    else if (l == "Triplet of") how = "triplet";
    else if (l == "Half-sibling of") how = "half";
    else if (l == "Step-sibling of") how = "step";
    else if (l == "Adoptive sibling of") how = "adoptive";
    else if (l == "Foster sibling of") how = "foster";
    else return false;
    return true;
}

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
    std::set<Id> parents, partners, siblings, children, former;
    std::map<Id, std::string> linkedSibling;   // siblings by their own link, with its kind
    auto live = [&](const Id& x) { const Entry* e = entry(x); return e && !e->deleted && x != id; };
    for (const EntryLink& l : linksOf(id)) {
        const bool out = l.from == id;
        const Id other = out ? l.to : l.from;
        if (!live(other)) continue;
        std::string how;
        if ((out && childLink(l.label, how)) || (!out && parentLink(l.label, how))) {
            parents.insert(other);
            if (!f.how.count(other) || how.empty()) f.how[other] = how;   // a birth link wins over another kind
        } else if ((out && parentLink(l.label, how)) || (!out && childLink(l.label, how))) {
            children.insert(other);
            if (!f.how.count(other) || how.empty()) f.how[other] = how;
        } else if (isPartnerLink(l.label)) {
            partners.insert(other);
            const std::string h = l.label == "Married to" ? "married" : l.label == "Engaged to" ? "engaged" : "";
            auto& cur = f.partnerHow[other];
            if (cur != "married") cur = h.empty() ? cur : h;   // married over engaged over a plain partner
        } else if (l.label == "Former partner of") former.insert(other);
        else if (siblingLink(l.label, how)) {
            siblings.insert(other);
            if (!linkedSibling.count(other) || !how.empty()) linkedSibling[other] = how;   // the more exact kind wins
        }
    }
    // A triplet's triplets are this person's too (Zia linked to Jimmy and Johney makes Jimmy and Johney triplets) (0.57).
    {
        std::vector<Id> trip;
        for (auto& [o, how] : linkedSibling) if (how == "triplet") trip.push_back(o);
        for (const Id& t : trip)
            for (const EntryLink& l : linksOf(t)) {
                if (l.label != "Triplet of") continue;
                const Id x = l.from == t ? l.to : l.from;
                if (!live(x)) continue;
                siblings.insert(x);
                linkedSibling[x] = "triplet";
            }
    }
    for (const Id& p : partners) former.erase(p);   // partners again now
    // Anyone else sharing a parent is a sibling: full when their birth parents are the same, half when not, and of the
    // parent's kind when the parent they share is adoptive, step or foster to either.
    if (sharedParents) {
        std::set<Id> myBirth;
        for (const Id& p : parents) if (f.how[p].empty()) myBirth.insert(p);
        for (const Id& p : parents) {
            const Family pf = familyOf(p, false);
            for (const Id& c : pf.children) {
                if (c == id || !live(c)) continue;
                siblings.insert(c);
                if (linkedSibling.count(c)) continue;
                std::string how;
                const std::string mine = f.how[p];
                const auto it = pf.how.find(c);
                const std::string theirs = it == pf.how.end() ? std::string() : it->second;
                if (!mine.empty()) how = mine;
                else if (!theirs.empty()) how = theirs;
                else {
                    std::set<Id> theirBirth;
                    const Family cf = familyOf(c, false);
                    for (const Id& q : cf.parents) if (auto h = cf.how.find(q); h == cf.how.end() || h->second.empty()) theirBirth.insert(q);
                    how = (myBirth.size() >= 2 || theirBirth.size() >= 2) && myBirth != theirBirth ? "half" : "";
                }
                // Through two parents, the closer kind stands (full over half, half over step...).
                auto rank = [](const std::string& h) { return h.empty() ? 0 : h == "half" ? 1 : h == "adoptive" ? 2 : h == "step" ? 3 : 4; };
                if (auto cur = f.how.find(c); cur == f.how.end() || rank(how) < rank(cur->second)) f.how[c] = how;
            }
        }
    }
    for (auto& [o, how] : linkedSibling) f.how[o] = how;
    auto sorted = [&](const std::set<Id>& s) {
        std::vector<Id> v(s.begin(), s.end());
        std::sort(v.begin(), v.end(), [&](const Id& a, const Id& b) { return entry(a)->name < entry(b)->name; });
        return v;
    };
    f.parents = sorted(parents);
    f.partners = sorted(partners);
    f.siblings = sorted(siblings);
    f.children = sorted(children);
    f.formerPartners = sorted(former);
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
        // Founders together: partners with no parents, and the other parents (partnered or not, with no parents) of the
        // founders' children, followed on from each other (a founder's two wives in turn, say) (0.46).
        for (size_t i = 0; i < order.size(); ++i) {
            const Id f = order[i];
            if (gen[f] != 0) continue;
            // A founder's own brothers and sisters belong to the founding generation too (Mah'los beside Zah'ra) (0.47).
            if (famOf(f).parents.empty())
                for (const Id& sb : famOf(f).siblings)
                    if (famOf(sb).parents.empty()) add(sb, 0);
            for (const Id& p : famOf(f).partners)
                if (famOf(p).parents.empty()) add(p, 0);
            for (const Id& c : famOf(f).children)
                for (const Id& op : famOf(c).parents)
                    if (op != f && famOf(op).parents.empty()) add(op, 0);
        }
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
    for (auto& [par, ch] : pc) {
        const auto& h = famOf(ch).how;
        const auto it = h.find(par);
        out.parentHow[{par, ch}] = it == h.end() ? std::string() : it->second;
    }
    // Brothers and sisters of a kind, and any joined only by their own link with no shared parent drawn.
    {
        std::set<std::pair<Id, Id>> seen;
        for (const Id& id : order)
            for (const Id& sb : famOf(id).siblings) {
                if (!gen.count(sb) || gen[sb] != gen[id]) continue;
                const auto key = id < sb ? std::make_pair(id, sb) : std::make_pair(sb, id);
                if (!seen.insert(key).second) continue;
                bool shared = false;
                for (const Id& p : famOf(id).parents)
                    if (gen.count(p) && std::find(famOf(sb).parents.begin(), famOf(sb).parents.end(), p) != famOf(sb).parents.end()) shared = true;
                const auto it = famOf(id).how.find(sb);
                const std::string how = it == famOf(id).how.end() ? std::string() : it->second;
                if (how.empty() && shared) continue;   // full brothers and sisters: their parents' lines say it
                if (how == "half" && shared) continue;  // half: shown by hanging from different parents
                out.siblings.push_back({key.first, key.second, how, shared});
            }
    }
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
        // Someone with two or more partners goes in the middle, a partner either side, so each couple sits together
        // (a man between his late first wife and his second, say) (0.46).
        if (u.size() >= 3) {
            Id hub;
            size_t most = 0;
            for (const Id& m : u) {
                size_t n = 0;
                for (const Id& p : famOf(m).partners) if (std::find(u.begin(), u.end(), p) != u.end()) ++n;
                if (n > most) { most = n; hub = m; }
            }
            if (most >= 2) {
                std::vector<Id> left, right, rest;
                for (const Id& p : famOf(hub).partners)
                    if (std::find(u.begin(), u.end(), p) != u.end()) (left.size() <= right.size() ? left : right).push_back(p);
                for (const Id& m : u)
                    if (m != hub && std::find(left.begin(), left.end(), m) == left.end() && std::find(right.begin(), right.end(), m) == right.end())
                        rest.push_back(m);
                std::vector<Id> ordered(left.rbegin(), left.rend());
                ordered.push_back(hub);
                ordered.insert(ordered.end(), right.begin(), right.end());
                ordered.insert(ordered.end(), rest.begin(), rest.end());
                u = ordered;
            }
        }
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
        // Twins side by side: a unit holding someone's twin moves to just after theirs (0.46).
        for (size_t i = 0; i < us.size(); ++i)
            for (size_t j = i + 2; j < us.size(); ++j) {
                bool twins = false;
                for (const Id& a : units[(size_t)us[i]])
                    for (const Id& b : units[(size_t)us[j]])
                        if (auto h = famOf(a).how.find(b); h != famOf(a).how.end() && (h->second == "twin" || h->second == "triplet")) twins = true;
                if (twins) { const int u = us[j]; us.erase(us.begin() + (long)j); us.insert(us.begin() + (long)i + 1, u); break; }
            }
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
    for (const Id& id : order) {
        FamilyLayout::Person p{id, gen[id], x[id] - minX, inLaws.count(id) > 0, {}};
        if (p.inLaw) {
            // How they joined (0.47): married to one of the family, their partner, or a parent of a child of theirs.
            bool married = false, partner = false, engaged = false, former = false;
            for (const EntryLink& l : linksOf(id)) {
                const Id other = l.from == id ? l.to : l.from;
                if (!gen.count(other) || inLaws.count(other)) continue;
                if (l.label == "Married to") married = true;
                else if (l.label == "Partner of") partner = true;
                else if (l.label == "Engaged to") engaged = true;
                else if (l.label == "Former partner of") former = true;
            }
            p.joined = married ? "married" : engaged ? "engaged" : partner ? "partner" : former ? "former" : "parent";
        }
        out.people.push_back(p);
    }
    out.width = order.empty() ? 0 : maxX - minX + 1;
    return out;
}

std::vector<Library::FamilyGroup> Library::families(const Id& worldId, const Id& bookId) const {
    std::vector<FamilyGroup> out;
    std::vector<const Entry*> chars;
    for (const Entry* e : entriesOf(worldId, bookId)) if (e->kind == "character") chars.push_back(e);
    // Every possible founder's tree first (0.47): someone with no parents who has children, or brothers and sisters.
    // Founders whose children's other parents have parents of their own married into that family instead.
    struct Candidate { Id root; std::set<Id> own; int people = 0; std::string surname; };
    std::vector<Candidate> cands;
    for (const Entry* e : chars) {
        const Family f = familyOf(e->id);
        if (!f.parents.empty() || (f.children.empty() && f.siblings.empty())) continue;
        bool marriedIn = false;
        for (const Id& c : f.children)
            for (const Id& op : familyOf(c).parents)
                if (op != e->id && !familyOf(op).parents.empty()) marriedIn = true;
        if (marriedIn) continue;
        const FamilyLayout lay = familyLayout(e->id, 400, true);
        if (lay.people.size() < 2) continue;
        Candidate c{e->id, {}, (int)lay.people.size(), {}};
        std::map<std::string, int> names;
        for (const auto& p : lay.people) {
            if (p.inLaw) continue;
            c.own.insert(p.id);
            if (const Entry* m = entry(p.id)) if (const std::string sn = surnameOf(m->name); !sn.empty()) ++names[sn];
        }
        int best = 1;
        for (auto& [sn, n] : names) if (n > best) { best = n; c.surname = sn; }
        cands.push_back(std::move(c));
    }
    // The biggest trees first; a tree whose own people are mostly in one already kept is the same family seen from
    // another founder (Maria's beside Iris's), so it's left out.
    std::stable_sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) {
        return a.own.size() != b.own.size() ? a.own.size() > b.own.size() : a.people > b.people;
    });
    std::set<Id> taken;
    for (const Candidate& c : cands) {
        size_t shared = 0;
        for (const Id& id : c.own) shared += taken.count(id);
        if (shared * 2 > c.own.size() || taken.count(c.root)) continue;
        taken.insert(c.own.begin(), c.own.end());
        out.push_back({c.root, c.surname, c.people, {}});
    }
    std::stable_sort(out.begin(), out.end(), [](const FamilyGroup& a, const FamilyGroup& b) { return a.people > b.people; });
    // Names, told apart by the founder's first name (then their whole name) when two families share a surname.
    std::map<std::string, int> uses;
    for (auto& g : out) {
        const Entry* f = entry(g.root);
        g.name = g.surname.empty() ? (f ? f->name : std::string()) + "'s family" : "The " + g.surname + " family";
        ++uses[g.name];
    }
    std::map<std::string, int> again;
    for (auto& g : out) {
        if (uses[g.name] < 2) continue;
        const Entry* f = entry(g.root);
        if (!f) continue;
        std::string first = f->name.substr(0, f->name.find(' '));
        std::string name = g.name + " (" + first + "'s)";
        if (++again[name] > 1) name = g.name + " (" + f->name + "'s)";
        g.name = name;
    }
    return out;
}

}  // namespace studio
