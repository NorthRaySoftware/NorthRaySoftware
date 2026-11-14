// Sharing a world bible as a website or a printable book (see studio/bible_export.hpp).
#include "studio/bible_export.hpp"

#include "studio/document.hpp"
#include "internal.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace studio::bible {

namespace fs = std::filesystem;
using detail::u8path;

// ----------------------------------------------------------------- what's shown

std::vector<Id> booksInOrder(const Library& lib, const Id& worldId, const Id& bookId) {
    std::vector<Id> out;
    if (worldId.empty()) { if (!bookId.empty()) out.push_back(bookId); return out; }
    for (const Series* s : lib.seriesIn(worldId))
        for (const Book* b : lib.booksInSeries(s->id))
            if (std::find(out.begin(), out.end(), b->id) == out.end()) out.push_back(b->id);
    for (const Book* b : lib.standaloneBooksIn(worldId))
        if (std::find(out.begin(), out.end(), b->id) == out.end()) out.push_back(b->id);
    return out;
}

View::View(const Library& lib, const Options& options) : lib_(lib), options_(options) {
    books_ = booksInOrder(lib, options.worldId, options.bookId);
    if (options_.readersGuide && !options_.upTo.empty())
        for (size_t i = 0; i < books_.size(); ++i) if (books_[i] == options_.upTo) upToIndex_ = (int)i;
    for (const Entry* e : lib.entriesOf(options.worldId, options.worldId.empty() ? options.bookId : Id{}))
        if (revealedInTime(e->revealedIn("entry"))) entries_.push_back(e);
}

bool View::revealedInTime(const Id& book) const {
    if (book.empty() || upToIndex_ < 0) return true;
    for (size_t i = 0; i < books_.size(); ++i) if (books_[i] == book) return (int)i <= upToIndex_;
    return true;   // a book no longer in the world: not hidden
}

bool View::shows(const Entry& e, const std::string& key) const { return revealedInTime(e.revealedIn(key)); }

bool View::shows(const Id& entryId) const {
    return std::any_of(entries_.begin(), entries_.end(), [&](const Entry* e) { return e->id == entryId; });
}

std::string View::title() const {
    if (const World* w = lib_.world(options_.worldId)) return w->name;
    if (const Book* b = lib_.book(options_.bookId)) return b->title;
    return "World bible";
}

// ----------------------------------------------------------------- pictures

std::string pictureType(const std::string& b) {
    if (b.size() > 8 && (unsigned char)b[0] == 0x89 && b.compare(1, 3, "PNG") == 0) return "png";
    if (b.size() > 3 && (unsigned char)b[0] == 0xFF && (unsigned char)b[1] == 0xD8) return "jpg";
    if (b.size() > 6 && b.compare(0, 3, "GIF") == 0) return "gif";
    if (b.size() > 12 && b.compare(0, 4, "RIFF") == 0 && b.compare(8, 4, "WEBP") == 0) return "webp";
    return {};
}

std::pair<int, int> pictureSize(const std::string& b) {
    auto be16 = [&](size_t i) { return ((unsigned char)b[i] << 8) | (unsigned char)b[i + 1]; };
    auto be32 = [&](size_t i) { return (int)(((unsigned)(unsigned char)b[i] << 24) | ((unsigned char)b[i + 1] << 16) | ((unsigned char)b[i + 2] << 8) | (unsigned char)b[i + 3]); };
    const std::string t = pictureType(b);
    if (t == "png" && b.size() >= 24) return {be32(16), be32(20)};
    if (t == "gif" && b.size() > 10) return {(unsigned char)b[6] | ((unsigned char)b[7] << 8), (unsigned char)b[8] | ((unsigned char)b[9] << 8)};
    if (t == "jpg") {
        size_t i = 2;
        while (i + 9 < b.size()) {
            if ((unsigned char)b[i] != 0xFF) { ++i; continue; }
            const unsigned char m = (unsigned char)b[i + 1];
            if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) return {be16(i + 7), be16(i + 5)};
            if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
            i += 2 + be16(i + 2);
        }
    }
    if (t == "webp" && b.size() > 30) {
        if (b.compare(12, 4, "VP8X") == 0)
            return {1 + ((unsigned char)b[24] | ((unsigned char)b[25] << 8) | ((unsigned char)b[26] << 16)),
                    1 + ((unsigned char)b[27] | ((unsigned char)b[28] << 8) | ((unsigned char)b[29] << 16))};
        if (b.compare(12, 4, "VP8 ") == 0) return {((unsigned char)b[26] | ((unsigned char)b[27] << 8)) & 0x3FFF, ((unsigned char)b[28] | ((unsigned char)b[29] << 8)) & 0x3FFF};
    }
    return {0, 0};
}

