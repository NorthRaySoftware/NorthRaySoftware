// Manuscripts: parts, chapters and scenes, their text, and saved versions.
#include "internal.hpp"
#include "studio/document.hpp"
#include "studio/library.hpp"

#include <algorithm>
#include <functional>

namespace studio {

using detail::Stmt;

namespace {
struct KindRow { NodeKind k; const char* id; const char* label; };
const KindRow kKinds[] = {{NodeKind::Part, "part", "Part"}, {NodeKind::Chapter, "chapter", "Chapter"}, {NodeKind::Scene, "scene", "Scene"}};
struct StatusRow { NodeStatus s; const char* id; const char* label; };
const StatusRow kStatuses[] = {{NodeStatus::Outline, "outline", "Outline"}, {NodeStatus::Draft, "draft", "Draft"},
                               {NodeStatus::Revised, "revised", "Revised"}, {NodeStatus::Final, "final", "Final"}};
}  // namespace

const char* toString(NodeKind k) { for (auto& r : kKinds) if (r.k == k) return r.id; return "chapter"; }
const char* label(NodeKind k) { for (auto& r : kKinds) if (r.k == k) return r.label; return "Chapter"; }
const char* toString(NodeStatus s) { for (auto& r : kStatuses) if (r.s == s) return r.id; return "draft"; }
const char* label(NodeStatus s) { for (auto& r : kStatuses) if (r.s == s) return r.label; return "Draft"; }
std::optional<NodeKind> nodeKindFrom(const std::string& s) { for (auto& r : kKinds) if (s == r.id) return r.k; return std::nullopt; }
std::optional<NodeStatus> nodeStatusFrom(const std::string& s) { for (auto& r : kStatuses) if (s == r.id) return r.s; return std::nullopt; }

bool Library::canContain(std::optional<NodeKind> parent, NodeKind child) {
    if (!parent) return true;                                   // anything at book level
    if (*parent == NodeKind::Part) return child != NodeKind::Part;
    if (*parent == NodeKind::Chapter) return child == NodeKind::Scene;
    return false;                                               // scenes hold text only
}

bool Library::loadNodes() {
    nodes_.clear();
    Stmt q(db_, "SELECT id, book_id, parent_id, kind, title, synopsis, status, sort_order, word_count, created, modified, deleted, "
                "pov_id, target_words FROM nodes ORDER BY sort_order, created");
    if (!q) { error_ = sqlite3_errmsg(db_); return false; }
    while (q.row()) {
        ManuscriptNode n;
        n.id = q.text(0); n.bookId = q.text(1); n.parentId = q.text(2);
        n.kind = nodeKindFrom(q.text(3)).value_or(NodeKind::Chapter);
        n.title = q.text(4); n.synopsis = q.text(5); n.status = nodeStatusFrom(q.text(6)).value_or(NodeStatus::Draft);
        n.order = q.dbl(7); n.wordCount = q.i64(8); n.created = q.i64(9); n.modified = q.i64(10); n.deleted = q.i64(11);
        n.povId = q.text(12); n.targetWords = q.i64(13);
        nodes_.push_back(std::move(n));
    }
    return true;
}

const ManuscriptNode* Library::node(const Id& id) const {
    for (auto& n : nodes_) if (n.id == id) return &n;
    return nullptr;
}
ManuscriptNode* Library::nodeMut(const Id& id) { return const_cast<ManuscriptNode*>(node(id)); }

std::vector<const ManuscriptNode*> Library::childrenOf(const Id& bookId, const Id& parentId) const {
    std::vector<const ManuscriptNode*> out;
    for (auto& n : nodes_)
        if (n.bookId == bookId && n.parentId == parentId && !n.deleted) out.push_back(&n);
    std::stable_sort(out.begin(), out.end(), [](auto* a, auto* b) { return a->order < b->order; });
    return out;
}

std::vector<const ManuscriptNode*> Library::manuscriptOf(const Id& bookId) const {
    std::vector<const ManuscriptNode*> out;
    std::function<void(const Id&)> walk = [&](const Id& parent) {
        for (auto* n : childrenOf(bookId, parent)) { out.push_back(n); walk(n->id); }
    };
    walk({});
    return out;
}

std::vector<const ManuscriptNode*> Library::trashedNodesOf(const Id& bookId) const {
    std::vector<const ManuscriptNode*> out;
    for (auto& n : nodes_) {
        if (n.bookId != bookId || !n.deleted) continue;
        const ManuscriptNode* p = n.parentId.empty() ? nullptr : node(n.parentId);
        if (p && p->deleted == n.deleted) continue;  // trashed along with its parent
        out.push_back(&n);
    }
    std::sort(out.begin(), out.end(), [](auto* a, auto* b) { return a->deleted > b->deleted; });
    return out;
}

void Library::collectSubtree(const Id& id, std::vector<Id>& out) const {
    out.push_back(id);
    for (auto& n : nodes_)
        if (n.parentId == id) collectSubtree(n.id, out);
}

bool Library::saveNode(const ManuscriptNode& n) {
    Stmt s(db_, "INSERT INTO nodes(id, book_id, parent_id, kind, title, synopsis, status, sort_order, word_count, created, modified, deleted, "
                "pov_id, target_words) "
                "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14) ON CONFLICT(id) DO UPDATE SET parent_id=excluded.parent_id, "
                "kind=excluded.kind, title=excluded.title, synopsis=excluded.synopsis, status=excluded.status, "
                "sort_order=excluded.sort_order, word_count=excluded.word_count, modified=excluded.modified, deleted=excluded.deleted, "
                "pov_id=excluded.pov_id, target_words=excluded.target_words");
    s.bind(1, n.id).bind(2, n.bookId).bindIdOrNull(3, n.parentId).bind(4, std::string(toString(n.kind))).bind(5, n.title)
        .bind(6, n.synopsis).bind(7, std::string(toString(n.status))).bind(8, n.order).bind(9, n.wordCount)
        .bind(10, n.created).bind(11, n.modified).bind(12, n.deleted).bind(13, n.povId).bind(14, std::max<std::int64_t>(0, n.targetWords));
    if (!s.run()) { error_ = sqlite3_errmsg(db_); return false; }
    return true;
}

std::int64_t Library::bookWordCount(const Id& bookId) const {
    std::int64_t total = 0;
    for (auto* n : manuscriptOf(bookId)) total += n->wordCount;
    return total;
}

bool Library::updateBookWordCountTx(const Id& bookId) {
    Book* b = bookMut(bookId);
    if (!b) return true;
    Stmt s(db_, "UPDATE books SET word_count = ?2 WHERE id = ?1");
    const std::int64_t wc = bookWordCount(bookId);
    s.bind(1, bookId).bind(2, wc);
    if (!s.run()) { error_ = sqlite3_errmsg(db_); return false; }
    b->wordCount = wc;
    return true;
}

static std::string defaultTitle(NodeKind kind, int number) {
    return std::string(label(kind)) + " " + std::to_string(number);
}

Id Library::createNode(const Id& bookId, const Id& parentId, NodeKind kind, const std::string& titleIn, const Id& afterId) {
    if (!book(bookId)) { error_ = "No such book."; return {}; }
    const ManuscriptNode* parent = parentId.empty() ? nullptr : node(parentId);
    if (!parentId.empty() && (!parent || parent->bookId != bookId || parent->deleted)) { error_ = "No such part or chapter."; return {}; }
    if (!canContain(parent ? std::optional<NodeKind>(parent->kind) : std::nullopt, kind)) {
        error_ = std::string("A ") + label(kind) + " can't go inside a " + (parent ? label(parent->kind) : "book") + ".";
        return {};
    }
    ManuscriptNode n;
    n.id = newId();
    n.bookId = bookId;
    n.parentId = parentId;
    n.kind = kind;
    std::string t;
    for (char c : titleIn) t += c;
    while (!t.empty() && (t.back() == ' ' || t.back() == '\n')) t.pop_back();
    while (!t.empty() && (t.front() == ' ' || t.front() == '\n')) t.erase(0, 1);
    if (t.empty()) {
        // Number by kind across the book for chapters and parts; within the chapter for scenes.
        int count = 1;
        if (kind == NodeKind::Scene) {
            for (auto* s : childrenOf(bookId, parentId)) if (s->kind == NodeKind::Scene) ++count;
        } else {
            for (auto* s : manuscriptOf(bookId)) if (s->kind == kind) ++count;
        }
        t = defaultTitle(kind, count);
    }
    n.title = t;
    n.status = NodeStatus::Draft;
    n.created = n.modified = now();
    // Position: after `afterId`, or at the end.
    auto sibs = childrenOf(bookId, parentId);
    double order = sibs.empty() ? 1 : sibs.back()->order + 1;
    for (size_t i = 0; i < sibs.size(); ++i)
        if (sibs[i]->id == afterId) {
            order = i + 1 < sibs.size() ? (sibs[i]->order + sibs[i + 1]->order) / 2 : sibs[i]->order + 1;
            break;
        }
    n.order = order;
    if (!begin()) return {};
    if (!saveNode(n)) { rollback(); return {}; }
    {
        Stmt s(db_, "UPDATE nodes SET content = ?2 WHERE id = ?1");
        s.bind(1, n.id).bind(2, doc::emptyDoc());
        if (!s.run()) { error_ = sqlite3_errmsg(db_); rollback(); return {}; }
    }
    if (!commit()) return {};
    nodes_.push_back(n);
    return n.id;
}

bool Library::updateNode(const ManuscriptNode& in) {
    ManuscriptNode* n = nodeMut(in.id);
    if (!n) return false;
    ManuscriptNode next = *n;
    next.title = in.title;
    next.synopsis = in.synopsis;
    next.status = in.status;
    next.povId = in.povId;
    next.targetWords = std::max<std::int64_t>(0, in.targetWords);
    if (in.kind != n->kind) {
        // A kind change must still fit its parent and children.
        const ManuscriptNode* p = n->parentId.empty() ? nullptr : node(n->parentId);
        bool ok = canContain(p ? std::optional<NodeKind>(p->kind) : std::nullopt, in.kind);
        for (auto* c : childrenOf(n->bookId, n->id)) ok = ok && canContain(in.kind, c->kind);
        if (!ok) { error_ = "That change doesn't fit where it is in the book."; return false; }
        next.kind = in.kind;
    }
    next.modified = now();
    if (!begin()) return false;
    if (!saveNode(next)) { rollback(); return false; }
    if (!commit()) return false;
    *n = next;
    return true;
}

bool Library::moveNode(const Id& id, const Id& newParentId, int index) {
    ManuscriptNode* n = nodeMut(id);
    if (!n || n->deleted) return false;
    const ManuscriptNode* p = newParentId.empty() ? nullptr : node(newParentId);
    if (!newParentId.empty() && (!p || p->bookId != n->bookId || p->deleted)) { error_ = "No such part or chapter."; return false; }
    // Not into itself or its own descendants.
    for (const ManuscriptNode* a = p; a; a = a->parentId.empty() ? nullptr : node(a->parentId))
        if (a->id == id) { error_ = "Something can't be moved inside itself."; return false; }
    if (!canContain(p ? std::optional<NodeKind>(p->kind) : std::nullopt, n->kind)) {
        error_ = std::string("A ") + label(n->kind) + " can't go inside a " + (p ? label(p->kind) : "book") + ".";
        return false;
    }
    std::vector<const ManuscriptNode*> sibs;
    for (auto* s : childrenOf(n->bookId, newParentId)) if (s->id != id) sibs.push_back(s);
    if (index < 0 || index > (int)sibs.size()) index = (int)sibs.size();
    if (!begin()) return false;
    // Renumber the whole sibling list: simple, and keeps orders tidy.
    std::vector<ManuscriptNode> list;
    for (auto* x : sibs) list.push_back(*x);
    ManuscriptNode moved = *n;
    moved.parentId = newParentId;
    moved.modified = now();
    list.insert(list.begin() + index, moved);
    for (size_t i = 0; i < list.size(); ++i) {
        list[i].order = double(i + 1);
        if (!saveNode(list[i])) { rollback(); return false; }
    }
    if (!commit()) return false;
    return loadNodes();
}

bool Library::trashNode(const Id& id) {
    ManuscriptNode* n = nodeMut(id);
    if (!n || n->deleted) return false;
    const Id bookId = n->bookId;
    std::vector<Id> sub;
    collectSubtree(id, sub);
    const Time when = now();
    if (!begin()) return false;
    for (const Id& x : sub) {
        ManuscriptNode next = *nodeMut(x);
        if (next.deleted) continue;  // already in the Trash on its own
        next.deleted = when;
        if (!saveNode(next)) { rollback(); return false; }
    }
    if (!commit()) return false;
    for (const Id& x : sub) if (nodeMut(x)->deleted == 0) nodeMut(x)->deleted = when;
    begin();
    updateBookWordCountTx(bookId);
    commit();
    return true;
}

std::string Library::nodeContent(const Id& id) const {
    Stmt q(db_, "SELECT content FROM nodes WHERE id = ?1");
    q.bind(1, id);
    if (q && q.row()) {
        std::string c = q.text(0);
        if (!c.empty()) return c;
    }
    return doc::emptyDoc();
}

bool Library::addVersionTx(const Id& nodeId, const std::string& content, std::int64_t words, const std::string& reason) {
    Stmt s(db_, "INSERT INTO node_versions(node_id, saved, word_count, reason, content) VALUES (?1, ?2, ?3, ?4, ?5)");
    s.bind(1, nodeId).bind(2, now()).bind(3, words).bind(4, reason).bind(5, content);
    if (!s.run()) { error_ = sqlite3_errmsg(db_); return false; }
    Stmt p(db_, "DELETE FROM node_versions WHERE node_id = ?1 AND id NOT IN "
                "(SELECT id FROM node_versions WHERE node_id = ?1 ORDER BY saved DESC, id DESC LIMIT ?2)");
    p.bind(1, nodeId).bind(2, (std::int64_t)kVersionsKept);
    if (!p.run()) { error_ = sqlite3_errmsg(db_); return false; }
    return true;
}

bool Library::saveNodeContent(const Id& id, const std::string& docJson) {
    ManuscriptNode* n = nodeMut(id);
    if (!n) { error_ = "No such chapter or scene."; return false; }
    const std::string clean = doc::sanitise(docJson);
    const std::string before = nodeContent(id);
    if (clean == before) return true;
    const std::int64_t words = doc::wordCount(clean);
    // Keep the previous text as a version when the last one is old enough.
    bool keep = false;
    {
        Stmt q(db_, "SELECT MAX(saved) FROM node_versions WHERE node_id = ?1");
        q.bind(1, id);
        const Time last = q && q.row() ? q.i64(0) : 0;
        keep = now() - last >= kVersionInterval;
    }
    ManuscriptNode next = *n;
    next.wordCount = words;
    next.modified = now();
    if (!begin()) return false;
    if (keep && n->wordCount > 0 && !addVersionTx(id, before, n->wordCount, "autosave")) { rollback(); return false; }
    {
        Stmt s(db_, "UPDATE nodes SET content = ?2, word_count = ?3, modified = ?4 WHERE id = ?1");
        s.bind(1, id).bind(2, clean).bind(3, words).bind(4, next.modified);
        if (!s.run()) { error_ = sqlite3_errmsg(db_); rollback(); return false; }
    }
    // Words added count towards the day's writing.
    if (words > n->wordCount) {
        Stmt s(db_, "INSERT INTO writing_log(day, book_id, words) VALUES (?1, ?2, ?3) "
                    "ON CONFLICT(day, book_id) DO UPDATE SET words = words + excluded.words");
        s.bind(1, localDay(now())).bind(2, n->bookId).bind(3, words - n->wordCount);
        if (!s.run()) { error_ = sqlite3_errmsg(db_); rollback(); return false; }
    }
    *n = next;
    if (!updateBookWordCountTx(n->bookId)) { rollback(); return false; }
    return commit();
}

std::vector<NodeVersion> Library::nodeVersions(const Id& id) const {
    std::vector<NodeVersion> out;
    Stmt q(db_, "SELECT id, saved, word_count, reason FROM node_versions WHERE node_id = ?1 ORDER BY saved DESC, id DESC");
    q.bind(1, id);
    while (q && q.row()) out.push_back({q.i64(0), q.i64(1), q.i64(2), q.text(3)});
    return out;
}

std::string Library::nodeVersionContent(std::int64_t versionId) const {
    Stmt q(db_, "SELECT content FROM node_versions WHERE id = ?1");
    q.bind(1, versionId);
    return q && q.row() ? q.text(0) : std::string{};
}

bool Library::restoreNodeVersion(const Id& id, std::int64_t versionId) {
    const std::string old = nodeVersionContent(versionId);
    if (old.empty()) { error_ = "That version no longer exists."; return false; }
    return restoreNodeText(id, old);
}

bool Library::restoreNodeText(const Id& id, const std::string& docJson) {
    ManuscriptNode* n = nodeMut(id);
    if (!n) return false;
    const std::string old = doc::sanitise(docJson);
    const std::string current = nodeContent(id);
    const std::int64_t words = doc::wordCount(old);
    if (!begin()) return false;
    if (!addVersionTx(id, current, n->wordCount, "before restore")) { rollback(); return false; }
    {
        Stmt s(db_, "UPDATE nodes SET content = ?2, word_count = ?3, modified = ?4 WHERE id = ?1");
        s.bind(1, id).bind(2, old).bind(3, words).bind(4, now());
        if (!s.run()) { error_ = sqlite3_errmsg(db_); rollback(); return false; }
    }
    n->wordCount = words;
    n->modified = now();
    if (!updateBookWordCountTx(n->bookId)) { rollback(); return false; }
    return commit();
}

std::vector<Id> Library::addNodes(const Id& bookId, const Id& parentId, const std::vector<NewNode>& tree) {
    std::vector<Id> top;
    if (!book(bookId)) { error_ = "No such book."; return top; }
    const ManuscriptNode* parent = parentId.empty() ? nullptr : node(parentId);
    auto sibs = childrenOf(bookId, parentId);
    double order = sibs.empty() ? 1 : sibs.back()->order + 1;
    const Time t = now();
    std::vector<ManuscriptNode> added;
    std::function<bool(const NewNode&, const Id&, std::optional<NodeKind>, double)> add =
        [&](const NewNode& nn, const Id& par, std::optional<NodeKind> parKind, double ord) -> bool {
        if (!canContain(parKind, nn.kind)) { error_ = std::string("A ") + label(nn.kind) + " can't go there."; return false; }
        ManuscriptNode n;
        n.id = newId(); n.bookId = bookId; n.parentId = par; n.kind = nn.kind;
        n.title = nn.title.empty() ? std::string(label(nn.kind)) : nn.title;
        n.order = ord; n.created = n.modified = t;
        const std::string content = doc::sanitise(nn.content.empty() ? doc::emptyDoc() : nn.content);
        n.wordCount = doc::wordCount(content);
        if (!saveNode(n)) return false;
        Stmt s(db_, "UPDATE nodes SET content = ?2 WHERE id = ?1");
        s.bind(1, n.id).bind(2, content);
        if (!s.run()) { error_ = sqlite3_errmsg(db_); return false; }
        added.push_back(n);
        if (par == parentId) top.push_back(n.id);
        double co = 1;
        for (auto& c : nn.children) if (!add(c, n.id, n.kind, co++)) return false;
        return true;
    };
    if (!begin()) return {};
    for (auto& nn : tree)
        if (!add(nn, parentId, parent ? std::optional<NodeKind>(parent->kind) : std::nullopt, order++)) {
            rollback();
            return {};
        }
    nodes_.insert(nodes_.end(), added.begin(), added.end());
    if (!updateBookWordCountTx(bookId)) { rollback(); return {}; }
    if (!commit()) return {};
    return top;
}

}  // namespace studio
