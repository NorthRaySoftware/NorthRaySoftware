// Writing Studio: structures, plot threads and outlines as plain text. See studio/planning.hpp.
#include "studio/planning.hpp"
#include "studio/calendar.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <functional>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>
#include <sstream>

namespace studio::plan {

using nlohmann::json;

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == '\r') continue;
        if (c == '\n') { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    // A byte-order mark at the start.
    if (!out.empty() && out[0].rfind("\xEF\xBB\xBF", 0) == 0) out[0] = out[0].substr(3);
    return out;
}

// "Label: value" with a label among `names` (any case); the value is trimmed. The label may be bold (**Label:**).
bool field(const std::string& line, std::initializer_list<const char*> names, std::string& value) {
    std::string l = trim(line);
    if (l.rfind("**", 0) == 0) {
        l = l.substr(2);
        const size_t e = l.find("**");
        if (e != std::string::npos) l.erase(e, 2);
    }
    const size_t colon = l.find(':');
    if (colon == std::string::npos || colon == 0) return false;
    const std::string key = lower(trim(l.substr(0, colon)));
    for (const char* n : names)
        if (key == n) { value = trim(l.substr(colon + 1)); return true; }
    return false;
}

std::vector<std::string> splitList(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',' || c == ';') { if (!trim(cur).empty()) out.push_back(trim(cur)); cur.clear(); }
        else cur += c;
    }
    if (!trim(cur).empty()) out.push_back(trim(cur));
    return out;
}

std::string slug(const std::string& name) {
    std::string out;
    for (char c : name) {
        if (std::isalnum((unsigned char)c)) out += (char)std::tolower((unsigned char)c);
        else if (!out.empty() && out.back() != '-') out += '-';
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "beat" : out;
}

std::string unique(const std::string& base, const std::set<std::string>& taken) {
    if (!taken.count(base)) return base;
    for (int i = 2;; ++i) {
        const std::string s = base + "-" + std::to_string(i);
        if (!taken.count(s)) return s;
    }
}

bool allDigits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit((unsigned char)c); });
}

std::string percent(double at) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%g%%", std::round(at * 1000) / 10);
    return buf;
}

json parseObj(const std::string& s) {
    json j = json::parse(s, nullptr, false);
    return j.is_object() ? j : json::object();
}

Structure fromJson(const json& j) {
    Structure s;
    s.id = j.value("id", "");
    s.name = j.value("name", "");
    s.about = j.value("about", "");
    s.own = true;
    if (j.contains("beats") && j["beats"].is_array())
        for (auto& b : j["beats"])
            if (b.is_object()) s.beats.push_back({b.value("id", ""), b.value("name", ""), b.value("at", 0.0), b.value("about", "")});
    return s;
}

json toJson(const Structure& s) {
    json beats = json::array();
    for (auto& b : s.beats) beats.push_back({{"id", b.id}, {"name", b.name}, {"at", b.at}, {"about", b.about}});
    return {{"id", s.id}, {"name", s.name}, {"about", s.about}, {"beats", beats}};
}

// Chapters of the story in reading order (not front or back matter): numbered 1, 2, 3...
std::vector<const ManuscriptNode*> storyChapters(const Library& lib, const Id& book) {
    std::vector<const ManuscriptNode*> out;
    for (const ManuscriptNode* n : lib.manuscriptOf(book))
        if (n->kind == NodeKind::Chapter && n->matter == 0) out.push_back(n);
    return out;
}

const char* units[] = {"day", "week", "month", "year"};

}  // namespace

// ------------------------------------------------------------------ structures

const std::vector<Structure>& builtInStructures() {
    static const std::vector<Structure> all = [] {
        std::vector<Structure> t = {
            {"three-act", "Three acts", "The classic shape: set up a world and a want, complicate it until everything is at stake, then resolve it.",
             {{"setup", "Setup", 0.0, "Who we're with, what they want, and the world they want it in."},
              {"inciting", "Inciting incident", 0.12, "Something happens that the hero can't ignore. The story begins to move."},
              {"turn1", "First turning point", 0.25, "A decision that can't be undone carries the hero into the new situation of the middle."},
              {"midpoint", "Midpoint", 0.5, "A reversal or revelation: the goal or the stakes change, and the hero stops reacting and starts acting."},
              {"turn2", "Second turning point", 0.75, "The worst moment: what the hero relied on fails, and a last, costly choice is made."},
              {"climax", "Climax", 0.88, "The final confrontation, where the want and the need meet."},
              {"resolution", "Resolution", 0.97, "The new normal. What changed, and what it cost."}}},
            {"four-act", "Four acts", "The middle split in two at the midpoint, so each quarter has its own job: set up, react, act, resolve.",
             {{"setup", "Act one: setup", 0.0, "The hero's world and want; ends when they're pushed into the story."},
              {"reaction", "Act two: reaction", 0.25, "The hero responds to the new situation, mostly on the back foot."},
              {"midpoint", "Midpoint", 0.5, "A discovery or reversal that turns the hero from reacting to acting."},
              {"action", "Act three: action", 0.5, "The hero takes the fight to the problem; it gets harder and costs more."},
              {"crisis", "Crisis", 0.75, "Everything falls apart, and the last act begins with a choice."},
              {"resolution", "Act four: resolution", 0.75, "The climax, and the world after it."}}},
            {"fifteen", "Fifteen beats", "A detailed beat sheet popularised for screenwriting (Blake Snyder's Save the Cat!) and widely used for novels.",
             {{"opening", "Opening image", 0.0, "A snapshot of the hero's life before: the 'before' picture."},
              {"theme", "Theme stated", 0.05, "Someone hints at what the hero must learn, though they don't see it yet."},
              {"setup", "Setup", 0.06, "The hero's world, flaws and the things that need fixing."},
              {"catalyst", "Catalyst", 0.1, "The event that knocks life off course."},
              {"debate", "Debate", 0.15, "Doubt: should the hero go? Can they?"},
              {"break2", "Break into two", 0.2, "The hero chooses to step into the new world."},
              {"bstory", "B story", 0.22, "A second thread, often a relationship, that carries the theme."},
              {"fun", "Fun and games", 0.3, "The promise of the premise: the new world explored."},
              {"midpoint", "Midpoint", 0.5, "A false victory or false defeat; the stakes rise."},
              {"closein", "Bad guys close in", 0.55, "Pressure from outside and doubt from within."},
              {"lost", "All is lost", 0.75, "The low point; something (or someone) is lost."},
              {"dark", "Dark night of the soul", 0.78, "The hero sits with the loss, and finds the lesson."},
              {"break3", "Break into three", 0.8, "The answer, from the A and B stories together."},
              {"finale", "Finale", 0.85, "The hero puts the lesson to work and wins (or loses well)."},
              {"final", "Final image", 1.0, "The 'after' picture, mirroring the opening."}}},
            {"hero", "The hero's journey", "Twelve stages of the mythic journey, after Joseph Campbell as set out for writers by Christopher Vogler.",
             {{"ordinary", "Ordinary world", 0.0, "Home, before the adventure."},
              {"call", "Call to adventure", 0.08, "A problem or challenge appears."},
              {"refusal", "Refusal of the call", 0.12, "Fear or duty holds the hero back."},
              {"mentor", "Meeting the mentor", 0.17, "Advice, a gift or training."},
              {"threshold", "Crossing the threshold", 0.25, "Into the special world; no way back."},
              {"tests", "Tests, allies, enemies", 0.35, "Learning the rules of the new world, and who's who."},
              {"approach", "Approach to the inmost cave", 0.45, "Preparing for the central ordeal."},
              {"ordeal", "The ordeal", 0.5, "A brush with death, real or figurative."},
              {"reward", "Reward", 0.6, "The prize claimed: an object, a truth, a reconciliation."},
              {"road", "The road back", 0.75, "Consequences follow; the way home is dangerous."},
              {"resurrection", "Resurrection", 0.88, "A final test where everything learned is used."},
              {"elixir", "Return with the elixir", 0.97, "Home again, changed, bringing something back."}}},
            {"story-circle", "Story circle", "Eight steps round a circle, from comfort to change and back (after Dan Harmon, from Campbell).",
             {{"you", "You", 0.0, "A character in their zone of comfort."},
              {"need", "Need", 0.12, "They want something."},
              {"go", "Go", 0.25, "They enter an unfamiliar situation."},
              {"search", "Search", 0.37, "They adapt to it, and struggle."},
              {"find", "Find", 0.5, "They get what they wanted."},
              {"take", "Take", 0.62, "They pay a heavy price for it."},
              {"return", "Return", 0.75, "They go back to the familiar."},
              {"change", "Change", 0.88, "Having changed."}}},
            {"seven", "Seven points", "Work from the end backwards: the resolution mirrors the hook, with turns and pinches between (after Dan Wells).",
             {{"hook", "Hook", 0.0, "The starting state, the opposite of the resolution."},
              {"turn1", "Plot turn 1", 0.15, "The call to adventure: the world changes."},
              {"pinch1", "Pinch 1", 0.35, "Pressure: the antagonist shows its strength; the hero must act."},
              {"midpoint", "Midpoint", 0.5, "The hero moves from reaction to action."},
              {"pinch2", "Pinch 2", 0.65, "Things go wrong; the plan fails, the jaws of defeat."},
              {"turn2", "Plot turn 2", 0.85, "The hero finds the last piece needed to win."},
              {"resolution", "Resolution", 1.0, "The climax and the end state."}}},
            {"fichtean", "Fichtean curve", "Straight into trouble: a run of rising crises, each worse than the last, to a climax near the end (after John Gardner).",
             {{"crisis1", "First crisis", 0.0, "Open in the middle of a problem; backstory comes later, in passing."},
              {"crisis2", "Second crisis", 0.2, "A harder problem, often born of the first one's solution."},
              {"crisis3", "Third crisis", 0.4, "The stakes climb; what the hero learns along the way is revealed."},
              {"crisis4", "Fourth crisis", 0.6, "The pressure is nearly unbearable."},
              {"climax", "Climax", 0.8, "The biggest crisis, and the turning point of the whole story."},
              {"falling", "Falling action", 0.92, "A short close: the consequences settle."}}},
            {"five-act", "Five acts", "Gustav Freytag's pyramid, from classical drama: rising to a peak and falling to a close.",
             {{"exposition", "Exposition", 0.0, "The situation, the people and the conflict to come."},
              {"rising", "Rising action", 0.2, "Complications build."},
              {"climax", "Climax", 0.5, "The turning point, at the top of the pyramid."},
              {"falling", "Falling action", 0.7, "Consequences unwind."},
              {"denouement", "Dénouement", 0.9, "The knots are untied; the end."}}},
            {"kishotenketsu", "Kishōtenketsu", "Four parts from East Asian storytelling, built on contrast rather than conflict; also used for four-panel comics.",
             {{"ki", "Ki (introduction)", 0.0, "Characters and setting."},
              {"sho", "Shō (development)", 0.25, "The situation develops, without a major change."},
              {"ten", "Ten (twist)", 0.5, "Something unexpected appears, seemingly unconnected."},
              {"ketsu", "Ketsu (reconciliation)", 0.75, "The parts come together and make a new sense."}}},
            {"mystery", "Mystery", "The shape of a whodunit: a crime, an investigation with false leads, and a reveal that was fair all along.",
             {{"crime", "The crime", 0.0, "The body, the theft, the disappearance: the question the book will answer."},
              {"detective", "The detective commits", 0.1, "Someone takes the case, and has a reason to see it through."},
              {"clues", "Clues and suspects", 0.2, "The suspects, their secrets, and the first clues (the real one hidden among them)."},
              {"twist", "A false solution", 0.5, "A suspect seems guilty, or a second crime upends the theory."},
              {"danger", "The detective in danger", 0.75, "The culprit pushes back; the stakes become personal."},
              {"reveal", "The reveal", 0.88, "The truth, explained from the clues the reader already had."},
              {"aftermath", "Aftermath", 0.97, "Justice, or its failure, and what it cost."}}},
            {"romance", "Romance", "The beats of a love story: meeting, resisting, falling, breaking apart and choosing each other.",
             {{"apart", "Apart", 0.0, "Each lead's life, and what's missing from it."},
              {"meet", "The meeting", 0.08, "They meet; something sparks, or grates."},
              {"resist", "Resisting", 0.2, "Reasons this can't work, inside and out."},
              {"falling", "Falling", 0.35, "Time together; the walls come down."},
              {"together", "Together", 0.5, "A kiss, a night, a promise: they're in it now."},
              {"doubt", "Doubt", 0.65, "The old wound or the outside pressure returns."},
              {"breakup", "The break", 0.75, "It falls apart, and it seems for good."},
              {"grand", "The grand gesture", 0.88, "One of them risks everything to show they've changed."},
              {"ever", "Together, for good", 0.97, "The happy ending (or the hopeful one)."}}},
        };
        return t;
    }();
    return all;
}