namespace {

std::string readFile(const std::string& path) {
    std::ifstream f(u8path(path), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// "Label: value" lines.
std::vector<std::pair<std::string, std::string>> detailLines(const std::string& details) {
    std::vector<std::pair<std::string, std::string>> out;
    std::istringstream in(details);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        const size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0 || colon > 40) { out.push_back({{}, line}); continue; }
        std::string v = line.substr(colon + 1);
        while (!v.empty() && v.front() == ' ') v.erase(0, 1);
        out.push_back({line.substr(0, colon), v});
    }
    return out;
}

std::vector<std::string> paragraphs(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line, cur;
    while (std::getline(in, line)) {
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) { if (!cur.empty()) out.push_back(cur); cur.clear(); continue; }
        cur += (cur.empty() ? "" : " ") + line;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// The relationships an entry has with shown entries, read from its side ("Lives in" Highcliff).
std::vector<std::pair<std::string, const Entry*>> relationsOf(const Library& lib, const View& v, const Entry& e) {
    std::vector<std::pair<std::string, const Entry*>> out;
    static const std::set<std::string> family{"Parent of", "Child of", "Partner of", "Married to", "Sibling of"};
    for (const EntryLink& l : lib.linksOf(e.id)) {
        if (family.count(l.label)) continue;   // in the family list
        const bool mine = l.from == e.id;
        const Entry* o = lib.entry(mine ? l.to : l.from);
        if (!o || !v.shows(o->id)) continue;
        out.push_back({mine ? l.label : reverseRelation(l.label), o});
    }
    return out;
}

// ----------------------------------------------------------------- website

std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '&') o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else if (c == '"') o += "&quot;";
        else o += c;
    }
    return o;
}

std::string pageName(const Id& id) { return "e-" + id + ".html"; }

// Text with the bible's names linked to their pages (never to the entry itself).
std::string linked(const std::string& text, const std::vector<const Entry*>& shown, const Id& self) {
    std::string out;
    size_t at = 0;
    for (const NameSpan& sp : findNames(text, shown)) {
        out += esc(text.substr(at, sp.start - at));
        Id to;
        for (const Id& i : sp.ids) if (i != self) { to = i; break; }
        const std::string word = text.substr(sp.start, sp.end - sp.start);
        out += to.empty() ? esc(word) : "<a href=\"" + pageName(to) + "\">" + esc(word) + "</a>";
        at = sp.end;
    }
    return out + esc(text.substr(at));
}

