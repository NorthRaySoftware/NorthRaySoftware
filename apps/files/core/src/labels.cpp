#include "nrf/labels.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cwctype>

namespace nrf::labels {

namespace {

const wchar_t* kNames[kColours] = {L"None", L"Red", L"Orange", L"Yellow", L"Green", L"Blue", L"Purple", L"Grey"};
const char* kKeys[kColours] = {"", "red", "orange", "yellow", "green", "blue", "purple", "grey"};

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

bool sameTag(const std::wstring& a, const std::wstring& b) { return lower(a) == lower(b); }

}  // namespace

const wchar_t* colourName(Colour c) { return kNames[std::clamp((int)c, 0, kColours - 1)]; }

std::wstring lowerName(Colour c) { return lower(colourName(c)); }

Colour colourFromName(const std::wstring& name) {
    const std::wstring l = lower(name);
    if (l == L"gray") return Colour::Grey;
    for (int i = 1; i < kColours; ++i)
        if (l == lower(kNames[i])) return (Colour)i;
    return Colour::None;
}

std::wstring normaliseTag(const std::wstring& tag) {
    std::wstring o;
    for (wchar_t c : tag) {
        if (iswspace(c) || c < 32) {
            if (!o.empty() && o.back() != L' ') o += L' ';
        } else {
            o += c;
        }
    }
    while (!o.empty() && o.back() == L' ') o.pop_back();
    if (o.size() > 40) o.resize(40);
    while (!o.empty() && o.back() == L' ') o.pop_back();
    return o;
}

std::wstring pathKey(const std::wstring& path) {
    std::wstring k = lower(path);
    while (k.size() > 3 && (k.back() == L'\\' || k.back() == L'/')) k.pop_back();
    return k;
}

bool within(const std::wstring& path, const std::wstring& folder) {
    const std::wstring p = pathKey(path), f = pathKey(folder);
    if (p == f) return true;
    if (p.size() <= f.size() || p.compare(0, f.size(), f) != 0) return false;
    return f.back() == L'\\' || p[f.size()] == L'\\';
}

Label& Store::at(const std::wstring& path) {
    Label& l = labels_[pathKey(path)];
    l.path = path;
    return l;
}

void Store::tidy(const std::wstring& key) {
    auto it = labels_.find(key);
    if (it != labels_.end() && it->second.empty()) labels_.erase(it);
}

const Label* Store::find(const std::wstring& path) const {
    auto it = labels_.find(pathKey(path));
    return it == labels_.end() ? nullptr : &it->second;
}

Colour Store::colourOf(const std::wstring& path) const {
    const Label* l = find(path);
    return l ? l->colour : Colour::None;
}

void Store::setColour(const std::wstring& path, Colour c) {
    if (colourOf(path) == c) return;
    at(path).colour = c;
    tidy(pathKey(path));
    ++changes_;
}

bool Store::addTag(const std::wstring& path, const std::wstring& tagIn) {
    const std::wstring tag = normaliseTag(tagIn);
    if (tag.empty() || hasTag(path, tag)) return false;
    // A tag already in use keeps the way it was first written.
    std::wstring written = tag;
    for (auto& [k, l] : labels_)
        for (auto& t : l.tags)
            if (sameTag(t, tag)) written = t;
    at(path).tags.push_back(written);
    ++changes_;
    return true;
}

bool Store::removeTag(const std::wstring& path, const std::wstring& tag) {
    auto it = labels_.find(pathKey(path));
    if (it == labels_.end()) return false;
    auto& v = it->second.tags;
    const auto n = v.size();
    v.erase(std::remove_if(v.begin(), v.end(), [&](const std::wstring& t) { return sameTag(t, tag); }), v.end());
    if (v.size() == n) return false;
    tidy(it->first);
    ++changes_;
    return true;
}

bool Store::hasTag(const std::wstring& path, const std::wstring& tag) const {
    const Label* l = find(path);
    if (!l) return false;
    return std::any_of(l->tags.begin(), l->tags.end(), [&](const std::wstring& t) { return sameTag(t, tag); });
}

std::vector<std::pair<std::wstring, int>> Store::tags() const {
    std::map<std::wstring, std::pair<std::wstring, int>> count;   // by lower case
    for (auto& [k, l] : labels_)
        for (auto& t : l.tags) {
            auto& c = count[lower(t)];
            if (c.first.empty()) c.first = t;
            ++c.second;
        }
    std::vector<std::pair<std::wstring, int>> out;
    for (auto& [k, v] : count) out.push_back(v);
    std::stable_sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.second > b.second; });
    return out;
}

