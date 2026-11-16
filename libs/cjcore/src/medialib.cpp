#include "cj/medialib.hpp"

#include <miniz.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

namespace cj::medialib {

using nlohmann::json;

namespace {
const char* const kKeys[] = {"picture", "video", "music", "sound", "design", "other"};
const char* const kNames[] = {"Picture", "Video", "Music", "Sound", "Overlay design", "Other"};

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
std::string extOf(const std::string& f) {
    const auto slash = f.find_last_of("\\/");
    const auto dot = f.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    return lower(f.substr(dot));
}
bool within(const std::string& folder, const std::string& top) {   // folder is top or inside it
    return folder == top || (folder.size() > top.size() && folder.compare(0, top.size(), top) == 0 && folder[top.size()] == '/');
}
}  // namespace

const char* kindKey(Kind k) { return kKeys[(int)k]; }
const char* kindName(Kind k) { return kNames[(int)k]; }

bool kindFromKey(const std::string& key, Kind& out) {
    for (int i = 0; i < 6; ++i)
        if (key == kKeys[i]) {
            out = (Kind)i;
            return true;
        }
    return false;
}

Kind kindOf(const std::string& fileName) {
    static const std::set<std::string> pics = {".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp", ".tif", ".tiff", ".heic"};
    static const std::set<std::string> vids = {".mp4", ".mov", ".mkv", ".avi", ".wmv", ".webm", ".m4v"};
    static const std::set<std::string> auds = {".mp3", ".wav", ".flac", ".ogg", ".m4a", ".aac", ".wma", ".opus"};
    const std::string e = extOf(fileName);
    if (pics.count(e)) return Kind::Picture;
    if (vids.count(e)) return Kind::Video;
    if (auds.count(e)) return Kind::Music;
    if (e == ".nrdesign") return Kind::Design;
    return Kind::Other;
}

std::string Library::cleanFolder(const std::string& path) {
    std::string out, part;
    auto flush = [&] {
        const std::string t = trim(part);
        part.clear();
        if (t.empty() || t == "." || t == "..") return;
        if (!out.empty()) out += '/';
        out += t;
    };
    for (char c : path) {
        if (c == '/' || c == '\\') flush();
        else part += c;
    }
    flush();
    return out.substr(0, 400);
}

std::string Library::cleanTag(const std::string& tag) {
    std::string t = lower(trim(tag));
    if (t.size() > 40) {
        std::size_t cut = 40;
        while (cut > 0 && (static_cast<unsigned char>(t[cut]) & 0xC0) == 0x80) --cut;
        t = trim(t.substr(0, cut));
    }
    return t;
}

const Item* Library::item(const std::string& id) const {
    for (auto& i : items_)
        if (i.id == id) return &i;
    return nullptr;
}

std::string Library::storedName(const std::string& original) const {
    std::string base = original;
    const auto slash = base.find_last_of("\\/");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    for (auto& c : base)
        if (std::string("<>:\"|?*").find(c) != std::string::npos || (unsigned char)c < 32) c = '_';
    base = trim(base);
    if (base.empty() || base == "." || base == "..") base = "file";
    const std::string ext = extOf(base);
    const std::string stem = base.substr(0, base.size() - ext.size());
    auto taken = [&](const std::string& n) {
        const std::string ln = lower(n);
        return std::any_of(items_.begin(), items_.end(), [&](const Item& i) { return lower(i.file) == ln; });
    };
    if (!taken(base)) return base;
    for (int n = 2;; ++n) {
        const std::string c = stem + " (" + std::to_string(n) + ")" + ext;
        if (!taken(c)) return c;
    }
}

std::string Library::add(const std::string& storedFile, const std::string& name, Kind kind, const std::string& folder, const std::string& day) {
    if (storedFile.empty() || storedName(storedFile) != storedFile) return {};   // already used (or not a clean name)
    Item i;
    i.id = "m" + std::to_string(next_++);
    i.file = storedFile;
    i.name = trim(name).empty() ? storedFile : trim(name).substr(0, 200);
    i.kind = kind;
    i.folder = cleanFolder(folder);
    i.added = day;
    items_.push_back(i);
    return i.id;
}

bool Library::remove(const std::string& id) {
    const auto n = items_.size();
    items_.erase(std::remove_if(items_.begin(), items_.end(), [&](const Item& i) { return i.id == id; }), items_.end());
    return items_.size() != n;
}

namespace {
template <class F>
bool change(std::vector<Item>& items, const std::string& id, F f) {
    for (auto& i : items)
        if (i.id == id) return f(i);
    return false;
}
}  // namespace

bool Library::rename(const std::string& id, const std::string& name) {
    const std::string n = trim(name);
    if (n.empty()) return false;
    return change(items_, id, [&](Item& i) {
        i.name = n.substr(0, 200);
        return true;
    });
}

bool Library::setKind(const std::string& id, Kind kind) {
    return change(items_, id, [&](Item& i) {
        i.kind = kind;
        return true;
    });
}

bool Library::moveTo(const std::string& id, const std::string& folder) {
    return change(items_, id, [&](Item& i) {
        i.folder = cleanFolder(folder);
        return true;
    });
}

bool Library::addTag(const std::string& id, const std::string& tag) {
    const std::string t = cleanTag(tag);
    if (t.empty()) return false;
    return change(items_, id, [&](Item& i) {
        if (std::find(i.tags.begin(), i.tags.end(), t) != i.tags.end() || i.tags.size() >= 30) return false;
        i.tags.push_back(t);
        std::sort(i.tags.begin(), i.tags.end());
        return true;
    });
}

bool Library::removeTag(const std::string& id, const std::string& tag) {
    const std::string t = cleanTag(tag);
    return change(items_, id, [&](Item& i) {
        const auto n = i.tags.size();
        i.tags.erase(std::remove(i.tags.begin(), i.tags.end(), t), i.tags.end());
        return i.tags.size() != n;
    });
}

std::vector<std::string> Library::folders() const {
    std::set<std::string> all;
    auto withParents = [&](const std::string& f) {
        for (std::size_t p = 0; (p = f.find('/', p)) != std::string::npos; ++p) all.insert(f.substr(0, p));
        if (!f.empty()) all.insert(f);
    };
    for (auto& f : folders_) withParents(f);
    for (auto& i : items_) withParents(i.folder);
    return {all.begin(), all.end()};
}

bool Library::addFolder(const std::string& path) {
    const std::string f = cleanFolder(path);
    if (f.empty()) return false;
    return folders_.insert(f).second;
}

bool Library::renameFolder(const std::string& fromIn, const std::string& toIn) {
    const std::string from = cleanFolder(fromIn), to = cleanFolder(toIn);
    if (from.empty() || to.empty() || from == to || within(to, from)) return false;   // not into itself
    const auto fs = folders();
    if (std::find(fs.begin(), fs.end(), from) == fs.end()) return false;
    auto moved = [&](const std::string& f) { return within(f, from) ? to + f.substr(from.size()) : f; };
    for (auto& i : items_) i.folder = moved(i.folder);
    std::set<std::string> nf;
    for (auto& f : folders_) nf.insert(moved(f));
    folders_ = nf;
    folders_.insert(to);
    return true;
}

bool Library::removeFolder(const std::string& pathIn) {
    const std::string path = cleanFolder(pathIn);
    if (path.empty()) return false;
    const auto slash = path.find_last_of('/');
    const std::string parent = slash == std::string::npos ? std::string() : path.substr(0, slash);
    bool any = false;
    // What was directly inside moves up a level; folders inside keep their place under the parent.
    auto up = [&](const std::string& f) -> std::string {
        if (!within(f, path)) return f;
        any = true;
        const std::string rest = f.size() > path.size() ? f.substr(path.size() + 1) : std::string();
        return parent.empty() ? rest : rest.empty() ? parent : parent + "/" + rest;
    };
    for (auto& i : items_) i.folder = up(i.folder);
    std::set<std::string> nf;
    for (auto& f : folders_) {
        const std::string u = up(f);
        if (!u.empty() && f != path) nf.insert(u);
    }
    folders_ = nf;
    return any;
}

std::vector<const Item*> Library::search(const Query& q) const {
    std::vector<std::string> words;
    {
        std::string w;
        for (char c : lower(q.text) + " ") {
            if (std::isspace((unsigned char)c)) {
                if (!w.empty()) words.push_back(w);
                w.clear();
            } else w += c;
        }
    }
    const std::string folder = cleanFolder(q.folder), tag = cleanTag(q.tag);
    std::vector<const Item*> out;
    for (auto& i : items_) {
        if (q.kind >= 0 && (int)i.kind != q.kind) continue;
        if (!folder.empty() && !within(i.folder, folder)) continue;
        if (!tag.empty() && std::find(i.tags.begin(), i.tags.end(), tag) == i.tags.end()) continue;
        std::string hay = lower(i.name + " " + i.folder + " " + i.file);
        for (auto& t : i.tags) hay += " " + t;
        if (!std::all_of(words.begin(), words.end(), [&](const std::string& w) { return hay.find(w) != std::string::npos; })) continue;
        out.push_back(&i);
    }
    std::stable_sort(out.begin(), out.end(), [](const Item* a, const Item* b) {
        if (a->added != b->added) return a->added > b->added;
        return lower(a->name) < lower(b->name);
    });
    return out;
}

std::map<std::string, int> Library::tags() const {
    std::map<std::string, int> m;
    for (auto& i : items_)
        for (auto& t : i.tags) ++m[t];
    return m;
}

std::string Library::toJson() const {
    json items = json::array();
    for (auto& i : items_)
        items.push_back({{"id", i.id}, {"file", i.file}, {"name", i.name}, {"kind", kindKey(i.kind)}, {"folder", i.folder}, {"tags", i.tags}, {"added", i.added}});
    return json{{"format", "northray-media-library"}, {"version", 1}, {"next", next_}, {"folders", folders_}, {"items", items}}.dump(1);
}

bool Library::fromJson(const std::string& text, Library& out) {
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object() || j.value("format", "") != "northray-media-library") return false;
    Library l;
    if (j.contains("folders") && j["folders"].is_array())
        for (auto& f : j["folders"])
            if (f.is_string() && !cleanFolder(f.get<std::string>()).empty()) l.folders_.insert(cleanFolder(f.get<std::string>()));
    int highest = 0;
    if (j.contains("items") && j["items"].is_array())
        for (auto& e : j["items"]) {
            if (!e.is_object()) continue;
            auto str = [&](const char* k) { return e.contains(k) && e[k].is_string() ? e[k].get<std::string>() : std::string(); };
            Item i;
            i.id = str("id");
            i.file = str("file");
            if (i.id.size() < 2 || i.id[0] != 'm' || i.file.empty() || l.item(i.id) || l.storedName(i.file) != i.file) continue;   // bad or repeated
            i.name = str("name").empty() ? i.file : str("name").substr(0, 200);
            if (!kindFromKey(str("kind"), i.kind)) i.kind = kindOf(i.file);
            i.folder = cleanFolder(str("folder"));
            if (e.contains("tags") && e["tags"].is_array())
                for (auto& t : e["tags"])
                    if (t.is_string()) {
                        const std::string c = cleanTag(t.get<std::string>());
                        if (!c.empty() && std::find(i.tags.begin(), i.tags.end(), c) == i.tags.end()) i.tags.push_back(c);
                    }
            std::sort(i.tags.begin(), i.tags.end());
            i.added = str("added").substr(0, 10);
            highest = std::max(highest, std::atoi(i.id.c_str() + 1));
            l.items_.push_back(i);
        }
    l.next_ = std::max(highest + 1, j.contains("next") && j["next"].is_number_integer() ? j["next"].get<int>() : 1);
    out = l;
    return true;
}

}  // namespace cj::medialib