std::vector<Structure> ownStructures(const Library& lib, const std::string& scope) {
    std::vector<Structure> out;
    const json j = json::parse(lib.setting("structures:" + scope), nullptr, false);
    if (!j.is_array()) return out;
    for (auto& s : j)
        if (s.is_object()) {
            Structure st = fromJson(s);
            if (!st.id.empty() && !st.name.empty()) out.push_back(std::move(st));
        }
    return out;
}

bool setOwnStructures(Library& lib, const std::string& scope, const std::vector<Structure>& list) {
    json j = json::array();
    for (auto& s : list) j.push_back(toJson(s));
    return lib.setSetting("structures:" + scope, j.dump());
}

const Structure* findStructure(const std::vector<Structure>& all, const std::string& id) {
    for (auto& s : all) if (s.id == id) return &s;
    return nullptr;
}

std::vector<Structure> allStructures(const Library& lib, const std::string& scope) {
    std::vector<Structure> out = builtInStructures();
    for (auto& s : ownStructures(lib, scope)) out.push_back(std::move(s));
    return out;
}

std::string structureText(const Structure& s) {
    std::string out = "Structure: " + s.name + "\n";
    if (!s.about.empty()) out += "About: " + s.about + "\n";
    for (auto& b : s.beats) {
        out += percent(b.at) + " " + b.name;
        if (!b.about.empty()) out += ": " + b.about;
        out += "\n";
    }
    return out;
}

std::vector<Structure> parseStructures(const std::string& text, std::vector<std::string>* skipped) {
    std::vector<Structure> out;
    std::vector<std::vector<bool>> placed;   // per structure, per beat: had a percentage
    std::set<std::string> takenIds;
    for (const auto& b : builtInStructures()) takenIds.insert(b.id);
    auto finish = [&] {
        if (out.empty()) return;
        Structure& s = out.back();
        // Beats without a percentage are spread evenly between their neighbours.
        auto& p = placed.back();
        const size_t n = s.beats.size();
        for (size_t i = 0; i < n; ++i) {
            if (p[i]) continue;
            size_t j = i;
            while (j < n && !p[j]) ++j;
            const double from = i == 0 ? 0.0 : s.beats[i - 1].at;
            const double to = j < n ? s.beats[j].at : 1.0;
            const double count = double(j - i);
            for (size_t k = i; k < j; ++k) {
                const double step = double(k - i);
                s.beats[k].at = i == 0 ? from + (to - from) * step / count : from + (to - from) * (step + 1) / (count + 1);
                p[k] = true;
            }
            i = j - 1;
        }
        std::stable_sort(s.beats.begin(), s.beats.end(), [](const Beat& a, const Beat& b) { return a.at < b.at; });
        std::set<std::string> beatIds;
        for (auto& b : s.beats) { b.id = unique(slug(b.name), beatIds); beatIds.insert(b.id); }
        if (s.name.empty()) s.name = "My structure";
        s.id = unique("own-" + slug(s.name), takenIds);
        takenIds.insert(s.id);
    };
    for (const std::string& raw : lines(text)) {
        std::string l = trim(raw);
        if (l.empty() || l.rfind("//", 0) == 0) continue;
        while (!l.empty() && (l[0] == '#' || l[0] == '-' || l[0] == '*' || l[0] == '>')) l = trim(l.substr(1));   // headings, bullets
        std::string v;
        if (field(l, {"structure", "template"}, v)) {
            finish();
            Structure s;
            s.name = v;
            s.own = true;
            out.push_back(s);
            placed.emplace_back();
            continue;
        }
        if (out.empty()) {   // beats before any "Structure:" line start one
            out.push_back(Structure{});
            out.back().own = true;
            placed.emplace_back();
        }
        Structure& s = out.back();
        if (field(l, {"about", "description"}, v)) { s.about = v; continue; }
        if (field(l, {"beat"}, v)) l = v;
        // An optional percentage (or a fraction 0..1 with a point), then the name, then ": about" or "| about".
        bool has = false;
        double at = 0;
        size_t i = 0;
        while (i < l.size() && (std::isdigit((unsigned char)l[i]) || l[i] == '.')) ++i;
        if (i > 0) {
            const std::string num = l.substr(0, i);
            size_t k = i;
            const bool pc = k < l.size() && l[k] == '%';
            if (pc) ++k;
            if (k == l.size() || std::isspace((unsigned char)l[k]) || l[k] == ':' || l[k] == '|') {
                try { at = std::stod(num); has = true; } catch (...) {}
                if (has) {
                    if (pc || at > 1.0) at /= 100.0;
                    at = std::clamp(at, 0.0, 1.0);
                    l = trim(l.substr(k));
                    if (!l.empty() && (l[0] == ':' || l[0] == '|')) l = trim(l.substr(1));
                }
            }
        }
        size_t cut = l.find('|');
        if (cut == std::string::npos) cut = l.find(':');
        Beat b;
        b.name = trim(cut == std::string::npos ? l : l.substr(0, cut));
        b.about = cut == std::string::npos ? "" : trim(l.substr(cut + 1));
        b.at = at;
        if (b.name.empty()) { if (skipped) skipped->push_back(raw); continue; }
        s.beats.push_back(b);
        placed.back().push_back(has);
    }
    finish();
    // Drop structures with no beats.
    out.erase(std::remove_if(out.begin(), out.end(), [](const Structure& s) { return s.beats.empty(); }), out.end());
    return out;
}

