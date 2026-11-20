// Sharing a world bible as a website or a printable book (see studio/bible_export.hpp).
#include "studio/bible_export.hpp"
#include "studio/fonts.hpp"
#include "studio/family_tree.hpp"

#include "studio/document.hpp"
#include "internal.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

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

bool writeFile(const std::string& path, const std::string& data) {
    std::ofstream f(u8path(path), std::ios::binary | std::ios::trunc);
    f.write(data.data(), (std::streamsize)data.size());
    return (bool)f;
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
    static const std::set<std::string> family{"Parent of", "Child of", "Partner of", "Married to", "Sibling of", "Twin of",
                                              "Half-sibling of", "Step-sibling of", "Adoptive sibling of", "Foster sibling of",
                                              "Adoptive parent of", "Adopted child of", "Step-parent of", "Stepchild of",
                                              "Foster parent of", "Foster child of"};
    for (const EntryLink& l : lib.linksOf(e.id)) {
        if (family.count(l.label)) continue;   // in the family list
        const bool mine = l.from == e.id;
        const Entry* o = lib.entry(mine ? l.to : l.from);
        if (!o || !v.shows(o->id)) continue;
        out.push_back({mine ? l.label : lib.relationReverse(v.options().worldId, v.options().bookId, l.label), o});
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

// For a JavaScript string inside a <script> file.
std::string jsStr(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else if (c == '\r') continue;
        else if (c == '<') o += "\\u003c";
        else if ((unsigned char)c < 0x20) continue;
        else o += c;
    }
    return o + "\"";
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

// Paragraphs, with lines starting "• " or "- " as a list.
std::string prose(const std::string& text, const std::vector<const Entry*>& shown, const Id& self) {
    std::string out, list;
    auto flush = [&] { if (!list.empty()) { out += "<ul>" + list + "</ul>"; list.clear(); } };
    std::istringstream in(text);
    std::string line, para;
    auto endPara = [&] { if (!para.empty()) { flush(); out += "<p>" + linked(para, shown, self) + "</p>"; para.clear(); } };
    while (std::getline(in, line)) {
        while (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = line;
        while (!t.empty() && t.front() == ' ') t.erase(0, 1);
        if (t.empty()) { endPara(); flush(); continue; }
        if (t.rfind("\xE2\x80\xA2 ", 0) == 0 || t.rfind("- ", 0) == 0 || t.rfind("* ", 0) == 0) {
            endPara();
            list += "<li>" + linked(t.substr(t[0] == '-' || t[0] == '*' ? 2 : 4), shown, self) + "</li>";
            continue;
        }
        flush();
        para += (para.empty() ? "" : " ") + t;
    }
    endPara();
    flush();
    return out;
}

std::string initials(const std::string& name) {
    std::string out;
    std::istringstream in(name);
    for (std::string w; in >> w && out.size() < 2;) {
        if (w == "the" || w == "The" || w == "of" || w == "and") continue;
        const unsigned char c = (unsigned char)w[0];
        size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        out += w.substr(0, n);
        if (out.size() >= 2 && c < 0x80) break;
    }
    return out;
}

struct Palette { const char *paper, *card, *ink, *muted, *faint, *rule, *accent, *head; };
struct ThemeDef { const char* id; Palette light, dark; const char* headings; const char* body; bool caps; };
const std::vector<ThemeDef>& themeDefs() {
    static const std::vector<ThemeDef> t = {
        {"parchment", {"#f4ecdc", "#fffaf0", "#2b2118", "#6a5a48", "#9a8a74", "#e0d2b8", "#8a3b2b", "#ece0c6"},
         {"#1c1712", "#251f18", "#efe6d6", "#bfae94", "#8c7c66", "#3a3026", "#d99a6c", "#211b14"}, "crimson", "book", false},
        {"clean", {"#fafbfc", "#ffffff", "#1d2330", "#5d6577", "#8d93a1", "#e3e6ec", "#2d4a6e", "#ffffff"},
         {"#12151c", "#1a1e27", "#e9ebf0", "#aab0bd", "#7c8290", "#2c3240", "#8fb3e0", "#161a22"}, "modern", "georgia", false},
        {"night", {"#0b1120", "#121a2e", "#e8ecf6", "#aab4cc", "#77819a", "#24304a", "#59d6a8", "#0e1628"},
         {"#0b1120", "#121a2e", "#e8ecf6", "#aab4cc", "#77819a", "#24304a", "#59d6a8", "#0e1628"}, "crimson", "modern", false},
        {"grimoire", {"#f1e8d6", "#fbf5e8", "#2a1d12", "#6c5539", "#9b8466", "#dccaa6", "#9a6b17", "#e8dabd"},
         {"#17110d", "#211913", "#efe2c6", "#c2ad89", "#8f7c5e", "#3b2c1f", "#d4a64a", "#1d150f"}, "garamond", "gentium", true},
        {"atlas", {"#eef2f4", "#ffffff", "#1b2a35", "#53636f", "#87949e", "#d5dde3", "#2f7d6d", "#e4ebef"},
         {"#101a20", "#16232b", "#e3ecf1", "#a3b3bd", "#748591", "#26353f", "#5cc2ab", "#13212a"}, "humanist", "georgia", false},
    };
    return t;
}
const ThemeDef& themeDef(const std::string& id) {
    for (auto& t : themeDefs()) if (id == t.id) return t;
    return themeDefs()[0];
}
struct FontDef { const char* id; const char* stack; const char* bundled; };
const std::vector<FontDef>& fontDefs() {
    static const std::vector<FontDef> f = {
        {"crimson", "\"Crimson Text\",Georgia,serif", "crimson-text"},
        {"gentium", "\"Gentium Book Plus\",\"Palatino Linotype\",Georgia,serif", "gentium-book-plus"},
        {"book", "\"Palatino Linotype\",\"Book Antiqua\",Palatino,\"Iowan Old Style\",Georgia,serif", ""},
        {"georgia", "Georgia,\"Times New Roman\",serif", ""},
        {"garamond", "\"EB Garamond\",Garamond,\"Adobe Garamond Pro\",Georgia,serif", "eb-garamond"},
        {"baskerville", "\"Libre Baskerville\",Baskerville,Georgia,serif", "libre-baskerville"},
        {"lora", "Lora,Georgia,serif", "lora"},
        {"alegreya", "Alegreya,Georgia,serif", "alegreya"},
        {"spectral", "Spectral,Georgia,serif", "spectral"},
        {"fell", "\"IM Fell English\",Georgia,serif", "im-fell-english"},
        {"modern", "\"Segoe UI\",system-ui,-apple-system,\"Helvetica Neue\",Arial,sans-serif", ""},
        {"sourcesans", "\"Source Sans 3\",\"Segoe UI\",system-ui,sans-serif", "source-sans-3"},
        {"humanist", "\"Gill Sans\",\"Gill Sans MT\",Calibri,\"Trebuchet MS\",sans-serif", ""},
        {"cinzel", "Cinzel,\"Trajan Pro\",Georgia,serif", "cinzel"},
        {"uncial", "\"Uncial Antiqua\",Georgia,serif", "uncial-antiqua"},
        {"medieval", "MedievalSharp,Georgia,serif", "medievalsharp"},
        {"caveat", "Caveat,\"Segoe Print\",cursive", "caveat"},
        {"dancing", "\"Dancing Script\",\"Segoe Script\",cursive", "dancing-script"},
        {"typewriter", "\"Courier Prime\",\"Courier New\",monospace", "courier-prime"},
        {"elite", "\"Special Elite\",\"Courier Prime\",monospace", "special-elite"},
    };
    return f;
}
// A world's own typeface ("family:<id>", 0.40): its @font-face family name, and the stack that uses it.
bool isWorldFamily(const std::string& id) { return id.rfind("family:", 0) == 0 && id.size() > 7; }
std::string worldFamilyName(const std::string& id) { return "NR " + id.substr(7); }
const FontDef& fontDef(const std::string& id) {
    for (auto& f : fontDefs()) if (id == f.id) return f;
    return fontDefs()[0];
}
std::string fontStack(const std::string& id) {
    if (isWorldFamily(id)) return "\"" + worldFamilyName(id) + "\",Georgia,serif";
    return fontDef(id).stack;
}

const char* kindHex(const std::string& kind, bool dark) {
    struct K { const char* k; const char* light; const char* dark; };
    static const K ks[] = {{"character", "#3E7BD6", "#7AA9F0"}, {"creature", "#C26A2E", "#E69A5F"}, {"place", "#2E9A6C", "#5BC99A"},
                           {"race", "#6B8E23", "#A5C76A"},      {"realm", "#5B5FC7", "#9A9DF0"},    {"group", "#7D5BD0", "#AE93F2"},
                           {"thing", "#B08A1E", "#E0BE55"},     {"lore", "#1E97A6", "#55C6D4"},     {"magic", "#B3439E", "#E37FD0"},
                           {"event", "#C9476A", "#F07C98"}};
    for (auto& x : ks) if (kind == x.k) return dark ? x.dark : x.light;
    return dark ? "#7AA9F0" : "#3E7BD6";
}

bool validHex(const std::string& h) {
    return h.size() == 7 && h[0] == '#' && h.find_first_not_of("0123456789abcdefABCDEF", 1) == std::string::npos;
}

const char* kSiteJs = R"JS((function(){
var q=document.getElementById('q'),box=document.getElementById('qr');
if(q&&box&&window.NR_BIBLE){
 var data=window.NR_BIBLE,sel=-1,hits=[];
 function norm(s){return (s||'').toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g,'').replace(/[‘’]/g,"'");}
 function show(){var t=norm(q.value.trim());box.innerHTML='';hits=[];sel=-1;
  if(!t){box.hidden=true;return;}
  data.forEach(function(e){var n=norm(e.n),score=n.indexOf(t)===0?3:n.indexOf(t)>=0?2:0;
   if(!score)e.a.forEach(function(a){if(norm(a).indexOf(t)>=0)score=Math.max(score,1);});
   if(!score&&t.length>2&&norm(e.s).indexOf(t)>=0)score=0.5;
   if(score)hits.push([score,e]);});
  hits.sort(function(a,b){return b[0]-a[0]||a[1].n.localeCompare(b[1].n);});hits=hits.slice(0,12);
  if(!hits.length){box.innerHTML='<div class="none">Nothing called that.</div>';box.hidden=false;return;}
  hits.forEach(function(h,i){var e=h[1],a=document.createElement('a');a.href=e.u;a.className='k-'+e.c;
   a.innerHTML='<i></i><b></b><span></span>';a.querySelector('b').textContent=e.n;a.querySelector('span').textContent=e.k;box.appendChild(a);});
  box.hidden=false;}
 q.addEventListener('input',show);
 q.addEventListener('keydown',function(ev){var links=box.querySelectorAll('a');
  if(ev.key==='ArrowDown'||ev.key==='ArrowUp'){if(!links.length)return;ev.preventDefault();sel=(sel+(ev.key==='ArrowDown'?1:-1)+links.length)%links.length;
   links.forEach(function(l,i){l.classList.toggle('on',i===sel);});}
  else if(ev.key==='Enter'){var l=links[sel<0?0:sel];if(l)location.href=l.href;}
  else if(ev.key==='Escape'){q.value='';show();}});
 document.addEventListener('click',function(ev){if(!box.contains(ev.target)&&ev.target!==q)box.hidden=true;});
 document.addEventListener('keydown',function(ev){if(ev.key==='/'&&document.activeElement!==q&&!/input|textarea/i.test(document.activeElement.tagName)){ev.preventDefault();q.focus();}});
}
document.querySelectorAll('input.filter').forEach(function(f){var t=document.getElementById(f.dataset.for);if(!t)return;
 f.addEventListener('input',function(){var s=f.value.toLowerCase();t.querySelectorAll('tbody tr').forEach(function(r){r.hidden=s&&r.textContent.toLowerCase().indexOf(s)<0;});});});
})();
)JS";

}  // namespace

