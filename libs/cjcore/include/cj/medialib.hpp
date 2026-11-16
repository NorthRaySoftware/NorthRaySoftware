// The NorthRay media library: one library for the whole suite (%LOCALAPPDATA%\NorthRay\Library). Files are
// copied in (files\), and library.json lists them: a name, a kind (picture, video, music, sound, overlay
// design), a folder, tags and when each was added. Portable and unit-tested; the apps do the copying and
// show it (NorthRay Capture's Library first).
#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

namespace cj::medialib {

enum class Kind { Picture, Video, Music, Sound, Design, Other };
const char* kindKey(Kind k);     // "picture", "video", "music", "sound", "design", "other"
bool kindFromKey(const std::string& key, Kind& out);
const char* kindName(Kind k);    // "Picture", "Video", "Music", "Sound", "Overlay design", "Other"
// From the file's extension: pictures, videos, audio (music; a short sound is marked Sound by whoever adds
// it), ".nrdesign" (an overlay design), anything else Other.
Kind kindOf(const std::string& fileName);

struct Item {
    std::string id;       // "m1", "m2"...
    std::string file;     // the stored file's name, in files\ (unique in the library)
    std::string name;     // shown; starts as the original name without its extension
    Kind kind = Kind::Other;
    std::string folder;   // "" (the top) or "Overlays/Starting soon" ('/'-separated)
    std::vector<std::string> tags;   // lower case, sorted, no repeats
    std::string added;    // "2026-11-16"
    bool operator==(const Item&) const = default;
};

class Library {
public:
    std::string toJson() const;
    static bool fromJson(const std::string& json, Library& out);   // false if it isn't a library

    const std::vector<Item>& items() const { return items_; }
    const Item* item(const std::string& id) const;

    // A name for a new stored file from the original's, not used yet ("photo.png", "photo (2).png"); bad
    // characters for Windows file names are replaced.
    std::string storedName(const std::string& original) const;
    // Adds an item for a file already copied in as `storedFile`; returns its id ("" if that name's taken).
    std::string add(const std::string& storedFile, const std::string& name, Kind kind, const std::string& folder, const std::string& day);
    bool remove(const std::string& id);   // the app deletes (recycles) the file
    bool rename(const std::string& id, const std::string& name);
    bool setKind(const std::string& id, Kind kind);
    bool moveTo(const std::string& id, const std::string& folder);
    bool addTag(const std::string& id, const std::string& tag);
    bool removeTag(const std::string& id, const std::string& tag);

    // Folders: the ones made (even empty) and every item's, with their parents. Renaming moves what's inside;
    // removing moves what's inside up to its parent.
    std::vector<std::string> folders() const;
    bool addFolder(const std::string& path);
    bool renameFolder(const std::string& from, const std::string& to);
    bool removeFolder(const std::string& path);

    struct Query {
        std::string text;          // words that must all appear in the name, folder or tags
        int kind = -1;             // a Kind, or -1 for any
        std::string folder;        // "" any; otherwise this folder and those inside it
        std::string tag;           // "" any
    };
    std::vector<const Item*> search(const Query& q) const;   // newest first, then by name
    std::map<std::string, int> tags() const;                  // each tag and how many items have it

    static std::string cleanFolder(const std::string& path);  // trimmed parts, no empty ones, '\' as '/'
    static std::string cleanTag(const std::string& tag);      // trimmed, lower case, at most 40 bytes

private:
    std::vector<Item> items_;
    std::set<std::string> folders_;
    int next_ = 1;
};

}  // namespace cj::medialib