BookStructure bookStructure(const Library& lib, const Id& book) {
    BookStructure out;
    const json j = parseObj(lib.setting("structure:" + book));
    out.templateId = j.value("template", "");
    if (j.contains("beats") && j["beats"].is_object())
        for (auto& [beat, v] : j["beats"].items()) {
            std::vector<Id> ids;
            if (v.is_string() && !v.get<std::string>().empty()) ids.push_back(v.get<std::string>());
            else if (v.is_array())
                for (auto& x : v) if (x.is_string() && !x.get<std::string>().empty()) ids.push_back(x.get<std::string>());
            if (!ids.empty()) out.beats[beat] = ids;
        }
    if (j.contains("notes") && j["notes"].is_object())
        for (auto& [beat, v] : j["notes"].items())
            if (v.is_string() && !v.get<std::string>().empty()) out.notes[beat] = v.get<std::string>();
    return out;
}

bool setBookStructure(Library& lib, const Id& book, const BookStructure& s) {
    json beats = json::object(), notes = json::object();
    for (auto& [k, v] : s.beats) if (!v.empty()) beats[k] = v;
    for (auto& [k, v] : s.notes) if (!v.empty()) notes[k] = v;
    json j = {{"template", s.templateId}, {"beats", beats}};
    if (!notes.empty()) j["notes"] = notes;
    return lib.setSetting("structure:" + book, j.dump());
}

// ------------------------------------------------------------------ plot threads

const Thread* Threads::find(const std::string& id) const {
    for (auto& t : threads) if (t.id == id) return &t;
    return nullptr;
}

const Thread* Threads::named(const std::string& name) const {
    const std::string n = lower(trim(name));
    for (auto& t : threads) if (lower(trim(t.name)) == n) return &t;
    return nullptr;
}

bool Threads::has(const Id& node, const std::string& thread) const {
    auto it = nodes.find(node);
    return it != nodes.end() && std::find(it->second.begin(), it->second.end(), thread) != it->second.end();
}

Threads threadsOf(const Library& lib, const Id& book) {
    Threads out;
    const json j = parseObj(lib.setting("threads:" + book));
    if (j.contains("threads") && j["threads"].is_array())
        for (auto& t : j["threads"])
            if (t.is_object() && !t.value("id", "").empty())
                out.threads.push_back({t.value("id", ""), t.value("name", ""), t.value("colour", ""), t.value("about", "")});
    if (j.contains("nodes") && j["nodes"].is_object())
        for (auto& [node, v] : j["nodes"].items()) {
            if (!v.is_array()) continue;
            std::vector<std::string> ids;
            for (auto& x : v)
                if (x.is_string() && out.find(x.get<std::string>())) ids.push_back(x.get<std::string>());
            if (!ids.empty()) out.nodes[node] = ids;
        }
    for (size_t i = 0; i < out.threads.size(); ++i)
        if (out.threads[i].colour.empty()) out.threads[i].colour = threadColour(i);
    return out;
}

bool setThreads(Library& lib, const Id& book, const Threads& t) {
    json threads = json::array(), nodes = json::object();
    for (auto& x : t.threads) threads.push_back({{"id", x.id}, {"name", x.name}, {"colour", x.colour}, {"about", x.about}});
    for (auto& [node, ids] : t.nodes) {
        std::vector<std::string> keep;
        for (auto& id : ids) if (t.find(id) && std::find(keep.begin(), keep.end(), id) == keep.end()) keep.push_back(id);
        if (!keep.empty()) nodes[node] = keep;
    }
    return lib.setSetting("threads:" + book, json{{"threads", threads}, {"nodes", nodes}}.dump());
}

std::string newThreadId(const Threads& t) {
    for (int i = (int)t.threads.size() + 1;; ++i) {
        const std::string id = "t" + std::to_string(i);
        if (!t.find(id)) return id;
    }
}

std::string threadColour(size_t n) {
    static const char* palette[] = {"#D9534F", "#2E86C1", "#28A745", "#E08E0B", "#8E44AD", "#16A085", "#D35490", "#7F8C3D", "#5D6D7E", "#C0703A"};
    return palette[n % (sizeof palette / sizeof *palette)];
}

std::string threadsText(const Library& lib, const Id& book, const Threads& t) {
    const auto chapters = storyChapters(lib, book);
    std::map<Id, int> number;
    for (size_t i = 0; i < chapters.size(); ++i) number[chapters[i]->id] = (int)i + 1;
    std::string out;
    for (auto& th : t.threads) {
        if (!out.empty()) out += "\n";
        out += "Thread: " + th.name + "\n";
        if (!th.colour.empty()) out += "Colour: " + th.colour + "\n";
        if (!th.about.empty()) out += "About: " + th.about + "\n";
        std::vector<int> nums;
        std::vector<std::string> titles;
        for (const ManuscriptNode* n : lib.manuscriptOf(book)) {
            if (!t.has(n->id, th.id)) continue;
            auto it = number.find(n->id);
            if (it != number.end()) nums.push_back(it->second);
            else titles.push_back(n->title);
        }
        std::vector<std::string> parts;
        for (size_t i = 0; i < nums.size();) {
            size_t j = i;
            while (j + 1 < nums.size() && nums[j + 1] == nums[j] + 1) ++j;
            parts.push_back(j > i + 1 ? std::to_string(nums[i]) + "-" + std::to_string(nums[j]) : std::to_string(nums[i]));
            if (j == i + 1) parts.push_back(std::to_string(nums[j]));
            i = j + 1;
        }
        for (auto& s : titles) parts.push_back(s);
        if (!parts.empty()) {
            out += "Chapters: ";
            for (size_t i = 0; i < parts.size(); ++i) out += (i ? ", " : "") + parts[i];
            out += "\n";
        }
    }
    return out;
}

namespace {
// Chapter numbers, ranges or titles to nodes; titles not found go to `unknown`.
std::vector<Id> resolveChapters(const Library& lib, const Id& book, const std::string& list, std::vector<std::string>& unknown) {
    const auto chapters = storyChapters(lib, book);
    std::vector<Id> out;
    for (const std::string& item : splitList(list)) {
        std::string a = item, b;
        const size_t dash = item.find('-');
        if (dash != std::string::npos) { a = trim(item.substr(0, dash)); b = trim(item.substr(dash + 1)); }
        if (allDigits(a) && (b.empty() || allDigits(b)) && (dash == std::string::npos || !b.empty())) {
            int from = std::stoi(a), to = b.empty() ? from : std::stoi(b);
            if (to < from) std::swap(from, to);
            bool any = false;
            for (int k = from; k <= to; ++k)
                if (k >= 1 && k <= (int)chapters.size()) { out.push_back(chapters[k - 1]->id); any = true; }
            if (!any) unknown.push_back(item);
            continue;
        }
        const std::string want = lower(item);
        bool found = false;
        for (const ManuscriptNode* n : lib.manuscriptOf(book))
            if (lower(trim(n->title)) == want) { out.push_back(n->id); found = true; break; }
        if (!found) unknown.push_back(item);
    }
    return out;
}

void tag(Threads& t, const Id& node, const std::string& thread) {
    auto& v = t.nodes[node];
    if (std::find(v.begin(), v.end(), thread) == v.end()) v.push_back(thread);
}
}  // namespace