const std::vector<Theme>& themes() {
    static const std::vector<Theme> t = {
        {"parchment", "Parchment", "Warm paper and a deep red, serif type: an old book."},
        {"clean", "Clean", "White and blue-grey with modern headings: a reference site."},
        {"night", "Night sky", "Dark blue with stars and aurora green, dark however it's read."},
        {"grimoire", "Grimoire", "Leather and gold with spaced capitals: a book of lore."},
        {"atlas", "Atlas", "Cool grey and sea green with clear headings: a travel guide."},
    };
    return t;
}

ThemeColours themeColours(const std::string& theme, bool dark) {
    const ThemeDef& t = themeDef(theme);
    const bool night = std::string(t.id) == "night";
    const Palette& p = dark || night ? t.dark : t.light;
    return {p.paper, p.card, p.ink, p.muted, p.rule, p.accent, p.head, t.caps, night};
}

const std::vector<FontChoice>& fonts() {
    static const std::vector<FontChoice> f = {
        {"crimson", "Crimson Text (a book serif, included in the site)", "crimson-text"},
        {"gentium", "Gentium Book Plus (wide-ranging accents, included)", "gentium-book-plus"},
        {"garamond", "EB Garamond (classic book serif, included)", "eb-garamond"},
        {"baskerville", "Libre Baskerville (sturdy serif, included)", "libre-baskerville"},
        {"lora", "Lora (warm serif, included)", "lora"},
        {"alegreya", "Alegreya (calligraphic serif, included)", "alegreya"},
        {"spectral", "Spectral (elegant screen serif, included)", "spectral"},
        {"fell", "IM Fell English (old printed book, included)", "im-fell-english"},
        {"book", "Palatino (book)", ""},
        {"georgia", "Georgia (screen serif)", ""},
        {"modern", "Segoe UI (modern sans)", ""},
        {"sourcesans", "Source Sans 3 (clean sans, included)", "source-sans-3"},
        {"humanist", "Gill Sans (humanist sans)", ""},
        {"cinzel", "Cinzel (carved capitals for headings, included)", "cinzel"},
        {"uncial", "Uncial Antiqua (Celtic, for headings, included)", "uncial-antiqua"},
        {"medieval", "MedievalSharp (medieval hand, for headings, included)", "medievalsharp"},
        {"caveat", "Caveat (handwriting, included)", "caveat"},
        {"dancing", "Dancing Script (flowing script, included)", "dancing-script"},
        {"typewriter", "Courier Prime (typewriter, included)", "courier-prime"},
        {"elite", "Special Elite (worn typewriter, included)", "special-elite"},
    };
    return f;
}

std::string toJson(const Look& l) {
    nlohmann::json j{{"theme", l.theme}, {"mode", l.mode}, {"accent", l.accent}, {"headings", l.headings}, {"body", l.body},
                     {"title", l.title},  {"tagline", l.tagline}, {"cards", l.cards}, {"search", l.search}, {"kindColours", l.kindColours}};
    return j.dump();
}

Look lookFromJson(const std::string& text) {
    Look l;
    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object()) return l;
    auto str = [&](const char* k, std::string& v) { if (j.contains(k) && j[k].is_string()) v = j[k].get<std::string>(); };
    auto flag = [&](const char* k, bool& v) { if (j.contains(k) && j[k].is_boolean()) v = j[k].get<bool>(); };
    str("theme", l.theme);
    str("mode", l.mode);
    str("accent", l.accent);
    str("headings", l.headings);
    str("body", l.body);
    str("title", l.title);
    str("tagline", l.tagline);
    flag("cards", l.cards);
    flag("search", l.search);
    flag("kindColours", l.kindColours);
    if (std::none_of(themes().begin(), themes().end(), [&](const Theme& t) { return l.theme == t.id; })) l.theme = "parchment";
    if (l.mode != "light" && l.mode != "dark") l.mode = "auto";
    if (!l.accent.empty() && !validHex(l.accent)) l.accent.clear();
    if (l.title.size() > 200) l.title.resize(200);
    if (l.tagline.size() > 400) l.tagline.resize(400);
    return l;
}

std::vector<conlang::Language> languagesOf(const Library& lib, const Id& worldId, const Id& bookId) {
    const std::string key = worldId.empty() ? "conlangs:book:" + bookId : "conlangs:world:" + worldId;
    return conlang::fromJson(lib.setting(key));
}

