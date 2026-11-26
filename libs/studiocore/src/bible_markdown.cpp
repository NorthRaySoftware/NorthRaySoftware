// The bible as Markdown that Import many pages reads back exactly (0.95). See studio/bible_markdown.hpp.
#include "studio/bible_markdown.hpp"
#include "studio/bible_export.hpp"
#include "internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <tuple>
#include <set>
#include <sstream>

namespace studio::paste {

namespace {

const char* kArrow = " \xE2\x86\x92 ";   // " → "

std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) ++a;
    return s.substr(a);
}

std::vector<std::string> linesOf(const std::string& t) {
    std::vector<std::string> v;
    std::string cur;
    for (char c : t) {
        if (c == '\n') { v.push_back(cur); cur.clear(); }
        else cur += c;
    }
    v.push_back(cur);
    return v;
}

// Text kept as written: each line, with a \ before one that starts with # or \ so it can't be read as a heading.
std::string block(const std::string& text) {
    std::string o;
    for (const std::string& l : linesOf(text)) o += (!l.empty() && (l[0] == '#' || l[0] == '\\') ? "\\" : "") + l + "\n";
    return o + "\n";
}
std::string unescape(const std::string& l) { return !l.empty() && l[0] == '\\' ? l.substr(1) : l; }
// A one-line value: newlines can't be in it.
std::string oneLine(std::string s) {
    for (auto& c : s) if (c == '\n' || c == '\r') c = ' ';
    return s;
}

std::string plural(const std::string& kind) {
    std::string l = entryKindLabel(kind);
    if (kind == "lore" || kind == "magic") return l;
    if (!l.empty() && l.back() == 's') return l + "es";
    if (!l.empty() && l.back() == 'y') return l.substr(0, l.size() - 1) + "ies";
    return l + "s";
}