bool importThreads(Library& lib, const Id& book, const std::string& text, ThreadImport& out, bool chapterLists) {
    Threads t = threadsOf(lib, book);
    std::string current;   // id of the thread being read
    std::set<std::string> described;   // threads whose description this text has started (it replaces the old one)
    for (const std::string& raw : lines(text)) {
        std::string l = trim(raw);
        while (!l.empty() && (l[0] == '#' || l[0] == '-' || l[0] == '*')) l = trim(l.substr(1));
        if (l.empty()) continue;
        std::string v;
        if (field(l, {"thread", "plot thread", "subplot"}, v)) {
            if (v.empty()) { current.clear(); continue; }
            if (const Thread* had = t.named(v)) { current = had->id; ++out.updated; }
            else {
                Thread n{newThreadId(t), v, threadColour(t.threads.size()), ""};
                t.threads.push_back(n);
                current = n.id;
                ++out.added;
            }
            continue;
        }
        if (current.empty()) continue;
        Thread* th = nullptr;
        for (auto& x : t.threads) if (x.id == current) th = &x;
        if (!th) continue;
        if (field(l, {"colour", "color"}, v)) {
            if (!v.empty() && v[0] != '#') v = "#" + v;
            if (v.size() == 7) th->colour = v;
        } else if (field(l, {"about", "description", "notes"}, v)) {
            if (described.insert(current).second) th->about.clear();
            if (lower(trim(v)) != lower(trim(th->about))) th->about = th->about.empty() ? v : th->about + "\n" + v;
        } else if (field(l, {"chapters", "scenes", "in"}, v)) {
            if (!chapterLists) continue;
            for (const Id& n : resolveChapters(lib, book, v, out.unknown)) {
                if (!t.has(n, current)) ++out.tagged;
                tag(t, n, current);
            }
        } else {
            if (described.insert(current).second) th->about.clear();
            // A line repeating the description (as older copies of a plan have) isn't added twice.
            bool repeat = false;
            for (const std::string& had : lines(th->about)) repeat = repeat || lower(trim(had)) == lower(trim(l));
            if (!repeat) th->about = th->about.empty() ? l : th->about + "\n" + l;
        }
    }
    return setThreads(lib, book, t);
}

// ------------------------------------------------------------------ places and story events

std::map<Id, Id> nodePlaces(const Library& lib, const Id& book) {
    std::map<Id, Id> out;
    const json j = parseObj(lib.setting("nodeplaces:" + book));
    for (auto& [node, v] : j.items())
        if (v.is_string() && !v.get<std::string>().empty()) out[node] = v.get<std::string>();
    return out;
}

bool setNodePlace(Library& lib, const Id& book, const Id& node, const Id& place) {
    json j = parseObj(lib.setting("nodeplaces:" + book));
    if (place.empty()) j.erase(node);
    else j[node] = place;
    return lib.setSetting("nodeplaces:" + book, j.dump());
}

std::map<Id, std::vector<Id>> nodeCast(const Library& lib, const Id& book) {
    std::map<Id, std::vector<Id>> out;
    const json j = parseObj(lib.setting("nodecast:" + book));
    for (auto& [node, v] : j.items()) {
        if (!v.is_array()) continue;
        std::vector<Id> ids;
        for (auto& x : v)
            if (x.is_string() && !x.get<std::string>().empty() && std::find(ids.begin(), ids.end(), x.get<std::string>()) == ids.end())
                ids.push_back(x.get<std::string>());
        if (!ids.empty()) out[node] = std::move(ids);
    }
    return out;
}

bool setNodeCast(Library& lib, const Id& book, const Id& node, const std::vector<Id>& cast) {
    json j = parseObj(lib.setting("nodecast:" + book));
    json arr = json::array();
    for (const Id& id : cast)
        if (!id.empty() && std::find(arr.begin(), arr.end(), id) == arr.end() && arr.size() < 200) arr.push_back(id);
    if (arr.empty()) j.erase(node);
    else j[node] = arr;
    return lib.setSetting("nodecast:" + book, j.dump());
}

