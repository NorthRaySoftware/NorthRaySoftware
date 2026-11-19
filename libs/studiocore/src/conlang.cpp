#include "studio/conlang.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <set>

namespace studio::conlang {

using nlohmann::json;

namespace {

std::string trim(std::string s) {
    auto sp = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; };
    while (!s.empty() && sp((unsigned char)s.back())) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && sp((unsigned char)s[i])) ++i;
    s.erase(0, i);
    // A no-break space at either end (often pasted from web pages).
    while (s.size() >= 2 && (unsigned char)s[0] == 0xC2 && (unsigned char)s[1] == 0xA0) s.erase(0, 2);
    while (s.size() >= 2 && (unsigned char)s[s.size() - 2] == 0xC2 && (unsigned char)s.back() == 0xA0) s.resize(s.size() - 2);
    return s;
}

// ASCII lower case, for comparing column names and spellings (other letters are compared as they are).
std::string lowerAscii(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string clip(std::string s, std::size_t n) {
    if (s.size() <= n) return s;
    s.resize(n);
    while (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0x80) s.pop_back();   // not inside a character
    if (!s.empty() && ((unsigned char)s.back() & 0x80)) s.pop_back();
    return s;
}

Word cleanWord(Word w) {
    w.word = clip(trim(w.word), 200);
    w.meaning = clip(trim(w.meaning), 2000);
    w.part = clip(trim(w.part), 80);
    w.pronunciation = clip(trim(w.pronunciation), 200);
    w.notes = clip(trim(w.notes), 4000);
    return w;
}

void addWord(Language& l, Word w) {
    w = cleanWord(std::move(w));
    if (w.word.empty() || l.words.size() >= kMaxWords) return;
    l.words.push_back(std::move(w));
}

std::string extensionOf(const std::string& file) {
    const auto dot = file.rfind('.');
    const auto slash = file.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    return lowerAscii(file.substr(dot));
}

std::string stemOf(const std::string& file) {
    std::string s = file;
    const auto slash = s.find_last_of("/\\");
    if (slash != std::string::npos) s = s.substr(slash + 1);
    for (const char* ext : {".nrlang", ".json", ".csv", ".tsv", ".txt", ".sfm", ".db", ".mdf", ".lex"})
        if (s.size() > std::strlen(ext) && lowerAscii(s.substr(s.size() - std::strlen(ext))) == ext) {
            s.resize(s.size() - std::strlen(ext));
            break;
        }
    return trim(s).empty() ? std::string("Language") : clip(trim(s), 80);
}

// Text from bytes: a UTF-8 BOM dropped; UTF-16 (with its BOM) turned into UTF-8.
std::string asUtf8(const std::string& b) {
    if (b.size() >= 3 && (unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB && (unsigned char)b[2] == 0xBF) return b.substr(3);
    const bool le = b.size() >= 2 && (unsigned char)b[0] == 0xFF && (unsigned char)b[1] == 0xFE;
    const bool be = b.size() >= 2 && (unsigned char)b[0] == 0xFE && (unsigned char)b[1] == 0xFF;
    if (!le && !be) return b;
    std::string out;
    for (std::size_t i = 2; i + 1 < b.size(); i += 2) {
        unsigned cp = le ? ((unsigned char)b[i] | ((unsigned char)b[i + 1] << 8)) : (((unsigned char)b[i] << 8) | (unsigned char)b[i + 1]);
        if (cp >= 0xD800 && cp < 0xDC00 && i + 3 < b.size()) {
            const unsigned lo = le ? ((unsigned char)b[i + 2] | ((unsigned char)b[i + 3] << 8)) : (((unsigned char)b[i + 2] << 8) | (unsigned char)b[i + 3]);
            if (lo >= 0xDC00 && lo < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) out += (char)(0xC0 | (cp >> 6)), out += (char)(0x80 | (cp & 0x3F));
        else if (cp < 0x10000) out += (char)(0xE0 | (cp >> 12)), out += (char)(0x80 | ((cp >> 6) & 0x3F)), out += (char)(0x80 | (cp & 0x3F));
        else
            out += (char)(0xF0 | (cp >> 18)), out += (char)(0x80 | ((cp >> 12) & 0x3F)), out += (char)(0x80 | ((cp >> 6) & 0x3F)),
                out += (char)(0x80 | (cp & 0x3F));
    }
    return out;
}

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at <= text.size()) {
        std::size_t nl = text.find('\n', at);
        if (nl == std::string::npos) nl = text.size();
        std::string l = text.substr(at, nl - at);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        out.push_back(std::move(l));
        at = nl + 1;
    }
    return out;
}

// --- JSON ---

Language languageFrom(const json& j) {
    Language l;
    auto str = [](const json& o, const char* k) { return o.contains(k) && o[k].is_string() ? o[k].get<std::string>() : std::string(); };
    l.id = clip(trim(str(j, "id")), 40);
    l.name = clip(trim(str(j, "name")), 80);
    l.notes = clip(str(j, "notes"), 20000);
    if (j.contains("words") && j["words"].is_array())
        for (auto& w : j["words"]) {
            if (w.is_string()) addWord(l, {w.get<std::string>()});
            else if (w.is_object()) addWord(l, {str(w, "word"), str(w, "meaning"), str(w, "part"), str(w, "pronunciation"), str(w, "notes")});
        }
    return l;
}

json languageJson(const Language& l, bool withId) {
    json j;
    if (withId) j["id"] = l.id;
    j["name"] = l.name;
    if (!l.notes.empty()) j["notes"] = l.notes;
    json words = json::array();
    for (auto& w : l.words) {
        json o{{"word", w.word}};
        if (!w.meaning.empty()) o["meaning"] = w.meaning;
        if (!w.part.empty()) o["part"] = w.part;
        if (!w.pronunciation.empty()) o["pronunciation"] = w.pronunciation;
        if (!w.notes.empty()) o["notes"] = w.notes;
        words.push_back(std::move(o));
    }
    j["words"] = std::move(words);
    return j;
}

// --- delimited text (CSV, TSV, semicolons) ---

std::vector<std::vector<std::string>> parseDelimited(const std::string& text, char sep) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string field;
    bool quoted = false, any = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') field += '"', ++i;
                else quoted = false;
            } else {
                field += c;
            }
            continue;
        }
        if (c == '"' && trim(field).empty()) {
            field.clear();
            quoted = true;
            any = true;
        } else if (c == sep) {
            row.push_back(field);
            field.clear();
            any = true;
        } else if (c == '\n' || c == '\r') {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            row.push_back(field);
            field.clear();
            if (any || !trim(row[0]).empty() || row.size() > 1) rows.push_back(row);
            row.clear();
            any = false;
        } else {
            field += c;
            any = true;
        }
        if (rows.size() > kMaxWords + 1) break;
    }
    if (any || !field.empty()) {
        row.push_back(field);
        rows.push_back(row);
    }
    return rows;
}

