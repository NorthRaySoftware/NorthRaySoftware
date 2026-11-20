// Writing Studio: a world's own typefaces (0.40). Portable and tested.
//
// Fonts the author imports are copied into the library folder, one folder per world (or per independent book):
// <library>/fonts/world-<id>/<family>/<Name>-<hash>-Regular.ttf (and Italic, Bold, BoldItalic), with name.txt holding
// the family's name and the licence file if one came with it. Everything that sets type (typesetting and PDFs, the
// cover designer, the EPUB, the bible website and PDF) takes a list of font folders, "app fonts|world fonts", so a
// book sees the bundled typefaces and its world's own.
//
// Only TrueType outlines are taken: the PDF writer embeds TrueType (CIDFontType2), so a font with PostScript (CFF)
// outlines, most .otf files, is refused with a reason, as are web fonts (WOFF), font collections and fonts whose
// licence forbids embedding them in documents.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace studio::fonts {

// "a|b|c" -> {"a", "b", "c"} (empty parts dropped). '|' can't be in a Windows path.
std::vector<std::string> folders(const std::string& list);
std::string joinFolders(const std::vector<std::string>& folders);
// The folder of family `id` in the first folder of the list that has it ("" if none).
std::string familyFolder(const std::string& list, const std::string& id);

// <libraryFolder>/fonts/world-<world> (or book-<book> when world is empty).
std::string scopeFolder(const std::string& libraryFolder, const std::string& world, const std::string& book);

// What a font file is.
struct FaceInfo {
    std::string family;          // "Cinzel"
    std::string style;           // Regular, Italic, Bold or BoldItalic
    std::string styleName;       // the font's own name for it ("Light Italic")
    int weight = 400;            // OS/2 usWeightClass
    bool variable = false;       // has variations (only its default is used)
    std::string problem;         // why it can't be used; empty if it can
};
FaceInfo inspect(const std::string& bytes);

struct Family {
    std::string id, name;
    std::vector<std::string> styles;   // the styles it has, Regular first
    bool licence = false;              // a licence file came with it
};
// The families in one folder (a world's).
std::vector<Family> familiesIn(const std::string& folder);

struct ImportResult {
    std::vector<std::string> added;      // family names added or given more styles
    std::vector<std::string> problems;   // "Cinzel-Black.otf: …"
};
// Copies font files into `folder`, grouped into families by their names. A style a family already has is replaced.
// `takenIds` are family ids already used by other folders (the bundled ones), so a world's own never shadows them.
// A file named OFL.txt or LICENSE.txt (or LICENCE.txt) beside a font is copied with it.
ImportResult importFiles(const std::string& folder, const std::vector<std::string>& files, const std::vector<std::string>& takenIds);

// Removes a family's folder. False with the reason if it couldn't be removed.
bool removeFamily(const std::string& folder, const std::string& id, std::string& error);
// Renames a family (its name.txt; the id stays).
bool renameFamily(const std::string& folder, const std::string& id, const std::string& name);

// A line of text set in a family's style, for previews: straight RGBA, `ink` (0xRRGGBB) on transparent.
struct Sample { int w = 0, h = 0; std::vector<std::uint8_t> rgba; };
Sample sample(const std::string& folderList, const std::string& id, const std::string& style, const std::string& text, float px,
              std::uint32_t ink, int maxWidth = 2000);

}  // namespace studio::fonts
