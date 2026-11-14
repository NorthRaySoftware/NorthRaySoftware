// Fonts and shaping for the typesetter.
#include "typeset_internal.hpp"

#include "internal.hpp"

#include FT_SFNT_NAMES_H
#include FT_TRUETYPE_TABLES_H

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace studio::typeset {
namespace detail {

namespace fs = std::filesystem;

Fonts& Fonts::get() {
    static Fonts f;
    return f;
}

Fonts::Fonts() { FT_Init_FreeType(&ft_); }

int Fonts::open(const std::string& path) {
    std::lock_guard<std::recursive_mutex> g(mutex_);
    if (auto it = byPath_.find(path); it != byPath_.end()) return it->second;
    auto f = std::make_unique<Face>();
    f->path = path;
    {
        std::ifstream in(studio::detail::u8path(path), std::ios::binary);
        if (!in) return -1;
        std::ostringstream ss;
        ss << in.rdbuf();
        f->data = ss.str();
    }
    if (f->data.size() < 12 ||
        FT_New_Memory_Face(ft_, reinterpret_cast<const FT_Byte*>(f->data.data()), (FT_Long)f->data.size(), 0, &f->ft) != 0)
        return -1;
    f->blob = hb_blob_create(f->data.data(), (unsigned)f->data.size(), HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    f->hbFace = hb_face_create(f->blob, 0);
    f->hb = hb_font_create(f->hbFace);
    f->upem = (int)hb_face_get_upem(f->hbFace);
    hb_font_set_scale(f->hb, f->upem, f->upem);
    // Small capitals?
    {
        hb_tag_t tags[256];
        unsigned n = 256;
        hb_ot_layout_table_get_feature_tags(f->hbFace, HB_OT_TAG_GSUB, 0, &n, tags);
        for (unsigned i = 0; i < n; ++i) if (tags[i] == HB_TAG('s', 'm', 'c', 'p')) f->smallCaps = true;
    }
    if (f->ft->units_per_EM) {
        f->ascender = (double)f->ft->ascender / f->ft->units_per_EM;
        f->descender = -(double)f->ft->descender / f->ft->units_per_EM;
        if (auto* os2 = static_cast<TT_OS2*>(FT_Get_Sfnt_Table(f->ft, FT_SFNT_OS2)); os2 && os2->version >= 2 && os2->sCapHeight > 0)
            f->capHeight = (double)os2->sCapHeight / f->ft->units_per_EM;
    }
    if (const char* ps = FT_Get_Postscript_Name(f->ft)) f->postscriptName = ps;
    if (f->postscriptName.empty()) f->postscriptName = fs::path(studio::detail::u8path(path)).stem().string();
    faces_.push_back(std::move(f));
    const int idx = (int)faces_.size() - 1;
    byPath_[path] = idx;
    return idx;
}

int Fonts::advance(int face, std::uint32_t glyph) {
    std::lock_guard<std::recursive_mutex> g(mutex_);
    return (int)hb_font_get_glyph_h_advance(faces_[face]->hb, glyph);
}

void Fonts::useFallbacks(const std::string& folder) {
    std::lock_guard<std::recursive_mutex> g(mutex_);
    if (folder == fallbackFolder_) return;
    fallbackFolder_ = folder;
    fallbacks_.clear();
    std::vector<std::string> files;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(studio::detail::u8path(folder), ec)) {
        const std::string n = studio::detail::fromPath(e.path().filename());
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ".ttf") == 0) files.push_back(studio::detail::fromPath(e.path()));
    }
    std::sort(files.begin(), files.end());
    for (auto& p : files) if (const int i = open(p); i >= 0) fallbacks_.push_back(i);
}

bool Fonts::hasChar(int face, std::uint32_t cp) {
    std::lock_guard<std::recursive_mutex> g(mutex_);
    return FT_Get_Char_Index(faces_[face]->ft, cp) != 0;
}