enum Column { CWord, CMeaning, CPart, CPron, CNotes, CNone };

Column columnFor(std::string name) {
    name = lowerAscii(trim(name));
    for (auto& c : name)
        if (c == '_' || c == '-') c = ' ';
    static const std::map<std::string, Column> known = {
        {"word", CWord},          {"conword", CWord},           {"con word", CWord},      {"conlang", CWord},       {"lemma", CWord},
        {"headword", CWord},      {"head word", CWord},         {"lexeme", CWord},        {"form", CWord},          {"native", CWord},
        {"meaning", CMeaning},    {"meanings", CMeaning},       {"gloss", CMeaning},      {"definition", CMeaning}, {"english", CMeaning},
        {"translation", CMeaning}, {"local word", CMeaning},    {"localword", CMeaning},  {"natlang", CMeaning},    {"sense", CMeaning},
        {"part of speech", CPart}, {"pos", CPart},              {"class", CPart},         {"word class", CPart},    {"type", CPart},
        {"category", CPart},      {"pronunciation", CPron},     {"ipa", CPron},           {"phonetic", CPron},      {"notes", CNotes},
        {"note", CNotes},         {"comment", CNotes},          {"comments", CNotes},     {"etymology", CNotes},    {"usage", CNotes}};
    auto it = known.find(name);
    return it == known.end() ? CNone : it->second;
}