// ------------------------------------------------------------------ template packs


namespace cj::medialib {

namespace {
bool unzipEntry(mz_zip_archive& z, const char* name, std::string& out, std::size_t limit) {
    const int i = mz_zip_reader_locate_file(&z, name, nullptr, 0);
    if (i < 0) return false;
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&z, (mz_uint)i, &st) || st.m_uncomp_size > limit) return false;
    out.resize((std::size_t)st.m_uncomp_size);
    return out.empty() || mz_zip_reader_extract_to_mem(&z, (mz_uint)i, out.data(), out.size(), 0);
}
}  // namespace

std::string buildPack(const Library& lib, const std::string& folderIn, const std::string& name,
                      const std::function<bool(const std::string&, std::string&)>& read, PackResult& result) {
    result = {};
    const std::string folder = Library::cleanFolder(folderIn);
    mz_zip_archive z{};
    if (!mz_zip_writer_init_heap(&z, 0, 1 << 16)) {
        result.error = "The pack couldn't be started.";
        return {};
    }
    json items = json::array();
    std::size_t total = 0;
    int n = 0;
    for (auto& it : lib.items()) {
        if (!folder.empty() && !(it.folder == folder || (it.folder.size() > folder.size() && it.folder.compare(0, folder.size(), folder) == 0 &&
                                                         it.folder[folder.size()] == '/')))
            continue;
        if ((std::size_t)n >= kPackMaxItems) break;
        std::string bytes;
        if (!read(it.file, bytes)) continue;
        total += bytes.size();
        if (total > kPackMaxBytes) {
            result.error = "The pack would be over 1 GB.";
            mz_zip_writer_end(&z);
            return {};
        }
        const std::string entry = "files/" + std::to_string(n) + "-" + it.file;
        if (!mz_zip_writer_add_mem(&z, entry.c_str(), bytes.data(), bytes.size(), MZ_DEFAULT_COMPRESSION)) continue;
        const std::string rel = folder.empty() ? it.folder : it.folder.size() > folder.size() ? it.folder.substr(folder.size() + 1) : std::string();
        items.push_back({{"entry", entry}, {"file", it.file}, {"name", it.name}, {"kind", kindKey(it.kind)}, {"folder", rel}, {"tags", it.tags}});
        ++n;
    }
    if (n == 0) {
        result.error = "There's nothing in that folder to pack.";
        mz_zip_writer_end(&z);
        return {};
    }
    const std::string manifest = json{{"format", "northray-pack"}, {"version", 1}, {"name", name.substr(0, 120)}, {"items", items}}.dump(1);
    void* buf = nullptr;
    std::size_t size = 0;
    if (!mz_zip_writer_add_mem(&z, "pack.json", manifest.data(), manifest.size(), MZ_DEFAULT_COMPRESSION) ||
        !mz_zip_writer_finalize_heap_archive(&z, &buf, &size)) {
        result.error = "The pack couldn't be made.";
        mz_zip_writer_end(&z);
        return {};
    }
    std::string out(static_cast<const char*>(buf), size);
    mz_free(buf);   // finishing hands the buffer over
    mz_zip_writer_end(&z);
    result.ok = true;
    result.count = n;
    return out;
}

