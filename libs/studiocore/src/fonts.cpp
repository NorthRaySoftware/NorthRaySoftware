// A world's own typefaces: see studio/fonts.hpp.
#include "studio/fonts.hpp"

#include "internal.hpp"
#include "typeset_internal.hpp"

#include FT_TRUETYPE_TABLES_H
#include FT_SFNT_NAMES_H
#include FT_TRUETYPE_IDS_H
#include FT_TRUETYPE_TAGS_H

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace studio::fonts {

namespace fs = std::filesystem;
using studio::detail::fromPath;
using studio::detail::u8path;

namespace {

const char* const kStyles[] = {"Regular", "Italic", "Bold", "BoldItalic"};

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool writeAll(const fs::path& p, const std::string& data) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(data.data(), (std::streamsize)data.size());
    return (bool)out;
}

std::string trimmed(std::string s) {
    while (!s.empty() && (unsigned char)s.back() <= ' ') s.pop_back();
    size_t i = 0;
    while (i < s.size() && (unsigned char)s[i] <= ' ') ++i;
    return s.substr(i);
}

// "Cinzel Decorative" -> "cinzel-decorative" (ASCII letters and digits; anything else joins words).
std::string slug(const std::string& name) {
    std::string out;
    for (unsigned char c : name) {
        if (std::isalnum(c) && c < 0x80) out += (char)std::tolower(c);
        else if (!out.empty() && out.back() != '-') out += '-';
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? std::string("font") : out;
}

// "Cinzel Decorative" -> "CinzelDecorative" for file names.
std::string fileStem(const std::string& name) {
    std::string out;
    for (unsigned char c : name) if (std::isalnum(c) && c < 0x80) out += (char)c;
    return out.empty() ? std::string("Font") : out;
}

std::string hash8(const std::string& data) {
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : data) { h ^= c; h *= 1099511628211ull; }
    char buf[17];
    std::snprintf(buf, sizeof buf, "%08x", (unsigned)(h ^ (h >> 32)));
    return buf;
}

// A name from the font's name table: Windows Unicode (UTF-16BE), else Mac Roman taken as ASCII.
std::string sfntName(FT_Face face, unsigned id) {
    std::string mac;
    const FT_UInt n = FT_Get_Sfnt_Name_Count(face);
    for (FT_UInt i = 0; i < n; ++i) {
        FT_SfntName nm;
        if (FT_Get_Sfnt_Name(face, i, &nm) != 0 || nm.name_id != id) continue;
        if (nm.platform_id == TT_PLATFORM_MICROSOFT && (nm.encoding_id == TT_MS_ID_UNICODE_CS || nm.encoding_id == TT_MS_ID_SYMBOL_CS)) {
            std::u32string u;
            for (FT_UInt k = 0; k + 1 < nm.string_len; k += 2) {
                char32_t c = (char32_t)(nm.string[k] << 8 | nm.string[k + 1]);
                if (c >= 0xD800 && c < 0xDC00 && k + 3 < nm.string_len) {
                    const char32_t lo = (char32_t)(nm.string[k + 2] << 8 | nm.string[k + 3]);
                    c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                    k += 2;
                }
                u += c;
            }
            std::string out;
            for (char32_t c : u) {
                if (c < 0x80) out += (char)c;
                else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
                else if (c < 0x10000) { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
                else { out += (char)(0xF0 | (c >> 18)); out += (char)(0x80 | ((c >> 12) & 0x3F)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
            }
            return trimmed(out);
        }
        if (nm.platform_id == TT_PLATFORM_MACINTOSH && mac.empty())
            for (FT_UInt k = 0; k < nm.string_len; ++k) if (nm.string[k] < 0x80) mac += (char)nm.string[k];
    }
    return trimmed(mac);
}

bool hasTable(FT_Face face, FT_ULong tag) {
    FT_ULong len = 0;
    return FT_Load_Sfnt_Table(face, tag, 0, nullptr, &len) == 0 && len > 0;
}

std::string lowerExt(const fs::path& p) {
    std::string e = fromPath(p.extension());
    for (auto& c : e) c = (char)std::tolower((unsigned char)c);
    return e;
}

// The family's style files, by style.
std::map<std::string, fs::path> styleFiles(const fs::path& dir) {
    std::map<std::string, fs::path> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        const std::string n = fromPath(e.path().filename());
        for (const char* s : kStyles) {
            const std::string want = std::string("-") + s + ".ttf";
            if (n.size() > want.size() && n.compare(n.size() - want.size(), want.size(), want) == 0) out[s] = e.path();
        }
    }
    return out;
}

std::string familyName(const fs::path& dir) {
    std::string n = trimmed(readAll(dir / "name.txt"));
    if (!n.empty()) return n;
    // "crimson-text" -> "Crimson Text"
    const std::string id = fromPath(dir.filename());
    bool up = true;
    for (char c : id) {
        if (c == '-') { n += ' '; up = true; continue; }
        n += up ? (char)std::toupper((unsigned char)c) : c;
        up = false;
    }
    return n;
}

}  // namespace

std::vector<std::string> folders(const std::string& list) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i <= list.size(); ++i) {
        if (i == list.size() || list[i] == '|') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += list[i];
    }
    return out;
}

