// Editorial tools: snapshots and the writing log (Library), comparing texts,
// and checking names.
#include "studio/review.hpp"

#include "studio/document.hpp"
#include "studio/typeset.hpp"   // utf32 / utf8

#include "internal.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <map>
#include <set>

namespace studio {

using namespace detail;

// ----------------------------------------------------------- snapshots

std::string Library::localDay(Time t) {
    const std::time_t tt = (std::time_t)t;
    std::tm lt{};
#if defined(_WIN32)
    localtime_s(&lt, &tt);
#else
    localtime_r(&tt, &lt);
#endif
    char b[16];
    std::strftime(b, sizeof b, "%Y-%m-%d", &lt);
    return b;
}

Id Library::takeSnapshot(const Id& bookId, const std::string& name) {
    if (!book(bookId)) { error_ = "No such book."; return {}; }
    const Id id = newId();
    const auto nodes = manuscriptOf(bookId);
    std::int64_t words = 0;
    for (auto* n : nodes) words += n->wordCount;
    if (!begin()) return {};
    {
        Stmt s(db_, "INSERT INTO snapshots(id, book_id, name, taken, words) VALUES (?1, ?2, ?3, ?4, ?5)");
        s.bind(1, id).bind(2, bookId).bind(3, name).bind(4, (std::int64_t)now()).bind(5, words);
        if (!s.run()) { error_ = sqlite3_errmsg(db_); rollback(); return {}; }
    }
    std::int64_t pos = 0;
    for (auto* n : nodes) {
        Stmt s(db_, "INSERT INTO snapshot_nodes(snapshot_id, node_id, kind, title, content, word_count, position) VALUES (?1,?2,?3,?4,?5,?6,?7)");
        s.bind(1, id).bind(2, n->id).bind(3, std::string(toString(n->kind))).bind(4, n->title).bind(5, nodeContent(n->id))
            .bind(6, n->wordCount).bind(7, pos++);
        if (!s.run()) { error_ = sqlite3_errmsg(db_); rollback(); return {}; }
    }
    if (!commit()) return {};
    return id;
}

std::vector<Snapshot> Library::snapshotsOf(const Id& bookId) const {
    std::vector<Snapshot> out;
    Stmt q(db_, "SELECT s.id, s.book_id, s.name, s.taken, s.words, (SELECT COUNT(*) FROM snapshot_nodes n WHERE n.snapshot_id = s.id) "
                "FROM snapshots s WHERE s.book_id = ?1 ORDER BY s.taken DESC, s.rowid DESC");
    q.bind(1, bookId);
    while (q && q.row()) out.push_back({q.text(0), q.text(1), q.text(2), q.i64(3), q.i64(4), (int)q.i64(5)});
    return out;
}

std::vector<SnapshotNode> Library::snapshotNodes(const Id& snapshotId) const {
    std::vector<SnapshotNode> out;
    Stmt q(db_, "SELECT node_id, kind, title, content, word_count FROM snapshot_nodes WHERE snapshot_id = ?1 ORDER BY position");
    q.bind(1, snapshotId);
    while (q && q.row()) {
        SnapshotNode n;
        n.nodeId = q.text(0);
        n.kind = q.text(1) == "part" ? NodeKind::Part : q.text(1) == "scene" ? NodeKind::Scene : NodeKind::Chapter;
        n.title = q.text(2);
        n.content = q.text(3);
        n.words = q.i64(4);
        out.push_back(std::move(n));
    }
    return out;
}

bool Library::renameSnapshot(const Id& id, const std::string& name) {
    Stmt s(db_, "UPDATE snapshots SET name = ?2 WHERE id = ?1");
    s.bind(1, id).bind(2, name);
    if (!s.run()) { error_ = sqlite3_errmsg(db_); return false; }
    return true;
}

bool Library::removeSnapshot(const Id& id) {
    Stmt s(db_, "DELETE FROM snapshots WHERE id = ?1");
    s.bind(1, id);
    if (!s.run()) { error_ = sqlite3_errmsg(db_); return false; }
    return true;
}

std::map<std::string, std::int64_t> Library::wordsWritten(const Id& bookId, const std::string& fromDay) const {
    std::map<std::string, std::int64_t> out;
    Stmt q(db_, bookId.empty() ? "SELECT day, SUM(words) FROM writing_log WHERE day >= ?1 GROUP BY day"
                               : "SELECT day, SUM(words) FROM writing_log WHERE day >= ?1 AND book_id = ?2 GROUP BY day");
    q.bind(1, fromDay);
    if (!bookId.empty()) q.bind(2, bookId);
    while (q && q.row()) out[q.text(0)] = q.i64(1);
    return out;
}

namespace review {

// ------------------------------------------------------------- comparing

namespace {

// Myers' difference algorithm over two token lists: the edit script as
// (kind, token index) steps. `limit` caps the work; past it, everything is a change.
template <class T>
std::vector<std::pair<int, int>> myers(const std::vector<T>& a, const std::vector<T>& b, int limit) {
    const int n = (int)a.size(), m = (int)b.size(), max = n + m;
    std::vector<std::pair<int, int>> script;   // (0 same / 1 removed / 2 added, index)
    if (max == 0) return script;
    std::vector<int> v(2 * max + 2, 0);
    std::vector<std::vector<int>> trace;
    int found = -1;
    for (int d = 0; d <= max && d <= limit; ++d) {
        trace.push_back(v);
        for (int k = -d; k <= d; k += 2) {
            int x = (k == -d || (k != d && v[max + k - 1] < v[max + k + 1])) ? v[max + k + 1] : v[max + k - 1] + 1;
            int y = x - k;
            while (x < n && y < m && a[x] == b[y]) ++x, ++y;
            v[max + k] = x;
            if (x >= n && y >= m) { found = d; break; }
        }
        if (found >= 0) break;
    }
    if (found < 0) {
        for (int i = 0; i < n; ++i) script.push_back({1, i});
        for (int j = 0; j < m; ++j) script.push_back({2, j});
        return script;
    }
    // Walk back through the trace.
    int x = n, y = m;
    for (int d = found; d > 0; --d) {
        const std::vector<int>& pv = trace[d];
        const int k = x - y;
        const bool down = k == -d || (k != d && pv[max + k - 1] < pv[max + k + 1]);
        const int pk = down ? k + 1 : k - 1;
        const int px = pv[max + pk], py = px - pk;
        while (x > px && y > py) { script.push_back({0, x - 1}); --x; --y; }
        if (down) script.push_back({2, y - 1});
        else script.push_back({1, x - 1});
        x = px;
        y = py;
    }
    while (x > 0 && y > 0) { script.push_back({0, x - 1}); --x; --y; }
    std::reverse(script.begin(), script.end());
    return script;
}

std::vector<std::string> paragraphs(const std::string& t) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at < t.size()) {
        size_t e = t.find("\n\n", at);
        if (e == std::string::npos) e = t.size();
        std::string p = t.substr(at, e - at);
        while (!p.empty() && (p.back() == '\n' || p.back() == ' ')) p.pop_back();
        while (!p.empty() && (p.front() == '\n')) p.erase(0, 1);
        if (!p.empty()) out.push_back(p);
        at = e + 2;
    }
    return out;
}

// Words with the space after them, so joining the tokens gives the text back.
std::vector<std::string> words(const std::string& p) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < p.size()) {
        size_t j = i;
        while (j < p.size() && p[j] != ' ' && p[j] != '\n') ++j;
        while (j < p.size() && (p[j] == ' ' || p[j] == '\n')) ++j;
        out.push_back(p.substr(i, j - i));
        i = j;
    }
    return out;
}

