// NorthRay Files: batch rename. Many files in one folder renamed by rules: a pattern ("{name}", "{n}" a
// counter, "{date}" the modified day, "{folder}" the folder's name), find and replace, and a change of case;
// the extension kept unless asked. Every new name is checked (renameProblem's rules, and against the others
// and what else is in the folder) before anything is done, and the renames are put in an order that never
// overwrites anything, using a temporary name where two swap. Portable and unit-tested; the window shows
// the preview and does the renames.
#pragma once

#include <string>
#include <vector>

namespace nrf::batch {

enum class Case { Keep, Lower, Upper, Title };

struct Rules {
    std::wstring pattern = L"{name}";   // {name} {n} {n:3} (3 digits) {date} {folder} {ext}; "" is {name}
    std::wstring find, replace;         // in the name after the pattern; "" finds nothing
    bool matchCase = false;
    Case caseChange = Case::Keep;
    int start = 1, step = 1;            // the counter {n}
    int digits = 0;                     // {n} padded with zeros (also {n:3})
    bool changeExtension = false;       // false: the extension is kept as it was and the rules see the name only
};

struct File {
    std::wstring name;   // "IMG_0412.jpg"
    std::wstring date;   // "2026-11-16" (modified), for {date}
    bool folder = false;
};

struct Change {
    std::wstring from, to;
    std::wstring problem;   // "" when it can be done
    bool same = false;      // the name doesn't change
};

// The new names, in the order given (the counter counts in that order). `others` are the folder's names
// that aren't being renamed.
std::vector<Change> plan(const std::vector<File>& files, const Rules& rules, const std::wstring& folderName,
                         const std::vector<std::wstring>& others);
bool canDo(const std::vector<Change>& changes);   // something changes and nothing has a problem

// The renames to make, one after another, so no step lands on a name still in use: chains are done from the
// end, and swaps go through a temporary name ("~nr-rename-1.tmp"; not one in `names`).
struct Step {
    std::wstring from, to;
};
std::vector<Step> order(const std::vector<Change>& changes, const std::vector<std::wstring>& names);

}  // namespace nrf::batch
