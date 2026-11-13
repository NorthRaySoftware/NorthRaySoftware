// Editions of a book: paperback, hardback, large print, ebook (schema 4).
#include "studio/library.hpp"

#include "internal.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace studio {

using namespace detail;
using nlohmann::json;

const char* editionKindLabel(const std::string& kind) {
    if (kind == "hardback") return "Hardback";
    if (kind == "large-print") return "Large print";
    if (kind == "ebook") return "Ebook";
    return "Paperback";
}

static std::string knownKind(const std::string& k) {
    return k == "hardback" || k == "large-print" || k == "ebook" ? k : std::string("paperback");
}

bool Library::loadEditions() {
    editions_.clear();
    Stmt q(db_, "SELECT id, book_id, kind, name, isbn, trim, design, paper, bleed, colour, sort_order, created, modified "
                "FROM editions ORDER BY sort_order, created");
    if (!q) { error_ = sqlite3_errmsg(db_); return false; }
    while (q.row()) {
        Edition e;
        e.id = q.text(0); e.bookId = q.text(1); e.kind = knownKind(q.text(2)); e.name = q.text(3); e.isbn = q.text(4);
        e.trim = q.text(5); e.design = q.text(6); e.paper = q.text(7) == "cream" ? "cream" : "white";
        e.bleed = q.i64(8) != 0; e.colour = q.i64(9) != 0; e.order = q.dbl(10); e.created = q.i64(11); e.modified = q.i64(12);
        editions_.push_back(std::move(e));
    }
    return true;
}

bool Library::saveEdition(const Edition& e) {
    Stmt s(db_, "INSERT INTO editions(id, book_id, kind, name, isbn, trim, design, paper, bleed, colour, sort_order, created, modified) "
                "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13) ON CONFLICT(id) DO UPDATE SET kind=excluded.kind, "
                "name=excluded.name, isbn=excluded.isbn, trim=excluded.trim, design=excluded.design, paper=excluded.paper, "
                "bleed=excluded.bleed, colour=excluded.colour, sort_order=excluded.sort_order, modified=excluded.modified");
    s.bind(1, e.id).bind(2, e.bookId).bind(3, e.kind).bind(4, e.name).bind(5, e.isbn).bind(6, e.trim).bind(7, e.design)
        .bind(8, e.paper).bind(9, (std::int64_t)e.bleed).bind(10, (std::int64_t)e.colour).bind(11, e.order)
        .bind(12, (std::int64_t)e.created).bind(13, (std::int64_t)e.modified);
    if (!s.run()) { error_ = sqlite3_errmsg(db_); return false; }
    return true;
}

std::vector<const Edition*> Library::editionsOf(const Id& bookId) const {
    std::vector<const Edition*> out;
    for (auto& e : editions_) if (e.bookId == bookId) out.push_back(&e);
    std::stable_sort(out.begin(), out.end(), [](auto* a, auto* b) { return a->order < b->order; });
    return out;
}

const Edition* Library::edition(const Id& id) const {
    for (auto& e : editions_) if (e.id == id) return &e;
    return nullptr;
}

Id Library::addEdition(const Id& bookId, const std::string& kind) {
    if (!book(bookId)) { error_ = "No such book."; return {}; }
    Edition e;
    e.id = newId();
    e.bookId = bookId;
    e.kind = knownKind(kind);
    if (e.kind == "large-print") e.design = "large-print";
    double last = 0;
    for (auto* x : editionsOf(bookId)) last = std::max(last, x->order);
    e.order = last + 1;
    e.created = e.modified = now();
    if (!saveEdition(e)) return {};
    editions_.push_back(e);
    return e.id;
}

bool Library::updateEdition(const Edition& in) {
    for (auto& e : editions_)
        if (e.id == in.id) {
            Edition n = in;
            n.bookId = e.bookId;
            n.kind = knownKind(n.kind);
            n.paper = n.paper == "cream" ? "cream" : "white";
            n.created = e.created;
            n.modified = now();
            if (!saveEdition(n)) return false;
            e = n;
            return true;
        }
    error_ = "No such edition.";
    return false;
}

bool Library::removeEdition(const Id& id) {
    Stmt s(db_, "DELETE FROM editions WHERE id = ?1");
    s.bind(1, id);
    if (!s.run()) { error_ = sqlite3_errmsg(db_); return false; }
    editions_.erase(std::remove_if(editions_.begin(), editions_.end(), [&](const Edition& e) { return e.id == id; }), editions_.end());
    return true;
}

bool Library::ensureEditions(const Id& bookId) {
    const Book* b = book(bookId);
    if (!b) return false;
    if (!editionsOf(bookId).empty()) return true;
    const Id p = addEdition(bookId, "paperback");
    const Id e = addEdition(bookId, "ebook");
    if (p.empty() || e.empty()) return false;
    // The paperback takes what the book already had: its ISBN and the publishing settings.
    Edition pb = *edition(p);
    pb.isbn = b->isbn;
    const json j = json::parse(setting("publish:" + bookId), nullptr, false);
    if (j.is_object()) {
        pb.paper = j.value("paper", 0) == 1 ? "cream" : "white";
        pb.bleed = j.value("bleed", false);
        pb.colour = j.value("colour", false);
    }
    return updateEdition(pb);
}

}  // namespace studio