std::string joinFolders(const std::vector<std::string>& fs) {
    std::string out;
    for (auto& f : fs) if (!f.empty()) out += (out.empty() ? "" : "|") + f;
    return out;
}

std::string familyFolder(const std::string& list, const std::string& id) {
    if (id.empty() || id.find('/') != std::string::npos || id.find('\\') != std::string::npos || id == "." || id == "..") return {};
    for (const std::string& f : folders(list)) {
        std::error_code ec;
        const fs::path dir = u8path(f) / u8path(id);
        if (fs::is_directory(dir, ec)) return fromPath(dir);
    }
    return {};
}

std::string scopeFolder(const std::string& libraryFolder, const std::string& world, const std::string& book) {
    if (libraryFolder.empty() || (world.empty() && book.empty())) return {};
    return fromPath(u8path(libraryFolder) / "fonts" / u8path(world.empty() ? "book-" + book : "world-" + world));
}

FaceInfo inspect(const std::string& bytes) {
    FaceInfo info;
    if (bytes.size() < 12) { info.problem = "it isn't a font file"; return info; }
    const std::string magic = bytes.substr(0, 4);
    if (magic == "OTTO") {
        info.problem = "it's an OpenType font with PostScript (CFF) outlines, which Writing Studio can't put in print PDFs yet. A TrueType (.ttf) "
                       "version of the same font works.";
        return info;
    }
    if (magic == "wOFF" || magic == "wOF2") { info.problem = "it's a web font (WOFF). Use the .ttf version of the font."; return info; }
    if (magic == "ttcf") { info.problem = "it's a font collection (.ttc). Use the separate .ttf files."; return info; }
    if (!(magic == std::string("\0\1\0\0", 4) || magic == "true")) { info.problem = "it isn't a TrueType font file"; return info; }
    auto& fontsLib = typeset::detail::Fonts::get();
    std::lock_guard<std::recursive_mutex> guard(fontsLib.lock());
    FT_Face face = nullptr;
    if (FT_New_Memory_Face(fontsLib.ft(), reinterpret_cast<const FT_Byte*>(bytes.data()), (FT_Long)bytes.size(), 0, &face) != 0 || !face) {
        info.problem = "it couldn't be read as a font";
        return info;
    }
    if (!hasTable(face, TTAG_glyf)) info.problem = "it has no TrueType outlines";
    std::string fam = sfntName(face, 16), sub = sfntName(face, 17);
    if (fam.empty()) fam = face->family_name ? trimmed(face->family_name) : std::string();
    if (sub.empty()) sub = face->style_name ? trimmed(face->style_name) : std::string();
    info.family = fam;
    info.styleName = sub.empty() ? "Regular" : sub;
    bool italic = (face->style_flags & FT_STYLE_FLAG_ITALIC) != 0;
    bool bold = (face->style_flags & FT_STYLE_FLAG_BOLD) != 0;
    if (auto* os2 = static_cast<TT_OS2*>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2))) {
        info.weight = os2->usWeightClass ? os2->usWeightClass : 400;
        bold = info.weight >= 600;
        if (os2->fsSelection & 1) italic = true;
        const unsigned fsType = os2->fsType;
        if (info.problem.empty() && ((fsType & 0x000F) == 0x0002 || (fsType & 0x0200)))
            info.problem = "its licence doesn't allow it to be put inside documents, so it couldn't be used in a PDF or ebook";
    } else if (bold) {
        info.weight = 700;
    }
    info.variable = hasTable(face, TTAG_fvar);
    info.style = bold ? (italic ? "BoldItalic" : "Bold") : (italic ? "Italic" : "Regular");
    if (info.family.empty() && info.problem.empty()) info.problem = "it has no family name";
    FT_Done_Face(face);
    return info;
}