const char* kCss = R"CSS(:root{--ink:#1d2330;--muted:#5d6577;--faint:#8d93a1;--rule:#dfe2e8;--paper:#fbfaf7;--card:#fff;--accent:#2d4a6e}
@media (prefers-color-scheme:dark){:root{--ink:#e9ebf0;--muted:#aab0bd;--faint:#7c8290;--rule:#2c3240;--paper:#12151c;--card:#1a1e27;--accent:#8fb3e0}}
*{box-sizing:border-box}body{margin:0;background:var(--paper);color:var(--ink);font:17px/1.6 Georgia,"Times New Roman",serif}
header{border-bottom:1px solid var(--rule);padding:14px 20px;font:15px/1.4 system-ui,sans-serif;display:flex;gap:18px;flex-wrap:wrap;align-items:baseline}
header a{color:var(--muted);text-decoration:none}header a:hover{color:var(--accent)}header .home{font-weight:600;color:var(--ink);font-size:17px}
main{max-width:860px;margin:0 auto;padding:28px 20px 80px}h1{font-size:2.3em;line-height:1.15;margin:.2em 0 .1em}h2{font:600 13px system-ui,sans-serif;letter-spacing:.08em;text-transform:uppercase;color:var(--faint);margin:2.2em 0 .6em}
a{color:var(--accent)}.kind{font:600 12px system-ui,sans-serif;letter-spacing:.08em;text-transform:uppercase;color:var(--accent)}.said,.also{color:var(--muted)}
.guide{background:var(--card);border:1px solid var(--rule);border-radius:8px;padding:10px 14px;font:14px system-ui,sans-serif;color:var(--muted);margin-bottom:20px}
.top{display:flex;gap:26px;align-items:flex-start;flex-wrap:wrap}.top img{width:220px;max-width:100%;border-radius:6px;box-shadow:0 2px 10px #0002}
table{border-collapse:collapse;width:100%;max-width:560px;font-size:15px}td{padding:5px 0;border-bottom:1px solid var(--rule);vertical-align:top}td:first-child{color:var(--muted);width:38%}
ul.list{list-style:none;padding:0;columns:2 260px}ul.list li{padding:3px 0}.gallery{display:flex;gap:12px;flex-wrap:wrap}.gallery figure{margin:0;max-width:260px}
.gallery img{max-width:260px;max-height:200px;border-radius:6px}figcaption{font:13px system-ui,sans-serif;color:var(--muted)}
.map{position:relative;display:inline-block;max-width:100%}.map img{max-width:100%;display:block;border-radius:6px}
.pin{position:absolute;transform:translate(-7px,-7px);display:flex;align-items:center;gap:6px;font:13px system-ui,sans-serif;text-decoration:none;color:var(--ink)}
.pin i{width:14px;height:14px;border-radius:50%;background:var(--accent);border:2px solid #fff;display:block}.pin span{background:#fffd;color:#1d2330;border-radius:10px;padding:1px 8px}
.letter{font-size:1.6em;color:var(--accent);border-bottom:1px solid var(--rule);margin:1.2em 0 .4em}dl dt{font-weight:600;margin-top:.8em}dl dd{margin:0;color:var(--muted)}
footer{max-width:860px;margin:0 auto;padding:0 20px 40px;color:var(--faint);font:13px system-ui,sans-serif}
)CSS";

struct Site {
    const Library& lib;
    const View& view;
    std::string folder;
    std::map<std::string, std::string> pictures;   // blob -> "images/<blob>.<ext>" ("" = couldn't)
    std::string error;

    std::string picture(const std::string& blob) {
        if (blob.empty()) return {};
        if (auto it = pictures.find(blob); it != pictures.end()) return it->second;
        const std::string bytes = readFile(lib.blobPath(blob));
        const std::string type = pictureType(bytes);
        std::string rel;
        if (!type.empty()) {
            rel = "images/" + blob.substr(0, 24) + "." + type;
            std::ofstream f(u8path(folder + "/" + rel), std::ios::binary | std::ios::trunc);
            f << bytes;
            if (!f.good()) rel.clear();
        }
        pictures[blob] = rel;
        return rel;
    }

    std::string page(const std::string& title, const std::string& body) const {
        std::string nav = "<header><a class=\"home\" href=\"index.html\">" + esc(view.title()) + "</a>";
        std::set<std::string> kinds;
        for (const Entry* e : view.entries()) kinds.insert(e->kind);
        for (const auto& k : entryKinds())
            if (kinds.count(k)) nav += "<a href=\"index.html#" + k + "\">" + esc(entryKindPlural(k)) + "</a>";
        nav += "<a href=\"glossary.html\">Glossary</a></header>";
        std::string guide;
        if (view.options().readersGuide) {
            const Book* b = lib.book(view.options().upTo);
            guide = "<div class=\"guide\">A reader's guide" + (b ? " for readers of <em>" + esc(b->title) + "</em>: nothing here is revealed later in the series." : std::string(".")) + "</div>";
        }
        return "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
               "<title>" + esc(title) + "</title><link rel=\"stylesheet\" href=\"style.css\"></head><body>" + nav + "<main>" + guide + body +
               "</main><footer>Made with NorthRay Writing Studio.</footer></body></html>\n";
    }

    bool write(const std::string& name, const std::string& text) {
        std::ofstream f(u8path(folder + "/" + name), std::ios::binary | std::ios::trunc);
        f << text;
        if (!f.good()) { error = "Couldn't write " + name + "."; return false; }
        return true;
    }

    std::string entryPage(const Entry& e) {
        const auto& shown = view.entries();
        std::string b;
        b += "<div class=\"top\">";
        if (const std::string img = picture(e.imageBlob); !img.empty()) b += "<img src=\"" + img + "\" alt=\"" + esc(e.name) + "\">";
        b += "<div><div class=\"kind\">" + esc(entryKindLabel(e.kind)) + "</div><h1>" + esc(e.name) + "</h1>";
        if (!e.pronunciation.empty()) b += "<div class=\"said\">Said “" + esc(e.pronunciation) + "”</div>";
        if (!e.aliases.empty()) {
            std::string a;
            for (auto& x : e.aliases) a += (a.empty() ? "" : ", ") + std::string("“") + esc(x) + "”";
            b += "<div class=\"also\">Also " + a + "</div>";
        }
        if (!e.summary.empty() && view.shows(e, "summary")) b += "<p>" + linked(e.summary, shown, e.id) + "</p>";
        std::string rows;
        for (auto& [label, value] : detailLines(e.details)) {
            if (!label.empty() && !view.shows(e, "detail:" + label)) continue;
            rows += label.empty() ? "<tr><td colspan=\"2\">" + linked(value, shown, e.id) + "</td></tr>"
                                  : "<tr><td>" + esc(label) + "</td><td>" + linked(value, shown, e.id) + "</td></tr>";
        }
        if (!rows.empty()) b += "<table>" + rows + "</table>";
        b += "</div></div>";
        for (auto& [title, text] : lib.entrySections(e)) {
            if (text.empty() || !view.shows(e, "section:" + title)) continue;
            b += "<h2>" + esc(title) + "</h2>";
            for (auto& p : paragraphs(text)) b += "<p>" + linked(p, shown, e.id) + "</p>";
        }
        if (!view.options().readersGuide) {
            for (const Id& bk : booksInOrder(lib, view.options().worldId, view.options().bookId)) {
                const std::string& note = e.bookNote(bk);
                const Book* book = lib.book(bk);
                if (note.empty() || !book) continue;
                b += "<h2>In " + esc(book->title) + "</h2>";
                for (auto& p : paragraphs(note)) b += "<p>" + linked(p, shown, e.id) + "</p>";
            }
        }
        if (e.kind == "character") {
            const auto f = lib.familyOf(e.id);
            std::string fam;
            auto row = [&](const char* label, const std::vector<Id>& ids) {
                std::string names;
                for (const Id& i : ids)
                    if (const Entry* o = lib.entry(i); o && view.shows(i)) names += (names.empty() ? "" : ", ") + std::string("<a href=\"") + pageName(i) + "\">" + esc(o->name) + "</a>";
                if (!names.empty()) fam += "<tr><td>" + std::string(label) + "</td><td>" + names + "</td></tr>";
            };
            row("Parents", f.parents);
            row("Partners", f.partners);
            row("Brothers and sisters", f.siblings);
            row("Children", f.children);
            if (!fam.empty()) b += "<h2>Family</h2><table>" + fam + "</table>";
        }
        if (const auto rel = relationsOf(lib, view, e); !rel.empty()) {
            b += "<h2>Relationships</h2><table>";
            for (auto& [label, o] : rel) b += "<tr><td>" + esc(label) + "</td><td><a href=\"" + pageName(o->id) + "\">" + esc(o->name) + "</a></td></tr>";
            b += "</table>";
        }
        if (!e.mapBlob.empty()) {
            if (const std::string img = picture(e.mapBlob); !img.empty()) {
                b += "<h2>Map</h2><div class=\"map\"><img src=\"" + img + "\" alt=\"Map of " + esc(e.name) + "\">";
                for (auto& p : e.pins)
                    if (const Entry* o = lib.entry(p.entry); o && view.shows(p.entry)) {
                        char pos[64];
                        std::snprintf(pos, sizeof pos, "left:%.2f%%;top:%.2f%%", p.x * 100, p.y * 100);
                        b += "<a class=\"pin\" style=\"" + std::string(pos) + "\" href=\"" + pageName(o->id) + "\"><i></i><span>" + esc(o->name) + "</span></a>";
                    }
                b += "</div>";
            }
        }
        std::string gal;
        for (auto& g : e.gallery)
            if (const std::string img = picture(g.blob); !img.empty())
                gal += "<figure><img src=\"" + img + "\" alt=\"" + esc(g.caption.empty() ? e.name : g.caption) + "\">" +
                       (g.caption.empty() ? "" : "<figcaption>" + esc(g.caption) + "</figcaption>") + "</figure>";
        if (!gal.empty()) b += "<h2>Gallery</h2><div class=\"gallery\">" + gal + "</div>";
        if (!view.options().readersGuide && !e.notes.empty()) {
            b += "<h2>Notes</h2>";
            for (auto& p : paragraphs(e.notes)) b += "<p>" + linked(p, shown, e.id) + "</p>";
        }
        return page(e.name + " - " + view.title(), b);
    }
};

}  // namespace

bool exportWebsite(const Library& lib, const Options& options, const std::string& folder, std::string& error, int* pages) {
    const View view(lib, options);
    std::error_code ec;
    fs::create_directories(u8path(folder + "/images"), ec);
    if (ec) { error = "Couldn't make the folder: " + ec.message(); return false; }
    Site site{lib, view, folder, {}, {}};
    if (!site.write("style.css", kCss)) { error = site.error; return false; }
    int n = 0;
    for (const Entry* e : view.entries()) {
        if (!site.write(pageName(e->id), site.entryPage(*e))) { error = site.error; return false; }
        ++n;
    }
    // Home: the world, then everything by kind.
    std::string home = "<h1>" + esc(view.title()) + "</h1>";
    if (const World* w = lib.world(options.worldId)) {
        if (const std::string img = site.picture(w->bannerBlob); !img.empty()) home = "<img src=\"" + img + "\" alt=\"\" style=\"width:100%;border-radius:8px\">" + home;
        for (auto& p : paragraphs(w->description)) home += "<p>" + linked(p, view.entries(), {}) + "</p>";
    }
    for (const auto& k : entryKinds()) {
        std::string items;
        for (const Entry* e : view.entries())
            if (e->kind == k) items += "<li><a href=\"" + pageName(e->id) + "\">" + esc(e->name) + "</a></li>";
        if (!items.empty()) home += "<h2 id=\"" + k + "\">" + esc(entryKindPlural(k)) + "</h2><ul class=\"list\">" + items + "</ul>";
    }
    if (!site.write("index.html", site.page(view.title(), home))) { error = site.error; return false; }
    // Glossary: every name and other name, A to Z.
    struct G { std::string key, name; const Entry* e; bool alias; };
    std::vector<G> g;
    auto key = [](std::string s) {
        for (auto& c : s) c = (char)std::tolower((unsigned char)c);
        if (s.rfind("the ", 0) == 0) s = s.substr(4);
        return s;
    };
    for (const Entry* e : view.entries()) {
        g.push_back({key(e->name), e->name, e, false});
        for (auto& a : e->aliases) if (!a.empty()) g.push_back({key(a), a, e, true});
    }
    std::sort(g.begin(), g.end(), [](const G& a, const G& b) { return a.key < b.key; });
    std::string gl = "<h1>Glossary</h1><dl>";
    char letter = 0;
    for (auto& x : g) {
        const char l = x.key.empty() ? '#' : (char)std::toupper((unsigned char)x.key[0]);
        if (l != letter) { letter = l; gl += "</dl><div class=\"letter\">" + std::string(1, std::isalpha((unsigned char)l) ? l : '#') + "</div><dl>"; }
        gl += "<dt><a href=\"" + pageName(x.e->id) + "\">" + esc(x.name) + "</a>";
        if (!x.alias && !x.e->pronunciation.empty()) gl += " <span class=\"said\">(" + esc(x.e->pronunciation) + ")</span>";
        gl += "</dt><dd>" + (x.alias ? "see " + esc(x.e->name)
                                     : esc(entryKindLabel(x.e->kind)) + (x.e->summary.empty() || !view.shows(*x.e, "summary") ? std::string() : ". " + esc(x.e->summary))) +
              "</dd>";
    }
    gl += "</dl>";
    if (!site.write("glossary.html", site.page("Glossary - " + view.title(), gl))) { error = site.error; return false; }
    if (pages) *pages = n + 2;
    return true;
}

// ----------------------------------------------------------------- printable

typeset::Source printableSource(const Library& lib, const Options& options, const std::string& title, const std::string& author) {
    const View view(lib, options);
    typeset::Source s;
    s.id = "bible-" + (options.worldId.empty() ? options.bookId : options.worldId);
    s.title = title;
    s.author = author;
    s.language = "en-GB";
    {
        const auto books = booksInOrder(lib, options.worldId, options.bookId);
        if (!books.empty()) if (const Book* b = lib.book(books.front())) { s.trim = b->trim; s.language = b->language; }
        const std::time_t t = std::time(nullptr);
        s.year = 1900 + std::gmtime(&t)->tm_year;
        if (options.readersGuide) if (const Book* b = lib.book(options.upTo)) s.subtitle = "A reader's guide to " + b->title;
    }
    auto addPicture = [&](const std::string& blob) {
        if (blob.empty() || s.picturePaths.count(blob)) return !blob.empty() && s.pictureSizes.count(blob);
        const std::string path = lib.blobPath(blob);
        const auto size = pictureSize(readFile(path));
        if (size.first <= 0 || size.second <= 0) return false;
        s.picturePaths[blob] = path;
        s.pictureSizes[blob] = size;
        return true;
    };
    for (const auto& k : entryKinds()) {
        std::vector<const Entry*> list;
        for (const Entry* e : view.entries()) if (e->kind == k) list.push_back(e);
        if (list.empty()) continue;
        typeset::Source::Section part;
        part.kind = NodeKind::Part;
        part.title = entryKindPlural(k);
        part.content = doc::emptyDoc();
        s.sections.push_back(part);
        for (const Entry* e : list) {
            doc::Builder d;
            if (addPicture(e->imageBlob)) d.image(e->imageBlob, e->name);
            auto para = [&](const std::string& text, const std::string& style = "first") {
                d.paragraph(style);
                d.text(text);
                d.endBlock();
            };
            if (!e->pronunciation.empty()) { d.paragraph("centered"); d.text("Said “" + e->pronunciation + "”", doc::Builder::Em); d.endBlock(); }
            if (!e->aliases.empty()) {
                std::string a;
                for (auto& x : e->aliases) a += (a.empty() ? "" : ", ") + x;
                d.paragraph("centered");
                d.text("Also known as " + a, doc::Builder::Em);
                d.endBlock();
            }
            if (!e->summary.empty() && view.shows(*e, "summary")) para(e->summary);
            for (auto& [label, value] : detailLines(e->details)) {
                if (!label.empty() && !view.shows(*e, "detail:" + label)) continue;
                d.paragraph("first");
                if (!label.empty()) d.text(label + ": ", doc::Builder::Strong);
                d.text(value);
                d.endBlock();
            }
            for (auto& [t, text] : lib.entrySections(*e)) {
                if (text.empty() || !view.shows(*e, "section:" + t)) continue;
                d.heading(2);
                d.text(t);
                d.endBlock();
                bool first = true;
                for (auto& p : paragraphs(text)) { para(p, first ? "first" : "body"); first = false; }
            }
            if (e->kind == "character") {
                const auto f = lib.familyOf(e->id);
                std::vector<std::pair<std::string, std::string>> fam;
                auto names = [&](const std::vector<Id>& ids) {
                    std::string out;
                    for (const Id& i : ids) if (const Entry* o = lib.entry(i); o && view.shows(i)) out += (out.empty() ? "" : ", ") + o->name;
                    return out;
                };
                if (auto n = names(f.parents); !n.empty()) fam.push_back({"Parents", n});
                if (auto n = names(f.partners); !n.empty()) fam.push_back({"Partners", n});
                if (auto n = names(f.siblings); !n.empty()) fam.push_back({"Brothers and sisters", n});
                if (auto n = names(f.children); !n.empty()) fam.push_back({"Children", n});
                if (!fam.empty()) {
                    d.heading(2);
                    d.text("Family");
                    d.endBlock();
                    for (auto& [l, v] : fam) {
                        d.paragraph("first");
                        d.text(l + ": ", doc::Builder::Strong);
                        d.text(v);
                        d.endBlock();
                    }
                }
            }
            if (const auto rel = relationsOf(lib, view, *e); !rel.empty()) {
                d.heading(2);
                d.text("Relationships");
                d.endBlock();
                for (auto& [l, o] : rel) {
                    d.paragraph("first");
                    d.text(l + " ", doc::Builder::Em);
                    d.text(o->name);
                    d.endBlock();
                }
            }
            if (!options.readersGuide) {
                for (const Id& bk : booksInOrder(lib, options.worldId, options.bookId)) {
                    const std::string& note = e->bookNote(bk);
                    const Book* book = lib.book(bk);
                    if (note.empty() || !book) continue;
                    d.heading(2);
                    d.text("In " + book->title);
                    d.endBlock();
                    bool first = true;
                    for (auto& p : paragraphs(note)) { para(p, first ? "first" : "body"); first = false; }
                }
                if (!e->notes.empty()) {
                    d.heading(2);
                    d.text("Notes");
                    d.endBlock();
                    bool first = true;
                    for (auto& p : paragraphs(e->notes)) { para(p, first ? "first" : "body"); first = false; }
                }
            }
            if (!e->mapBlob.empty() && addPicture(e->mapBlob)) d.image(e->mapBlob, "Map of " + e->name, "Map of " + e->name);
            for (auto& g : e->gallery) if (g.blob != e->imageBlob && addPicture(g.blob)) d.image(g.blob, g.caption.empty() ? e->name : g.caption, g.caption);
            if (d.empty()) para(entryKindLabel(e->kind));
            typeset::Source::Section ch;
            ch.kind = NodeKind::Chapter;
            ch.matter = 2;   // never "Chapter One": an entry is headed by its name only
            ch.title = e->name;
            ch.content = d.toJson();
            ch.words = d.words();
            s.sections.push_back(std::move(ch));
        }
    }
    // The glossary, at the back.
    {
        doc::Builder d;
        struct G { std::string key, name, line; };
        std::vector<G> g;
        for (const Entry* e : view.entries()) {
            std::string k = e->name;
            for (auto& c : k) c = (char)std::tolower((unsigned char)c);
            std::string line = std::string(entryKindLabel(e->kind));
            if (!e->summary.empty() && view.shows(*e, "summary")) line += ". " + e->summary;
            g.push_back({k, e->name + (e->pronunciation.empty() ? "" : " (" + e->pronunciation + ")"), line});
            for (auto& a : e->aliases) {
                std::string ak = a;
                for (auto& c : ak) c = (char)std::tolower((unsigned char)c);
                g.push_back({ak, a, "See " + e->name + "."});
            }
        }
        std::sort(g.begin(), g.end(), [](const G& a, const G& b) { return a.key < b.key; });
        for (auto& x : g) {
            d.paragraph("first");
            d.text(x.name + "  ", doc::Builder::Strong);
            d.text(x.line);
            d.endBlock();
        }
        if (!g.empty()) {
            typeset::Source::Section ch;
            ch.kind = NodeKind::Chapter;
            ch.matter = 2;
            ch.title = "Glossary";
            ch.content = d.toJson();
            ch.words = d.words();
            s.sections.push_back(std::move(ch));
        }
    }
    return s;
}

}  // namespace studio::bible