std::string websiteCss(const Look& look, const std::string& worldAccent, bool bundledHeadings, bool bundledBody) {
    const ThemeDef& t = themeDef(look.theme);
    const std::string hid = look.headings == "theme" || look.headings.empty() ? std::string(t.headings) : look.headings;
    const std::string bid = look.body == "theme" || look.body.empty() ? std::string(t.body) : look.body;
    struct Stack { std::string stack; };
    const Stack hf{fontStack(hid)}, bf{fontStack(bid)};
    (void)worldAccent;   // offered in the app as one of the choices; the theme's own colour otherwise
    const std::string accent = validHex(look.accent) ? look.accent : "";
    auto vars = [&](const Palette& p, bool dark) {
        std::string a = accent.empty() ? p.accent : accent;
        std::string v = "--paper:" + std::string(p.paper) + ";--card:" + p.card + ";--ink:" + p.ink + ";--muted:" + p.muted + ";--faint:" + p.faint +
                        ";--rule:" + p.rule + ";--accent:" + a + ";--head:" + p.head + ";";
        // A world's own colour can be too dark to read on a dark page: lifted towards the ink there.
        if (dark && !accent.empty()) v += "--accent:color-mix(in srgb," + a + " 62%,#ffffff);";
        for (const char* k : {"character", "creature", "place", "race", "realm", "group", "thing", "lore", "magic", "event"})
            v += std::string("--k-") + k + ":" + (look.kindColours ? kindHex(k, dark) : "var(--accent)") + ";";
        return v;
    };
    const bool night = std::string(t.id) == "night";
    std::string css;
    css += ":root{" + vars(look.mode == "dark" || night ? t.dark : t.light, look.mode == "dark" || night) +
           "--hf:" + hf.stack + ";--bf:" + bf.stack + ";color-scheme:" + (look.mode == "dark" || night ? "dark" : look.mode == "light" ? "light" : "light dark") + "}\n";
    if (look.mode == "auto" && !night) css += "@media (prefers-color-scheme:dark){:root{" + vars(t.dark, true) + "}}\n";
    (void)bundledHeadings;
    (void)bundledBody;
    css += R"CSS(*{box-sizing:border-box}
html{-webkit-text-size-adjust:100%}
body{margin:0;background:var(--paper);color:var(--ink);font:18px/1.65 var(--bf);text-rendering:optimizeLegibility}
a{color:var(--accent);text-decoration-thickness:1px;text-underline-offset:2px}
a:hover{text-decoration-thickness:2px}
img{max-width:100%}
.top{position:sticky;top:0;z-index:20;background:color-mix(in srgb,var(--head) 92%,transparent);backdrop-filter:blur(8px);border-bottom:1px solid var(--rule);
 display:flex;align-items:center;gap:20px;padding:10px 24px;font:15px/1.3 var(--bf);flex-wrap:wrap}
.brand{font:600 20px/1.2 var(--hf);color:var(--ink);text-decoration:none;margin-right:6px}
.top nav{display:flex;gap:4px 16px;flex-wrap:wrap;flex:1}
.top nav a{color:var(--muted);text-decoration:none;font-size:14px;white-space:nowrap}
.top nav a:hover,.top nav a.on{color:var(--accent)}
.search{position:relative}
#q{width:220px;max-width:60vw;font:15px var(--bf);padding:7px 12px 7px 32px;border:1px solid var(--rule);border-radius:20px;background:var(--card);color:var(--ink);
 background-image:radial-gradient(circle at 15px 15px,transparent 5px,var(--faint) 5.5px,var(--faint) 6.5px,transparent 7px);outline:none}