std::vector<Id> castOf(const Library& lib, const Id& book, const Id& node) {
    const auto all = nodeCast(lib, book);
    std::vector<Id> out;
    auto add = [&](const Id& n) {
        if (auto it = all.find(n); it != all.end())
            for (const Id& id : it->second) {
                const Entry* e = lib.entry(id);
                if (e && !e->deleted && std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
            }
    };
    add(node);
    // Its scenes, in order, and theirs.
    std::function<void(const Id&)> down = [&](const Id& parent) {
        for (const ManuscriptNode* c : lib.childrenOf(book, parent)) { add(c->id); down(c->id); }
    };
    down(node);
    return out;
}

std::map<Id, double> timelineEvents(const Library& lib, const Id& book) {
    std::map<Id, double> out;
    const json j = parseObj(lib.setting("timeline:" + book));
    if (j.contains("events") && j["events"].is_object())
        for (auto& [id, v] : j["events"].items())
            if (v.is_object() && v.contains("day") && v["day"].is_number()) out[id] = v["day"].get<double>();
    return out;
}

bool setTimelineEvent(Library& lib, const Id& book, const Id& entry, std::optional<double> day) {
    json j = parseObj(lib.setting("timeline:" + book));
    if (!j.contains("events") || !j["events"].is_object()) j["events"] = json::object();
    if (day) {
        // Keep its time of day (0.80).
        if (!j["events"].contains(entry) || !j["events"][entry].is_object()) j["events"][entry] = json::object();
        j["events"][entry]["day"] = *day;
    }
    else j["events"].erase(entry);
    return lib.setSetting("timeline:" + book, j.dump());
}

// A chapter's number in its title: "Chapter 12", "Chapter 12: Ashes", "Ch. 12", "12", "12. Ashes", "12: Ashes"; 0 if none.
int titleNumber(const std::string& title) {
    std::string t = lower(trim(title));
    for (const char* w : {"chapter", "chap.", "chap", "ch."}) {
        const std::string ws = w;
        if (t.rfind(ws, 0) == 0 && (t.size() == ws.size() || !std::isalpha((unsigned char)t[ws.size()]))) {
            t = trim(t.substr(ws.size()));
            break;
        }
    }
    size_t e = 0;
    while (e < t.size() && std::isdigit((unsigned char)t[e])) ++e;
    if (e == 0 || e > 6) return 0;
    if (e < t.size() && std::isalnum((unsigned char)t[e])) return 0;   // "12th", "3rd"
    return std::stoi(t.substr(0, e));
}

// A title without a leading "Chapter 12:" or "12." and in lower case ("Ashes and Embers"); "" if it's only a number.
std::string titleCore(const std::string& title) {
    std::string t = trim(title);
    if (titleNumber(t) > 0) {
        std::string l = lower(t);
        size_t i = 0;
        for (const char* w : {"chapter", "chap.", "chap", "ch."}) {
            const std::string ws = w;
            if (l.rfind(ws, 0) == 0) { i = ws.size(); break; }
        }
        while (i < t.size() && !std::isdigit((unsigned char)t[i])) ++i;
        while (i < t.size() && std::isdigit((unsigned char)t[i])) ++i;
        while (i < t.size() && (t[i] == ':' || t[i] == '.' || t[i] == '-' || t[i] == ' ' || t[i] == ',' || (unsigned char)t[i] == 0xE2)) {
            if ((unsigned char)t[i] == 0xE2 && i + 2 < t.size()) i += 3;   // an en or em dash
            else ++i;
        }
        t = t.substr(std::min(i, t.size()));
    }
    return lower(trim(t));
}

// What a title is matched by (0.62): its core with any marks or symbols around it taken off ("1. 🗡 Steel and Snow" and
// "Steel and Snow" are both "steel and snow"), or "#13" when it's only a number ("13. Chapter 13", "Chapter 13").
std::string matchKey(const std::string& title) {
    std::string t = trim(title);
    for (int round = 0; round < 3; ++round) {
        const int num = titleNumber(t);
        std::string core = titleCore(t);
        // Leading and trailing marks: punctuation, and symbols such as emoji, dingbats, arrows and the like.
        auto symbolAt = [&](size_t i, size_t& len) {
            const unsigned char c = (unsigned char)core[i];
            if (c < 0x80) { len = 1; return !std::isalnum(c); }
            unsigned cp = 0;
            if ((c & 0xE0) == 0xC0 && i + 1 < core.size()) { len = 2; cp = ((c & 0x1Fu) << 6) | (core[i + 1] & 0x3F); }
            else if ((c & 0xF0) == 0xE0 && i + 2 < core.size()) { len = 3; cp = ((c & 0x0Fu) << 12) | ((core[i + 1] & 0x3Fu) << 6) | (core[i + 2] & 0x3F); }
            else if ((c & 0xF8) == 0xF0 && i + 3 < core.size()) {
                len = 4;
                cp = ((c & 0x07u) << 18) | ((core[i + 1] & 0x3Fu) << 12) | ((core[i + 2] & 0x3Fu) << 6) | (core[i + 3] & 0x3F);
            } else { len = 1; return true; }
            return (cp >= 0x2000 && cp <= 0x2BFF) || (cp >= 0x1F000 && cp <= 0x1FAFF) || (cp >= 0xE000 && cp <= 0xF8FF) ||
                   (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0x00B7 || cp == 0x00A0 || (cp >= 0x3000 && cp <= 0x303F);
        };
        size_t a = 0, len = 0;
        while (a < core.size() && symbolAt(a, len)) a += len;
        core = core.substr(a);
        // Trailing: walk forward, remembering where the last letter or digit ended.
        size_t end = 0;
        for (size_t i = 0; i < core.size();) {
            const bool sym = symbolAt(i, len);
            i += len;
            if (!sym) end = i;
        }
        core = core.substr(0, end);
        std::string squeezed;
        for (char c : core) if (!(c == ' ' && (squeezed.empty() || squeezed.back() == ' '))) squeezed += c;
        while (!squeezed.empty() && squeezed.back() == ' ') squeezed.pop_back();
        if (squeezed.empty()) return num > 0 ? "#" + std::to_string(num) : std::string();
        if (squeezed == lower(trim(t))) return squeezed;
        t = squeezed;   // again, for "13. Chapter 13"
    }
    return lower(trim(t));
}

// A line that starts a numbered chapter: "Chapter 3", "Chapter 3: Ashes", "Chapter 12 - The Wall" (not "Chapter 3 ends
// with…", which is a sentence).
bool numberedChapter(const std::string& line) {
    const std::string l = lower(trim(line));
    if (l.rfind("chapter ", 0) != 0) return false;
    size_t i = 8;
    while (i < l.size() && l[i] == ' ') ++i;
    const size_t d = i;
    while (i < l.size() && std::isdigit((unsigned char)l[i])) ++i;
    if (i == d || i - d > 4) return false;
    while (i < l.size() && l[i] == ' ') ++i;
    return i == l.size() || l[i] == ':' || l[i] == '.' || l[i] == '-' || (unsigned char)l[i] == 0xE2;
}

// ------------------------------------------------------------------ outlines

std::vector<OutlineItem> parseOutline(const std::string& text, std::string* structure) {
    const auto ls = lines(text);
    // Heading levels in use, for markdown outlines.
    std::set<int> levels;
    auto headingLevel = [](const std::string& l) {
        int n = 0;
        while (n < (int)l.size() && l[n] == '#') ++n;
        return n > 0 && n < (int)l.size() && l[n] == ' ' ? n : 0;
    };
    for (auto& raw : ls) if (int h = headingLevel(trim(raw))) levels.insert(h);
    std::vector<int> lv(levels.begin(), levels.end());
    auto kindOfLevel = [&](int h) {
        const int i = int(std::find(lv.begin(), lv.end(), h) - lv.begin());
        if (lv.size() >= 3) return i == 0 ? NodeKind::Part : i == 1 ? NodeKind::Chapter : NodeKind::Scene;
        if (lv.size() == 2) return i == 0 ? NodeKind::Chapter : NodeKind::Scene;
        return NodeKind::Chapter;
    };

    std::vector<OutlineItem> out;
    bool blank = false;
    for (const std::string& raw : ls) {
        std::string l = trim(raw);
        if (l.empty()) { blank = true; continue; }
        std::string v;
        std::optional<NodeKind> starts;
        std::string title;
        if (int h = headingLevel(l)) {
            std::string rest = trim(l.substr(h));
            // "## Chapter: The Forge" in a heading counts by its word.
            if (field(rest, {"part"}, v)) starts = NodeKind::Part, title = v;
            else if (field(rest, {"chapter"}, v)) starts = NodeKind::Chapter, title = v;
            else if (field(rest, {"scene"}, v)) starts = NodeKind::Scene, title = v;
            else starts = kindOfLevel(h), title = rest;
        } else if (field(l, {"part"}, v)) starts = NodeKind::Part, title = v;
        else if (field(l, {"chapter"}, v)) starts = NodeKind::Chapter, title = v;
        else if (numberedChapter(l)) starts = NodeKind::Chapter, title = l;   // "Chapter 3: Ashes", "Chapter 12" (0.54)
        else if (field(l, {"scene"}, v)) starts = NodeKind::Scene, title = v;
        if (starts) {
            OutlineItem it;
            it.kind = *starts;
            it.title = title;
            out.push_back(it);
            blank = false;
            continue;
        }
        if (field(l, {"book structure", "structure used"}, v)) { if (structure) *structure = v; continue; }
        if (out.empty()) continue;   // text before the first part or chapter
        OutlineItem& it = out.back();
        if (field(l, {"viewpoint", "pov", "point of view"}, v)) it.viewpoint = v;
        else if (field(l, {"status"}, v)) it.status = v;
        else if (field(l, {"target", "words", "target words"}, v)) {
            std::string digits;
            for (char c : v) if (std::isdigit((unsigned char)c)) digits += c; else if (c != ',' && c != ' ' && c != '.') break;
            if (!digits.empty() && digits.size() < 10) it.target = std::stoll(digits);
        } else if (field(l, {"threads", "thread", "plot threads"}, v)) {
            for (auto& s : splitList(v)) it.threads.push_back(s);
        } else if (field(l, {"beat", "beats"}, v)) {
            for (auto& x : splitList(v)) it.beats.push_back(x);
        } else if (field(l, {"when"}, v)) it.when = v;
        else if (field(l, {"time", "time of day"}, v)) {
            // A clock time (0.75) or a named time of day (0.80), worked out against the book's calendar on import;
            // anything else is still words for when it happens.
            it.time = v;
        }
        else if (field(l, {"place", "where", "setting", "location"}, v)) it.place = v;
        else if (field(l, {"characters", "characters present", "present", "cast", "who", "who's there", "in it", "creatures"}, v)) {
            for (auto& x : splitList(v)) it.cast.push_back(x);
        } else {
            if (field(l, {"synopsis", "summary"}, v)) l = v;
            if (l.empty()) continue;
            if (!it.synopsis.empty()) it.synopsis += blank ? "\n\n" : "\n";
            it.synopsis += l;
        }
        blank = false;
    }
    return out;
}

std::string outlineText(const Library& lib, const Id& book) {
    const Threads th = threadsOf(lib, book);
    const json tl = parseObj(lib.setting("timeline:" + book));
    std::string unit = tl.value("unit", "day");
    if (unit != "week" && unit != "month" && unit != "year") unit = "day";
    // The structure it follows, and which beats each chapter carries.
    const BookStructure bs = bookStructure(lib, book);
    const auto all = allStructures(lib, structureScope(lib, book));
    const Structure* st = findStructure(all, bs.templateId);
    std::map<Id, std::vector<std::string>> beatsOf;
    if (st)
        for (auto& b : st->beats) {
            auto it = bs.beats.find(b.id);
            if (it != bs.beats.end()) for (auto& n : it->second) beatsOf[n].push_back(b.name);
        }
    const auto places = nodePlaces(lib, book);
    const auto casts = nodeCast(lib, book);
    // The world's calendar, when the timeline is in its dates (0.75).
    const cal::Calendar calendar = cal::calendarOfBook(lib, book);
    const cal::StorySpan span = cal::storySpan(lib, book);
    const bool inDates = span.on && !calendar.empty();
    std::string out;
    // What sort of book it is, and its place in each series (0.76).
    if (const Book* b = lib.book(book)) {
        if (const auto t = lib.bookType(b->type)) out += "Book type: " + t->name + "\n";
        for (const Series* sr : lib.seriesOfBook(book))
            if (const Membership* m = lib.membership(sr->id, book)) {
                std::string line = "Series: " + sr->name;
                if (!m->number.empty()) line += ", Book " + m->number;
                if (m->role == SeriesRole::Between) line += ", set between books";
                else if (m->role == SeriesRole::During)
                    if (const Book* host = lib.book(m->during)) line += ", set during " + host->title;
                out += line + "\n";
            }
    }
    if (st) out += "Book structure: " + st->name + "\n";
    for (const ManuscriptNode* n : lib.manuscriptOf(book)) {
        if (!out.empty()) out += "\n";
        out += std::string(n->kind == NodeKind::Part ? "Part: " : n->kind == NodeKind::Chapter ? "Chapter: " : "Scene: ") + n->title + "\n";
        if (!n->povId.empty())
            if (const Entry* e = lib.entry(n->povId)) out += "Viewpoint: " + e->name + "\n";
        if (n->kind != NodeKind::Part) out += std::string("Status: ") + label(n->status) + "\n";
        if (n->targetWords > 0) out += "Target: " + std::to_string(n->targetWords) + "\n";
        if (auto pl = places.find(n->id); pl != places.end())
            if (const Entry* e = lib.entry(pl->second); e && !e->deleted) out += "Place: " + e->name + "\n";
        if (auto cs = casts.find(n->id); cs != casts.end()) {
            std::string names;
            for (const Id& id : cs->second)
                if (const Entry* e = lib.entry(id); e && !e->deleted) names += (names.empty() ? "" : ", ") + e->name;
            if (!names.empty()) out += "Characters: " + names + "\n";
        }
        auto it = th.nodes.find(n->id);
        if (it != th.nodes.end()) {
            std::string names;
            for (auto& id : it->second)
                if (const Thread* t = th.find(id)) names += (names.empty() ? "" : ", ") + t->name;
            if (!names.empty()) out += "Threads: " + names + "\n";
        }
        if (auto b = beatsOf.find(n->id); b != beatsOf.end()) {
            std::string names;
            for (auto& x : b->second) names += (names.empty() ? "" : ", ") + x;
            out += "Beat: " + names + "\n";
        }
        if (tl.contains("nodes") && tl["nodes"].is_object() && tl["nodes"].contains(n->id)) {
            const json& v = tl["nodes"][n->id];
            std::string when;
            if (v.contains("day") && v["day"].is_number()) {
                if (inDates) {
                    when = cal::formatDate(calendar, cal::fromDay(calendar, span.start + (std::int64_t)std::floor(v["day"].get<double>())));
                } else {
                    char buf[32];
                    std::snprintf(buf, sizeof buf, "%g", v["day"].get<double>());
                    std::string u = unit;
                    u[0] = (char)std::toupper((unsigned char)u[0]);
                    when = u + " " + buf;
                }
            }
            const std::string words = v.value("when", "");
            if (!words.empty()) when += (when.empty() ? "" : inDates ? "; " : ", ") + words;   // a date may hold a comma
            if (!when.empty()) out += "When: " + when + "\n";
            if (v.contains("time") && v["time"].is_number_integer() && v["time"].get<int>() >= 0)
                out += "Time: " + cal::formatTime(calendar, v["time"].get<int>()) + "\n";
            else if (v.contains("part") && v["part"].is_string() && !v["part"].get<std::string>().empty())
                out += "Time: " + lower(v["part"].get<std::string>()) + "\n";
        }
        if (!n->synopsis.empty()) out += n->synopsis + "\n";
    }
    return out;
}

bool importOutline(Library& lib, const Id& book, const std::string& text, OutlineImport& out, const std::string& scopeIn) {
    const Book* b = lib.book(book);
    if (!b) return false;
    std::string structureName;
    const auto items = parseOutline(text, &structureName);
    // The structure the book follows: named here, or the one it has.
    const std::string scope = scopeIn.empty() ? structureScope(lib, book) : scopeIn;
    const auto structures = allStructures(lib, scope);
    BookStructure bs = bookStructure(lib, book);
    bool bsChanged = false;
    if (!structureName.empty()) {
        const Structure* found = nullptr;
        for (auto& x : structures) if (lower(x.name) == lower(structureName) || x.id == structureName) { found = &x; break; }
        if (found) {
            if (bs.templateId != found->id) { bs = BookStructure{}; bs.templateId = found->id; }
            out.structure = found->name;
            bsChanged = true;
        } else out.unknownStructure = true;
    }
    // An outline with no "Book structure:" line replaces the book's structure with none (0.83), as blank details clear.
    if (structureName.empty() && !items.empty() && !bs.templateId.empty()) {
        const Structure* had = findStructure(structures, bs.templateId);
        out.removedStructure = had ? had->name : bs.templateId;
        bs = BookStructure{};
        bsChanged = true;
    }
    const Structure* st = findStructure(structures, bs.templateId);
    if (items.empty()) {
        if (bsChanged) setBookStructure(lib, book, bs);
        return bsChanged;
    }
    const auto people = lib.entriesOf(b->worldId, b->worldId.empty() ? book : Id{});
    auto person = [&](const std::string& name) -> Id {
        const std::string n = lower(trim(name));
        for (const Entry* e : people) if (lower(e->name) == n) return e->id;
        for (const Entry* e : people) for (auto& a : e->aliases) if (lower(trim(a)) == n) return e->id;
        // A first name alone: "Tom" for "Tom Smith", if only one fits.
        Id one;
        int count = 0;
        for (const Entry* e : people) {
            const std::string en = lower(e->name);
            if (e->kind == "character" && en.rfind(n + " ", 0) == 0) { one = e->id; ++count; }
        }
        return count == 1 ? one : Id{};
    };
    auto placeNamed = [&](const std::string& name) -> Id {
        const std::string n = lower(trim(name));
        for (const Entry* e : people)
            if ((e->kind == "place" || e->kind == "realm") && lower(e->name) == n) return e->id;
        for (const Entry* e : people)
            if (e->kind == "place" || e->kind == "realm")
                for (auto& a : e->aliases) if (lower(trim(a)) == n) return e->id;
        return {};
    };
    Threads th = threadsOf(lib, book);
    json tl = parseObj(lib.setting("timeline:" + book));
    const cal::Calendar calendar = cal::calendarOfBook(lib, book);   // dates in "When:" lines (0.75)
    if (!tl.contains("nodes") || !tl["nodes"].is_object()) tl["nodes"] = json::object();
    const bool hadUnit = tl.contains("unit");
    // Parts, chapters and scenes already in the book are matched and filled in rather than added again (0.54): by title
    // (with or without a "Chapter 3:" before it), else a chapter by its number among the story's chapters.
    std::vector<const ManuscriptNode*> partsHad, chaptersHad, storyChapters;   // titles match any chapter; numbers count the story's
    for (const ManuscriptNode* n : lib.manuscriptOf(book)) {
        if (n->kind == NodeKind::Part) partsHad.push_back(n);
        else if (n->kind == NodeKind::Chapter) {
            chaptersHad.push_back(n);
            if (n->matter == 0) storyChapters.push_back(n);
        }
    }
    std::set<Id> used;
    // Words in a part, chapter or scene with everything inside it: among copies of one chapter, the one with writing wins.
    std::map<Id, std::int64_t> words;
    {
        const auto all = lib.manuscriptOf(book);
        for (auto it = all.rbegin(); it != all.rend(); ++it) {
            words[(*it)->id] += (*it)->wordCount;
            if (!(*it)->parentId.empty()) words[(*it)->parentId] += words[(*it)->id];
        }
    }
    // Matched by title, or by its key ("1. 🗡 Steel and Snow" is "Steel and Snow"; "13. Chapter 13" is "Chapter 13"), the one
    // with most written first; else a chapter by its number among the story's chapters (0.54, 0.62).
    auto matchIn = [&](const std::vector<const ManuscriptNode*>& pool, const std::string& title, bool byNumber) -> Id {
        const std::vector<const ManuscriptNode*>& numbered = &pool == &chaptersHad ? storyChapters : pool;
        const std::string want = lower(trim(title)), key = matchKey(title);
        const ManuscriptNode* best = nullptr;
        bool bestExact = false;
        for (const ManuscriptNode* n : pool) {
            if (used.count(n->id)) continue;
            const bool exact = lower(trim(n->title)) == want;
            if (!exact && (key.empty() || matchKey(n->title) != key)) continue;
            if (!best || words[n->id] > words[best->id] || (words[n->id] == words[best->id] && exact && !bestExact)) { best = n; bestExact = exact; }
        }
        if (best) return best->id;
        if (byNumber)
            if (const int num = titleNumber(title); num > 0 && num <= (int)numbered.size() && !used.count(numbered[(size_t)num - 1]->id)) {
                // By number only when the title names no other chapter of the book.
                bool namesAnother = false;
                const std::string core = key.empty() || key[0] == '#' ? std::string() : key;
                if (!core.empty())
                    for (const ManuscriptNode* n : pool) namesAnother = namesAnother || matchKey(n->title) == core;
                if (!namesAnother) return numbered[(size_t)num - 1]->id;
            }
        return {};
    };
    // The plan replaces the book's plan (0.62): its threads, beats, places, who's there and times are set afresh from it.
    const auto before = lib.manuscriptOf(book);
    for (const ManuscriptNode* n : before) th.nodes.erase(n->id);
    bs.beats.clear();
    bsChanged = true;
    std::vector<std::pair<Id, Id>> placed;   // each planned node and its parent, in the plan's order
    Id part, chapter;
    for (const OutlineItem& it : items) {
        Id parent;
        if (it.kind == NodeKind::Chapter) parent = part;
        else if (it.kind == NodeKind::Scene) parent = !chapter.empty() ? chapter : part;
        Id id;
        if (it.kind == NodeKind::Part) id = matchIn(partsHad, it.title, false);
        else if (it.kind == NodeKind::Chapter) id = matchIn(chaptersHad, it.title, true);
        else if (!parent.empty()) id = matchIn(lib.childrenOf(book, parent), it.title, false);
        const bool isNew = id.empty();
        if (isNew) {
            id = lib.createNode(book, parent, it.kind, it.title);
            if (id.empty()) id = lib.createNode(book, {}, it.kind, it.title);
            if (id.empty()) continue;
            out.made.push_back(id);
            if (it.kind == NodeKind::Part) ++out.parts;
            else if (it.kind == NodeKind::Chapter) ++out.chapters;
            else ++out.scenes;
        } else {
            used.insert(id);
            out.updated.push_back(id);
            if (it.kind == NodeKind::Part) ++out.updatedParts;
            else if (it.kind == NodeKind::Chapter) ++out.updatedChapters;
            else ++out.updatedScenes;
        }
        if (it.kind == NodeKind::Part) { part = id; chapter.clear(); }
        else if (it.kind == NodeKind::Chapter) chapter = id;
        placed.push_back({id, parent});
        ManuscriptNode n = *lib.node(id);
        // The plan's say replaces what was there (0.62); only the status (how far the writing has got) and the title
        // (the manuscript's) stay when the plan doesn't give them. A new one starts as an outline.
        n.synopsis = it.synopsis;
        if (!it.status.empty())
            if (auto s = nodeStatusFrom(lower(trim(it.status)))) n.status = *s;
        if (isNew) {
            if (it.kind == NodeKind::Part) n.status = NodeStatus::Draft;
            else if (it.status.empty()) n.status = NodeStatus::Outline;
        }
        n.targetWords = it.target;
        n.povId.clear();
        if (!it.viewpoint.empty()) {
            n.povId = person(it.viewpoint);
            if (n.povId.empty() && std::find(out.unknownViewpoints.begin(), out.unknownViewpoints.end(), it.viewpoint) == out.unknownViewpoints.end())
                out.unknownViewpoints.push_back(it.viewpoint);
        }
        lib.updateNode(n);
        setNodePlace(lib, book, id, {});
        setNodeCast(lib, book, id, {});
        tl["nodes"].erase(id);
        if (!it.place.empty()) {
            const Id pl = placeNamed(it.place);
            if (!pl.empty()) setNodePlace(lib, book, id, pl);
            else if (std::find(out.unknownPlaces.begin(), out.unknownPlaces.end(), it.place) == out.unknownPlaces.end())
                out.unknownPlaces.push_back(it.place);
        }
        if (!it.cast.empty()) {
            // Who's present: characters or creatures by name, other name or a first name only one has.
            std::vector<Id> cast;
            for (const std::string& name : it.cast) {
                Id who = person(name);
                if (const Entry* e = who.empty() ? nullptr : lib.entry(who); e && e->kind != "character" && e->kind != "creature") who.clear();
                if (who.empty()) {
                    if (std::find(out.unknownCast.begin(), out.unknownCast.end(), name) == out.unknownCast.end()) out.unknownCast.push_back(name);
                    continue;
                }
                if (std::find(cast.begin(), cast.end(), who) == cast.end()) cast.push_back(who);
            }
            if (!cast.empty()) setNodeCast(lib, book, id, cast);
        }
        for (const std::string& name : it.threads) {
            const Thread* t = th.named(name);
            if (!t) {
                th.threads.push_back({newThreadId(th), name, threadColour(th.threads.size()), ""});
                t = &th.threads.back();
                ++out.threads;
            }
            tag(th, id, t->id);
        }
        for (const std::string& name : it.beats) {
            const Beat* beat = nullptr;
            if (st) for (auto& x : st->beats) if (lower(x.name) == lower(name) || x.id == name) { beat = &x; break; }
            if (!beat) {
                if (std::find(out.unknownBeats.begin(), out.unknownBeats.end(), name) == out.unknownBeats.end()) out.unknownBeats.push_back(name);
                continue;
            }
            auto& v = bs.beats[beat->id];
            if (std::find(v.begin(), v.end(), id) == v.end()) { v.push_back(id); ++out.beats; bsChanged = true; }
        }
        if (!it.when.empty() || !it.time.empty()) {
            // "Day 3", "Week 2, the storm", a date in the world's calendar (0.75), or just words.
            json v = json::object();
            std::string rest = it.when;
            if (!calendar.empty() && !rest.empty()) {
                // The longest start of it (up to a ";" or the end) that reads as a date.
                std::vector<size_t> cuts{rest.size()};
                for (size_t k = 0; k < rest.size(); ++k) if (rest[k] == ';' || rest[k] == ',') cuts.push_back(k);
                std::sort(cuts.rbegin(), cuts.rend());
                for (size_t cut : cuts) {
                    if (const auto d = cal::parseDate(calendar, rest.substr(0, cut))) {
                        const std::int64_t abs = cal::toDay(calendar, *d);
                        if (!tl.value("calendar", false)) {
                            tl["calendar"] = true;
                            if (!tl.contains("start") || !tl["start"].is_number()) tl["start"] = abs;
                            tl["unit"] = "day";
                        }
                        const std::int64_t start = (std::int64_t)std::floor(tl["start"].get<double>());
                        v["day"] = (double)(abs - start);
                        v["date"] = trim(rest.substr(0, cut));   // as written, to be read again if the calendar changes (0.82)
                        ++out.datesRead;
                        rest = trim(cut < rest.size() ? rest.substr(cut + 1) : std::string());
                        break;
                    }
                }
            }
            if (!it.time.empty()) {
                const auto t = cal::parseTimeOfDay(calendar.empty() ? cal::gregorian() : calendar, it.time);
                if (t && t->minutes >= 0) v["time"] = t->minutes;
                else if (t) v["part"] = t->part;
                else rest = rest.empty() ? it.time : rest + ", " + it.time;   // not a time: words for when
            }
            const std::string lw = lower(rest);
            for (const char* u : units) {
                if (v.contains("day")) break;
                const std::string un = u;
                if (lw.rfind(un + " ", 0) != 0) continue;
                size_t k = un.size() + 1;
                size_t e = k;
                while (e < rest.size() && (std::isdigit((unsigned char)rest[e]) || rest[e] == '.' || rest[e] == '-')) ++e;
                if (e == k) break;
                try {
                    v["day"] = std::stod(rest.substr(k, e - k));
                    if (!hadUnit) tl["unit"] = un;
                    rest = trim(rest.substr(e));
                    if (!rest.empty() && (rest[0] == ',' || rest[0] == ';')) rest = trim(rest.substr(1));
                } catch (...) {}
                break;
            }
            if (!rest.empty()) v["when"] = rest;
            // Words that look like a date but weren't read as one (no calendar yet, or one that doesn't know them) are
            // kept as words and said in the import's note (0.82); a calendar saved later reads them again.
            if (!v.contains("day") && !it.when.empty()) {
                bool digit = false, mark = false;
                for (size_t k = 0; k < it.when.size(); ++k) {
                    const unsigned char ch = (unsigned char)it.when[k];
                    if (std::isdigit(ch)) digit = true;
                    else if ((ch == '/' || ch == '-' || ch == '.') && k > 0 && std::isdigit((unsigned char)it.when[k - 1])) mark = true;
                    else if (std::isalpha(ch)) mark = true;
                }
                if (digit && mark) out.unreadDates.push_back(it.title + ": " + it.when);
            }
            if (!v.empty()) tl["nodes"][id] = v;
        }
    }
    // In the plan's order (0.62): each under its part or chapter, one after another.
    std::map<Id, int> next;
    for (auto& [id, parent] : placed) {
        const ManuscriptNode* n = lib.node(id);
        if (!n) continue;
        int& at = next[parent];
        lib.moveNode(id, parent, at);
        ++at;
    }
    // What the plan doesn't have: empty ones go to the Trash (restorable); anything with writing stays, and is named.
    std::set<Id> planned;
    for (auto& [id, parent] : placed) planned.insert(id);
    for (const ManuscriptNode* n : lib.manuscriptOf(book)) {
        if (planned.count(n->id)) continue;
        bool holdsPlanned = false, inTrashedAlready = false;
        for (const ManuscriptNode* m : lib.manuscriptOf(book))
            for (const ManuscriptNode* p = lib.node(m->parentId); p && !holdsPlanned; p = lib.node(p->parentId))
                if (p->id == n->id && planned.count(m->id)) holdsPlanned = true;
        for (const ManuscriptNode* p = lib.node(n->parentId); p; p = lib.node(p->parentId)) if (p->deleted) inTrashedAlready = true;
        if (inTrashedAlready || n->deleted) continue;
        if (words[n->id] == 0 && !holdsPlanned) {
            if (lib.trashNode(n->id)) { ++out.trashed; tl["nodes"].erase(n->id); }
        } else if (n->kind != NodeKind::Part && words[n->id] > 0) {
            out.keptWithWriting.push_back(n->title.empty() ? std::string("(untitled)") : n->title);
        }
    }
    setThreads(lib, book, th);
    lib.setSetting("timeline:" + book, tl.dump());
    if (bsChanged) setBookStructure(lib, book, bs);
    return !out.made.empty() || !out.updated.empty() || bsChanged;
}

// ------------------------------------------------------------------ all of it

PlanFit planFit(const Library& lib, const Id& book, const std::string& text) {
    PlanFit f;
    std::set<std::string> titles, keys;
    for (const ManuscriptNode* n : lib.manuscriptOf(book))
        if (n->kind == NodeKind::Chapter && !n->deleted) {
            ++f.bookChapters;
            titles.insert(lower(trim(n->title)));
            keys.insert(matchKey(n->title));
        }
    for (const OutlineItem& it : parseOutline(text)) {
        if (it.kind != NodeKind::Chapter) continue;
        ++f.planned;
        const std::string k = matchKey(it.title);
        if (titles.count(lower(trim(it.title))) || (!k.empty() && k[0] != '#' && keys.count(k))) ++f.matched;
    }
    return f;
}

std::string structureScope(const Library& lib, const Id& book) {
    const Book* b = lib.book(book);
    if (!b) return {};
    return b->worldId.empty() ? "book:" + book : "world:" + b->worldId;
}

bool PlanningImport::any() const {
    return !outline.made.empty() || !outline.updated.empty() || !outline.structure.empty() || threads.added || threads.updated || threads.tagged || structuresAdded || structuresReplaced || bookTypeSet || placements ||
           !removedThreads.empty() || !outline.removedStructure.empty();
}

bool importPlanning(Library& lib, const Id& book, const std::string& scope, const std::string& text, PlanningImport& out) {
    // "Book type:" and "Series:" lines (0.76), before the outline starts.
    for (const std::string& raw : lines(text)) {
        std::string l = trim(raw);
        while (!l.empty() && (l[0] == '#' || l[0] == '-' || l[0] == '*' || l[0] == '>')) l = trim(l.substr(1));
        std::string v;
        if (field(l, {"part", "chapter", "scene"}, v) || numberedChapter(l)) break;
        if (field(l, {"book type", "type of book"}, v)) {
            const std::string want = lower(trim(v));
            for (const BookType& t : lib.bookTypes())
                if (lower(t.name) == want || t.id == want || lower(t.name).rfind(want + " ", 0) == 0) {
                    if (const Book* b = lib.book(book); b && b->type != t.id) {
                        Book n = *b;
                        n.type = t.id;
                        lib.updateBook(n);
                    }
                    out.bookTypeSet = true;
                    break;
                }
        } else if (field(l, {"series"}, v)) {
            // "The Alvora Chronicles, Book 2.5, set during The Envoy of Telvana" (or "set between books").
            std::vector<std::string> bits = splitList(v);
            if (bits.empty()) continue;
            const Book* b = lib.book(book);
            const Series* sr = nullptr;
            for (const Series* x : b ? lib.seriesOfBook(book) : std::vector<const Series*>{})
                if (lower(x->name) == lower(bits[0])) sr = x;
            if (!sr) {
                if (b) for (const Series* x : lib.seriesIn(b->worldId)) if (lower(x->name) == lower(bits[0])) sr = x;
                if (sr && !lib.addBookToSeries(book, sr->id)) sr = nullptr;
            }
            if (!sr) { out.unknownSeries.push_back(bits[0]); continue; }
            const Membership* m = lib.membership(sr->id, book);
            if (!m) continue;
            Membership next = *m;
            SeriesRole role = SeriesRole::Main;
            Id during;
            for (size_t i = 1; i < bits.size(); ++i) {
                const std::string lb = lower(bits[i]);
                if (lb.rfind("book ", 0) == 0) next.number = trim(bits[i].substr(5));
                else if (lb.rfind("set between", 0) == 0) role = SeriesRole::Between;
                else if (lb.rfind("set during ", 0) == 0) {
                    const std::string title = lower(trim(bits[i].substr(11)));
                    for (const Book* x : lib.booksInSeries(sr->id)) if (lower(x->title) == title && x->id != book) during = x->id;
                    if (!during.empty()) role = SeriesRole::During;
                    else out.unknownSeries.push_back(trim(bits[i].substr(11)));
                }
            }
            lib.setSeriesRole(sr->id, book, role, during);
            next = *lib.membership(sr->id, book);
            for (size_t i = 1; i < bits.size(); ++i)
                if (lower(bits[i]).rfind("book ", 0) == 0) next.number = trim(bits[i].substr(5));
            lib.updateMembership(next);
            ++out.placements;
        }
    }
    // Split the text: each line goes to the part it belongs to, by the last heading word seen.
    enum Part { Outline, Structures, ThreadsPart } part = Outline;
    std::string outline, structures, threads;
    for (const std::string& raw : lines(text)) {
        std::string l = trim(raw);
        while (!l.empty() && (l[0] == '#' || l[0] == '-' || l[0] == '*' || l[0] == '>')) l = trim(l.substr(1));
        std::string v;
        if (field(l, {"structure", "template"}, v)) part = Structures;
        else if (field(l, {"thread", "plot thread", "subplot"}, v)) part = ThreadsPart;
        else if (field(l, {"part", "chapter", "scene", "book structure", "structure used", "book type", "type of book", "series"}, v)) part = Outline;
        else if (part != Outline && raw.size() > 0 && raw[0] == '#' && trim(raw).find(':') == std::string::npos) part = Outline;   // a plain heading
        (part == Structures ? structures : part == ThreadsPart ? threads : outline) += raw + "\n";
    }
    if (!trim(structures).empty()) {
        auto own = ownStructures(lib, scope);
        for (Structure& s : parseStructures(structures)) {
            bool replaced = false;
            for (auto& o : own)
                if (lower(o.name) == lower(s.name)) { s.id = o.id; o = s; replaced = true; break; }
            if (replaced) ++out.structuresReplaced;
            else {
                std::set<std::string> taken;
                for (auto& b : builtInStructures()) taken.insert(b.id);
                for (auto& o : own) taken.insert(o.id);
                s.id = unique("own-" + slug(s.name), taken);
                own.push_back(s);
                ++out.structuresAdded;
            }
        }
        setOwnStructures(lib, scope, own);
    }
    std::set<std::string> had;   // threads the book had before (the outline's new ones aren't "updated")
    for (auto& t : threadsOf(lib, book).threads) had.insert(lower(trim(t.name)));
    bool outlineTags = false;
    if (!trim(outline).empty()) {
        for (auto& it : parseOutline(outline)) outlineTags = outlineTags || !it.threads.empty();
        importOutline(lib, book, outline, out.outline, scope);
    }
    if (!trim(threads).empty()) {
        // A thread's "Chapters:" list only when the outline didn't say which threads each chapter has: its numbers are
        // the book's chapters as they were when it was written out.
        importThreads(lib, book, threads, out.threads, !outlineTags);
        out.threads.updated = 0;
        for (const std::string& l : lines(threads)) {
            std::string v, t = trim(l);
            while (!t.empty() && (t[0] == '#' || t[0] == '-' || t[0] == '*')) t = trim(t.substr(1));
            if (field(t, {"thread", "plot thread", "subplot"}, v) && had.count(lower(v))) ++out.threads.updated;
        }
    }
    // A plan with an outline replaces the book's plan (0.83): threads it doesn't list go, with their tags.
    if (!trim(outline).empty() && !parseOutline(outline).empty()) {
        std::set<std::string> listed;
        for (auto& it : parseOutline(outline)) for (auto& t : it.threads) listed.insert(lower(trim(t)));
        for (const std::string& l : lines(threads)) {
            std::string v, t = trim(l);
            while (!t.empty() && (t[0] == '#' || t[0] == '-' || t[0] == '*')) t = trim(t.substr(1));
            if (field(t, {"thread", "plot thread", "subplot"}, v)) listed.insert(lower(trim(v)));
        }
        Threads th = threadsOf(lib, book);
        std::set<std::string> gone;
        std::vector<Thread> keep;
        for (auto& t : th.threads) {
            if (listed.count(lower(trim(t.name)))) keep.push_back(t);
            else { gone.insert(t.id); out.removedThreads.push_back(t.name); }
        }
        if (!gone.empty()) {
            th.threads = keep;
            for (auto it = th.nodes.begin(); it != th.nodes.end();) {
                auto& v = it->second;
                v.erase(std::remove_if(v.begin(), v.end(), [&](const std::string& id) { return gone.count(id) > 0; }), v.end());
                it = v.empty() ? th.nodes.erase(it) : std::next(it);
            }
            setThreads(lib, book, th);
        }
    }
    return out.any();
}

std::string planningText(const Library& lib, const Id& book, const std::string& scope) {
    std::string out = outlineText(lib, book);
    const Threads th = threadsOf(lib, book);
    if (!th.threads.empty()) out += (out.empty() ? "" : "\n") + threadsText(lib, book, th);
    const BookStructure bs = bookStructure(lib, book);
    for (const Structure& s : ownStructures(lib, scope))
        if (s.id == bs.templateId) out += (out.empty() ? "" : "\n") + structureText(s);
    return out;
}

}  // namespace studio::plan
