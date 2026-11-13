// Comments on a chapter's or scene's text (schema 8).
#include "studio/library.hpp"

#include "internal.hpp"

#include <algorithm>
#include <functional>
#include <nlohmann/json.hpp>

namespace studio {

using namespace detail;
using nlohmann::json;

static bool validCommentId(const std::string& id) {
    return id.size() >= 8 && id.size() <= 32 && id.find_first_not_of("0123456789abcdef") == std::string::npos;
}

bool Library::loadComments() {
    comments_.clear();
    Stmt q(db_, "SELECT id, node_id, body, quote, created, modified, resolved FROM comments ORDER BY created, rowid");
    if (!q) { error_ = sqlite3_errmsg(db_); return false; }
    while (q.row()) comments_.push_back({q.text(0), q.text(1), q.text(2), q.text(3), q.i64(4), q.i64(5), q.i64(6)});
    return true;
}

bool Library::saveComment(const Comment& c) {
    Stmt s(db_, "INSERT INTO comments(id, node_id, body, quote, created, modified, resolved) VALUES (?1,?2,?3,?4,?5,?6,?7) "
                "ON CONFLICT(id) DO UPDATE SET body=excluded.body, quote=excluded.quote, modified=excluded.modified, resolved=excluded.resolved");
    s.bind(1, c.id).bind(2, c.nodeId).bind(3, c.body).bind(4, c.quote).bind(5, (std::int64_t)c.created).bind(6, (std::int64_t)c.modified)
        .bind(7, (std::int64_t)c.resolved);
    if (!s.run()) { error_ = sqlite3_errmsg(db_); return false; }
    return true;
}

std::vector<const Comment*> Library::commentsOf(const Id& nodeId) const {
    std::vector<const Comment*> out;
    for (auto& c : comments_) if (c.nodeId == nodeId) out.push_back(&c);
    return out;
}

std::vector<const Comment*> Library::commentsInBook(const Id& bookId) const {
    std::vector<const Comment*> out;
    for (const ManuscriptNode* n : manuscriptOf(bookId))
        for (auto& c : comments_) if (c.nodeId == n->id) out.push_back(&c);
    return out;
}

const Comment* Library::comment(const Id& id) const {
    for (auto& c : comments_) if (c.id == id) return &c;
    return nullptr;
}

bool Library::addComment(const Id& nodeId, const Id& id, const std::string& quote) {
    if (!node(nodeId)) { error_ = "No such chapter or scene."; return false; }
    if (!validCommentId(id)) { error_ = "That comment can't be stored."; return false; }
    if (comment(id)) return true;
    Comment c;
    c.id = id;
    c.nodeId = nodeId;
    c.quote = quote.size() > 2000 ? quote.substr(0, 2000) : quote;
    c.created = c.modified = now();
    if (!saveComment(c)) return false;
    comments_.push_back(c);
    return true;
}

bool Library::updateComment(const Id& id, const std::string& body) {
    for (auto& c : comments_)
        if (c.id == id) {
            if (c.body == body) return true;
            Comment n = c;
            n.body = body;
            n.modified = now();
            if (!saveComment(n)) return false;
            c = n;
            return true;
        }
    error_ = "No such comment.";
    return false;
}

bool Library::stripCommentMark(const Id& nodeId, const Id& commentId) {
    json d = json::parse(nodeContent(nodeId), nullptr, false);
    if (!d.is_object()) return true;
    bool changed = false;
    std::function<void(json&)> walk = [&](json& n) {
        if (n.contains("marks") && n["marks"].is_array()) {
            json kept = json::array();
            for (auto& m : n["marks"]) {
                const bool mine = m.is_object() && m.value("type", "") == "comment" && m.contains("attrs") && m["attrs"].is_object() &&
                                  m["attrs"].value("id", "") == commentId;
                if (mine) changed = true;
                else kept.push_back(m);
            }
            if (kept.empty()) n.erase("marks");
            else n["marks"] = kept;
        }
        if (n.contains("content") && n["content"].is_array())
            for (auto& c : n["content"]) if (c.is_object()) walk(c);
    };
    walk(d);
    if (!changed) return true;
    Stmt s(db_, "UPDATE nodes SET content = ?2 WHERE id = ?1");
    s.bind(1, nodeId).bind(2, d.dump());
    if (!s.run()) { error_ = sqlite3_errmsg(db_); return false; }
    return true;
}

bool Library::resolveComment(const Id& id, bool resolved, bool stripText) {
    for (auto& c : comments_)
        if (c.id == id) {
            Comment n = c;
            n.resolved = resolved ? now() : 0;
            n.modified = now();
            if (!begin()) return false;
            if (!saveComment(n) || (resolved && stripText && !stripCommentMark(c.nodeId, id))) { rollback(); return false; }
            if (!commit()) return false;
            c = n;
            return true;
        }
    error_ = "No such comment.";
    return false;
}

bool Library::removeComment(const Id& id, bool stripText) {
    const Comment* c = comment(id);
    if (!c) return true;
    const Id nodeId = c->nodeId;
    if (!begin()) return false;
    Stmt s(db_, "DELETE FROM comments WHERE id = ?1");
    s.bind(1, id);
    if (!s.run() || (stripText && !stripCommentMark(nodeId, id))) { error_ = sqlite3_errmsg(db_); rollback(); return false; }
    if (!commit()) return false;
    comments_.erase(std::remove_if(comments_.begin(), comments_.end(), [&](const Comment& x) { return x.id == id; }), comments_.end());
    return true;
}

bool Library::commentAnchored(const Comment& c) const {
    // Ids are random, so finding the id in the text means its mark is there.
    return nodeContent(c.nodeId).find("\"" + c.id + "\"") != std::string::npos;
}

}  // namespace studio