bool readDelimited(const std::string& text, char sep, Language& l) {
    auto rows = parseDelimited(text, sep);
    if (rows.empty()) return false;
    // A header row if it names a word column and a meaning column (or at least the word column and nothing odd).
    std::vector<Column> cols;
    bool header = false;
    {
        int words = 0, meanings = 0, known = 0;
        for (auto& c : rows[0]) {
            const Column k = columnFor(c);
            cols.push_back(k);
            if (k == CWord) ++words;
            if (k == CMeaning) ++meanings;
            if (k != CNone) ++known;
        }
        header = words >= 1 || (meanings >= 1 && known >= 2);
        if (header && !words) {   // a header without a word column: the first unnamed one is the word
            for (auto& k : cols)
                if (k == CNone) {
                    k = CWord;
                    break;
                }
        }
    }
    if (!header) cols = {CWord, CMeaning, CPart, CPron, CNotes};
    for (std::size_t r = header ? 1 : 0; r < rows.size(); ++r) {
        Word w;
        for (std::size_t c = 0; c < rows[r].size() && c < cols.size(); ++c) {
            std::string* into = nullptr;
            switch (cols[c]) {
                case CWord: into = w.word.empty() ? &w.word : nullptr; break;
                case CMeaning: into = &w.meaning; break;
                case CPart: into = w.part.empty() ? &w.part : nullptr; break;
                case CPron: into = w.pronunciation.empty() ? &w.pronunciation : nullptr; break;
                case CNotes: into = &w.notes; break;
                default: break;
            }
            if (!into) continue;
            const std::string v = trim(rows[r][c]);
            if (v.empty()) continue;
            if (!into->empty()) *into += "; ";   // two meaning or notes columns: both kept
            *into += v;
        }
        addWord(l, std::move(w));
    }
    return !l.words.empty();
}

// --- SFM / MDF ---

bool readSfm(const std::string& text, Language& l) {
    Word w;
    bool have = false;
    auto flush = [&] {
        if (have) addWord(l, w);
        w = Word{};
        have = false;
    };
    std::string* last = nullptr;
    for (const std::string& raw : lines(text)) {
        if (raw.empty() || raw[0] != '\\') {
            if (last && !trim(raw).empty()) *last += " " + trim(raw);   // a field continued on the next line
            continue;
        }
        const auto sp = raw.find_first_of(" \t");
        const std::string marker = lowerAscii(raw.substr(1, sp == std::string::npos ? std::string::npos : sp - 1));
        const std::string value = sp == std::string::npos ? std::string() : trim(raw.substr(sp + 1));
        last = nullptr;
        if (marker == "lx") {
            flush();
            w.word = value;
            have = true;
            last = &w.word;
        } else if (!have) {
            continue;   // the file's header (\_sh ...) before the first entry
        } else if (marker == "ge" || marker == "gn" || marker == "de" || marker == "dn") {
            if (!(marker == "de" || marker == "dn") || w.meaning.empty()) {
                if (!w.meaning.empty()) w.meaning += "; ";
                w.meaning += value;
                last = &w.meaning;
            }
        } else if (marker == "ps" || marker == "pn") {
            if (w.part.empty()) w.part = value, last = &w.part;
        } else if (marker == "ph" || marker == "pr") {
            if (w.pronunciation.empty()) w.pronunciation = value, last = &w.pronunciation;
        } else if (marker == "nt" || marker == "et" || marker == "ee") {
            if (!w.notes.empty()) w.notes += " ";
            w.notes += value;
            last = &w.notes;
        }
    }
    flush();
    return !l.words.empty();
}

// --- plain word lists ---

