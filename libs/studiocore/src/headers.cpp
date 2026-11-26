// Writing Studio: chapter header pictures (0.89). Portable and tested.
#include "studio/headers.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <set>

namespace studio::headers {

namespace {
using nlohmann::json;

std::string oneOf(const std::string& v, std::initializer_list<const char*> allowed, const char* fallback) {
    for (const char* a : allowed) if (v == a) return v;
    return fallback;
}

json toJ(const Header& h) {
    json j = json::object();
    if (h.none) { j["none"] = true; return j; }
    if (!h.blob.empty()) j["blob"] = h.blob;
    if (!h.symbol.empty()) j["symbol"] = h.symbol;
    j["size"] = h.size;
    j["title"] = h.title;
    if (h.title == "over") j["tone"] = h.tone;
    if (!h.alt.empty()) j["alt"] = h.alt;
    return j;
}

Header fromJ(const json& j) {
    Header h;
    if (!j.is_object()) return h;
    h.none = j.value("none", false);
    if (h.none) return h;
    if (auto b = j.find("blob"); b != j.end() && b->is_string()) {
        const std::string v = b->get<std::string>();
        // A picture is named by its SHA-256 (64 hex digits); anything else is left out.
        if (v.size() == 64 && std::all_of(v.begin(), v.end(), [](char c) { return std::isxdigit((unsigned char)c); })) h.blob = v;
    }
    if (auto s = j.find("symbol"); s != j.end() && s->is_string()) h.symbol = s->get<std::string>().substr(0, 64);
    h.size = oneOf(j.value("size", std::string()), {"full", "band", "ornament"}, "band");
    h.title = oneOf(j.value("title", std::string()), {"above", "below", "over"}, "below");
    h.tone = oneOf(j.value("tone", std::string()), {"dark", "fade"}, "dark");
    if (auto a = j.find("alt"); a != j.end() && a->is_string()) h.alt = a->get<std::string>();
    return h;
}

const Entry* liveEntry(const Library& lib, const Id& id) {
    if (id.empty()) return nullptr;
    const Entry* e = lib.entry(id);
    return e && !e->deleted ? e : nullptr;
}

// The chapter's viewpoint: its own, else its first scene's that has one.
Id viewpointOf(const Library& lib, const ManuscriptNode& n) {
    if (!n.povId.empty()) return n.povId;
    for (const ManuscriptNode* m : lib.manuscriptOf(n.bookId))
        if (m->parentId == n.id && m->kind == NodeKind::Scene && !m->povId.empty()) return m->povId;
    return {};
}
}  // namespace

std::string headerToJson(const Header& h) { return toJ(h).dump(); }
Header headerFromJson(const std::string& text) { return fromJ(json::parse(text, nullptr, false)); }

std::string toJson(const BookHeaders& b) {
    json j = json::object();
    if (!b.chapters.empty()) j["chapters"] = toJ(b.chapters);
    if (!b.parts.empty()) j["parts"] = toJ(b.parts);
    json n = json::object();
    for (auto& [id, h] : b.nodes) if (h.none || !h.empty()) n[id] = toJ(h);
    if (!n.empty()) j["nodes"] = n;
    return j.dump();
}

BookHeaders fromJson(const std::string& text) {
    BookHeaders b;
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return b;
    if (j.contains("chapters")) { b.chapters = fromJ(j["chapters"]); b.chapters.none = false; }
    if (j.contains("parts")) { b.parts = fromJ(j["parts"]); b.parts.none = false; }
    if (auto n = j.find("nodes"); n != j.end() && n->is_object())
        for (auto it = n->begin(); it != n->end(); ++it) {
            Header h = fromJ(*it);
            if (h.none || !h.empty()) b.nodes[it.key()] = h;
        }
    return b;
}

BookHeaders read(const Library& lib, const Id& book) { return fromJson(lib.setting("headers:" + book)); }

void write(Library& lib, const Id& book, const BookHeaders& b) {
    const std::string j = toJson(b);
    lib.setSetting("headers:" + book, j == "{}" ? std::string() : j);
}

bool printableSymbol(const std::string& s) {
    if (s.empty() || s.rfind("icon:", 0) == 0) return false;
    return std::any_of(s.begin(), s.end(), [](char c) { return !std::isspace((unsigned char)c); });
}

std::optional<Resolved> resolve(const Library& lib, const ManuscriptNode& n, const BookHeaders& b) {
    if (n.kind == NodeKind::Scene) return std::nullopt;
    if (auto it = b.nodes.find(n.id); it != b.nodes.end()) {
        if (it->second.none || it->second.empty()) return std::nullopt;
        return Resolved{it->second, "own", {}};
    }
    if (n.kind == NodeKind::Part) {
        if (b.parts.empty()) return std::nullopt;
        return Resolved{b.parts, "parts", {}};
    }
    if (n.matter != 0) return std::nullopt;   // front and back matter: only their own
    if (const Entry* v = liveEntry(lib, viewpointOf(lib, n))) {
        Header h;
        if (!v->headerBlob.empty()) {
            // Their picture, sized and placed as the book's chapters are (a band with the title under it by default).
            if (!b.chapters.empty()) h = b.chapters;
            h.blob = v->headerBlob;
            h.symbol.clear();
            if (h.size == "ornament" && b.chapters.blob.empty()) h.size = "band";
            h.alt.clear();
            return Resolved{h, "viewpoint", v->id};
        }
        if (printableSymbol(v->symbol)) {
            h.symbol = v->symbol;
            h.size = "ornament";
            h.title = "below";
            return Resolved{h, "viewpoint", v->id};
        }
    }
    if (b.chapters.empty()) return std::nullopt;
    return Resolved{b.chapters, "chapters", {}};
}

std::vector<std::string> blobsUsed(const Library& lib, const Id& book) {
    const BookHeaders b = read(lib, book);
    std::set<std::string> seen;
    std::vector<std::string> out;
    auto add = [&](const std::string& blob) { if (!blob.empty() && seen.insert(blob).second) out.push_back(blob); };
    for (const ManuscriptNode* n : lib.manuscriptOf(book))
        if (auto r = resolve(lib, *n, b)) add(r->header.blob);
    return out;
}

}  // namespace studio::headers