std::vector<std::wstring> Store::pathsWithTag(const std::wstring& tag) const {
    std::vector<std::wstring> out;
    for (auto& [k, l] : labels_)
        if (std::any_of(l.tags.begin(), l.tags.end(), [&](const std::wstring& t) { return sameTag(t, tag); })) out.push_back(l.path);
    return out;
}

std::vector<std::wstring> Store::pathsWithColour(Colour c) const {
    std::vector<std::wstring> out;
    for (auto& [k, l] : labels_)
        if (l.colour == c && c != Colour::None) out.push_back(l.path);
    return out;
}

std::vector<const Label*> Store::all() const {
    std::vector<const Label*> out;
    for (auto& [k, l] : labels_) out.push_back(&l);
    return out;
}

void Store::moved(const std::wstring& from, const std::wstring& to) {
    if (pathKey(from) == pathKey(to) && from == to) return;
    auto rebase = [&](const std::wstring& p) { return to + p.substr(pathKey(from).size()); };
    std::vector<Label> moving;
    for (auto it = labels_.begin(); it != labels_.end();) {
        if (within(it->second.path, from)) {
            Label l = it->second;
            l.path = rebase(l.path);
            moving.push_back(std::move(l));
            it = labels_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& l : moving) labels_[pathKey(l.path)] = std::move(l);
    for (auto& c : collections_)
        for (auto& p : c.items)
            if (within(p, from)) p = rebase(p);
    ++changes_;
}

void Store::removed(const std::wstring& path) {
    for (auto it = labels_.begin(); it != labels_.end();) {
        if (within(it->second.path, path)) it = labels_.erase(it);
        else ++it;
    }
    for (auto& c : collections_)
        c.items.erase(std::remove_if(c.items.begin(), c.items.end(), [&](const std::wstring& p) { return within(p, path); }), c.items.end());
    ++changes_;
}

const Collection* Store::collection(const std::string& id) const {
    for (auto& c : collections_)
        if (c.id == id) return &c;
    return nullptr;
}

std::string Store::createCollection(const std::wstring& nameIn) {
    std::wstring name = normaliseTag(nameIn);
    if (name.empty()) name = L"Collection";
    Collection c;
    for (;;) {
        c.id = "c" + std::to_string(nextId_++);
        if (!collection(c.id)) break;
    }
    c.name = name;
    collections_.push_back(c);
    ++changes_;
    return c.id;
}

bool Store::renameCollection(const std::string& id, const std::wstring& nameIn) {
    const std::wstring name = normaliseTag(nameIn);
    if (name.empty()) return false;
    for (auto& c : collections_)
        if (c.id == id) { c.name = name; ++changes_; return true; }
    return false;
}

bool Store::deleteCollection(const std::string& id) {
    const auto n = collections_.size();
    collections_.erase(std::remove_if(collections_.begin(), collections_.end(), [&](const Collection& c) { return c.id == id; }),
                       collections_.end());
    if (collections_.size() == n) return false;
    ++changes_;
    return true;
}

int Store::addToCollection(const std::string& id, const std::vector<std::wstring>& paths) {
    for (auto& c : collections_) {
        if (c.id != id) continue;
        int added = 0;
        for (auto& p : paths) {
            if (std::any_of(c.items.begin(), c.items.end(), [&](const std::wstring& x) { return pathKey(x) == pathKey(p); })) continue;
            c.items.push_back(p);
            ++added;
        }
        if (added) ++changes_;
        return added;
    }
    return 0;
}

int Store::removeFromCollection(const std::string& id, const std::vector<std::wstring>& paths) {
    for (auto& c : collections_) {
        if (c.id != id) continue;
        const auto n = c.items.size();
        c.items.erase(std::remove_if(c.items.begin(), c.items.end(),
                                     [&](const std::wstring& x) {
                                         return std::any_of(paths.begin(), paths.end(), [&](const std::wstring& p) { return pathKey(x) == pathKey(p); });
                                     }),
                      c.items.end());
        const int removed = (int)(n - c.items.size());
        if (removed) ++changes_;
        return removed;
    }
    return 0;
}

std::vector<std::string> Store::collectionsWith(const std::wstring& path) const {
    std::vector<std::string> out;
    for (auto& c : collections_)
        if (std::any_of(c.items.begin(), c.items.end(), [&](const std::wstring& x) { return pathKey(x) == pathKey(path); })) out.push_back(c.id);
    return out;
}

std::string Store::toJson() const {
    nlohmann::json j;
    j["version"] = 1;
    j["nextId"] = nextId_;
    auto& files = j["files"] = nlohmann::json::array();
    for (auto& [k, l] : labels_) {
        nlohmann::json f;
        f["path"] = toUtf8(l.path);
        if (l.colour != Colour::None) f["colour"] = kKeys[(int)l.colour];
        if (!l.tags.empty()) {
            auto& t = f["tags"] = nlohmann::json::array();
            for (auto& x : l.tags) t.push_back(toUtf8(x));
        }
        files.push_back(f);
    }
    auto& cols = j["collections"] = nlohmann::json::array();
    for (auto& c : collections_) {
        nlohmann::json o;
        o["id"] = c.id;
        o["name"] = toUtf8(c.name);
        auto& items = o["items"] = nlohmann::json::array();
        for (auto& p : c.items) items.push_back(toUtf8(p));
        cols.push_back(o);
    }
    return j.dump(1);
}

bool Store::fromJson(const std::string& text) {
    labels_.clear();
    collections_.clear();
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    if (j.contains("nextId") && j["nextId"].is_number_unsigned()) nextId_ = j["nextId"].get<std::uint64_t>();
    if (j.contains("files") && j["files"].is_array())
        for (auto& f : j["files"]) {
            if (!f.is_object() || !f.contains("path") || !f["path"].is_string()) continue;
            Label l;
            l.path = fromUtf8(f["path"].get<std::string>());
            if (f.contains("colour") && f["colour"].is_string()) {
                const std::string c = f["colour"].get<std::string>();
                for (int i = 1; i < kColours; ++i)
                    if (c == kKeys[i]) l.colour = (Colour)i;
            }
            if (f.contains("tags") && f["tags"].is_array())
                for (auto& t : f["tags"])
                    if (t.is_string()) {
                        const std::wstring tag = normaliseTag(fromUtf8(t.get<std::string>()));
                        if (!tag.empty()) l.tags.push_back(tag);
                    }
            if (!l.path.empty() && !l.empty()) labels_[pathKey(l.path)] = std::move(l);
        }
    if (j.contains("collections") && j["collections"].is_array())
        for (auto& o : j["collections"]) {
            if (!o.is_object() || !o.contains("id") || !o["id"].is_string()) continue;
            Collection c;
            c.id = o["id"].get<std::string>();
            if (o.contains("name") && o["name"].is_string()) c.name = fromUtf8(o["name"].get<std::string>());
            if (c.name.empty()) c.name = L"Collection";
            if (o.contains("items") && o["items"].is_array())
                for (auto& p : o["items"])
                    if (p.is_string()) c.items.push_back(fromUtf8(p.get<std::string>()));
            if (!collection(c.id)) collections_.push_back(std::move(c));
        }
    return true;
}

std::string toUtf8(const std::wstring& w) {
    std::string o;
    for (std::size_t i = 0; i < w.size(); ++i) {
        std::uint32_t cp = (std::uint32_t)w[i];
        if (sizeof(wchar_t) == 2 && cp >= 0xD800 && cp < 0xDC00 && i + 1 < w.size()) {
            const std::uint32_t lo = (std::uint32_t)w[i + 1];
            if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); ++i; }
        }
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        else {
            o += (char)(0xF0 | (cp >> 18));
            o += (char)(0x80 | ((cp >> 12) & 0x3F));
            o += (char)(0x80 | ((cp >> 6) & 0x3F));
            o += (char)(0x80 | (cp & 0x3F));
        }
    }
    return o;
}

std::wstring fromUtf8(const std::string& s) {
    std::wstring o;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        std::uint32_t cp;
        int n;
        if (c < 0x80) { cp = c; n = 0; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; n = 1; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; n = 2; }
        else if ((c >> 3) == 30) { cp = c & 0x07; n = 3; }
        else { ++i; o += L'�'; continue; }
        if (i + (std::size_t)n >= s.size() + (n == 0 ? 1 : 0) && n > 0 && i + (std::size_t)n > s.size() - 1) { o += L'�'; break; }
        for (int k = 1; k <= n; ++k) cp = (cp << 6) | ((unsigned char)s[i + (std::size_t)k] & 0x3F);
        i += (std::size_t)n + 1;
        if (cp >= 0x10000 && sizeof(wchar_t) == 2) {
            cp -= 0x10000;
            o += (wchar_t)(0xD800 + (cp >> 10));
            o += (wchar_t)(0xDC00 + (cp & 0x3FF));
        } else {
            o += (wchar_t)cp;
        }
    }
    return o;
}

}  // namespace nrf::labels
