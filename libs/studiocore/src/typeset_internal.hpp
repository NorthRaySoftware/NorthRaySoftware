// Typesetting internals shared by typeset*.cpp. Not a public header.
#pragma once

#include "studio/typeset.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>
#include <hb-ot.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace studio::typeset::detail {

// One font file, open in both FreeType (outlines, rasterising) and HarfBuzz (shaping).
struct Face {
    std::string path;
    std::string data;            // the file's bytes; both libraries read from here
    FT_Face ft = nullptr;
    hb_blob_t* blob = nullptr;
    hb_face_t* hbFace = nullptr;
    hb_font_t* hb = nullptr;
    int upem = 1000;
    bool smallCaps = false;      // has the 'smcp' feature
    double ascender = 0.8, descender = 0.2, capHeight = 0.7;   // fractions of the em
    std::string postscriptName;
};

// All open fonts, for the whole program. FreeType and HarfBuzz aren't used
// from two threads at once: every call takes lock() (it's recursive, so a
// caller drawing a whole page can hold it throughout).
class Fonts {
public:
    static Fonts& get();
    std::recursive_mutex& lock() { return mutex_; }
    int open(const std::string& path);   // -1 if it can't be read
    Face& face(int i) { std::lock_guard<std::recursive_mutex> g(mutex_); return *faces_[i]; }
    FT_Library ft() const { return ft_; }
    // Advance of a glyph in font units.
    int advance(int face, std::uint32_t glyph);
private:
    Fonts();
    std::recursive_mutex mutex_;
    FT_Library ft_ = nullptr;
    std::vector<std::unique_ptr<Face>> faces_;
    std::unordered_map<std::string, int> byPath_;
};

struct Shaped {
    std::vector<std::uint32_t> glyphs;
    std::vector<float> x;        // pen position of each glyph, pt from the start
    std::vector<float> dy;       // vertical offset, pt (up negative)
    std::vector<std::uint32_t> clusters;   // byte offset into the text of each glyph's cluster
    float width = 0;             // total advance, pt
};
// Shapes `text` in one face at `size` pt.
Shaped shape(int face, const std::string& text, float size, bool smallCaps, const std::string& language);

// A family: regular, italic, bold, bold italic (missing styles fall back to regular).
struct Family {
    int regular = -1, italic = -1, bold = -1, boldItalic = -1;
    int pick(bool b, bool i) const {
        if (b && i && boldItalic >= 0) return boldItalic;
        if (b && bold >= 0) return bold;
        if (i && italic >= 0) return italic;
        return regular;
    }
};
bool openFamily(const std::string& fontsFolder, const std::string& id, Family& out);

}  // namespace studio::typeset::detail
