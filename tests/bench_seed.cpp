// Fills a library with 600 entries and their links, to try Worldbuilding with a big bible (a developer tool, not shipped).
// Usage: studio_bench_seed <library folder>   (adds to the first world, or the one named Alvora)
#include "studio/library.hpp"
#include <cstdio>
#include <string>
using namespace studio;
int main(int argc, char** argv) {
    OpenResult r;
    auto lib = Library::open(argv[1], r);
    if (!lib) { std::printf("open failed\n"); return 1; }
    const World* wo = nullptr;
    for (auto* w : lib->liveWorlds()) if (w->name.find("Alvora") != std::string::npos || !wo) wo = w;
    if (!wo) return 2;
    const char* kinds[] = {"character","character","character","place","group","thing","creature","lore","event","magic"};
    const char* races[] = {"Human","Elf","Dwarf","Wyvernkin"};
    const char* sur[] = {"Smith","Vale","Ashford","Marrow","Kestrel","Thorne"};
    std::vector<Id> ids, chars;
    for (int i = 0; i < 600; ++i) {
        const char* k = kinds[i % 10];
        std::string name = std::string(k == std::string("character") ? "Bench " : "Bench ") + k + " " + std::to_string(i);
        if (std::string(k) == "character") name = "Bench" + std::to_string(i) + " " + sur[i % 6];
        Id id = lib->createEntry(wo->id, {}, k, name);
        Entry e = *lib->entry(id);
        e.summary = "A test entry made to see how Worldbuilding copes with a big bible. It has a summary of two lines or so.";
        e.details = std::string("Gender: ") + (i % 2 ? "Female" : "Male") + "\nRace: " + races[i % 4] + "\nAge: " + std::to_string(20 + i % 50) + "\nHome: Highcliff\nOccupation: Tester\n";
        lib->updateEntry(e);
        ids.push_back(id);
        if (std::string(k) == "character") chars.push_back(id);
    }
    for (size_t i = 0; i + 1 < ids.size(); ++i) lib->addLink(ids[i], ids[(i * 7 + 3) % ids.size()], "Friend of");
    for (size_t i = 0; i + 4 < chars.size(); i += 4) { lib->addLink(chars[i], chars[i + 1], "Married to"); lib->addLink(chars[i], chars[i + 2], "Parent of"); lib->addLink(chars[i + 1], chars[i + 2], "Parent of"); lib->addLink(chars[i], chars[i + 3], "Parent of"); }
    std::printf("ok %zu\n", ids.size());
}
