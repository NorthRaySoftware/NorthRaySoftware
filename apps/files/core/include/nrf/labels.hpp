// NorthRay Files: colour labels, tags and collections (0.5). Portable, so the rules are tested
// everywhere; the Windows layer saves it (labels.json beside the settings) and keeps it up to date
// when Files moves, renames or deletes things.
//
// Labels belong to paths, kept in NorthRay Files' own file, not inside the files themselves, so any
// kind of file can have them and nothing about the file changes. Things moved or renamed by another
// program keep their place in a collection but lose their labels until they're labelled again; the
// window says so where it matters (an item in a collection that isn't there any more).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace nrf::labels {

enum class Colour { None = 0, Red, Orange, Yellow, Green, Blue, Purple, Grey };
constexpr int kColours = 8;
const wchar_t* colourName(Colour c);              // "Red"
Colour colourFromName(const std::wstring& name);   // "red", "Red", "grey"/"gray"; None if not one
std::wstring lowerName(Colour c);                  // "red", as searches write it

// A tag as it's kept: spaces at the ends gone, runs of spaces made one, at most 40 characters;
// "" if nothing's left.
std::wstring normaliseTag(const std::wstring& tag);

struct Label {
    std::wstring path;                 // as written
    Colour colour = Colour::None;
    std::vector<std::wstring> tags;    // in the order added
    bool empty() const { return colour == Colour::None && tags.empty(); }
};

struct Collection {
    std::string id;                    // stays the same when it's renamed
    std::wstring name;
    std::vector<std::wstring> items;   // paths, in the order added
};

class Store {
public:
    const Label* find(const std::wstring& path) const;
    Colour colourOf(const std::wstring& path) const;
    void setColour(const std::wstring& path, Colour c);
    bool addTag(const std::wstring& path, const std::wstring& tag);      // false if it was already there (or empty)
    bool removeTag(const std::wstring& path, const std::wstring& tag);
    bool hasTag(const std::wstring& path, const std::wstring& tag) const;

    // Every tag in use, the most used first (then by name), with how many things have it.
    std::vector<std::pair<std::wstring, int>> tags() const;
    std::vector<std::wstring> pathsWithTag(const std::wstring& tag) const;
    std::vector<std::wstring> pathsWithColour(Colour c) const;
    std::vector<const Label*> all() const;

    // Files moved or renamed `from` -> `to` (a folder takes what's inside it along), or deleted.
    void moved(const std::wstring& from, const std::wstring& to);
    void removed(const std::wstring& path);

    // Collections: groups of files and folders from anywhere, without moving them.
    const std::vector<Collection>& collections() const { return collections_; }
    const Collection* collection(const std::string& id) const;
    std::string createCollection(const std::wstring& name);
    bool renameCollection(const std::string& id, const std::wstring& name);
    bool deleteCollection(const std::string& id);
    int addToCollection(const std::string& id, const std::vector<std::wstring>& paths);         // how many were new
    int removeFromCollection(const std::string& id, const std::vector<std::wstring>& paths);
    std::vector<std::string> collectionsWith(const std::wstring& path) const;

    std::string toJson() const;
    bool fromJson(const std::string& json);   // false (and empty) if it isn't the right shape
    std::uint64_t changes() const { return changes_; }   // goes up with every change, for saving

private:
    std::map<std::wstring, Label> labels_;   // by lower-case path
    std::vector<Collection> collections_;
    std::uint64_t changes_ = 0;
    std::uint64_t nextId_ = 1;
    Label& at(const std::wstring& path);
    void tidy(const std::wstring& key);
};

// Paths compared as Windows does (case doesn't matter).
std::wstring pathKey(const std::wstring& path);
// Whether `path` is `folder` or inside it.
bool within(const std::wstring& path, const std::wstring& folder);

std::string toUtf8(const std::wstring& w);
std::wstring fromUtf8(const std::string& s);

}  // namespace nrf::labels
