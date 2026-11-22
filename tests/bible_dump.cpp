// Developer tool: builds a sample world bible (CJ's pasted pages from tests/fixtures/paste plus a family, places and a
// language) and shares it as a website in every theme and as a PDF, for looking over. Usage:
//   studio_bible_dump <repo folder> <out folder>
#include "studio/bible_export.hpp"
#include "studio/conlang.hpp"
#include "studio/entry_paste.hpp"
#include "studio/library.hpp"
#include "studio/typeset.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace studio;
namespace fs = std::filesystem;

static std::string readFile(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: studio_bible_dump <repo> <out>\n"); return 2; }
    const std::string repo = argv[1], out = argv[2];
    fs::remove_all(out);
    fs::create_directories(out);
    OpenResult r;
    auto lib = Library::open(out + "/library", r);
    if (!lib) { std::fprintf(stderr, "open failed: %s\n", r.error.c_str()); return 1; }
    const Id world = lib->createWorld("Alvora");
    if (World w = *lib->world(world); true) {
        w.description = "A world of shattered light, guardian beasts and desert kingdoms, shared by every book of the Alvora Chronicles.";
        w.accent = "#2E6DA4";
        lib->updateWorld(w);
    }
    const Id series = lib->createSeries("The Alvora Chronicles", world);
    const Id b1 = lib->createBook("Shards of Light", world, series);
    const Id b2 = lib->createBook("The Reign of Zornov", world, series);
    for (const char* f : {"tom.txt", "sarrah.txt", "light.txt", "telvana.txt", "aaliyah.txt"}) {
        const auto page = paste::parse(readFile(repo + "/tests/fixtures/paste/" + f));
        std::string kind = std::string(f) == "light.txt" ? "magic" : std::string(f) == "telvana.txt" ? "place" : "character";
        const Id e = lib->createEntry(world, {}, kind, page.name.empty() ? std::string(f) : page.name);
        paste::Report rep;
        paste::apply(*lib, e, page, {b1, b2}, rep);
    }
    auto person = [&](const char* name, const char* details, const char* summary) {
        const Id id = lib->createEntry(world, {}, "character", name);
        Entry e = *lib->entry(id);
        e.details = details;
        e.summary = summary;
        lib->updateEntry(e);
        return id;
    };
    auto find = [&](const std::string& name) -> Id {
        for (const Entry* e : lib->entriesOf(world)) if (e->name == name) return e->id;
        return {};
    };
    const Id ralph = find("Ralph Smith").empty() ? person("Ralph Smith", "Gender: Male\nRace: Human", "A blacksmith of Elandroth.") : find("Ralph Smith");
    const Id maria = find("Maria Susan Smith").empty() ? person("Maria Susan Smith", "Gender: Female\nAge: Deceased", "Ralph's late wife.") : find("Maria Susan Smith");
    Id tom;
    for (const Entry* e : lib->entriesOf(world)) if (e->name.rfind("Tom", 0) == 0) tom = e->id;
    const Id astrid = person("Astrid Mira Smith", "Gender: Female\nAge: 24", "Tom's twin sister and the lead of Echoes of Night.");
    const Id lillia = person("Lillia Smith", "Gender: Female\nAge: Child", "A young Fluxian girl Tom rescues from slavers.");
    lib->addLink(ralph, maria, "Married to");
    for (const Id& c : {tom, astrid}) if (!c.empty()) { lib->addLink(ralph, c, "Parent of"); lib->addLink(maria, c, "Parent of"); }
    if (!tom.empty()) lib->addLink(tom, astrid, "Twin of");
    lib->addLink(ralph, lillia, "Adoptive parent of");
    const Id elan = lib->createEntry(world, {}, "place", "Elandroth");
    const Id aq = lib->createEntry(world, {}, "place", "Aquiera");
    lib->addLink(aq, elan, "Located in");
    if (Entry e = *lib->entry(elan); true) { e.summary = "A continent, a nation and that nation's capital city, all sharing the same name."; e.details = "Type: Continent"; lib->updateEntry(e); }
    if (Entry e = *lib->entry(aq); true) { e.summary = "A nation of the Elandroth continent."; e.details = "Type: Nation"; lib->updateEntry(e); }
    // A language.
    conlang::Language tv{"lang1", "Telvani", "The old tongue of the Telvani sages.", {}};
    tv.words = {{"veyth'ar", "light", "noun", "VAY-thar", ""}, {"sel-amar", "farewell", "interjection", "", ""}, {"orun", "stone", "noun", "OH-roon", ""}};
    lib->setSetting("conlangs:world:" + world, conlang::toJson({tv}));

    const std::string fonts = repo + "/apps/studio/fonts";
    bible::Options o{world, {}, false, {}};
    o.treeFonts = fonts;
    o.treeFolder = out + "/trees";
    for (const auto& t : bible::themes()) {
        bible::Look look;
        look.theme = t.id;
        std::string err;
        int pages = 0;
        if (!bible::exportWebsite(*lib, o, out + "/site-" + t.id, err, &pages, look, fonts)) std::fprintf(stderr, "%s: %s\n", t.id, err.c_str());
        else std::printf("site %s: %d pages\n", t.id, pages);
    }
    const auto src = bible::printableSource(*lib, o, "The Alvora Bible", "A. Writer");
    typeset::Design design = typeset::presets()[0].design;
    design.chaptersOnRecto = false;
    design.chapterSinkLines = std::min(design.chapterSinkLines, 3);
    typeset::Resources res{fonts, repo + "/apps/studio/hyphenation"};
    const auto book = typeset::typesetBook(src, design, res);
    std::string err;
    if (!typeset::writePdf(book, out + "/bible.pdf", err)) std::fprintf(stderr, "pdf: %s\n", err.c_str());
    else std::printf("pdf: %zu pages\n", book.pages.size());
    return 0;
}