bool readPlain(const std::string& text, Language& l) {
    for (const std::string& raw : lines(text)) {
        const std::string line = trim(raw);
        if (line.empty() || line[0] == '#') continue;
        Word w;
        std::size_t at = std::string::npos, len = 0;
        for (const char* sep : {"\t", " = ", " \xE2\x80\x94 ", " \xE2\x80\x93 ", " - ", ": "}) {   // tab, =, em dash, en dash, -, colon
            const auto p = line.find(sep);
            if (p != std::string::npos && p > 0) {
                at = p;
                len = std::strlen(sep);
                break;
            }
        }
        if (at == std::string::npos) {
            w.word = line;
        } else {
            w.word = line.substr(0, at);
            w.meaning = line.substr(at + len);
        }
        // A heading line ("word = meaning") isn't a word.
        if (l.words.empty() && columnFor(w.word) == CWord && (w.meaning.empty() ? false : columnFor(w.meaning) == CMeaning)) continue;
        addWord(l, std::move(w));
        if (l.words.size() >= kMaxWords) break;
    }
    return !l.words.empty();
}

bool looksSfm(const std::string& text) {
    int markers = 0, lx = 0;
    for (const std::string& raw : lines(text.substr(0, std::min<std::size_t>(text.size(), 20000)))) {
        if (raw.size() > 1 && raw[0] == '\\' && std::isalpha((unsigned char)raw[1])) ++markers;
        if (raw.rfind("\\lx ", 0) == 0) ++lx;
    }
    return lx > 0 && markers >= 2;
}

char sniffSeparator(const std::string& text) {
    const std::string head = text.substr(0, std::min<std::size_t>(text.size(), 4000));
    const std::string first = head.substr(0, head.find('\n'));
    const auto count = [&](char c) { return std::count(first.begin(), first.end(), c); };
    if (count('\t') > 0) return '\t';
    if (count(';') > count(',')) return ';';
    if (count(',') > 0) return ',';
    return 0;
}

std::string csvField(const std::string& v) {
    if (v.find_first_of(",\"\r\n") == std::string::npos) return v;
    std::string o = "\"";
    for (char c : v) o += c == '"' ? std::string("\"\"") : std::string(1, c);
    return o + "\"";
}

}  // namespace

std::string toJson(const std::vector<Language>& langs) {
    json arr = json::array();
    for (auto& l : langs) arr.push_back(languageJson(l, true));
    return json{{"version", 1}, {"languages", arr}}.dump();
}

std::vector<Language> fromJson(const std::string& text) {
    std::vector<Language> out;
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("languages") || !j["languages"].is_array()) return out;
    std::set<std::string> ids;
    for (auto& lj : j["languages"]) {
        if (!lj.is_object() || out.size() >= kMaxLanguages) continue;
        Language l = languageFrom(lj);
        if (l.name.empty()) continue;
        if (l.id.empty() || ids.count(l.id)) l.id = newId(out);
        ids.insert(l.id);
        out.push_back(std::move(l));
    }
    return out;
}

std::string newId(const std::vector<Language>& langs) {
    for (int n = 1;; ++n) {
        const std::string id = "lang" + std::to_string(n);
        if (std::none_of(langs.begin(), langs.end(), [&](const Language& l) { return l.id == id; })) return id;
    }
}

bool importFile(const std::string& fileName, const std::string& bytes, std::vector<Language>& out, std::string& error) {
    out.clear();
    if (bytes.size() > kMaxFileBytes) {
        error = "The file is too big to be a word list (over 32 MB).";
        return false;
    }
    const std::string text = asUtf8(bytes);
    const std::string ext = extensionOf(fileName);
    const std::string name = stemOf(fileName);
    // NorthRay's own file, or JSON that looks like it.
    const std::size_t firstChar = text.find_first_not_of(" \t\r\n");
    if (ext == ".nrlang" || ext == ".json" || (firstChar != std::string::npos && text[firstChar] == '{')) {
        const json j = json::parse(text, nullptr, false);
        if (!j.is_object()) {
            error = "The file isn't readable JSON.";
            return false;
        }
        std::vector<json> langs;
        if (j.contains("languages") && j["languages"].is_array())
            for (auto& l : j["languages"]) langs.push_back(l);
        else langs.push_back(j);
        for (auto& lj : langs) {
            if (!lj.is_object() || out.size() >= kMaxLanguages) continue;
            Language l = languageFrom(lj);
            l.id.clear();
            if (l.name.empty()) l.name = name;
            if (!l.words.empty()) out.push_back(std::move(l));
        }
        if (out.empty()) error = "There were no words in the file (it needs a \"words\" list).";
        return !out.empty();
    }
    Language l;
    l.name = name;
    bool ok = false;
    if (ext == ".sfm" || ext == ".db" || ext == ".mdf" || ext == ".lex" || looksSfm(text)) {
        ok = readSfm(text, l);
    } else if (ext == ".csv" || ext == ".tsv") {
        const char sep = ext == ".tsv" ? '\t' : sniffSeparator(text);
        ok = readDelimited(text, sep ? sep : ',', l);
    } else {
        // A text file: columns if its first line has separators and a header, otherwise a word list.
        const char sep = sniffSeparator(text);
        if (sep && sep != '\t') {
            const auto rows = parseDelimited(text.substr(0, text.find('\n')), sep);
            bool header = false;
            if (!rows.empty())
                for (auto& c : rows[0]) header = header || columnFor(c) == CWord;
            if (header) ok = readDelimited(text, sep, l);
        }
        if (!ok) ok = readPlain(text, l);
    }
    if (!ok) {
        error = "No words were found in the file. Writing Studio reads CSV or TSV word lists (a word and its meaning per row), "
                "SFM lexicons (\\lx), plain lists (a word a line) and its own .nrlang files.";
        return false;
    }
    out.push_back(std::move(l));
    return true;
}

