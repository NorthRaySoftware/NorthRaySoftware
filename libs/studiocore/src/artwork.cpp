// Artwork library, image headers and print maths.
#include "studio/artwork.hpp"
#include "studio/library.hpp"

#include "internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <nlohmann/json.hpp>
#include <regex>
#include <set>
#include <sstream>

namespace studio {

using namespace detail;
using nlohmann::json;

// ------------------------------------------------------------- image files

namespace {
std::uint32_t be16(const std::string& b, size_t i) { return ((std::uint8_t)b[i] << 8) | (std::uint8_t)b[i + 1]; }
std::uint32_t be32(const std::string& b, size_t i) { return (be16(b, i) << 16) | be16(b, i + 2); }
std::uint32_t le16(const std::string& b, size_t i) { return (std::uint8_t)b[i] | ((std::uint8_t)b[i + 1] << 8); }
std::uint32_t le24(const std::string& b, size_t i) { return le16(b, i) | ((std::uint8_t)b[i + 2] << 16); }
std::uint32_t le32(const std::string& b, size_t i) { return le16(b, i) | (le16(b, i + 2) << 16); }
}  // namespace

ImageInfo imageInfo(const std::string& b) {
    ImageInfo r;
    auto done = [&](std::int64_t w, std::int64_t h, const char* f) {
        if (w > 0 && h > 0 && w < 1000000 && h < 1000000) { r.ok = true; r.width = (int)w; r.height = (int)h; r.format = f; }
        return r;
    };
    if (b.size() >= 24 && b.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0 && b.compare(12, 4, "IHDR") == 0)
        return done(be32(b, 16), be32(b, 20), "png");
    if (b.size() >= 10 && (b.compare(0, 6, "GIF87a") == 0 || b.compare(0, 6, "GIF89a") == 0))
        return done(le16(b, 6), le16(b, 8), "gif");
    if (b.size() >= 26 && b.compare(0, 2, "BM") == 0) {
        const std::int32_t h = (std::int32_t)le32(b, 22);
        return done((std::int32_t)le32(b, 18), h < 0 ? -(std::int64_t)h : h, "bmp");
    }
    if (b.size() >= 30 && b.compare(0, 4, "RIFF") == 0 && b.compare(8, 4, "WEBP") == 0) {
        const std::string chunk = b.substr(12, 4);
        if (chunk == "VP8X") return done(le24(b, 24) + 1, le24(b, 27) + 1, "webp");
        if (chunk == "VP8L" && (std::uint8_t)b[20] == 0x2f) {
            const std::uint32_t v = le32(b, 21);
            return done((v & 0x3FFF) + 1, ((v >> 14) & 0x3FFF) + 1, "webp");
        }
        if (chunk == "VP8 " && b.size() >= 30) return done(le16(b, 26) & 0x3FFF, le16(b, 28) & 0x3FFF, "webp");
        return r;
    }
    if (b.size() >= 4 && (std::uint8_t)b[0] == 0xFF && (std::uint8_t)b[1] == 0xD8) {
        // Walk the JPEG markers to the first start-of-frame.
        size_t i = 2;
        while (i + 9 < b.size()) {
            if ((std::uint8_t)b[i] != 0xFF) { ++i; continue; }
            const std::uint8_t m = (std::uint8_t)b[i + 1];
            if (m == 0xFF) { ++i; continue; }
            if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
            const std::uint32_t len = be16(b, i + 2);
            if (len < 2) break;
            const bool sof = (m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 && m != 0xCC;
            if (sof) return done(be16(b, i + 7), be16(b, i + 5), "jpeg");
            i += 2 + len;
        }
    }
    return r;
}

// --------------------------------------------------------------- trim sizes

const std::vector<TrimSize>& trimSizes() {
    static const std::vector<TrimSize> v = {
        {"6x9in", "6 × 9 in (US trade)", 6.0, 9.0},
        {"5.5x8.5in", "5.5 × 8.5 in", 5.5, 8.5},
        {"5.25x8in", "5.25 × 8 in", 5.25, 8.0},
        {"5x8in", "5 × 8 in", 5.0, 8.0},
        {"b-format", "B-format, 129 × 198 mm", 129 / 25.4, 198 / 25.4},
        {"a5", "A5, 148 × 210 mm", 148 / 25.4, 210 / 25.4},
        {"royal", "Royal, 156 × 234 mm", 156 / 25.4, 234 / 25.4},
        {"8.5x11in", "8.5 × 11 in (large format)", 8.5, 11.0},
    };
    return v;
}
const TrimSize& trimSize(const std::string& id) {
    for (auto& t : trimSizes()) if (id == t.id) return t;
    return trimSizes()[0];
}
double textWidthInches(const std::string& trimId) { return std::max(1.0, trimSize(trimId).widthIn - 1.25); }

// --------------------------------------------------------------- placement

Placement clampPlacement(Placement p) {
    auto c = [](double v, double lo, double hi) { return std::isfinite(v) ? std::clamp(v, lo, hi) : lo; };
    p.cropW = c(p.cropW, 0.05, 1);
    p.cropH = c(p.cropH, 0.05, 1);
    p.cropX = c(p.cropX, 0, 1 - p.cropW);
    p.cropY = c(p.cropY, 0, 1 - p.cropH);
    p.width = c(p.width, 0.1, 1);
    if (p.align != "left" && p.align != "right") p.align = "center";
    return p;
}

PrintSize printSize(int pw, int ph, const Placement& in, double textWidthIn) {
    const Placement p = clampPlacement(in);
    PrintSize s;
    if (pw <= 0 || ph <= 0 || textWidthIn <= 0) return s;
    s.widthIn = p.width * textWidthIn;
    const double cropPxW = pw * p.cropW, cropPxH = ph * p.cropH;
    s.heightIn = s.widthIn * cropPxH / cropPxW;
    s.ppi = cropPxW / s.widthIn;
    return s;
}

PrintQuality printQuality(double ppi) {
    if (ppi >= 299.5) return PrintQuality::Good;
    if (ppi >= 150) return PrintQuality::Soft;
    return PrintQuality::Poor;
}
const char* describe(PrintQuality q) {
    switch (q) {
        case PrintQuality::Good: return "Sharp in print (300 ppi or more).";
        case PrintQuality::Soft: return "May look soft in print: below the 300 ppi printers ask for.";
        case PrintQuality::Poor: return "Will look blurred or blocky in print: below 150 ppi.";
    }
    return "";
}
double sharpWidthFraction(int pixelWidth, double cropW, double textWidthIn) {
    if (pixelWidth <= 0 || textWidthIn <= 0) return 0;
    return std::min(1.0, pixelWidth * std::clamp(cropW, 0.05, 1.0) / 300.0 / textWidthIn);
}

// ------------------------------------------------------------------- units

const char* toString(Unit u) {
    switch (u) {
        case Unit::Inches: return "in";
        case Unit::Centimetres: return "cm";
        case Unit::Millimetres: return "mm";
        case Unit::Pixels: return "px";
    }
    return "in";
}
Unit unitFrom(const std::string& s) {
    if (s == "cm") return Unit::Centimetres;
    if (s == "mm") return Unit::Millimetres;
    if (s == "px") return Unit::Pixels;
    return Unit::Inches;
}
std::string formatLength(double inches, Unit u, double ppi) {
    char buf[48];
    switch (u) {
        case Unit::Inches: std::snprintf(buf, sizeof buf, "%.2f in", inches); break;
        case Unit::Centimetres: std::snprintf(buf, sizeof buf, "%.1f cm", inches * 2.54); break;
        case Unit::Millimetres: std::snprintf(buf, sizeof buf, "%.0f mm", inches * 25.4); break;
        case Unit::Pixels: std::snprintf(buf, sizeof buf, "%.0f px", inches * ppi); break;
    }
    return buf;
}

// ------------------------------------------------------------------- kinds

const char* toString(ArtworkKind k) {
    switch (k) {
        case ArtworkKind::Illustration: return "illustration";
        case ArtworkKind::Map: return "map";
        case ArtworkKind::Ornament: return "ornament";
        case ArtworkKind::Photo: return "photo";
        case ArtworkKind::Diagram: return "diagram";
        case ArtworkKind::Other: return "other";
    }
    return "illustration";
}
const char* label(ArtworkKind k) {
    switch (k) {
        case ArtworkKind::Illustration: return "Illustration";
        case ArtworkKind::Map: return "Map";
        case ArtworkKind::Ornament: return "Ornament";
        case ArtworkKind::Photo: return "Photograph";
        case ArtworkKind::Diagram: return "Diagram";
        case ArtworkKind::Other: return "Other";
    }
    return "Illustration";
}
ArtworkKind artworkKindFrom(const std::string& s) {
    for (int i = 0; i <= (int)ArtworkKind::Other; ++i)
        if (s == toString((ArtworkKind)i)) return (ArtworkKind)i;
    return ArtworkKind::Illustration;
}

// --------------------------------------------------------- artwork library

bool Library::loadArtworks() {
    artworks_.clear();
    Stmt q(db_, "SELECT id, blob, name, kind, width, height, format, source, notes, created, modified, deleted "
                "FROM artworks ORDER BY created DESC");
    if (!q) { error_ = sqlite3_errmsg(db_); return false; }
    while (q.row()) {
        Artwork a;
        a.id = q.text(0); a.blob = q.text(1); a.name = q.text(2); a.kind = q.text(3);
        a.width = (int)q.i64(4); a.height = (int)q.i64(5); a.format = q.text(6); a.source = q.text(7); a.notes = q.text(8);
        a.created = q.i64(9); a.modified = q.i64(10); a.deleted = q.i64(11);
        artworks_.push_back(std::move(a));
    }
    return true;
}

bool Library::saveArtwork(const Artwork& a) {
    Stmt s(db_, "INSERT INTO artworks(id, blob, name, kind, width, height, format, source, notes, created, modified, deleted) "
                "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12) ON CONFLICT(id) DO UPDATE SET name=excluded.name, "
                "kind=excluded.kind, width=excluded.width, height=excluded.height, format=excluded.format, "
                "source=excluded.source, notes=excluded.notes, modified=excluded.modified, deleted=excluded.deleted");
    s.bind(1, a.id).bind(2, a.blob).bind(3, a.name).bind(4, a.kind).bind(5, (std::int64_t)a.width).bind(6, (std::int64_t)a.height)
        .bind(7, a.format).bind(8, a.source).bind(9, a.notes).bind(10, (std::int64_t)a.created)
        .bind(11, (std::int64_t)a.modified).bind(12, (std::int64_t)a.deleted);
    if (!s.run()) { error_ = sqlite3_errmsg(db_); return false; }
    return true;
}

std::vector<const Artwork*> Library::liveArtworks() const {
    std::vector<const Artwork*> out;
    for (auto& a : artworks_) if (!a.deleted) out.push_back(&a);
    std::stable_sort(out.begin(), out.end(), [](auto* x, auto* y) { return x->created > y->created; });
    return out;
}
const Artwork* Library::artwork(const Id& id) const {
    for (auto& a : artworks_) if (a.id == id) return &a;
    return nullptr;
}
const Artwork* Library::artworkForBlob(const std::string& hash) const {
    for (auto& a : artworks_) if (a.blob == hash) return &a;
    return nullptr;
}
Artwork* Library::artworkMut(const Id& id) { return const_cast<Artwork*>(artwork(id)); }

Id Library::registerArtwork(const std::string& hash, const std::string& name, const std::string& source) {
    const std::string path = blobPath(hash);
    if (path.empty()) { error_ = "That isn't a stored picture."; return {}; }
    if (const Artwork* existing = artworkForBlob(hash)) {
        if (existing->deleted) {
            Artwork next = *existing;
            next.deleted = 0;
            next.modified = now();
            if (!begin()) return {};
            if (!saveArtwork(next)) { rollback(); return {}; }
            if (!commit()) return {};
            *artworkMut(next.id) = next;
        }
        return existing->id;
    }
    std::ifstream f(u8path(path), std::ios::binary);
    if (!f) { error_ = "The picture's file is missing from the objects folder."; return {}; }
    std::string head(64 * 1024, '\0');
    f.read(head.data(), (std::streamsize)head.size());
    head.resize((size_t)f.gcount());
    ImageInfo info = imageInfo(head);
    if (!info.ok) {
        // A JPEG's frame header can come after large metadata: read the whole file.
        std::ostringstream ss;
        f.clear();
        f.seekg(0);
        ss << f.rdbuf();
        info = imageInfo(ss.str());
    }
    if (!info.ok) { error_ = "That file isn't a picture the Studio can use (PNG, JPEG, GIF, WebP or BMP)."; return {}; }
    Artwork a;
    a.id = newId();
    a.blob = hash;
    a.name = name.empty() ? "Untitled picture" : name;
    a.width = info.width;
    a.height = info.height;
    a.format = info.format;
    a.source = source;
    a.created = a.modified = now();
    if (!begin()) return {};
    if (!saveArtwork(a)) { rollback(); return {}; }
    if (!commit()) return {};
    artworks_.insert(artworks_.begin(), a);
    return a.id;
}

Id Library::addArtwork(const std::string& bytes, const std::string& name, const std::string& source) {
    if (!imageInfo(bytes).ok) { error_ = "That file isn't a picture the Studio can use (PNG, JPEG, GIF, WebP or BMP)."; return {}; }
    const std::string hash = storeBlob(bytes);
    if (hash.empty()) return {};
    return registerArtwork(hash, name, source);
}

bool Library::updateArtwork(const Artwork& in) {
    Artwork* a = artworkMut(in.id);
    if (!a) { error_ = "That picture is no longer in the artwork library."; return false; }
    Artwork next = *a;
    next.name = in.name.empty() ? a->name : in.name;
    next.kind = toString(artworkKindFrom(in.kind));
    next.notes = in.notes;
    next.modified = now();
    if (!begin()) return false;
    if (!saveArtwork(next)) { rollback(); return false; }
    if (!commit()) return false;
    *a = next;
    return true;
}

bool Library::trashArtwork(const Id& id) {
    Artwork* a = artworkMut(id);
    if (!a) return false;
    Artwork next = *a;
    next.deleted = now();
    if (!begin()) return false;
    if (!saveArtwork(next)) { rollback(); return false; }
    if (!commit()) return false;
    *a = next;
    return true;
}

std::vector<const ManuscriptNode*> Library::artworkUsage(const std::string& hash) const {
    std::vector<const ManuscriptNode*> out;
    if (hash.size() != 64) return out;
    Stmt q(db_, "SELECT id FROM nodes WHERE deleted = 0 AND instr(content, ?1) > 0");
    q.bind(1, hash);
    while (q && q.row())
        if (const ManuscriptNode* n = node(q.text(0))) out.push_back(n);
    return out;
}

bool Library::registerArtworksInText() {
    static const std::regex blobRe("\"blob\"\\s*:\\s*\"([0-9a-f]{64})\"");
    std::set<std::string> seen;
    Stmt q(db_, "SELECT content FROM nodes WHERE instr(content, '\"image\"') > 0");
    std::vector<std::string> found;
    while (q && q.row()) {
        const std::string c = q.text(0);
        for (std::sregex_iterator it(c.begin(), c.end(), blobRe), end; it != end; ++it)
            if (seen.insert((*it)[1].str()).second) found.push_back((*it)[1].str());
    }
    bool ok = true;
    int n = 0;
    for (auto& h : found) {
        if (artworkForBlob(h)) continue;
        if (registerArtwork(h, "Picture " + std::to_string(++n), "From the text of an existing book").empty()) ok = false;
    }
    return ok;
}

}  // namespace studio