void push(std::vector<DiffPart>& out, DiffPart::Kind k, const std::string& t) {
    if (t.empty()) return;
    if (!out.empty() && out.back().kind == k) out.back().text += t;
    else out.push_back({k, t});
}

}  // namespace

std::vector<DiffPart> diffText(const std::string& before, const std::string& after) {
    std::vector<DiffPart> out;
    const auto pa = paragraphs(before), pb = paragraphs(after);
    const auto script = myers(pa, pb, 4000);
    bool first = true;
    auto sep = [&](DiffPart::Kind k) { if (!first) push(out, k, "\n\n"); first = false; };
    // Runs of removed and added paragraphs next to each other are compared word by word.
    size_t i = 0;
    while (i < script.size()) {
        if (script[i].first == 0) {
            sep(DiffPart::Same);
            push(out, DiffPart::Same, pa[script[i].second]);
            ++i;
            continue;
        }
        std::vector<int> rem, add;
        while (i < script.size() && script[i].first != 0) {
            (script[i].first == 1 ? rem : add).push_back(script[i].second);
            ++i;
        }
        const size_t pairs = std::min(rem.size(), add.size());
        for (size_t p = 0; p < pairs; ++p) {
            sep(DiffPart::Same);
            const auto wa = words(pa[rem[p]]), wb = words(pb[add[p]]);
            // Compared without the spaces after them ("harbour." at the end is "harbour. " mid-text).
            auto keys = [](const std::vector<std::string>& v) {
                std::vector<std::string> k;
                for (auto t : v) { while (!t.empty() && (t.back() == ' ' || t.back() == '\n')) t.pop_back(); k.push_back(t); }
                return k;
            };
            const auto ka = keys(wa), kb = keys(wb);
            const auto ws = myers(ka, kb, 2000);
            // A same word is shown as it is now; spacing follows the new text.
            size_t bi = 0;
            for (auto& [k, idx] : ws) {
                if (k == 0) { push(out, DiffPart::Same, bi < wb.size() ? wb[bi] : wa[idx]); ++bi; }
                else if (k == 1) {
                    std::string t = wa[idx];
                    if (t.empty() || t.back() != ' ') t += ' ';
                    push(out, DiffPart::Removed, t);
                } else { push(out, DiffPart::Added, wb[idx]); bi = idx + 1; }
            }
        }
        for (size_t p = pairs; p < rem.size(); ++p) { sep(DiffPart::Removed); push(out, DiffPart::Removed, pa[rem[p]]); }
        for (size_t p = pairs; p < add.size(); ++p) { sep(DiffPart::Added); push(out, DiffPart::Added, pb[add[p]]); }
    }
    return out;
}