std::vector<Family> familiesIn(const std::string& folder) {
    std::vector<Family> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(u8path(folder), ec)) {
        if (!e.is_directory()) continue;
        const auto files = styleFiles(e.path());
        if (!files.count("Regular")) continue;
        Family f;
        f.id = fromPath(e.path().filename());
        f.name = familyName(e.path());
        for (const char* s : kStyles) if (files.count(s)) f.styles.push_back(s);
        f.licence = fs::exists(e.path() / "OFL.txt", ec) || fs::exists(e.path() / "LICENSE.txt", ec);
        out.push_back(f);
    }
    std::sort(out.begin(), out.end(), [](const Family& a, const Family& b) { return a.name < b.name; });
    return out;
}

ImportResult importFiles(const std::string& folder, const std::vector<std::string>& files, const std::vector<std::string>& takenIds) {
    ImportResult r;
    struct Pick { fs::path src; std::string bytes; FaceInfo info; };
    std::map<std::string, std::map<std::string, Pick>> byFamily;   // family -> style -> the file
    for (const std::string& f : files) {
        const fs::path p = u8path(f);
        const std::string label = fromPath(p.filename());
        const std::string ext = lowerExt(p);
        std::string bytes = readAll(p);
        if (bytes.empty()) { r.problems.push_back(label + ": it couldn't be read"); continue; }
        if (bytes.size() > (64u << 20)) { r.problems.push_back(label + ": it's too big for a font (over 64 MB)"); continue; }
        FaceInfo info = inspect(bytes);
        if (!info.problem.empty()) { r.problems.push_back(label + ": " + info.problem); continue; }
        (void)ext;
        auto& slot = byFamily[info.family];
        auto it = slot.find(info.style);
        const int want = info.style.rfind("Bold", 0) == 0 ? 700 : 400;
        if (it != slot.end()) {
            // Two files for one style (Light and Regular): the weight nearest a book's keeps it.
            Pick& had = it->second;
            const bool better = std::abs(info.weight - want) < std::abs(had.info.weight - want);
            const Pick& left = better ? had : Pick{p, {}, info};
            r.problems.push_back(fromPath(left.src.filename()) + ": " + info.family + " " + left.info.styleName +
                                 " wasn't added. A book uses four styles (regular, italic, bold, bold italic), and " +
                                 (better ? info.styleName : had.info.styleName) + " is nearer the " +
                                 (want == 700 ? std::string("bold") : std::string("regular")) + " weight.");
            if (!better) continue;
        }
        slot[info.style] = Pick{p, std::move(bytes), info};
    }
    std::error_code ec;
    fs::create_directories(u8path(folder), ec);
    const auto existing = familiesIn(folder);
    std::set<std::string> taken(takenIds.begin(), takenIds.end());
    for (auto& f : existing) taken.insert(f.id);
    for (auto& [name, styles] : byFamily) {
        // The family's folder: the one already holding that name, else a new id.
        std::string id;
        for (auto& f : existing) if (f.name == name) id = f.id;
        const bool fresh = id.empty();
        if (fresh) {
            const std::string base = slug(name);
            id = base;
            for (int n = 2; taken.count(id); ++n) id = base + "-" + std::to_string(n);
            taken.insert(id);
        }
        const fs::path dir = u8path(folder) / u8path(id);
        fs::create_directories(dir, ec);
        if (ec) { r.problems.push_back(name + ": its folder couldn't be made (" + ec.message() + ")"); continue; }
        const auto had = styleFiles(dir);
        if (fresh && !styles.count("Regular")) {
            // No regular style: the first there is stands in for it, so the family can be used at all.
            auto first = styles.begin();
            r.problems.push_back(name + ": there was no regular style, so " + first->second.info.styleName + " is used as its regular.");
            styles["Regular"] = first->second;
            if (first->first != "Regular") styles.erase(first);
        }
        bool ok = true;
        for (auto& [style, pick] : styles) {
            if (auto h = had.find(style); h != had.end()) fs::remove(h->second, ec);
            const fs::path to = dir / u8path(fileStem(name) + "-" + hash8(pick.bytes) + "-" + style + ".ttf");
            if (!writeAll(to, pick.bytes)) { r.problems.push_back(fromPath(pick.src.filename()) + ": it couldn't be copied into the library"); ok = false; continue; }
            // A licence beside it.
            for (const char* lic : {"OFL.txt", "LICENSE.txt", "LICENCE.txt", "License.txt", "license.txt", "OFL.TXT"}) {
                const fs::path l = pick.src.parent_path() / lic;
                if (fs::exists(l, ec)) {
                    const std::string to2 = std::string(lic).rfind("OFL", 0) == 0 ? "OFL.txt" : "LICENSE.txt";
                    fs::copy_file(l, dir / to2, fs::copy_options::overwrite_existing, ec);
                    break;
                }
            }
        }
        if (fresh) writeAll(dir / "name.txt", name);
        if (ok || !styleFiles(dir).empty()) r.added.push_back(name);
        if (styleFiles(dir).empty()) fs::remove_all(dir, ec);
    }
    return r;
}