std::string pictureExtension(const std::string& path) {
    std::ifstream f(detail::u8path(path), std::ios::binary);
    unsigned char b[12] = {0};
    f.read(reinterpret_cast<char*>(b), sizeof b);
    if (b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') return ".png";
    if (b[0] == 0xFF && b[1] == 0xD8) return ".jpg";
    if (b[0] == 'G' && b[1] == 'I' && b[2] == 'F') return ".gif";
    if (b[0] == 'R' && b[1] == 'I' && b[2] == 'F' && b[3] == 'F' && b[8] == 'W' && b[9] == 'E' && b[10] == 'B' && b[11] == 'P') return ".webp";
    if (b[0] == 'B' && b[1] == 'M') return ".bmp";
    return ".img";
}
bool looksLikePicture(const std::string& bytes) {
    if (bytes.size() < 12) return false;
    const auto* b = reinterpret_cast<const unsigned char*>(bytes.data());
    return (b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') || (b[0] == 0xFF && b[1] == 0xD8) || (b[0] == 'G' && b[1] == 'I' && b[2] == 'F') ||
           (b[0] == 'R' && b[1] == 'I' && b[2] == 'F' && b[3] == 'F' && b[8] == 'W' && b[9] == 'E') || (b[0] == 'B' && b[1] == 'M');
}

// A book's title for a note or a spoiler, and back.
std::string bookTitle(const Library& lib, const Id& id) {
    if (id.rfind(kLaterBook, 0) == 0) return id.substr(std::string(kLaterBook).size());
    const Book* b = lib.book(id);
    return b ? b->title : std::string();
}
Id bookByTitle(const Library& lib, const std::vector<Id>& books, const std::string& title) {
    for (const Id& id : books)
        if (const Book* b = lib.book(id); b && b->title == title) return id;
    return {};
}

// The kind written after a name in a relationship line: "Ralph Smith (character)".
void splitKind(const std::string& s, std::string& name, std::string& kind) {
    name = trimmed(s);
    kind.clear();
    if (name.empty() || name.back() != ')') return;
    const size_t open = name.rfind(" (");
    if (open == std::string::npos) return;
    const std::string k = name.substr(open + 2, name.size() - open - 3);
    const auto& kinds = entryKinds();
    if (std::find(kinds.begin(), kinds.end(), k) == kinds.end()) return;
    kind = k;
    name = trimmed(name.substr(0, open));
}

// A page as the export writes it.
struct Exact {
    std::string name, kind, pronunciation, symbol, symbolPicture, colour, iconStyle, picture, headerPicture, map, summary, details, notes;
    bool hasPicture = false, hasHeader = false, hasMap = false, hasGallery = false, hasPins = false, hasSymbolPicture = false;
    std::vector<std::string> aliases;
    std::vector<std::pair<std::string, std::string>> sections, bookNotes;   // title -> text; book title -> text
    struct Link { std::string label, name, kind; };
    std::vector<Link> links;
    std::vector<std::pair<std::string, std::string>> revealed;   // key -> book title
    std::vector<std::pair<std::string, std::string>> gallery;    // path -> caption
    struct Pin { std::string name, kind; double x = 0, y = 0; };
    std::vector<Pin> pins;
    std::vector<std::string> unread;
};

Exact readPage(const std::string& text) {
    Exact p;
    std::vector<std::string> ls = linesOf(text);
    if (!ls.empty() && ls.back().empty()) ls.pop_back();   // the text's own last newline
    enum Part { Head, Text, Links, Spoilers, Gallery, Pins, Skip } part = Head;
    std::string* target = nullptr;
    std::string buf;
    bool inBuf = false;
    bool afterHeading = false;   // the blank line the writer puts under every heading
    auto flush = [&] {
        if (inBuf && target) {
            // The writer puts "\n\n" after each text: take exactly that off.
            std::string t = buf;
            if (t.size() >= 2 && t.compare(t.size() - 2, 2, "\n\n") == 0) t.erase(t.size() - 2);
            else while (!t.empty() && t.back() == '\n') t.pop_back();
            *target = t;
        }
        buf.clear();
        inBuf = false;
        target = nullptr;
    };
    for (const std::string& raw : ls) {
        std::string l = raw;
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (l.rfind("## ", 0) == 0 && p.name.empty()) { p.name = l.substr(3); continue; }
        if (l.rfind("#### ", 0) == 0) {
            flush();
            p.bookNotes.emplace_back(l.substr(5), std::string());
            target = &p.bookNotes.back().second;
            inBuf = true;
            afterHeading = true;
            part = Text;
            continue;
        }
        if (l.rfind("### ", 0) == 0) {
            flush();
            const std::string h = l.substr(4);
            part = Text;
            afterHeading = true;
            if (h.find("(generated") != std::string::npos) part = Skip;
            else if (h == "Summary") { target = &p.summary; inBuf = true; }
            else if (h == "Details") { target = &p.details; inBuf = true; }
            else if (h == "Notes") { target = &p.notes; inBuf = true; }
            else if (h.rfind("Section: ", 0) == 0) {
                p.sections.emplace_back(h.substr(9), std::string());
                target = &p.sections.back().second;
                inBuf = true;
            } else if (h == "Notes per book") part = Skip;   // each book a #### of its own
            else if (h == "Family" || h == "Relationships") part = Links;
            else if (h == "Spoilers") part = Spoilers;
            else if (h == "Gallery") { part = Gallery; p.hasGallery = true; }
            else if (h == "Map pins") { part = Pins; p.hasPins = true; }
            else { part = Skip; p.unread.push_back(l); }
            continue;
        }
        if (afterHeading) { afterHeading = false; if (l.empty()) continue; }
        if (inBuf) { buf += unescape(l) + "\n"; continue; }
        const std::string t = trimmed(l);
        if (t.empty()) continue;
        if (part == Head) {
            const size_t c = t.find(": ");
            const std::string lab = c == std::string::npos ? t : t.substr(0, c), val = c == std::string::npos ? std::string() : t.substr(c + 2);
            if (lab == "Kind") p.kind = val;
            else if (lab == "Other name") p.aliases.push_back(val);
            else if (lab == "How it's said") p.pronunciation = val;
            else if (lab == "Symbol") p.symbol = val;
            else if (lab == "Symbol picture") { p.symbolPicture = val; p.hasSymbolPicture = true; }
            else if (lab == "Colour") p.colour = val;
            else if (lab == "Icon") p.iconStyle = val == "symbol alone" ? "symbol" : "";
            else if (lab == "Picture") { p.picture = val; p.hasPicture = true; }
            else if (lab == "Header picture") { p.headerPicture = val; p.hasHeader = true; }
            else if (lab == "Map") { p.map = val; p.hasMap = true; }
            else p.unread.push_back(t);
            continue;
        }
        if (part == Skip) continue;
        const std::string item = t.rfind("- ", 0) == 0 ? t.substr(2) : t;
        if (part == Links || part == Spoilers) {
            const size_t a = item.find(kArrow);
            if (a == std::string::npos) { p.unread.push_back(t); continue; }
            const std::string left = item.substr(0, a), right = item.substr(a + std::string(kArrow).size());
            if (part == Spoilers) { p.revealed.emplace_back(left, right); continue; }
            Exact::Link k;
            k.label = left;
            splitKind(right, k.name, k.kind);
            p.links.push_back(k);
        } else if (part == Gallery) {
            const size_t bar = item.find(" | ");
            p.gallery.emplace_back(bar == std::string::npos ? item : item.substr(0, bar), bar == std::string::npos ? std::string() : item.substr(bar + 3));
        } else if (part == Pins) {
            // "Name (kind) @ 0.25, 0.4"
            const size_t at = item.rfind(" @ ");
            const size_t comma = at == std::string::npos ? std::string::npos : item.find(", ", at);
            if (comma == std::string::npos) { p.unread.push_back(t); continue; }
            Exact::Pin pin;
            splitKind(item.substr(0, at), pin.name, pin.kind);
            pin.x = std::atof(item.substr(at + 3, comma - at - 3).c_str());
            pin.y = std::atof(item.substr(comma + 2).c_str());
            p.pins.push_back(pin);
        }
    }
    flush();
    return p;
}

std::string number(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

}  // namespace

bool isMarkdownExport(const std::string& text) {
    size_t a = 0;
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) a = 3;
    while (a < text.size() && (text[a] == '\n' || text[a] == '\r' || text[a] == ' ')) ++a;
    return text.compare(a, std::string(kExportMark).size(), kExportMark) == 0;
}

MarkdownExport exportMarkdown(const Library& lib, const Id& world, const Id& book, const MarkdownOptions& o) {
    MarkdownExport out;
    std::string& t = out.text;
    t = std::string(kExportMark) + ", format 1. Import many pages reads this file back exactly: change entries here and import it "
        "again. A line starting with a backslash has it only so that a # or \\ at its start isn't read as a heading. -->\n\n";
    const Id scopeBook = world.empty() ? book : Id{};
    const std::vector<Id> books = bible::booksInOrder(lib, world, scopeBook);
    std::map<std::string, std::string> picturePath;   // blob -> path
    auto pictureFor = [&](const std::string& blob) -> std::string {
        if (blob.empty() || o.picturesFolder.empty()) return {};
        if (auto it = picturePath.find(blob); it != picturePath.end()) return it->second;
        const std::string src = lib.blobPath(blob);
        std::error_code ec;
        if (!std::filesystem::exists(detail::u8path(src), ec)) return {};
        const std::string path = o.picturesFolder + "/" + blob.substr(0, 16) + pictureExtension(src);
        picturePath[blob] = path;
        out.pictures.emplace_back(blob, path);
        return path;
    };
    const auto all = lib.entriesOf(world, scopeBook);
    auto ref = [&](const Id& id) -> std::string {
        const Entry* e = lib.entry(id);
        return e ? oneLine(e->name) + " (" + e->kind + ")" : std::string();
    };
    std::unique_ptr<bible::View> view;
    std::map<Id, std::string> texts;
    if (o.generated) {
        bible::Options vo;
        vo.worldId = world;
        vo.bookId = scopeBook;
        view = std::make_unique<bible::View>(lib, vo);
    }
    for (const std::string& kind : entryKinds()) {
        if (!o.kinds.empty() && std::find(o.kinds.begin(), o.kinds.end(), kind) == o.kinds.end()) continue;
        bool heading = false;
        for (const Entry* e : all) {
            if (e->kind != kind || e->deleted) continue;
            if (!o.entries.empty() && std::find(o.entries.begin(), o.entries.end(), e->id) == o.entries.end()) continue;
            if (!heading) { t += "# " + plural(kind) + "\n\n"; heading = true; }
            ++out.entries;
            t += "## " + oneLine(e->name) + "\n\nKind: " + kind + "\n";
            for (const std::string& a : e->aliases) t += "Other name: " + oneLine(a) + "\n";
            if (!e->pronunciation.empty()) t += "How it's said: " + oneLine(e->pronunciation) + "\n";
            if (e->symbol.rfind("blob:", 0) == 0) {   // a picture as its symbol (0.101)
                if (const std::string p = pictureFor(e->symbol.substr(5)); !p.empty()) t += "Symbol picture: " + p + "\n";
            } else if (!e->symbol.empty())
                t += "Symbol: " + oneLine(e->symbol) + "\n";
            if (!e->colour.empty()) t += "Colour: " + e->colour + "\n";
            if (e->iconStyle == "symbol") t += "Icon: symbol alone\n";
            if (const std::string p = pictureFor(e->imageBlob); !p.empty()) t += "Picture: " + p + "\n";
            if (const std::string p = pictureFor(e->headerBlob); !p.empty()) t += "Header picture: " + p + "\n";
            if (const std::string p = pictureFor(e->mapBlob); !p.empty()) t += "Map: " + p + "\n";
            t += "\n";
            if (!e->summary.empty()) t += "### Summary\n\n" + block(e->summary);
            if (!e->details.empty()) t += "### Details\n\n" + block(e->details);
            for (const auto& [title, text] : e->sections)   // empty ones too: the page keeps its sections
                t += "### Section: " + oneLine(title) + "\n\n" + block(text);
            {
                std::string notes;
                for (const auto& [bid, text] : e->bookNotes) {
                    const std::string title = bookTitle(lib, bid);
                    if (!text.empty() && !title.empty()) notes += "#### " + oneLine(title) + "\n\n" + block(text);
                }
                if (!notes.empty()) t += "### Notes per book\n\n" + notes;
            }
            std::string family, others;
            for (const EntryLink& l : lib.linksOf(e->id)) {
                if (l.from != e->id) continue;   // each link once: on the page of the one it starts from
                const std::string r = ref(l.to);
                if (r.empty()) continue;
                (isFamilyRelation(l.label) ? family : others) += "- " + oneLine(l.label) + kArrow + r + "\n";
            }
            if (!family.empty()) t += "### Family\n\n" + family + "\n";
            if (!others.empty()) t += "### Relationships\n\n" + others + "\n";
            if (!e->notes.empty()) t += "### Notes\n\n" + block(e->notes);
            {
                std::string sp;
                for (const auto& [k, bid] : e->revealed)
                    if (const std::string title = bookTitle(lib, bid); !title.empty()) sp += "- " + oneLine(k) + kArrow + oneLine(title) + "\n";
                if (!sp.empty()) t += "### Spoilers\n\n" + sp + "\n";
            }
            {
                std::string g;
                for (const auto& pic : e->gallery)
                    if (const std::string p = pictureFor(pic.blob); !p.empty()) g += "- " + p + (pic.caption.empty() ? "" : " | " + oneLine(pic.caption)) + "\n";
                if (!g.empty()) t += "### Gallery\n\n" + g + "\n";
            }
            if (!e->pins.empty()) {
                std::string g;
                for (const auto& pin : e->pins)
                    if (const std::string r = ref(pin.entry); !r.empty()) g += "- " + r + " @ " + number(pin.x) + ", " + number(pin.y) + "\n";
                if (!g.empty()) t += "### Map pins\n\n" + g + "\n";
            }
            if (view) {
                std::string g;
                for (const Mention& m : lib.mentionsOf(*e)) {
                    const Book* b = lib.book(m.bookId);
                    const ManuscriptNode* n = lib.node(m.nodeId);
                    if (b && n) g += "- " + oneLine(b->title) + ", " + oneLine(n->title) + " (" + std::to_string(m.count) + ")\n";
                }
                if (!g.empty()) t += "### Appears in (generated: not imported)\n\n" + g + "\n";
                g.clear();
                for (const auto& [title, age] : bible::agesOfEntry(lib, *view, *e)) g += "- " + (title.empty() ? age : oneLine(title) + ": " + age) + "\n";
                if (!g.empty()) t += "### Ages (generated: not imported)\n\n" + g + "\n";
                g.clear();
                for (const bible::Excerpt& x : bible::excerptsOf(lib, *view, *e, texts)) g += "- " + oneLine(x.bookTitle + ", " + x.where + ": " + x.text) + "\n";
                if (!g.empty()) t += "### From the books (generated: not imported)\n\n" + g + "\n";
            }
        }
    }
    (void)books;
    return out;
}

std::vector<Batch> exportPages(const std::string& text) {
    std::vector<Batch> out;
    std::string kindHeading;
    Batch* cur = nullptr;
    std::vector<std::string> all = linesOf(text);
    if (!all.empty() && all.back().empty()) all.pop_back();
    for (const std::string& raw : all) {
        std::string l = raw;
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (l.rfind("# ", 0) == 0) { cur = nullptr; kindHeading = kindNamed(l.substr(2)); continue; }
        if (l.rfind("## ", 0) == 0) {
            out.push_back(Batch{kindHeading.empty() ? "character" : kindHeading, l.substr(3), l + "\n", true});
            cur = &out.back();
            continue;
        }
        if (cur) cur->text += l + "\n";
    }
    for (Batch& b : out) {
        const Exact p = readPage(b.text);
        if (!p.kind.empty()) {
            const auto& kinds = entryKinds();
            if (std::find(kinds.begin(), kinds.end(), p.kind) != kinds.end()) b.kind = p.kind;
        }
    }
    return out;
}

bool importExact(Library& lib, const Id& world, const Id& book, const std::vector<Batch>& pages, const std::vector<Id>& booksInOrder,
                 Import& out, const std::string& baseFolder) {
    const Id scopeBook = world.empty() ? book : Id{};
    std::vector<Exact> parsed;
    for (const Batch& b : pages) parsed.push_back(readPage(b.text));
    // An entry by its exact name: of that kind, else any kind.
    auto find = [&](const std::string& name, const std::string& kind) -> Id {
        Id any;
        for (const Entry* e : lib.entriesOf(world, scopeBook)) {
            if (e->name != name) continue;
            if (e->kind == kind) return e->id;
            if (any.empty()) any = e->id;
        }
        return kind.empty() ? any : Id{};
    };
    std::set<Id> seen;
    auto note = [&](const Id& id) {
        if (std::find(out.made.begin(), out.made.end(), id) != out.made.end() || !seen.insert(id).second) return;
        if (const Entry* e = lib.entry(id)) {
            out.updated.push_back(id);
            out.before.push_back(*e);
            out.linksBefore.push_back(lib.linksOf(id));
        }
    };
    // Every page's entry first, so pages naming each other link up.
    std::vector<Id> ids;
    for (size_t i = 0; i < pages.size(); ++i) {
        Id id = find(pages[i].name, pages[i].kind);
        if (id.empty()) {
            id = lib.createEntry(world, scopeBook, pages[i].kind, pages[i].name);
            if (id.empty()) return false;
            out.made.push_back(id);
        }
        ids.push_back(id);
    }
    auto entryFor = [&](const std::string& name, const std::string& kind) -> Id {
        Id id = find(name, kind);
        if (id.empty() && !kind.empty()) id = find(name, {});
        if (id.empty()) {
            id = lib.createEntry(world, scopeBook, kind.empty() ? "character" : kind, name);
            if (!id.empty()) out.extra.push_back(id);
        }
        return id;
    };
    auto picture = [&](const std::string& path) -> std::string {
        if (path.empty()) return {};
        std::filesystem::path p = detail::u8path(path);
        if (p.is_relative() && !baseFolder.empty()) p = detail::u8path(baseFolder) / p;
        std::ifstream f(p, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string bytes = ss.str();
        if (!f || !looksLikePicture(bytes)) { out.missingPictures.push_back(path); return {}; }
        const std::string blob = lib.storeBlob(bytes);
        if (!blob.empty()) ++out.pictures;
        return blob;
    };
    for (size_t i = 0; i < pages.size(); ++i) {
        const Exact& p = parsed[i];
        const Entry* had = lib.entry(ids[i]);
        if (!had) continue;
        note(ids[i]);
        Entry e = *had;
        for (const std::string& u : p.unread) out.unread.push_back(e.name + ": " + u);
        e.aliases = p.aliases;
        e.pronunciation = p.pronunciation;
        // A picture symbol stays when the file names no symbol (an export without its pictures), as Picture does.
        if (!(p.symbol.empty() && !p.hasSymbolPicture && e.symbol.rfind("blob:", 0) == 0)) e.symbol = p.symbol;
        if (p.hasSymbolPicture) {
            const std::string b = picture(p.symbolPicture);
            e.symbol = b.empty() ? std::string() : "blob:" + b;
        }
        e.colour = p.colour.size() == 7 && p.colour[0] == '#' ? p.colour : std::string();
        e.iconStyle = p.iconStyle;
        e.summary = p.summary;
        e.details = p.details;
        e.notes = p.notes;
        e.sections = p.sections;
        e.bookNotes.clear();
        for (const auto& [title, text] : p.bookNotes) {
            Id b = bookByTitle(lib, booksInOrder, title);
            if (b.empty()) b = std::string(kLaterBook) + title;   // kept until that book is here
            e.bookNotes.emplace_back(b, text);
        }
        e.revealed.clear();
        for (const auto& [k, title] : p.revealed) {
            const Id b = bookByTitle(lib, booksInOrder, title);
            if (b.empty()) out.unread.push_back(e.name + ": spoiler for a book that isn't here, " + title);
            else e.revealed.emplace_back(k, b);
        }
        if (p.hasPicture) e.imageBlob = picture(p.picture);
        if (p.hasHeader) e.headerBlob = picture(p.headerPicture);
        if (p.hasMap) e.mapBlob = picture(p.map);
        if (p.hasGallery) {
            std::vector<Entry::Picture> g;
            for (const auto& [path, caption] : p.gallery)
                if (const std::string b = picture(path); !b.empty()) g.push_back({b, caption});
            e.gallery = g;
        }
        if (p.hasPins) {
            e.pins.clear();
            for (const auto& pin : p.pins)
                if (const Id to = entryFor(pin.name, pin.kind); !to.empty()) e.pins.push_back({to, pin.x, pin.y});
        }
        if (!lib.updateEntry(e)) return false;
        // The relationships this entry starts: exactly the page's.
        std::vector<std::tuple<Id, std::string>> want;
        for (const auto& k : p.links)
            if (const Id to = entryFor(k.name, k.kind); !to.empty()) want.emplace_back(to, k.label);
        for (const EntryLink& l : lib.linksOf(e.id)) {
            if (l.from != e.id) continue;
            if (std::find(want.begin(), want.end(), std::make_tuple(l.to, l.label)) == want.end()) {
                note(l.to);
                lib.removeLink(l.from, l.to, l.label);
            }
        }
        const auto now = lib.linksOf(e.id);
        for (const auto& [to, label] : want) {
            const bool there = std::any_of(now.begin(), now.end(), [&](const EntryLink& l) { return l.from == e.id && l.to == to && l.label == label; });
            if (!there) {
                note(to);
                if (lib.addLink(e.id, to, label)) ++out.links;
            }
        }
    }
    // Pages that change nothing.
    for (size_t i = 0; i < out.updated.size(); ++i) {
        const Entry* now = lib.entry(out.updated[i]);
        const Entry& was = out.before[i];
        if (now && now->aliases == was.aliases && now->pronunciation == was.pronunciation && now->symbol == was.symbol && now->colour == was.colour &&
            now->iconStyle == was.iconStyle &&
            now->summary == was.summary && now->details == was.details && now->notes == was.notes && now->sections == was.sections &&
            now->bookNotes == was.bookNotes && now->revealed == was.revealed && now->imageBlob == was.imageBlob &&
            lib.linksOf(out.updated[i]).size() == out.linksBefore[i].size())
            ++out.unchanged;
    }
    return true;
}

}  // namespace studio::paste