DiffStats stats(const std::vector<DiffPart>& parts) {
    DiffStats s;
    for (const auto& p : parts) {
        if (p.kind == DiffPart::Same) continue;
        std::int64_t n = 0;
        bool in = false;
        for (unsigned char c : p.text) {
            const bool w = !std::isspace(c);
            if (w && !in) ++n;
            in = w;
        }
        (p.kind == DiffPart::Added ? s.added : s.removed) += n;
    }
    return s;
}

// --------------------------------------------------------------- names

int editDistance(const std::u32string& a, const std::u32string& b, int limit) {
    const int n = (int)a.size(), m = (int)b.size();
    if (std::abs(n - m) > limit) return limit + 1;
    std::vector<int> prev(m + 1), cur(m + 1);
    for (int j = 0; j <= m; ++j) prev[j] = j;
    for (int i = 1; i <= n; ++i) {
        cur[0] = i;
        int best = cur[0];
        for (int j = 1; j <= m; ++j) {
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
            best = std::min(best, cur[j]);
        }
        if (best > limit) return limit + 1;
        std::swap(prev, cur);
    }
    return prev[m];
}

namespace {

bool isUpper(char32_t c) { return (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7); }
bool isLetter(char32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7); }
char32_t lower(char32_t c) { return (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7) ? c + 32 : c; }
std::u32string lowered(std::u32string s) { for (auto& c : s) c = lower(c); return s; }

// Capitalised words that aren't names, however often they're capitalised mid-sentence.
const std::set<std::u32string>& notNames() {
    static const std::set<std::u32string> s = [] {
        std::set<std::u32string> x;
        for (const char* w : {"i", "i'm", "i'd", "i'll", "i've", "mr", "mrs", "ms", "dr", "sir", "lady", "lord", "miss", "mister", "madam",
                              "god", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday", "january", "february",
                              "march", "april", "may", "june", "july", "august", "september", "october", "november", "december",
                              "english", "french", "christmas", "ok", "okay", "tv", "chapter", "part", "mum", "mom", "dad", "ma", "pa",
                              "king", "queen", "prince", "princess", "captain", "father", "mother", "uncle", "aunt"})
            x.insert(typeset::utf32(w));
        return x;
    }();
    return s;
}

}  // namespace

