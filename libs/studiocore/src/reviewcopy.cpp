// Review copies: a book as one HTML file a beta reader opens in any browser and comments on, and
// bringing their comments back, each placed on the words it was about.
#include "studio/review.hpp"

#include "studio/document.hpp"
#include "studio/ebook.hpp"
#include "studio/headers.hpp"

#include "internal.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>

namespace studio::review {

using nlohmann::json;

namespace {

std::string base64(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8) | (unsigned char)in[i + 2];
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += t[v & 63];
    }
    if (i + 1 == in.size()) {
        const unsigned v = (unsigned char)in[i] << 16;
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        const unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8);
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += '=';
    }
    return out;
}

std::string mimeOf(const std::string& bytes) {
    if (bytes.size() > 4 && (unsigned char)bytes[0] == 0x89 && bytes.compare(1, 3, "PNG") == 0) return "image/png";
    if (bytes.size() > 3 && (unsigned char)bytes[0] == 0xFF && (unsigned char)bytes[1] == 0xD8) return "image/jpeg";
    if (bytes.size() > 6 && bytes.compare(0, 3, "GIF") == 0) return "image/gif";
    if (bytes.size() > 12 && bytes.compare(0, 4, "RIFF") == 0 && bytes.compare(8, 4, "WEBP") == 0) return "image/webp";
    return "";
}

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) s.replace(at, from.size(), to);
    return s;
}

std::string hexId(int digits) {
    static std::mt19937_64 rng{std::random_device{}()};
    std::string out;
    const char* hex = "0123456789abcdef";
    for (int i = 0; i < digits; ++i) out += hex[rng() & 15];
    return out;
}

bool isSpace(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// Whitespace runs as one space (the browser's selection and the stored text can differ there).
std::string squeeze(const std::string& s) {
    std::string out;
    bool sp = false;
    for (unsigned char c : s) {
        if (isSpace(c)) { if (!sp) out += ' '; sp = true; }
        else { out += (char)c; sp = false; }
    }
    return out;
}

// The text of a document as the review copy shows it, with where each byte came from.
struct Piece { json* node; size_t offset; };   // a text node and the byte inside it
struct Stream { std::string text; std::vector<Piece> at; };

void collect(json& content, Stream& s) {
    if (!content.is_array()) return;
    for (auto& n : content) {
        if (!n.is_object()) continue;
        const std::string t = n.value("type", "");
        if (t == "text" && n.contains("text") && n["text"].is_string()) {
            const std::string& tx = n["text"].get_ref<const std::string&>();
            for (size_t i = 0; i < tx.size(); ++i) { s.text += tx[i]; s.at.push_back({&n, i}); }
        } else if (t == "hard_break") {
            s.text += ' ';
            s.at.push_back({nullptr, 0});
        } else if (n.contains("content")) {
            collect(n["content"], s);
            if (t == "paragraph" || t == "heading") { s.text += ' '; s.at.push_back({nullptr, 0}); }   // between blocks
        }
    }
}

// Adds a comment mark over [from, to) of each text node the range crosses, splitting nodes as needed.
void markRange(json& content, const std::set<const json*>& touched, const std::map<const json*, std::pair<size_t, size_t>>& ranges,
               const std::string& id) {
    if (!content.is_array()) return;
    json out = json::array();
    for (auto& n : content) {
        if (n.is_object() && n.contains("content")) markRange(n["content"], touched, ranges, id);
        if (!touched.count(&n)) { out.push_back(n); continue; }
        const auto [a, z] = ranges.at(&n);
        const std::string tx = n["text"].get<std::string>();
        auto piece = [&](size_t s, size_t e, bool marked) {
            if (e <= s) return;
            json p = n;
            p["text"] = tx.substr(s, e - s);
            if (marked) {
                if (!p.contains("marks") || !p["marks"].is_array()) p["marks"] = json::array();
                p["marks"].push_back({{"type", "comment"}, {"attrs", {{"id", id}}}});
            }
            out.push_back(std::move(p));
        };
        piece(0, a, false);
        piece(a, z, true);
        piece(z, tx.size(), false);
    }
    content = std::move(out);
}

}  // namespace

bool markQuote(std::string& docJson, const std::string& quote, const std::string& prefix, const std::string& suffix, const std::string& id) {
    json doc = json::parse(docJson, nullptr, false);
    if (!doc.is_object() || !doc.contains("content") || squeeze(quote).empty()) return false;
    Stream s;
    collect(doc["content"], s);
    // Compare squeezed: map each squeezed byte back to the stream.
    std::string sq;
    std::vector<size_t> back;
    bool sp = false;
    for (size_t i = 0; i < s.text.size(); ++i) {
        const unsigned char c = (unsigned char)s.text[i];
        if (isSpace(c)) { if (!sp) { sq += ' '; back.push_back(i); } sp = true; }
        else { sq += (char)c; back.push_back(i); sp = false; }
    }
    std::string q = squeeze(quote);
    while (!q.empty() && q.front() == ' ') q.erase(0, 1);
    while (!q.empty() && q.back() == ' ') q.pop_back();
    const std::string pre = squeeze(prefix), suf = squeeze(suffix);
    size_t best = std::string::npos;
    int bestScore = -1;
    for (size_t i = sq.find(q); i != std::string::npos; i = sq.find(q, i + 1)) {
        int score = 0;
        for (size_t k = 1; k <= pre.size() && k <= i && sq[i - k] == pre[pre.size() - k]; ++k) ++score;
        for (size_t k = 0; k < suf.size() && i + q.size() + k < sq.size() && sq[i + q.size() + k] == suf[k]; ++k) ++score;
        if (score > bestScore) { best = i; bestScore = score; }
    }
    if (best == std::string::npos) return false;
    const size_t from = back[best], to = back[best + q.size() - 1] + 1;
    std::set<const json*> touched;
    std::map<const json*, std::pair<size_t, size_t>> ranges;
    for (size_t i = from; i < to; ++i) {
        const Piece& p = s.at[i];
        if (!p.node) continue;
        auto [it, fresh] = ranges.emplace(p.node, std::make_pair(p.offset, p.offset + 1));
        if (!fresh) it->second.second = p.offset + 1;
        touched.insert(p.node);
    }
    if (touched.empty()) return false;
    markRange(doc["content"], touched, ranges, id);
    docJson = doc.dump();
    return true;
}