bool removeFamily(const std::string& folder, const std::string& id, std::string& error) {
    const std::string dir = familyFolder(folder, id);
    if (dir.empty()) { error = "That typeface isn't in this world."; return false; }
    std::error_code ec;
    fs::remove_all(u8path(dir), ec);
    if (ec) { error = ec.message(); return false; }
    return true;
}

bool renameFamily(const std::string& folder, const std::string& id, const std::string& name) {
    const std::string dir = familyFolder(folder, id);
    const std::string n = trimmed(name);
    if (dir.empty() || n.empty()) return false;
    return writeAll(u8path(dir) / "name.txt", n);
}

Sample sample(const std::string& folderList, const std::string& id, const std::string& style, const std::string& text, float px,
              std::uint32_t ink, int maxWidth) {
    Sample s;
    const std::string dir = familyFolder(folderList, id);
    if (dir.empty() || text.empty()) return s;
    const auto files = styleFiles(u8path(dir));
    auto it = files.find(style);
    if (it == files.end()) it = files.find("Regular");
    if (it == files.end()) return s;
    auto& lib = typeset::detail::Fonts::get();
    std::lock_guard<std::recursive_mutex> guard(lib.lock());
    const int face = lib.open(fromPath(it->second));
    if (face < 0) return s;
    const typeset::detail::Shaped sh = typeset::detail::shape(face, text, px, false, "en");
    auto& f = lib.face(face);
    const int ascent = (int)std::ceil(f.ascender * px), descent = (int)std::ceil(f.descender * px);
    s.w = std::clamp((int)std::ceil(sh.width) + 4, 1, std::max(1, maxWidth));
    s.h = std::max(1, ascent + descent + 4);
    s.rgba.assign((size_t)s.w * (size_t)s.h * 4, 0);
    const std::uint8_t cr = (std::uint8_t)(ink >> 16), cg = (std::uint8_t)(ink >> 8), cb = (std::uint8_t)ink;
    for (size_t i = 0; i < (size_t)s.w * (size_t)s.h; ++i) { s.rgba[i * 4] = cr; s.rgba[i * 4 + 1] = cg; s.rgba[i * 4 + 2] = cb; }
    const double baseline = ascent + 2;
    for (size_t i = 0; i < sh.glyphs.size(); ++i) {
        const int fi = sh.faces.empty() ? face : sh.faces[i];
        FT_Face ft = lib.face(fi).ft;
        FT_Set_Char_Size(ft, 0, (FT_F26Dot6)std::lround(px * 64), 72, 72);
        const double gx = 2 + sh.x[i], gy = baseline + (sh.dy.empty() ? 0 : sh.dy[i]);
        if (gx > s.w) break;
        const double fx = std::floor(gx);
        FT_Vector delta{(FT_Pos)std::lround((gx - fx) * 64), 0};
        FT_Set_Transform(ft, nullptr, &delta);
        if (FT_Load_Glyph(ft, sh.glyphs[i], FT_LOAD_NO_HINTING | FT_LOAD_RENDER) == 0) {
            const FT_Bitmap& b = ft->glyph->bitmap;
            const int ox = (int)fx + ft->glyph->bitmap_left, oy = (int)std::lround(gy) - ft->glyph->bitmap_top;
            for (unsigned yy = 0; yy < b.rows; ++yy)
                for (unsigned xx = 0; xx < b.width; ++xx) {
                    const int x = ox + (int)xx, y = oy + (int)yy;
                    if (x < 0 || y < 0 || x >= s.w || y >= s.h) continue;
                    std::uint8_t& a = s.rgba[((size_t)y * (size_t)s.w + (size_t)x) * 4 + 3];
                    a = (std::uint8_t)std::max<int>(a, b.buffer[yy * (unsigned)b.pitch + xx]);
                }
        }
        FT_Set_Transform(ft, nullptr, nullptr);
    }
    return s;
}

}  // namespace studio::fonts