std::string exportJson(const Language& lang) {
    json j = languageJson(lang, false);
    j["format"] = "northray-conlang";
    j["version"] = 1;
    return j.dump(2);
}

std::string exportCsv(const Language& lang) {
    std::string out = "\xEF\xBB\xBFword,meaning,part of speech,pronunciation,notes\r\n";
    for (auto& w : lang.words)
        out += csvField(w.word) + "," + csvField(w.meaning) + "," + csvField(w.part) + "," + csvField(w.pronunciation) + "," + csvField(w.notes) + "\r\n";
    return out;
}

std::size_t merge(Language& lang, const Language& incoming) {
    std::map<std::string, std::size_t> at;
    for (std::size_t i = 0; i < lang.words.size(); ++i) at.emplace(lowerAscii(lang.words[i].word), i);
    std::size_t added = 0;
    for (const Word& raw : incoming.words) {
        const Word w = cleanWord(raw);
        if (w.word.empty()) continue;
        auto it = at.find(lowerAscii(w.word));
        if (it == at.end()) {
            if (lang.words.size() >= kMaxWords) break;
            at.emplace(lowerAscii(w.word), lang.words.size());
            lang.words.push_back(w);
            ++added;
            continue;
        }
        Word& have = lang.words[it->second];
        if (have.meaning.empty()) have.meaning = w.meaning;
        if (have.part.empty()) have.part = w.part;
        if (have.pronunciation.empty()) have.pronunciation = w.pronunciation;
        if (have.notes.empty()) have.notes = w.notes;
    }
    if (lang.notes.empty()) lang.notes = incoming.notes;
    return added;
}

SpellData spellData(const Language& lang) {
    SpellData d;
    std::set<std::string> words, pre, suf;
    for (auto& w : lang.words) {
        std::string s = trim(w.word);
        if (s.empty()) continue;
        if (s.size() > 1 && s[0] == '-' && s.back() != '-') {   // "-ar": an ending
            suf.insert(s.substr(1));
            continue;
        }
        if (s.size() > 1 && s.back() == '-' && s[0] != '-') {   // "ve-": a beginning
            pre.insert(s.substr(0, s.size() - 1));
            continue;
        }
        if (s.size() > 2 && s[0] == '-' && s.back() == '-') {   // "-in-": an infix; its letters aren't a word
            continue;
        }
        words.insert(s);
        // Hyphenated or several words: each part is a word too (the spell checker sees them one by one).
        std::string part;
        for (char c : s + " ") {
            if (c == '-' || c == ' ') {
                if (!part.empty()) words.insert(part);
                part.clear();
            } else {
                part += c;
            }
        }
    }
    d.words.assign(words.begin(), words.end());
    d.prefixes.assign(pre.begin(), pre.end());
    d.suffixes.assign(suf.begin(), suf.end());
    return d;
}

}  // namespace studio::conlang
