// Fills a library with 600 entries and their links, to try Worldbuilding with a big bible (a developer tool, not shipped).
// Usage: studio_bench_seed <library folder> [--book]   (adds to the first world, or the one named Alvora; --book also adds
// "Bench book": 4 parts of 20 chapters, each about 2,500 words naming bible entries, for timing Planning and Writing)
#include "studio/library.hpp"
#include <chrono>
#include <cstdio>
#include <string>
using namespace studio;
static double msSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}
int main(int argc, char** argv) {
    if (argc > 2 && std::string(argv[2]) == "--time") {
        // Times what opening and the busier pages ask of the library.
        auto t = std::chrono::steady_clock::now();
        OpenResult r;
        auto lib = Library::open(argv[1], r);
        if (!lib) { std::printf("open failed\n"); return 1; }
        std::printf("open %.1f ms\n", msSince(t));
        for (auto* w : lib->liveWorlds()) { t = std::chrono::steady_clock::now(); lib->tidyBible(w->id); std::printf("tidyBible %.1f ms\n", msSince(t)); }
        for (auto* w : lib->liveWorlds()) {
            t = std::chrono::steady_clock::now(); auto es = lib->entriesOf(w->id); std::printf("entriesOf %zu %.1f ms\n", es.size(), msSince(t));
            t = std::chrono::steady_clock::now(); auto fs = lib->families(w->id); std::printf("families %zu %.1f ms\n", fs.size(), msSince(t));
            t = std::chrono::steady_clock::now(); auto ks = lib->kindSuggestions(w->id); std::printf("kindSuggestions %zu %.1f ms\n", ks.size(), msSince(t));
        }
        for (auto* b : lib->liveBooks()) {
            t = std::chrono::steady_clock::now(); auto ms = lib->manuscriptOf(b->id); std::printf("manuscriptOf %s %zu %.1f ms\n", b->title.c_str(), ms.size(), msSince(t));
            t = std::chrono::steady_clock::now(); auto ap = lib->appearancesIn(b->id); std::printf("appearancesIn %.1f ms\n", msSince(t));
            t = std::chrono::steady_clock::now(); ap = lib->appearancesIn(b->id); std::printf("appearancesIn again %.1f ms\n", msSince(t));
        }
        return 0;
    }
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
    if (argc > 2 && std::string(argv[2]) == "--book") {
        const Id book = lib->createBook("Bench book", wo->id);
        const char* words[] = {"the", "harbour", "wind", "ledger", "storm", "came", "over", "wall", "and", "she", "watched", "light", "ships", "night"};
        int n = 0;
        for (int p = 0; p < 4; ++p) {
            const Id part = lib->createNode(book, {}, NodeKind::Part, "Part " + std::to_string(p + 1));
            for (int c = 0; c < 20; ++c) {
                const Id ch = lib->createNode(book, part, NodeKind::Chapter, "Chapter " + std::to_string(++n));
                std::string doc = R"({"type":"doc","content":[)";
                for (int para = 0; para < 25; ++para) {
                    std::string t;
                    for (int k = 0; k < 100; ++k) {
                        if (k % 17 == 5) t += lib->entry(ids[(size_t)(n * 31 + para * 7 + k) % ids.size()])->name + " ";   // a name in full, now and then
                        else t += std::string(words[(n + para + k) % 14]) + " ";
                    }
                    doc += std::string(para ? "," : "") + R"({"type":"paragraph","content":[{"type":"text","text":")" + t + R"("}]})";
                }
                doc += "]}";
                lib->saveNodeContent(ch, doc);
            }
        }
        std::printf("book with %d chapters\n", n);
    }
    std::printf("ok %zu\n", ids.size());
}
