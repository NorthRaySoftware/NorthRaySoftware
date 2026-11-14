// NorthRay Browser's reference boards: pictures saved from pages (right-click a picture,
// "Save to reference board") and colours picked from pages, kept on named boards for artists and
// writers (%LOCALAPPDATA%\NorthRay\Browser\User\references.db, the pictures in User\references\).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct sqlite3;

namespace nrb {

struct RefBoard {
    std::int64_t id = 0;
    std::string name;
    int pictures = 0;
};

struct RefPicture {
    std::int64_t id = 0;
    std::int64_t board = 0;
    std::string file;        // the picture's file name in the pictures folder
    std::string source;      // where the picture came from
    std::string page;        // the page it was on
    std::string title;       // that page's title
    int width = 0, height = 0;
    std::int64_t added = 0;
};

struct RefColour {
    std::int64_t id = 0;
    std::int64_t board = 0;
    std::uint32_t rgb = 0;   // 0xRRGGBB
    std::string page;        // where it was picked
    std::int64_t added = 0;
};

class ReferenceStore {
public:
    // Opens (creating) the store; a new store has one board, "References".
    static std::unique_ptr<ReferenceStore> open(const std::string& path, std::string& error);
    ~ReferenceStore();
    ReferenceStore(const ReferenceStore&) = delete;
    ReferenceStore& operator=(const ReferenceStore&) = delete;

    std::vector<RefBoard> boards() const;              // in the order made
    std::int64_t addBoard(const std::string& name);    // 0 if the name is empty or taken
    bool renameBoard(std::int64_t id, const std::string& name);
    // Removes a board and everything on it; returns the picture files that are no longer used.
    std::vector<std::string> removeBoard(std::int64_t id);
    std::int64_t firstBoard() const;

    std::int64_t addPicture(const RefPicture& p);
    std::vector<RefPicture> pictures(std::int64_t board) const;   // newest first
    const std::string pictureFile(std::int64_t id) const;
    bool movePicture(std::int64_t id, std::int64_t board);
    std::string removePicture(std::int64_t id);       // returns its file (to delete)
    bool hasSource(std::int64_t board, const std::string& source) const;   // saved there already

    std::int64_t addColour(const RefColour& c);       // the same colour on the same board isn't added twice
    std::vector<RefColour> colours(std::int64_t board) const;   // newest first
    bool removeColour(std::int64_t id);

private:
    ReferenceStore() = default;
    sqlite3* db_ = nullptr;
};

// "#1F3B2C".
std::string hexColour(std::uint32_t rgb);
// The main colours of a picture (RGBA, 8 bits each), most used first: at most `count`, close shades
// merged. Transparent pixels are left out.
std::vector<std::uint32_t> mainColours(const std::uint8_t* rgba, int width, int height, int count = 6);

}  // namespace nrb