std::string packName(const std::string& bytes) {
    mz_zip_archive z{};
    if (!mz_zip_reader_init_mem(&z, bytes.data(), bytes.size(), 0)) return {};
    std::string m;
    const bool ok = unzipEntry(z, "pack.json", m, 16u * 1024u * 1024u);
    mz_zip_reader_end(&z);
    if (!ok) return {};
    const json j = json::parse(m, nullptr, false);
    if (!j.is_object() || j.value("format", "") != "northray-pack") return {};
    const std::string n = j.contains("name") && j["name"].is_string() ? j["name"].get<std::string>() : std::string();
    return n.empty() ? std::string("Pack") : n;
}

PackResult readPack(Library& lib, const std::string& bytes, const std::string& intoIn, const std::string& day,
                    const std::function<bool(const std::string&, const std::string&)>& write) {
    PackResult r;
    mz_zip_archive z{};
    if (!mz_zip_reader_init_mem(&z, bytes.data(), bytes.size(), 0)) {
        r.error = "That isn't a NorthRay pack (it isn't a zip).";
        return r;
    }
    std::string m;
    if (!unzipEntry(z, "pack.json", m, 16u * 1024u * 1024u)) {
        mz_zip_reader_end(&z);
        r.error = "That isn't a NorthRay pack (there's no pack.json).";
        return r;
    }
    const json j = json::parse(m, nullptr, false);
    if (!j.is_object() || j.value("format", "") != "northray-pack" || !j.contains("items") || !j["items"].is_array()) {
        mz_zip_reader_end(&z);
        r.error = "That isn't a NorthRay pack.";
        return r;
    }
    const std::string into = Library::cleanFolder(intoIn);
    std::size_t total = 0;
    for (auto& e : j["items"]) {
        if ((std::size_t)r.count >= kPackMaxItems) break;
        if (!e.is_object()) continue;
        auto str = [&](const char* k) { return e.contains(k) && e[k].is_string() ? e[k].get<std::string>() : std::string(); };
        const std::string entry = str("entry");
        if (entry.rfind("files/", 0) != 0) continue;   // only the pack's own files
        std::string data;
        if (!unzipEntry(z, entry.c_str(), data, kPackMaxBytes - total)) continue;
        total += data.size();
        const std::string stored = lib.storedName(str("file").empty() ? entry.substr(6) : str("file"));
        if (!write(stored, data)) continue;
        Kind kind;
        if (!kindFromKey(str("kind"), kind)) kind = kindOf(stored);
        const std::string rel = Library::cleanFolder(str("folder"));
        const std::string id = lib.add(stored, str("name"), kind, into.empty() ? rel : rel.empty() ? into : into + "/" + rel, day);
        if (id.empty()) continue;
        if (e.contains("tags") && e["tags"].is_array())
            for (auto& t : e["tags"])
                if (t.is_string()) lib.addTag(id, t.get<std::string>());
        ++r.count;
    }
    mz_zip_reader_end(&z);
    if (!into.empty()) lib.addFolder(into);
    r.ok = r.count > 0;
    if (!r.ok) r.error = "The pack had nothing that could be added.";
    return r;
}

}  // namespace cj::medialib