std::vector<NameIssue> nameChecks(const Library& lib, const Id& bookId) {
    std::vector<NameIssue> out;
    const Book* b = lib.book(bookId);
    if (!b) return out;
    // Every word of every entry's names.
    struct Known { std::u32string word; std::string entryName; Id entry; };
    std::vector<Known> known;
    std::set<std::u32string> knownLower;
    for (const Entry* e : b->worldId.empty() ? lib.entriesOf({}, bookId) : lib.entriesOf(b->worldId)) {
        std::vector<std::string> names{e->name};
        names.insert(names.end(), e->aliases.begin(), e->aliases.end());
        for (const std::string& nm : names) {
            const std::u32string u = typeset::utf32(nm);
            std::u32string wd;
            for (size_t i = 0; i <= u.size(); ++i) {
                if (i < u.size() && (isLetter(u[i]) || u[i] == '\'' || u[i] == '-')) { wd += u[i]; continue; }
                if (wd.size() >= 2 && isUpper(wd[0])) { known.push_back({wd, e->name, e->id}); knownLower.insert(lowered(wd)); }
                wd.clear();
            }
        }
    }
    // Words in the text: how often capitalised mid-sentence, whether ever in lower case, and where.
    struct Seen { int mid = 0, total = 0; std::vector<Id> nodes; };
    std::map<std::u32string, Seen> seen;      // by the capitalised spelling
    std::set<std::u32string> lowerForms;      // lower-cased words seen in lower case
    for (const ManuscriptNode* n : lib.manuscriptOf(bookId)) {
        const std::u32string t = typeset::utf32(doc::plainText(lib.nodeContent(n->id)));
        bool sentenceStart = true;
        size_t i = 0;
        while (i < t.size()) {
            if (!isLetter(t[i])) {
                const char32_t c = t[i];
                if (c == '.' || c == '!' || c == '?' || c == '\n' || c == '"' || c == 0x201C || c == ':' || c == 0x2014 || c == 0x2026)
                    sentenceStart = true;
                ++i;
                continue;
            }
            size_t j = i;
            while (j < t.size() && (isLetter(t[j]) || ((t[j] == '\'' || t[j] == 0x2019 || t[j] == '-') && j + 1 < t.size() && isLetter(t[j + 1]))))
                ++j;
            std::u32string wd = t.substr(i, j - i);
            // Possessives belong to the name: "Maren's" is Maren.
            if (wd.size() > 2 && (wd.compare(wd.size() - 2, 2, U"'s") == 0 || wd.compare(wd.size() - 2, 2, U"’s") == 0)) wd.resize(wd.size() - 2);
            if (isUpper(wd[0])) {
                Seen& s = seen[wd];
                ++s.total;
                if (!sentenceStart) ++s.mid;
                if (s.nodes.empty() || s.nodes.back() != n->id) s.nodes.push_back(n->id);
            } else {
                lowerForms.insert(wd);
            }
            sentenceStart = false;
            i = j;
        }
    }
    for (auto& [wd, s] : seen) {
        const std::u32string lw = lowered(wd);
        if (wd.size() < 3 || lowerForms.count(lw) || notNames().count(lw)) continue;
        if (knownLower.count(lw)) continue;   // in the bible
        // All capitals ("NASA", shouting) aren't checked.
        bool allCaps = true;
        for (char32_t c : wd) if (isLetter(c) && !isUpper(c)) allCaps = false;
        if (allCaps) continue;
        // A near miss of a known name?
        const Known* near = nullptr;
        for (const Known& k : known) {
            if (k.word.size() < 4) continue;
            const int limit = k.word.size() >= 8 ? 2 : 1;
            if (editDistance(lw, lowered(k.word), limit) <= limit) { near = &k; break; }
        }
        // Only at the start of sentences, a word may be any word ("Then"): only a near miss of a name is worth showing.
        if (!near && s.mid == 0) continue;
        NameIssue is;
        is.name = typeset::utf8(wd);
        is.count = s.total;
        is.nodes = s.nodes;
        if (near) {
            is.kind = NameIssue::Variant;
            is.like = typeset::utf8(near->word);
            is.entryId = near->entry;
            out.push_back(std::move(is));
        } else if (s.total >= 3) {
            is.kind = NameIssue::Unknown;
            out.push_back(std::move(is));
        }
    }
    // Likely misspellings first, then the most used.
    std::stable_sort(out.begin(), out.end(), [](const NameIssue& a, const NameIssue& b) {
        if (a.kind != b.kind) return a.kind == NameIssue::Variant;
        return a.count > b.count;
    });
    return out;
}

}  // namespace review
}  // namespace studio