#q:focus{border-color:var(--accent);box-shadow:0 0 0 3px color-mix(in srgb,var(--accent) 22%,transparent)}
#qr{position:absolute;right:0;top:42px;width:340px;max-width:90vw;background:var(--card);border:1px solid var(--rule);border-radius:12px;box-shadow:0 12px 40px #0003;padding:6px;max-height:70vh;overflow:auto}
#qr a{display:flex;align-items:baseline;gap:10px;padding:7px 10px;border-radius:8px;text-decoration:none;color:var(--ink)}
#qr a i{width:8px;height:8px;border-radius:50%;background:var(--k);flex:none;align-self:center}
#qr a span{margin-left:auto;font-size:12px;color:var(--faint)}
#qr a:hover,#qr a.on{background:color-mix(in srgb,var(--accent) 12%,transparent)}
#qr .none{padding:10px;color:var(--faint);font-size:14px}
main{max-width:1120px;margin:0 auto;padding:28px 24px 90px}
h1,h2,h3{font-family:var(--hf);line-height:1.15;font-weight:600}
h1{font-size:clamp(2.1em,5vw,3.1em);margin:.1em 0 .15em;letter-spacing:-.01em}
.crumbs{font-size:14px;color:var(--faint);margin-bottom:14px}.crumbs a{color:var(--muted);text-decoration:none}.crumbs a:hover{color:var(--accent)}
.kind{font:700 12px/1.4 var(--bf);letter-spacing:.12em;text-transform:uppercase;color:var(--k,var(--accent))}
.said{color:var(--muted);font-style:italic}.also{color:var(--muted);font-size:.92em;margin-top:4px}
.guide{background:var(--card);border:1px solid var(--rule);border-left:4px solid var(--accent);border-radius:10px;padding:12px 16px;font-size:15px;color:var(--muted);margin:0 0 24px}
/* home */
.hero{position:relative;border-radius:18px;overflow:hidden;min-height:300px;display:flex;align-items:flex-end;margin-bottom:30px;
 background:radial-gradient(120% 140% at 10% 0%,color-mix(in srgb,var(--accent) 70%,#000) 0%,color-mix(in srgb,var(--accent) 30%,#000) 60%,#000 120%);background-size:cover;background-position:center}
.hero::after{content:"";position:absolute;inset:0;background:linear-gradient(180deg,transparent 30%,#000a 100%)}
.hero .in{position:relative;z-index:1;padding:36px 40px;color:#fff;max-width:820px}
.hero .eyebrow{font:700 12px var(--bf);letter-spacing:.18em;text-transform:uppercase;opacity:.8}
.hero h1{color:#fff;margin:.1em 0;font-size:clamp(2.4em,7vw,4em);text-shadow:0 2px 20px #0006}
.hero p{margin:0;font-size:1.15em;opacity:.92;font-style:italic}
.intro{max-width:760px;font-size:1.05em;margin:0 0 26px}
.chips{display:flex;flex-wrap:wrap;gap:8px;margin:0 0 34px}
.chips a{display:inline-flex;align-items:center;gap:8px;padding:6px 14px;border-radius:20px;background:var(--card);border:1px solid var(--rule);color:var(--ink);text-decoration:none;font-size:15px}
.chips a i{width:9px;height:9px;border-radius:50%;background:var(--k,var(--accent))}.chips a small{color:var(--faint)}
.chips a:hover{border-color:var(--k,var(--accent))}
.kindhead{display:flex;align-items:baseline;gap:12px;font-size:1.7em;margin:46px 0 16px;padding-bottom:8px;border-bottom:2px solid var(--k,var(--accent))}
.kindhead small{font:14px var(--bf);color:var(--faint)}
.cards{display:grid;grid-template-columns:repeat(auto-fill,minmax(250px,1fr));gap:16px}
.card{display:flex;gap:14px;background:var(--card);border:1px solid var(--rule);border-radius:14px;padding:12px;text-decoration:none;color:var(--ink);
 transition:transform .15s,box-shadow .15s,border-color .15s;border-top:3px solid var(--k,var(--accent))}
.card:hover{transform:translateY(-2px);box-shadow:0 10px 26px #0002;border-color:var(--k,var(--accent))}
.card .pic{flex:none;width:76px;height:96px;border-radius:9px;background:color-mix(in srgb,var(--k,var(--accent)) 16%,var(--card)) center/cover no-repeat;
 display:flex;align-items:center;justify-content:center;font:600 28px var(--hf);color:var(--k,var(--accent))}
.card b{display:block;font:600 1.12em/1.25 var(--hf);margin:2px 0 4px}
.card p{margin:0;font-size:14px;line-height:1.45;color:var(--muted);display:-webkit-box;-webkit-line-clamp:3;-webkit-box-orient:vertical;overflow:hidden}
ul.list{list-style:none;padding:0;columns:3 220px;margin:0}ul.list li{padding:3px 0;break-inside:avoid}
/* entry */
.layout{display:grid;grid-template-columns:minmax(0,1fr) 330px;gap:44px;align-items:start}
.main>p.lead{font-size:1.16em;color:var(--ink)}
.main h2{font-size:1.45em;margin:1.7em 0 .5em;padding-bottom:6px;border-bottom:1px solid var(--rule)}
.main ul{padding-left:1.2em}
.infobox{background:var(--card);border:1px solid var(--rule);border-radius:16px;overflow:hidden;position:sticky;top:76px;font-size:15px;border-top:4px solid var(--k,var(--accent))}
.infobox img.portrait{display:block;width:100%;max-height:420px;object-fit:cover}
.infobox .box{padding:14px 16px}
.infobox h3{font-size:1.25em;margin:0 0 8px}
.infobox h4{font:700 11px var(--bf);letter-spacing:.12em;text-transform:uppercase;color:var(--faint);margin:16px 0 4px}
table{border-collapse:collapse;width:100%}td{padding:6px 0;border-bottom:1px solid var(--rule);vertical-align:top}
td:first-child{color:var(--muted);width:40%;padding-right:10px}
tr:last-child td{border-bottom:0}
.gallery{display:grid;grid-template-columns:repeat(auto-fill,minmax(180px,1fr));gap:14px}
.gallery figure{margin:0}.gallery img{width:100%;height:200px;object-fit:cover;border-radius:10px;display:block}
figcaption{font-size:13px;color:var(--muted);margin-top:4px}
.map{position:relative;display:inline-block;max-width:100%}.map img{max-width:100%;display:block;border-radius:12px}
.pin{position:absolute;transform:translate(-8px,-8px);display:flex;align-items:center;gap:6px;font-size:13px;text-decoration:none;color:var(--ink)}
.pin i{width:16px;height:16px;border-radius:50%;background:var(--accent);border:2px solid #fff;box-shadow:0 1px 4px #0006;display:block}
.pin span{background:#fffe;color:#1d2330;border-radius:10px;padding:1px 8px;box-shadow:0 1px 4px #0003}
.pager{display:flex;justify-content:space-between;gap:16px;margin-top:56px;padding-top:18px;border-top:1px solid var(--rule)}
.pager a{text-decoration:none;max-width:48%}.pager small{display:block;color:var(--faint);font-size:12px;letter-spacing:.08em;text-transform:uppercase}
.pager a.next{text-align:right;margin-left:auto}
/* glossary and languages */
.letters{display:flex;flex-wrap:wrap;gap:4px;margin:0 0 20px;position:sticky;top:62px;background:var(--paper);padding:8px 0;z-index:5}
.letters a{width:32px;height:32px;display:flex;align-items:center;justify-content:center;border-radius:8px;text-decoration:none;font:600 15px var(--hf);background:var(--card);border:1px solid var(--rule)}
.letter{font:600 1.9em var(--hf);color:var(--accent);border-bottom:1px solid var(--rule);margin:1.2em 0 .4em;scroll-margin-top:120px}
dl dt{font-weight:600;margin-top:.9em}dl dd{margin:0;color:var(--muted)}
.lang{margin:0 0 50px}.lang .meta{color:var(--faint);font-size:14px}
.lang table{font-size:15px;background:var(--card);border:1px solid var(--rule);border-radius:12px;overflow:hidden}
.lang th{text-align:left;font:700 11px var(--bf);letter-spacing:.1em;text-transform:uppercase;color:var(--faint);padding:10px 12px;border-bottom:1px solid var(--rule)}
.lang td{padding:7px 12px;width:auto}.lang td:first-child{color:var(--ink);font-weight:600;width:auto}
.lang .ipa{color:var(--muted);font-style:italic}
input.filter{font:15px var(--bf);padding:7px 12px;border:1px solid var(--rule);border-radius:8px;background:var(--card);color:var(--ink);margin:6px 0 12px;width:280px;max-width:100%}
footer{max-width:1120px;margin:0 auto;padding:0 24px 40px;color:var(--faint);font-size:13px}
.ehead{margin-bottom:18px}
.kin{color:var(--faint);font-size:.85em;white-space:nowrap}
.treelink{display:inline-block;margin-top:10px;font-size:14px;color:var(--accent);text-decoration:none}
.treelink:hover{text-decoration:underline}
.kindhead a{color:inherit;text-decoration:none}.kindhead a:hover{color:var(--accent)}
.more{display:inline-block;margin-top:14px;font-size:15px;color:var(--accent);text-decoration:none;font-weight:600}
.more:hover{text-decoration:underline}
.kindtitle small,.subhead small{font:14px var(--bf);color:var(--faint)}
.subhead{font-size:1.3em;margin:34px 0 14px}
.treewrap{overflow-x:auto;background:#fff;border-radius:12px;border:1px solid var(--rule);padding:6px;scrollbar-width:thin}
.treewrap img{display:block;max-width:none;height:auto}
.treenote{font-size:13px;color:var(--faint);margin-top:6px}
section.tree{margin:28px 0 40px;scroll-margin-top:80px}
@media (max-width:860px){.layout{grid-template-columns:1fr}.infobox{position:static;order:-1}.top{padding:10px 16px;flex-wrap:wrap}.top nav{order:3;flex-basis:100%;flex-wrap:nowrap;overflow-x:auto;scrollbar-width:none;padding-bottom:2px;-webkit-mask-image:linear-gradient(90deg,#000 88%,transparent)}.top nav::-webkit-scrollbar{display:none}main{padding:20px 16px 70px}.hero .in{padding:26px 22px}#q{width:160px}}
@media print{.top,.search,.pager,.letters{display:none}body{background:#fff;color:#000}.card{break-inside:avoid}}
)CSS";
    if (night)
        css += "body{background:radial-gradient(1px 1px at 20% 30%,#fff8,transparent),radial-gradient(1px 1px at 70% 12%,#fff7,transparent),"
               "radial-gradient(1.5px 1.5px at 40% 80%,#fff6,transparent),radial-gradient(1px 1px at 85% 60%,#fff6,transparent),"
               "radial-gradient(1px 1px at 10% 75%,#fff5,transparent),var(--paper);background-size:420px 420px,380px 380px,520px 520px,300px 300px,460px 460px,auto;"
               "background-attachment:fixed}\n.hero{box-shadow:inset 0 -60px 80px -40px color-mix(in srgb,var(--accent) 50%,transparent)}\n";
    if (t.caps) css += "h1,h2,.brand,.kindhead{text-transform:uppercase;letter-spacing:.08em}.hero h1{letter-spacing:.06em}\n";
    for (const char* k : {"character", "creature", "place", "race", "realm", "group", "thing", "lore", "magic", "event"})
        css += std::string(".k-") + k + "{--k:var(--k-" + k + ")}";
    css += "\n";
    return css;
}

namespace {

// What a relative is, in words, from their gender line and the kind of link: "twin sister", "stepfather", "adopted son"
// ("" for a plain parent, partner, brother, sister or child) (0.46). `role`: 0 parent, 1 partner, 2 sibling, 3 child.
std::string kinWord(const Entry& o, int role, const std::string& how) {
    if (how.empty() || role == 1) return {};
    std::string g;
    for (auto& [label, value] : detailLines(o.details)) {
        std::string l = label;
        for (auto& c : l) c = (char)std::tolower((unsigned char)c);
        if (l == "gender" || l == "sex" || l == "gender or sex" || l == "gender/sex") { g = value; break; }
    }
    for (auto& c : g) c = (char)std::tolower((unsigned char)c);
    const int sex = g.find("female") != std::string::npos || g.find("woman") != std::string::npos || g == "f" ? 1
                  : g.find("male") != std::string::npos || g.find("man") != std::string::npos || g == "m" ? 0 : 2;
    const char* base = role == 0 ? (sex == 1 ? "mother" : sex == 0 ? "father" : "parent")
                     : role == 2 ? (sex == 1 ? "sister" : sex == 0 ? "brother" : "sibling")
                                 : (sex == 1 ? "daughter" : sex == 0 ? "son" : "child");
    if (how == "twin") return std::string("twin ") + base;
    if (how == "half") return std::string("half-") + base;
    if (how == "step") return std::string("step") + base;
    if (how == "foster") return std::string("foster ") + base;
    if (how == "adoptive") return (role == 3 ? std::string("adopted ") : std::string("adoptive ")) + base;
    return {};
}

struct Site {
    const Library& lib;
    const View& view;
    const Look& look;
    std::string folder;
    std::string title;
    std::vector<conlang::Language> langs;
    std::map<std::string, std::string> pictures;   // blob -> "images/<blob>.<ext>" ("" = couldn't)
    std::vector<std::pair<std::string, std::string>> trees;   // family trees (0.45): title -> "images/familytree-<id>.png"
    std::string error;
    // 0.46: each tree's anchor and picture width (at 1x), and the tree each person is in (anchor, title).
    std::vector<std::pair<std::string, int>> treeInfo;
    std::map<Id, std::pair<std::string, std::string>> treeOf;
    // A kind's own page.
    static std::string kindPage(const std::string& kind) { return "kind-" + kind + ".html"; }

    std::string picture(const std::string& blob) {
        if (blob.empty() || !view.options().pictures) return {};
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

    std::string page(const std::string& pageTitle, const std::string& body, const std::string& here = {}) const {
        std::string nav = "<header class=\"top\"><a class=\"brand\" href=\"index.html\">" + esc(title) + "</a><nav>";
        std::set<std::string> kinds;
        for (const Entry* e : view.entries()) kinds.insert(e->kind);
        for (const auto& k : entryKinds())
            if (kinds.count(k)) nav += "<a href=\"" + kindPage(k) + "\"" + (here == k ? " class=\"on\"" : "") + ">" + esc(entryKindPlural(k)) + "</a>";
        if (view.options().glossary) nav += "<a href=\"glossary.html\"" + std::string(here == "glossary" ? " class=\"on\"" : "") + ">Glossary</a>";
        if (!langs.empty()) nav += "<a href=\"languages.html\"" + std::string(here == "languages" ? " class=\"on\"" : "") + ">Languages</a>";
        if (!trees.empty()) nav += "<a href=\"families.html\"" + std::string(here == "families" ? " class=\"on\"" : "") + ">Family trees</a>";
        nav += "</nav>";
        if (look.search) nav += "<div class=\"search\"><input id=\"q\" type=\"search\" placeholder=\"Search\" autocomplete=\"off\" aria-label=\"Search the bible\"><div id=\"qr\" hidden></div></div>";
        nav += "</header>";
        std::string guide;
        if (view.options().readersGuide) {
            const Book* b = lib.book(view.options().upTo);
            guide = "<div class=\"guide\">A reader's guide" + (b ? " for readers of <em>" + esc(b->title) + "</em>: nothing here is revealed later in the series." : std::string(".")) + "</div>";
        }
        return "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
               "<title>" + esc(pageTitle) + "</title><link rel=\"stylesheet\" href=\"style.css\"></head><body>" + nav + "<main>" + guide + body +
               "</main><footer>" + esc(title) + " \xC2\xB7 made with NorthRay Writing Studio.</footer>" +
               (look.search ? "<script src=\"search.js\"></script><script src=\"site.js\"></script>" : "<script src=\"site.js\"></script>") + "</body></html>\n";
    }

    bool write(const std::string& name, const std::string& text) {
        std::ofstream f(u8path(folder + "/" + name), std::ios::binary | std::ios::trunc);
        f << text;
        if (!f.good()) { error = "Couldn't write " + name + "."; return false; }
        return true;
    }

    std::string card(const Entry& e) {
        const std::string img = picture(e.imageBlob);
        std::string summary = view.shows(e, "summary") ? e.summary : std::string();
        if (summary.size() > 260) {
            size_t cut = summary.rfind(' ', 250);
            summary = summary.substr(0, cut == std::string::npos ? 250 : cut) + "\xE2\x80\xA6";
        }
        return "<a class=\"card k-" + e.kind + "\" href=\"" + pageName(e.id) + "\"><div class=\"pic\"" +
               (img.empty() ? ">" + esc(initials(e.name)) : " style=\"background-image:url('" + img + "')\">") + "</div><div><div class=\"kind\">" +
               esc(entryKindLabel(e.kind)) + "</div><b>" + esc(e.name) + "</b>" + (summary.empty() ? "" : "<p>" + esc(summary) + "</p>") + "</div></a>";
    }

    std::string entryPage(const Entry& e, const Entry* prev, const Entry* next) {
        const auto& shown = view.entries();
        std::string head, main, aside;
        // The name and what it's called sit above the text and the box (so on a phone the name comes first) (0.46).
        head += "<div class=\"kind\">" + esc(entryKindLabel(e.kind)) + "</div><h1>" + esc(e.name) + "</h1>";
        if (!e.pronunciation.empty()) head += "<div class=\"said\">Said \xE2\x80\x9C" + esc(e.pronunciation) + "\xE2\x80\x9D</div>";
        if (!e.aliases.empty()) {
            std::string a;
            for (auto& x : e.aliases) a += (a.empty() ? "" : ", ") + std::string("\xE2\x80\x9C") + esc(x) + "\xE2\x80\x9D";
            head += "<div class=\"also\">Also known as " + a + "</div>";
        }
        if (!e.summary.empty() && view.shows(e, "summary")) main += "<p class=\"lead\">" + linked(e.summary, shown, e.id) + "</p>";
        for (auto& [t, text] : lib.entrySections(e)) {
            if (text.empty() || !view.shows(e, "section:" + t)) continue;
            main += "<h2>" + esc(t) + "</h2>" + prose(text, shown, e.id);
        }
        if (!view.options().readersGuide)
            for (const Id& bk : booksInOrder(lib, view.options().worldId, view.options().bookId)) {
                const std::string& note = e.bookNote(bk);
                const Book* book = lib.book(bk);
                if (note.empty() || !book) continue;
                main += "<h2>In <em>" + esc(book->title) + "</em></h2>" + prose(note, shown, e.id);
            }
        if (!e.mapBlob.empty())
            if (const std::string img = picture(e.mapBlob); !img.empty()) {
                main += "<h2>Map</h2><div class=\"map\"><img src=\"" + img + "\" alt=\"Map of " + esc(e.name) + "\">";
                for (auto& p : e.pins)
                    if (const Entry* o = lib.entry(p.entry); o && view.shows(p.entry)) {
                        char pos[64];
                        std::snprintf(pos, sizeof pos, "left:%.2f%%;top:%.2f%%", p.x * 100, p.y * 100);
                        main += "<a class=\"pin\" style=\"" + std::string(pos) + "\" href=\"" + pageName(o->id) + "\"><i></i><span>" + esc(o->name) + "</span></a>";
                    }
                main += "</div>";
            }
        std::string gal;
        for (auto& g : e.gallery)
            if (g.blob != e.imageBlob)
                if (const std::string img = picture(g.blob); !img.empty())
                    gal += "<figure><img src=\"" + img + "\" alt=\"" + esc(g.caption.empty() ? e.name : g.caption) + "\" loading=\"lazy\">" +
                           (g.caption.empty() ? "" : "<figcaption>" + esc(g.caption) + "</figcaption>") + "</figure>";
        if (!gal.empty()) main += "<h2>Gallery</h2><div class=\"gallery\">" + gal + "</div>";
        if (!view.options().readersGuide && !e.notes.empty()) main += "<h2>Notes</h2>" + prose(e.notes, shown, e.id);

        // The box at the side: picture, details, family, relationships.
        if (const std::string img = picture(e.imageBlob); !img.empty()) aside += "<img class=\"portrait\" src=\"" + img + "\" alt=\"" + esc(e.name) + "\">";
        aside += "<div class=\"box\"><h3>" + esc(e.name) + "</h3>";
        std::string rows;
        for (auto& [label, value] : detailLines(e.details)) {
            if (!label.empty() && !view.shows(e, "detail:" + label)) continue;
            rows += label.empty() ? "<tr><td colspan=\"2\">" + linked(value, shown, e.id) + "</td></tr>"
                                  : "<tr><td>" + esc(label) + "</td><td>" + linked(value, shown, e.id) + "</td></tr>";
        }
        if (!rows.empty()) aside += "<table>" + rows + "</table>";
        if (view.options().family) {
            if (e.kind == "character") {
                const auto f = lib.familyOf(e.id);
                std::string fam;
                auto row = [&](const char* label, const std::vector<Id>& ids, int role) {
                    std::string names;
                    for (const Id& i : ids)
                        if (const Entry* o = lib.entry(i); o && view.shows(i)) {
                            // What kind, when not plain: twin sister, stepfather, adopted son (0.46).
                            std::string kw;
                            if (auto h = f.how.find(i); h != f.how.end() && role != 1) kw = kinWord(*o, role, h->second);
                            names += (names.empty() ? "" : ", ") + std::string("<a href=\"") + pageName(i) + "\">" + esc(o->name) + "</a>" +
                                     (kw.empty() ? "" : " <small class=\"kin\">" + esc(kw) + "</small>");
                        }
                    if (!names.empty()) fam += "<tr><td>" + std::string(label) + "</td><td>" + names + "</td></tr>";
                };
                row("Parents", f.parents, 0);
                row("Partners", f.partners, 1);
                row("Brothers and sisters", f.siblings, 2);
                row("Children", f.children, 3);
                if (!fam.empty()) {
                    aside += "<h4>Family</h4><table>" + fam + "</table>";
                    if (auto t = treeOf.find(e.id); t != treeOf.end())
                        aside += "<a class=\"treelink\" href=\"families.html#" + t->second.first + "\">" + esc(t->second.second) + ": see the tree \xE2\x80\xBA</a>";
                }
            }
            if (const auto rel = relationsOf(lib, view, e); !rel.empty()) {
                // One row per relationship, names together.
                std::vector<std::pair<std::string, std::string>> grouped;
                for (auto& [label, o] : rel) {
                    const std::string link = "<a href=\"" + pageName(o->id) + "\">" + esc(o->name) + "</a>";
                    auto it = std::find_if(grouped.begin(), grouped.end(), [&](auto& g) { return g.first == label; });
                    if (it == grouped.end()) grouped.push_back({label, link});
                    else it->second += ", " + link;
                }
                aside += "<h4>Relationships</h4><table>";
                for (auto& [l, names] : grouped) aside += "<tr><td>" + esc(l) + "</td><td>" + names + "</td></tr>";
                aside += "</table>";
            }
        }
        aside += "</div>";
        std::string pager;
        if (prev || next) {
            pager = "<nav class=\"pager\">";
            if (prev) pager += "<a href=\"" + pageName(prev->id) + "\"><small>\xE2\x80\xB9 Previous</small>" + esc(prev->name) + "</a>";
            if (next) pager += "<a class=\"next\" href=\"" + pageName(next->id) + "\"><small>Next \xE2\x80\xBA</small>" + esc(next->name) + "</a>";
            pager += "</nav>";
        }
        const std::string crumbs = "<div class=\"crumbs\"><a href=\"index.html\">" + esc(title) + "</a> \xE2\x80\xBA <a href=\"" + kindPage(e.kind) + "\">" +
                                   esc(entryKindPlural(e.kind)) + "</a></div>";
        return page(e.name + " \xC2\xB7 " + title, crumbs + "<article class=\"k-" + e.kind + "\"><header class=\"ehead\">" + head + "</header><div class=\"layout\"><div class=\"main\">" + main +
                                                    "</div><aside class=\"infobox\">" + aside + "</aside></div>" + pager + "</article>",
                    e.kind);
    }
};

bool copyFontFamily(const std::string& fontsFolder, const std::string& id, const std::string& site, std::string& faces, const std::string& familyName) {
    if (fontsFolder.empty() || id.empty()) return false;
    std::error_code ec;
    const std::string where = studio::fonts::familyFolder(fontsFolder, id);   // the app's fonts, or a world's own
    if (where.empty()) return false;
    const fs::path src = u8path(where);
    if (!fs::is_directory(src, ec)) return false;
    fs::create_directories(u8path(site + "/fonts/" + id), ec);
    bool any = false;
    for (auto& f : fs::directory_iterator(src, ec)) {
        const auto u8 = f.path().filename().u8string();
        const std::string name(u8.begin(), u8.end());
        if (name.size() < 5) continue;
        std::string ext = name.substr(name.size() - 4);
        for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext != ".ttf" && name != "OFL.txt" && name != "LICENSE.txt") continue;
        fs::copy_file(f.path(), u8path(site + "/fonts/" + id + "/" + name), fs::copy_options::overwrite_existing, ec);
        if (ext != ".ttf") continue;
        const bool bold = name.find("Bold") != std::string::npos, italic = name.find("Italic") != std::string::npos;
        faces += "@font-face{font-family:\"" + familyName + "\";src:url(\"fonts/" + id + "/" + name + "\") format(\"truetype\");font-weight:" +
                 (bold ? "700" : "400") + ";font-style:" + (italic ? "italic" : "normal") + ";font-display:swap}\n";
        any = true;
    }
    return any;
}

}  // namespace

bool exportWebsite(const Library& lib, const Options& options, const std::string& folder, std::string& error, int* pages, const Look& look,
                   const std::string& fontsFolder) {
    const View view(lib, options);
    std::error_code ec;
    fs::create_directories(u8path(folder + "/images"), ec);
    if (ec) { error = "Couldn't make the folder: " + ec.message(); return false; }
    const World* world = lib.world(options.worldId);
    Site site{lib, view, look, folder, look.title.empty() ? view.title() : look.title, {}, {}, {}, {}};
    if (options.languages) site.langs = languagesOf(lib, options.worldId, options.worldId.empty() ? options.bookId : Id{});
    // Family trees as pictures (0.45), when the app's fonts are given; not in a reader's guide.
    if (options.family && !options.readersGuide && !options.treeFonts.empty())
        for (const auto& fg : lib.families(options.worldId, options.worldId.empty() ? options.bookId : Id{})) {
            familytree::Options to;
            to.scale = 2;
            to.heading = false;
            const auto pic = familytree::render(lib, fg.root, options.treeFonts, to);
            if (!pic.ok) continue;
            const std::string rel = "images/familytree-" + fg.root + ".png";
            if (!writeFile(folder + "/" + rel, pic.png)) continue;
            const std::string t = familytree::title(lib, fg.root, true), anchor = "tree-" + fg.root;
            site.trees.push_back({t, rel});
            site.treeInfo.push_back({anchor, pic.width / 2});
            for (const auto& p : lib.familyLayout(fg.root, 150, true).people)
                if (!p.inLaw && !site.treeOf.count(p.id)) site.treeOf[p.id] = {anchor, t};
        }
        // Married in: their partner's family's tree, when they have none of their own.
        for (const auto& fg : lib.families(options.worldId, options.worldId.empty() ? options.bookId : Id{})) {
            if (!options.family || options.readersGuide || options.treeFonts.empty()) break;
            const auto it = std::find_if(site.treeInfo.begin(), site.treeInfo.end(), [&](auto& ti) { return ti.first == "tree-" + fg.root; });
            if (it == site.treeInfo.end()) continue;
            const std::string t = familytree::title(lib, fg.root, true);
            for (const auto& p : lib.familyLayout(fg.root, 150, true).people)
                if (p.inLaw && !site.treeOf.count(p.id)) site.treeOf[p.id] = {it->first, t};
        }

    // The stylesheet, with the typefaces the look needs copied in.
    {
        const ThemeDef& t = themeDef(look.theme);
        const std::string hid = look.headings == "theme" || look.headings.empty() ? std::string(t.headings) : look.headings;
        const std::string bid = look.body == "theme" || look.body.empty() ? std::string(t.body) : look.body;
        std::string faces;
        std::set<std::string> done;
        for (const std::string& id : {hid, bid}) {
            if (isWorldFamily(id)) {
                // A world's own typeface, copied in with its licence.
                const std::string fid = id.substr(7);
                if (done.insert(fid).second) copyFontFamily(fontsFolder, fid, folder, faces, worldFamilyName(id));
                continue;
            }
            const FontDef& f = fontDef(id);
            if (!*f.bundled || done.count(f.bundled)) continue;
            done.insert(f.bundled);
            // The family name the stack asks for first ("EB Garamond", Lora).
            std::string fam = f.stack;
            if (!fam.empty() && fam[0] == '"') fam = fam.substr(1, fam.find('"', 1) - 1);
            else fam = fam.substr(0, fam.find(','));
            copyFontFamily(fontsFolder, f.bundled, folder, faces, fam);
        }
        if (!site.write("style.css", faces + websiteCss(look, world ? world->accent : std::string(), true, true))) { error = site.error; return false; }
    }
    if (!site.write("site.js", kSiteJs)) { error = site.error; return false; }

    // Entries, each with the one before and after it of the same kind.
    int n = 0;
    std::vector<const Entry*> ordered;
    for (const auto& k : entryKinds())
        for (const Entry* e : view.entries()) if (e->kind == k) ordered.push_back(e);
    for (size_t i = 0; i < ordered.size(); ++i) {
        const Entry* prev = i > 0 && ordered[i - 1]->kind == ordered[i]->kind ? ordered[i - 1] : nullptr;
        const Entry* next = i + 1 < ordered.size() && ordered[i + 1]->kind == ordered[i]->kind ? ordered[i + 1] : nullptr;
        if (!site.write(pageName(ordered[i]->id), site.entryPage(*ordered[i], prev, next))) { error = site.error; return false; }
        ++n;
    }

    // What the search box looks through.
    if (look.search) {
        std::string js = "window.NR_BIBLE=[";
        bool first = true;
        for (const Entry* e : ordered) {
            std::string aliases;
            for (auto& a : e->aliases) aliases += (aliases.empty() ? "" : ",") + jsStr(a);
            std::string summary = view.shows(*e, "summary") ? e->summary.substr(0, 240) : std::string();
            js += std::string(first ? "" : ",\n") + "{n:" + jsStr(e->name) + ",a:[" + aliases + "],k:" + jsStr(entryKindLabel(e->kind)) + ",c:" + jsStr(e->kind) +
                  ",u:" + jsStr(pageName(e->id)) + ",s:" + jsStr(summary) + "}";
            first = false;
        }
        js += "];\n";
        if (!site.write("search.js", js)) { error = site.error; return false; }
    }

    // Home: the world's banner and words, then everything by kind.
    int kindPages = 0;
    {
        std::string home;
        const std::string banner = world ? site.picture(world->bannerBlob) : std::string();
        home += "<section class=\"hero\"" + (banner.empty() ? std::string() : " style=\"background-image:url('" + banner + "')\"") + "><div class=\"in\"><div class=\"eyebrow\">" +
                (options.readersGuide ? "A reader's guide" : "World bible") + "</div><h1>" + esc(site.title) + "</h1>" +
                (look.tagline.empty() ? "" : "<p>" + esc(look.tagline) + "</p>") + "</div></section>";
        if (world && !world->description.empty()) home += "<div class=\"intro\">" + prose(world->description, view.entries(), {}) + "</div>";
        std::string chips;
        for (const auto& k : entryKinds()) {
            int c = 0;
            for (const Entry* e : view.entries()) c += e->kind == k;
            if (c) chips += "<a class=\"k-" + k + "\" href=\"" + Site::kindPage(k) + "\"><i></i>" + esc(entryKindPlural(k)) + " <small>" + std::to_string(c) + "</small></a>";
        }
        if (options.glossary) chips += "<a href=\"glossary.html\">Glossary</a>";
        if (!site.langs.empty()) chips += "<a href=\"languages.html\">Languages <small>" + std::to_string(site.langs.size()) + "</small></a>";
        if (!site.trees.empty()) chips += "<a href=\"families.html\">Family trees <small>" + std::to_string(site.trees.size()) + "</small></a>";
        home += "<div class=\"chips\">" + chips + "</div>";
        // On the home page, each kind's first few (those with a picture or a summary first); the rest on its own page (0.46).
        const size_t homeCount = 8;
        for (const auto& k : entryKinds()) {
            std::vector<const Entry*> of;
            for (const Entry* e : ordered) if (e->kind == k) of.push_back(e);
            if (of.empty()) continue;
            std::vector<const Entry*> pick = of;
            std::stable_partition(pick.begin(), pick.end(), [&](const Entry* e) {
                return (!e->imageBlob.empty() && options.pictures) || (!e->summary.empty() && view.shows(*e, "summary"));
            });
            if (pick.size() > homeCount) pick.resize(homeCount);
            std::string items;
            for (const Entry* e : pick) items += look.cards ? site.card(*e) : "<li><a href=\"" + pageName(e->id) + "\">" + esc(e->name) + "</a></li>";
            home += "<h2 class=\"kindhead k-" + k + "\" id=\"" + k + "\"><a href=\"" + Site::kindPage(k) + "\">" + esc(entryKindPlural(k)) + "</a><small>" +
                    std::to_string(of.size()) + "</small></h2>" + (look.cards ? "<div class=\"cards\">" + items + "</div>" : "<ul class=\"list\">" + items + "</ul>");
            if (of.size() > pick.size()) {
                std::string plural = entryKindPlural(k);
                if (!plural.empty()) plural[0] = (char)std::tolower((unsigned char)plural[0]);
                home += "<a class=\"more k-" + k + "\" href=\"" + Site::kindPage(k) + "\">All " + std::to_string(of.size()) + " " + esc(plural) + " \xE2\x80\xBA</a>";
            }
        }
        if (!site.write("index.html", site.page(site.title, home))) { error = site.error; return false; }
        // A page per kind: every entry, A to Z (characters by gender or sex when most have it written) (0.46).
        for (const auto& k : entryKinds()) {
            std::vector<const Entry*> of;
            for (const Entry* e : ordered) if (e->kind == k) of.push_back(e);
            if (of.empty()) continue;
            std::sort(of.begin(), of.end(), [](const Entry* a, const Entry* b) { return a->name < b->name; });
            auto block = [&](const std::vector<const Entry*>& list) {
                std::string items;
                for (const Entry* e : list) items += look.cards ? site.card(*e) : "<li><a href=\"" + pageName(e->id) + "\">" + esc(e->name) + "</a></li>";
                return look.cards ? "<div class=\"cards\">" + items + "</div>" : "<ul class=\"list\">" + items + "</ul>";
            };
            std::string body = "<div class=\"crumbs\"><a href=\"index.html\">" + esc(site.title) + "</a></div><h1 class=\"kindtitle k-" + k + "\">" +
                               esc(entryKindPlural(k)) + " <small>" + std::to_string(of.size()) + "</small></h1>";
            std::map<std::string, std::vector<const Entry*>> byGender;
            int withGender = 0;
            if (k == "character")
                for (const Entry* e : of) {
                    std::string g, glabel;
                    for (auto& [label, value] : detailLines(e->details)) {
                        std::string l = label;
                        for (auto& c : l) c = (char)std::tolower((unsigned char)c);
                        if (l == "gender" || l == "sex" || l == "gender or sex" || l == "gender/sex") { g = value; glabel = label; break; }
                    }
                    if (!g.empty() && view.shows(*e, "detail:" + glabel)) ++withGender; else g.clear();
                    byGender[g].push_back(e);
                }
            if (k == "character" && byGender.size() > 1 && withGender * 2 >= (int)of.size()) {
                for (auto& [g, list] : byGender) {
                    if (g.empty()) continue;
                    body += "<h2 class=\"subhead\">" + esc(g) + " <small>" + std::to_string(list.size()) + "</small></h2>" + block(list);
                }
                if (byGender.count("")) body += "<h2 class=\"subhead\">Not said <small>" + std::to_string(byGender[""].size()) + "</small></h2>" + block(byGender[""]);
            } else {
                body += block(of);
            }
            if (!site.write(Site::kindPage(k), site.page(std::string(entryKindPlural(k)) + " \xC2\xB7 " + site.title, body, k))) { error = site.error; return false; }
            ++kindPages;
        }
    }

    // Glossary: every name and other name, A to Z, with a row of letters to jump to.
    int extra = 1;
    if (options.glossary) {
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
        std::string body, letters;
        char letter = 0;
        body += "<dl>";
        for (auto& x : g) {
            const char l0 = x.key.empty() ? '#' : (char)std::toupper((unsigned char)x.key[0]);
            const char l = std::isalpha((unsigned char)l0) ? l0 : '#';
            if (l != letter) {
                letter = l;
                const std::string id = l == '#' ? "other" : std::string(1, (char)std::tolower((unsigned char)l));
                body += "</dl><div class=\"letter\" id=\"" + id + "\">" + std::string(1, l) + "</div><dl>";
                letters += "<a href=\"#" + id + "\">" + std::string(1, l) + "</a>";
            }
            body += "<dt><a href=\"" + pageName(x.e->id) + "\">" + esc(x.name) + "</a>";
            if (!x.alias && !x.e->pronunciation.empty()) body += " <span class=\"said\">(" + esc(x.e->pronunciation) + ")</span>";
            body += "</dt><dd>" + (x.alias ? "See " + esc(x.e->name) + "."
                                           : esc(entryKindLabel(x.e->kind)) + (x.e->summary.empty() || !view.shows(*x.e, "summary") ? std::string() : ". " + esc(x.e->summary))) +
                    "</dd>";
        }
        body += "</dl>";
        if (!site.write("glossary.html", site.page("Glossary \xC2\xB7 " + site.title, "<h1>Glossary</h1><div class=\"letters\">" + letters + "</div>" + body, "glossary"))) {
            error = site.error;
            return false;
        }
        ++extra;
    }

    // Languages: each a dictionary, with a box to find a word.
    if (!site.langs.empty()) {
        std::string body = "<h1>Languages</h1>";
        if (site.langs.size() > 1) {
            body += "<div class=\"chips\">";
            for (size_t i = 0; i < site.langs.size(); ++i) body += "<a href=\"#lang" + std::to_string(i) + "\">" + esc(site.langs[i].name) + " <small>" + std::to_string(site.langs[i].words.size()) + "</small></a>";
            body += "</div>";
        }
        for (size_t i = 0; i < site.langs.size(); ++i) {
            const auto& L = site.langs[i];
            const std::string tid = "lang" + std::to_string(i);
            bool hasPart = false, hasSay = false, hasNotes = false;
            for (auto& w : L.words) { hasPart |= !w.part.empty(); hasSay |= !w.pronunciation.empty(); hasNotes |= !w.notes.empty(); }
            body += "<section class=\"lang\" id=\"" + tid + "\"><h2>" + esc(L.name) + "</h2><div class=\"meta\">" + std::to_string(L.words.size()) +
                    (L.words.size() == 1 ? " word" : " words") + "</div>" + (L.notes.empty() ? "" : prose(L.notes, view.entries(), {}));
            body += "<input class=\"filter\" type=\"search\" placeholder=\"Find a word or meaning\" data-for=\"t" + tid + "\" aria-label=\"Find a word in " + esc(L.name) + "\">";
            body += "<table id=\"t" + tid + "\"><thead><tr><th>Word</th>" + std::string(hasSay ? "<th>Said</th>" : "") + (hasPart ? "<th>Part</th>" : "") +
                    "<th>Meaning</th>" + (hasNotes ? "<th>Notes</th>" : "") + "</tr></thead><tbody>";
            for (auto& w : L.words)
                body += "<tr><td>" + esc(w.word) + "</td>" + (hasSay ? "<td class=\"ipa\">" + esc(w.pronunciation) + "</td>" : "") +
                        (hasPart ? "<td>" + esc(w.part) + "</td>" : "") + "<td>" + esc(w.meaning) + "</td>" + (hasNotes ? "<td>" + esc(w.notes) + "</td>" : "") + "</tr>";
            body += "</tbody></table></section>";
        }
        if (!site.write("languages.html", site.page("Languages \xC2\xB7 " + site.title, body, "languages"))) { error = site.error; return false; }
        ++extra;
    }
    // Family trees: a page with each family's tree, the picture opening at full size.
    if (!site.trees.empty()) {
        std::string body = "<h1>Family trees</h1>";
        // At their own size, moved across when wider than the page; the picture opens on its own (0.46).
        if (site.trees.size() > 1) {
            body += "<div class=\"chips\">";
            for (size_t i = 0; i < site.trees.size(); ++i) body += "<a href=\"#" + site.treeInfo[i].first + "\">" + esc(site.trees[i].first) + "</a>";
            body += "</div>";
        }
        for (size_t i = 0; i < site.trees.size(); ++i) {
            const auto& [t, rel] = site.trees[i];
            body += "<section class=\"tree\" id=\"" + site.treeInfo[i].first + "\"><h2>" + esc(t) + "</h2><div class=\"treewrap\"><a href=\"" + rel +
                    "\" title=\"Open the picture on its own\"><img src=\"" + rel + "\" alt=\"" + esc(t) + " family tree\" loading=\"lazy\" width=\"" +
                    std::to_string(site.treeInfo[i].second) + "\"></a></div>" + (site.treeInfo[i].second > 1000 ? "<div class=\"treenote\">Wider than the page: move across it, or open the picture on its own.</div>" : "") + "</section>";
        }
        if (!site.write("families.html", site.page("Family trees \xC2\xB7 " + site.title, body, "families"))) { error = site.error; return false; }
        ++extra;
    }
    if (pages) *pages = n + extra + kindPages;
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
        if (!options.pictures) return false;
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
            if (e->kind == "character" && options.family) {
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
            if (const auto rel = options.family ? relationsOf(lib, view, *e) : decltype(relationsOf(lib, view, *e)){}; !rel.empty()) {
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
    // Family trees (0.45): a part with each family's tree, turned a quarter when it's much wider than tall.
    if (options.family && !options.readersGuide && !options.treeFolder.empty() && !options.treeFonts.empty()) {
        const auto fams = lib.families(options.worldId, options.worldId.empty() ? options.bookId : Id{});
        bool partAdded = false;
        std::error_code ec;
        std::filesystem::create_directories(detail::u8path(options.treeFolder), ec);
        for (const auto& fg : fams) {
            familytree::Options to;
            to.bloodline = true;
            to.scale = 3;
            to.heading = false;
            to.turnWide = true;
            const auto pic = familytree::render(lib, fg.root, options.treeFonts, to);
            if (!pic.ok) continue;
            // A picture id of the usual length (the document's pictures are named by their hash).
            std::string blob;
            for (unsigned salt = 0; salt < 4; ++salt) {
                std::uint64_t h = 1469598103934665603ull ^ salt;
                for (unsigned char c : "familytree:" + fg.root) { h ^= c; h *= 1099511628211ull; }
                char buf[17];
                std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)h);
                blob += buf;
            }
            const std::string path = detail::fromPath(detail::u8path(options.treeFolder) / ("familytree-" + fg.root + ".png"));
            if (!writeFile(path, pic.png)) continue;
            if (!partAdded) {
                typeset::Source::Section part;
                part.kind = NodeKind::Part;
                part.title = "Family trees";
                part.content = doc::emptyDoc();
                s.sections.push_back(part);
                partAdded = true;
            }
            s.picturePaths[blob] = path;
            s.pictureSizes[blob] = {pic.width, pic.height};
            const std::string name = familytree::title(lib, fg.root, true);
            doc::Builder d;
            d.paragraph("centered");
            d.text(std::to_string(pic.people) + (pic.people == 1 ? " person" : " people") + (pic.turned ? " (turn the book to read it)" : ""),
                   doc::Builder::Em);
            d.endBlock();
            d.image(blob, name);
            typeset::Source::Section ch;
            ch.kind = NodeKind::Chapter;
            ch.matter = 2;
            ch.title = name;
            ch.content = d.toJson();
            s.sections.push_back(std::move(ch));
        }
    }
    // The languages, each a dictionary: word, how it's said, part of speech, meaning.
    if (options.languages) {
        const auto langs = languagesOf(lib, options.worldId, options.worldId.empty() ? options.bookId : Id{});
        if (!langs.empty()) {
            typeset::Source::Section part;
            part.kind = NodeKind::Part;
            part.title = "Languages";
            part.content = doc::emptyDoc();
            s.sections.push_back(part);
        }
        for (const auto& L : langs) {
            doc::Builder d;
            bool first = true;
            for (auto& p : paragraphs(L.notes)) {
                d.paragraph(first ? "first" : "body");
                d.text(p);
                d.endBlock();
                first = false;
            }
            std::string letter;
            for (auto& w : L.words) {
                // A heading for each new first letter (by its first character, whatever the script).
                std::string l;
                if (!w.word.empty()) {
                    const unsigned char c = (unsigned char)w.word[0];
                    const size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
                    l = w.word.substr(0, n);
                    if (n == 1) l[0] = (char)std::toupper((unsigned char)l[0]);
                }
                if (L.words.size() > 30 && l != letter && !l.empty() && std::isalpha((unsigned char)l[0])) {
                    letter = l;
                    d.heading(3);
                    d.text(l);
                    d.endBlock();
                }
                d.paragraph("first");
                d.text(w.word, doc::Builder::Strong);
                if (!w.pronunciation.empty()) d.text("  " + w.pronunciation, doc::Builder::Em);
                if (!w.part.empty()) d.text("  " + w.part, doc::Builder::Em);
                if (!w.meaning.empty()) d.text("  \xE2\x80\x94 " + w.meaning);
                if (!w.notes.empty()) d.text(". " + w.notes);
                d.endBlock();
            }
            if (d.empty()) continue;
            typeset::Source::Section ch;
            ch.kind = NodeKind::Chapter;
            ch.matter = 2;
            ch.title = L.name;
            ch.content = d.toJson();
            ch.words = d.words();
            s.sections.push_back(std::move(ch));
        }
    }
    // The glossary, at the back.
    if (options.glossary) {
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