// The next UTF-8 code point at `i` (moved past it).
static std::uint32_t nextCodepoint(const std::string& s, size_t& i) {
    const unsigned char c = (unsigned char)s[i];
    int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    if (i + n > s.size()) n = 1;
    std::uint32_t cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
    for (int k = 1; k < n; ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
    i += n;
    return cp;
}

static Shaped shapeOne(int faceIdx, const std::string& text, float size, bool smallCaps, const std::string& language);

// Shapes in the asked-for face, handing characters it doesn't have (✧, 🗡, ★) to the symbol fonts.
Shaped shape(int faceIdx, const std::string& text, float size, bool smallCaps, const std::string& language) {
    Fonts& fonts = Fonts::get();
    std::lock_guard<std::recursive_mutex> g(fonts.lock());
    const auto& fb = fonts.fallbacks();
    // Runs of text by face. Spaces, joiners and variation selectors stay with what's around them.
    struct Run { int face; size_t from, to; };
    std::vector<Run> runs;
    for (size_t i = 0; i < text.size();) {
        const size_t at = i;
        const std::uint32_t cp = nextCodepoint(text, i);
        int face = faceIdx;
        const bool neutral = cp <= 0x20 || cp == 0x200D || (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0x1F3FB && cp <= 0x1F3FF);
        if (neutral && !runs.empty()) face = runs.back().face;
        else if (!neutral && !fonts.hasChar(faceIdx, cp))
            for (int f : fb) if (fonts.hasChar(f, cp)) { face = f; break; }
        if (!runs.empty() && runs.back().face == face) runs.back().to = i;
        else runs.push_back({face, at, i});
    }
    if (runs.size() <= 1) {
        Shaped s = shapeOne(runs.empty() ? faceIdx : runs[0].face, text, size, smallCaps, language);
        s.faces.assign(s.glyphs.size(), runs.empty() ? faceIdx : runs[0].face);
        return s;
    }
    Shaped out;
    for (const Run& r : runs) {
        const Shaped s = shapeOne(r.face, text.substr(r.from, r.to - r.from), size, smallCaps && r.face == faceIdx, language);
        for (size_t k = 0; k < s.glyphs.size(); ++k) {
            out.glyphs.push_back(s.glyphs[k]);
            out.clusters.push_back(s.clusters[k] + (std::uint32_t)r.from);
            out.x.push_back(out.width + s.x[k]);
            out.dy.push_back(s.dy[k]);
            out.faces.push_back(r.face);
        }
        out.width += s.width;
    }
    return out;
}

static Shaped shapeOne(int faceIdx, const std::string& text, float size, bool smallCaps, const std::string& language) {
    Fonts& fonts = Fonts::get();
    std::lock_guard<std::recursive_mutex> g(fonts.lock());
    Face& f = fonts.face(faceIdx);
    Shaped out;
    hb_buffer_t* buf = hb_buffer_create();
    hb_buffer_add_utf8(buf, text.data(), (int)text.size(), 0, (int)text.size());
    hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
    hb_buffer_set_script(buf, HB_SCRIPT_LATIN);
    hb_buffer_set_language(buf, hb_language_from_string(language.c_str(), (int)language.size()));
    hb_buffer_guess_segment_properties(buf);
    hb_feature_t feats[2];
    unsigned nf = 0;
    if (smallCaps && f.smallCaps) feats[nf++] = {HB_TAG('s', 'm', 'c', 'p'), 1, 0, (unsigned)-1};
    hb_shape(f.hb, buf, feats, nf);
    unsigned n = 0;
    hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &n);
    hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &n);
    const float scale = size / (float)f.upem;
    float pen = 0;
    out.glyphs.reserve(n);
    for (unsigned i = 0; i < n; ++i) {
        out.glyphs.push_back(info[i].codepoint);
        out.clusters.push_back(info[i].cluster);
        out.x.push_back(pen + pos[i].x_offset * scale);
        out.dy.push_back(-pos[i].y_offset * scale);
        pen += pos[i].x_advance * scale;
    }
    out.width = pen;
    hb_buffer_destroy(buf);
    return out;
}

static std::string findStyle(const fs::path& dir, const char* suffix) {
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        const std::string n = studio::detail::fromPath(e.path().filename());
        const std::string want = std::string("-") + suffix + ".ttf";
        if (n.size() > want.size() && n.compare(n.size() - want.size(), want.size(), want) == 0)
            return studio::detail::fromPath(e.path());
    }
    return {};
}

bool openFamily(const std::string& fontsFolder, const std::string& id, Family& out) {
    const fs::path dir = studio::detail::u8path(fontsFolder) / studio::detail::u8path(id);
    Fonts& fonts = Fonts::get();
    auto load = [&](const char* suffix) {
        const std::string p = findStyle(dir, suffix);
        return p.empty() ? -1 : fonts.open(p);
    };
    out.regular = load("Regular");
    out.italic = load("Italic");
    out.bold = load("Bold");
    out.boldItalic = load("BoldItalic");
    return out.regular >= 0;
}

}  // namespace detail

std::vector<FontFamilyInfo> fontFamilies(const std::string& fontsFolder) {
    namespace fs = std::filesystem;
    std::vector<FontFamilyInfo> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(studio::detail::u8path(fontsFolder), ec)) {
        if (!e.is_directory()) continue;
        const std::string id = studio::detail::fromPath(e.path().filename());
        if (detail::findStyle(e.path(), "Regular").empty()) continue;
        // "crimson-text" -> "Crimson Text"
        std::string name;
        bool up = true;
        for (char c : id) {
            if (c == '-') { name += ' '; up = true; continue; }
            name += up ? (char)std::toupper((unsigned char)c) : c;
            up = false;
        }
        out.push_back({id, name});
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.name < b.name; });
    return out;
}

// ------------------------------------------------------------------ UTF-8

std::u32string utf32(const std::string& s) {
    std::u32string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        char32_t cp;
        int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; len = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; len = 3; }
        else if ((c >> 3) == 30) { cp = c & 0x07; len = 4; }
        else { ++i; continue; }
        if (i + len > s.size()) break;
        for (int k = 1; k < len; ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        out += cp;
        i += len;
    }
    return out;
}

std::string utf8(const std::u32string& s) {
    std::string out;
    for (char32_t c : s) {
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
        else { out += (char)(0xF0 | (c >> 18)); out += (char)(0x80 | ((c >> 12) & 0x3F)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
    }
    return out;
}

}  // namespace studio::typeset
