// NorthRay Files: a folder's contents as the app shows them: entries, the
// order they're sorted in, which are hidden, and what's selected. Portable,
// so it's tested on every platform; the Windows layer fills the entries.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nrf {

// Windows' file attribute bits (the values of FILE_ATTRIBUTE_*), so this
// header needs no Windows headers.
namespace attr {
inline constexpr std::uint32_t ReadOnly = 0x1, Hidden = 0x2, System = 0x4, Directory = 0x10, Archive = 0x20,
                               ReparsePoint = 0x400, Compressed = 0x800, Offline = 0x1000, Encrypted = 0x4000,
                               RecallOnOpen = 0x40000, RecallOnDataAccess = 0x400000;
}

struct Entry {
    std::wstring name;
    std::uint64_t size = 0;          // bytes (0 for folders)
    std::uint64_t modified = 0;      // FILETIME: 100 ns since 1601 (UTC)
    std::uint32_t attributes = 0;
    std::uint32_t reparseTag = 0;    // what kind of link, for reparse points
    std::wstring type;               // "Text Document", filled in by the Windows layer
    int icon = -1;                   // system image list index, filled in by the Windows layer
    std::wstring location;           // search results: the folder it's in ("" = the folder shown); the Recycle Bin: where it was
    std::wstring binId, binFile;     // in the Recycle Bin: its name there, and where its contents are kept

    bool folder() const { return (attributes & attr::Directory) != 0; }
    bool hidden() const { return (attributes & attr::Hidden) != 0; }
    bool protectedSystem() const { return (attributes & (attr::Hidden | attr::System)) == (attr::Hidden | attr::System); }
    bool link() const { return (attributes & attr::ReparsePoint) != 0; }
    // In the cloud, not on this PC (OneDrive's online-only files): reading it downloads it.
    bool cloudOnly() const { return (attributes & (attr::Offline | attr::RecallOnOpen | attr::RecallOnDataAccess)) != 0; }
};

// What hidden things are shown. Protected system files (hidden + system)
// need their own setting, as in File Explorer.
struct Visibility {
    bool showHidden = false;
    bool showProtected = false;
};
bool isVisible(const Entry& e, const Visibility& v);

enum class SortKey { Name, Modified, Type, Size };
struct Sort {
    SortKey key = SortKey::Name;
    bool descending = false;
};

// Compares names as people read them, like File Explorer: case doesn't
// matter, and runs of digits compare as numbers ("file2" before "file10").
int naturalCompare(const std::wstring& a, const std::wstring& b);

// Folders first (in either direction), then by the key, then by name.
void sortEntries(std::vector<Entry>& entries, const Sort& sort);

// "0 bytes", "512 bytes", "1.5 KB", "23.4 MB", "1.07 GB": three significant
// figures, binary units (as File Explorer does).
std::wstring formatSize(std::uint64_t bytes);
// File Explorer's Size column: whole kilobytes, rounded up, with separators ("1,024 KB").
std::wstring formatSizeKb(std::uint64_t bytes);

// Which rows are selected, the focused row and the anchor for Shift ranges.
class Selection {
public:
    void reset(std::size_t count);          // a new listing: nothing selected
    std::size_t size() const { return selected_.size(); }
    bool selected(std::size_t i) const { return i < selected_.size() && selected_[i]; }
    std::size_t count() const;
    int focus() const { return focus_; }
    std::vector<std::size_t> indices() const;

    // A click on row i: alone it selects only i; with Ctrl it toggles i; with
    // Shift it selects from the anchor to i (adding to the rest with Ctrl too).
    void click(std::size_t i, bool ctrl, bool shift);
    // Clicking empty space: nothing selected (Ctrl keeps the selection).
    void clickEmpty(bool ctrl);
    void selectAll();
    void clear();
    // The keyboard: move the focus to row i; Shift extends from the anchor;
    // Ctrl moves the focus only.
    void moveTo(std::size_t i, bool ctrl, bool shift);
    // Ctrl+Space: toggle the focused row.
    void toggleFocused();
    void select(std::size_t i);   // just this one (e.g. after going up a level)

private:
    std::vector<bool> selected_;
    int focus_ = -1, anchor_ = -1;
};

}  // namespace nrf