std::string makeReviewCopy(const Library& lib, const Id& bookId, const std::string& templateHtml, std::string& error) {
    const Book* b = lib.book(bookId);
    if (!b) { error = "That book no longer exists."; return {}; }
    json chapters = json::array();
    // Pictures go inside the file (up to 3 MB each), so it still works when emailed.
    auto inside = [&](const std::string& blob) -> std::string {
        std::ifstream in(studio::detail::u8path(lib.blobPath(blob)), std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        const std::string bytes = ss.str();
        const std::string mime = mimeOf(bytes);
        if (mime.empty() || bytes.size() > 3 * 1024 * 1024) return {};
        return "data:" + mime + ";base64," + base64(bytes);
    };
    const headers::BookHeaders heads = headers::read(lib, bookId);
    for (const ManuscriptNode* n : lib.manuscriptOf(bookId)) {
        if (!n || n->deleted) continue;
        const std::string html = ebook::documentHtml(doc::withoutDraftNotes(lib.nodeContent(n->id)), inside);   // draft notes stay home (0.94)
        if (n->kind == NodeKind::Scene && html.empty()) continue;
        json ch = {{"node", n->id}, {"title", n->title.empty() ? std::string(label(n->kind)) : n->title},
                   {"part", n->kind == NodeKind::Part}, {"html", html}};
        // Its header picture or ornament (0.89), with an empty heading the page fills with the title.
        if (const auto hd = headers::resolve(lib, *n, heads)) {
            const std::string src = hd->header.blob.empty() ? std::string() : inside(hd->header.blob);
            if (hd->header.blob.empty() || !src.empty()) ch["head"] = ebook::headerHtml(hd->header, src, "<h1></h1>\n");
        }
        chapters.push_back(ch);
    }
    if (chapters.empty()) { error = "The book has no chapters to share yet."; return {}; }
    const std::string author = b->penName.empty() ? b->author : b->penName;
    json data = {{"format", "nrreview"}, {"version", 1}, {"copy", hexId(16)}, {"book", bookId}, {"title", b->title}, {"author", author},
                 {"chapters", chapters}};
    // Inside a <script>, "</" must never appear.
    std::string payload = replaceAll(data.dump(), "</", "<\\/");
    std::string html = templateHtml;
    html = replaceAll(html, "{{TITLE}}", ebook::escape(b->title));
    html = replaceAll(html, "{{AUTHOR}}", ebook::escape(author.empty() ? std::string("the author") : author));
    html = replaceAll(html, "{{LANG}}", ebook::escape(b->language.empty() ? std::string("en") : b->language));
    const size_t at = html.find("{{DATA}}");
    if (at == std::string::npos) { error = "The review copy template is damaged."; return {}; }
    html.replace(at, 8, payload);
    return html;
}

ImportReport importReviewComments(Library& lib, const Id& bookId, const std::string& fileText) {
    ImportReport r;
    const json j = json::parse(fileText, nullptr, false);
    if (!j.is_object() || j.value("format", "") != "nrcomments" || !j.contains("comments") || !j["comments"].is_array()) {
        r.error = "That isn't a comments file from a NorthRay review copy.";
        return r;
    }
    if (j.value("book", "") != bookId) {
        r.error = "Those comments are on another book (" + j.value("title", std::string("unknown")) + ").";
        return r;
    }
    r.reader = j.value("reader", std::string("A reader"));
    if (r.reader.empty()) r.reader = "A reader";
    for (const json& c : j["comments"]) {
        if (!c.is_object()) continue;
        const Id node = c.value("node", "");
        const ManuscriptNode* n = lib.node(node);
        const std::string text = c.value("text", "");
        if (!n || n->bookId != bookId || n->deleted || text.empty()) { ++r.skipped; continue; }
        const std::string quote = c.value("quote", "");
        const std::string id = hexId(16);
        std::string doc = lib.nodeContent(node);
        const bool placed = !quote.empty() && markQuote(doc, quote, c.value("prefix", ""), c.value("suffix", ""), id);
        if (placed && !lib.saveNodeContent(node, doc)) { ++r.skipped; continue; }
        // The comment row: the reader's name with what they said. A comment on a whole chapter, or on
        // words since changed, has no place in the text; it shows with the chapter's comments.
        std::string shown = quote;
        if (shown.size() > 200) shown = shown.substr(0, 197) + "...";
        if (!lib.addComment(node, id, shown)) { ++r.skipped; continue; }
        std::string body = r.reader + ": " + text;
        if (!quote.empty() && !placed) body += "\n(On words that have changed since: “" + shown + "”)";
        lib.updateComment(id, body);
        if (placed) ++r.placed; else ++r.unplaced;
        if (std::find(r.nodes.begin(), r.nodes.end(), node) == r.nodes.end()) r.nodes.push_back(node);
    }
    return r;
}

}  // namespace studio::review
