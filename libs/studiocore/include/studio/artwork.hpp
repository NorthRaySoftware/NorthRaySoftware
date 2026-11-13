// Artwork: pictures used in books, and the print maths around them.
//
// Pictures are stored once in the library's objects folder (by content) and
// listed in the artwork library (Library::artworks()). Where a picture sits in
// the text it carries a placement: a crop and a width, kept with the text and
// never applied to the stored file, so any crop can be undone or changed.
#pragma once

#include <string>
#include <vector>

namespace studio {

// ------------------------------------------------------------- image files

struct ImageInfo {
    bool ok = false;
    int width = 0, height = 0;   // pixels
    std::string format;          // "png", "jpeg", "gif", "webp", "bmp"
};
// Reads the size from the file's header (PNG, JPEG, GIF, WebP, BMP). Nothing is decoded.
ImageInfo imageInfo(const std::string& bytes);

// --------------------------------------------------------------- trim sizes

struct TrimSize {
    const char* id;
    const char* label;
    double widthIn, heightIn;
};
// Common trade-paperback and hardback sizes. The first is the default.
const std::vector<TrimSize>& trimSizes();
const TrimSize& trimSize(const std::string& id);    // unknown id: the default
// The width of the text on the page: the trim width less typical margins
// (about 1.25 in in all). An estimate until typesetting sets real margins.
double textWidthInches(const std::string& trimId);

// --------------------------------------------------------------- placement

// How a picture sits in the text (the editor's image attributes). Crop values
// are fractions of the picture (0..1); width is a fraction of the text width.
struct Placement {
    double cropX = 0, cropY = 0, cropW = 1, cropH = 1;
    double width = 1;
    std::string align = "center";    // "center", "left", "right"
};
Placement clampPlacement(Placement p);   // keeps every value in range (a crop at least 5% each way)

// Printed size and resolution of a placed picture.
struct PrintSize {
    double widthIn = 0, heightIn = 0;
    double ppi = 0;                  // effective pixels per inch at that size
};
PrintSize printSize(int pixelWidth, int pixelHeight, const Placement& p, double textWidthIn);

// 300 ppi or more prints sharply; below 300 it may look soft; below 150 it
// will look blurred or blocky.
enum class PrintQuality { Good, Soft, Poor };
PrintQuality printQuality(double ppi);
const char* describe(PrintQuality q);
// The widest a picture can print at 300 ppi, as a fraction of the text width (capped at 1).
double sharpWidthFraction(int pixelWidth, double cropW, double textWidthIn);

// ------------------------------------------------------------------- units

enum class Unit { Inches, Centimetres, Millimetres, Pixels };
const char* toString(Unit u);                 // "in", "cm", "mm", "px"
Unit unitFrom(const std::string& s);          // unknown: inches
// "4.25 in", "10.8 cm", "108 mm". Pixels need a resolution: `ppi`.
std::string formatLength(double inches, Unit u, double ppi = 300);

// ------------------------------------------------------------------- kinds

enum class ArtworkKind { Illustration, Map, Ornament, Photo, Diagram, Other };
const char* toString(ArtworkKind k);
const char* label(ArtworkKind k);
ArtworkKind artworkKindFrom(const std::string& s);

}  // namespace studio
