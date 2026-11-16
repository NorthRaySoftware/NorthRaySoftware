// NorthRay Capture: simple automation (Settings › Automation). A rule is "when a program opens (or
// closes), switch to a scene": open Krita and the Art scene comes up, close the game and you're back on
// Just chatting. Portable and unit-tested; the window lists the running programs every 2 seconds and
// switches the scene the way a timer reaching zero does (the program scene in studio mode).
//
// Rules fire on a change only: programs already open when Capture starts (or when a rule is made) don't
// count as opening. When several fire at once, the last one in the list wins. Scenes are kept by name,
// so a rule carries across scene collections that have a scene of that name.
#pragma once

#include <set>
#include <string>
#include <vector>

namespace ncap::automation {

enum class When { Opens, Closes };

struct Rule {
    std::string exe;            // the program's file name, lower case ("krita.exe")
    std::string label;          // what people call it ("Krita"); "" shows the file name
    When when = When::Opens;
    std::string scene;          // the scene's name
    bool on = true;
    bool onlyLive = false;      // only while streaming or recording
    bool operator==(const Rule&) const = default;
};

constexpr std::size_t kMaxRules = 20;

// "krita.exe" from "C:\Program Files\Krita\bin\Krita.EXE", "krita" or " Krita.exe "; "" if nothing's left.
std::string normaliseExe(const std::string& nameOrPath);
// "When Krita opens, switch to Art" (and ", while live" when it's only then).
std::string describe(const Rule& r);

std::string toJson(const std::vector<Rule>& rules);              // a JSON array
std::vector<Rule> fromJson(const std::string& json);             // bad entries skipped, at most kMaxRules

// Watches what's running (lower-case file names, normaliseExe'd) and says which rule fires.
class Watcher {
public:
    // The index of the rule to act on now, or -1. The first call only notes what's running.
    int update(const std::set<std::string>& running, const std::vector<Rule>& rules, bool live);
    void reset() { primed_ = false; }

private:
    bool primed_ = false;
    std::set<std::string> last_;
};

}  // namespace ncap::automation
