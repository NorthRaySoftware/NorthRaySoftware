// Writing Studio: the workspaces' toolbars (phase 3 of the director's combined request, 0.100). Portable and tested.
//  - Which tools a bar shows and in what order: the tools' default groups and order, with the writer's changes (order,
//    group, pinned, hidden) on top. New tools join their default group; forgotten ids are ignored.
//  - How they fit in ONE row: labels, then icons only (unpinned first), then the overflow menu (unpinned from the end
//    first). The row never wraps.
#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

namespace studio::toolbar {

// The groups, in their order along the bar. None: never on the bar (only in More).
enum Group { None = 0, Create, ImportExport, Organise, Check, Settings, kGroups };
const char* groupName(int group);   // "Create", "Import & export", ...

struct Tool {
    std::string id;
    int group = None;        // its default group
    bool rare = false;       // destructive or rare: always in More, never on the bar
};

// What the writer changed for one workspace's bar.
struct Custom {
    std::vector<std::string> order;          // tool ids in the order the writer left them (only those moved need be here)
    std::map<std::string, int> groups;       // tools moved to another group
    std::set<std::string> pinned, hidden;
    bool off = false;                        // the bar hidden (its menu button instead)
    bool empty() const { return order.empty() && groups.empty() && pinned.empty() && hidden.empty() && !off; }
    std::string toJson() const;
    static Custom fromJson(const std::string& json);   // empty if it isn't one
};

int groupOf(const Tool& t, const Custom& c);
// The tools on the bar, in order: by group (Create first), and within a group in the writer's order; tools not in
// that order (new ones, or before any move) follow in their default order. Hidden and rare tools and tools with no
// group are left out. Indexes into `tools`.
std::vector<size_t> arrange(const std::vector<Tool>& tools, const Custom& c);

// Moves `id` one place left (-1) or right (+1) among the bar's tools, within its group or across into the next group
// (it then joins that group). Writes the whole order into `c`.
void move(const std::vector<Tool>& tools, Custom& c, const std::string& id, int delta);
void setGroup(Custom& c, const std::string& id, int group, const std::vector<Tool>& tools);
void togglePinned(Custom& c, const std::string& id);
void setHidden(Custom& c, const std::string& id, bool hidden);

// Fitting one row. Each tool on the bar: its width with its label and with its icon alone (0 if it has no icon: it
// keeps its label). `avail` is the row's width; `gap` between tools, `groupGap` extra between groups; `moreW` the
// More button's width (shown when anything is in More, or `moreAlways`).
struct Measure { float withLabel = 0, iconOnly = 0; bool pinned = false; int group = None; };
struct Placed { size_t index = 0; bool label = true; float x = 0, w = 0; };
struct Fit {
    std::vector<Placed> shown;         // in order, x from 0
    std::vector<size_t> overflow;      // indexes (into the measures) that went into More
    bool more = false;                 // the More button shows
    float moreX = 0;
};
Fit fit(const std::vector<Measure>& m, float avail, float gap, float groupGap, float moreW, bool moreAlways);

}  // namespace studio::toolbar
