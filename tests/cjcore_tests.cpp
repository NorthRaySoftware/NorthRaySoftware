// Unit tests for cjcore. Deliberately dependency-free: run the binary, it
// prints each check and exits non-zero on any failure.
#include "cj/medialib.hpp"

#include <miniz.h>
#include <map>
#include "cj/registry.hpp"
#include "cj/skillswap.hpp"
#include <fstream>
#include <sstream>
#include "cj/time.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

static int g_failed = 0, g_passed = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (cond) { ++g_passed; }                                                   \
        else { ++g_failed; std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

using namespace cj;

static UtcSeconds utc(const char* iso) { return *parseUtcIso(iso); }

static void testTime() {
    std::puts("time");
    CHECK(weekday(2026, 11, 14) == 6);   // Saturday
    CHECK(weekday(1970, 1, 1) == 4);     // Thursday
    // 2026: BST from 29 March 01:00 UTC to 25 October 01:00 UTC.
    CHECK(!isBritishSummerTime(utc("2026-03-29T00:59:59Z")));
    CHECK(isBritishSummerTime(utc("2026-03-29T01:00:00Z")));
    CHECK(isBritishSummerTime(utc("2026-10-25T00:59:59Z")));
    CHECK(!isBritishSummerTime(utc("2026-10-25T01:00:00Z")));
    // Summer Saturday 19:00 UK = 18:00 UTC; winter = 19:00 UTC.
    CHECK(*ukToUtc(*parseLocalIso("2026-07-11T19:00")) == utc("2026-07-11T18:00:00Z"));
    CHECK(*ukToUtc(*parseLocalIso("2026-11-14T19:00")) == utc("2026-11-14T19:00:00Z"));
    // Spring-forward gap doesn't exist.
    CHECK(!ukToUtc(*parseLocalIso("2026-03-29T01:30")).has_value());
    // Fall-back ambiguity resolves to the earlier (BST) instant.
    CHECK(*ukToUtc(*parseLocalIso("2026-10-25T01:30")) == utc("2026-10-25T00:30:00Z"));
    // Round trip and validation.
    CHECK(formatUtcIso(utc("2027-02-28T23:05:00Z")) == "2027-02-28T23:05:00Z");
    CHECK(!parseLocalIso("2026-11-31T10:00").has_value());
    CHECK(!parseLocalIso("2026-11-14 19:00").has_value());
    CHECK(!parseLocalIso("2026-11-14T19:00junk").has_value());
}


static void testRegistry() {
    std::puts("registry");
    CHECK(isValidAppId("writing-studio"));
    CHECK(!isValidAppId("Writing Studio"));
    CHECK(!isValidAppId("a--b"));
    CHECK(!isValidAppId("-a"));
    std::string err;
    auto a = parseAppEntry(R"({"schema":1,"id":"skillswap","name":"SkillSwap","status":"in-development",
        "credits":{"associatedCreator":"A Creator"},"launch":{"executable":"SkillSwap.exe"}})", err);
    CHECK(a && a->name == "SkillSwap" && a->status == AppStatus::InDevelopment && a->developer.empty());
    CHECK(!parseAppEntry(R"({"id":"x","name":"X","launch":{"executable":"..\\..\\evil.exe"}})", err));
    CHECK(!parseAppEntry(R"({"id":"x","name":"X","launch":{"executable":"C:\\Windows\\evil.exe"}})", err));
    CHECK(!parseAppEntry(R"({"id":"x","name":"X","schema":2})", err));
    CHECK(!parseAppEntry("not json", err));
    LoadResult r;
    auto p = parseProfile(R"({"displayName":"Someone","localDemo":false})", r);
    CHECK(p && p->localDemo);   // files can't masquerade as online accounts
}

static std::string slurp(const char* p) { std::ifstream f(p); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }

static void testSkillSwap() {
    std::puts("skillswap");
    using namespace cj::skillswap;
    Catalogue c = loadCatalogue(slurp(CJ_SOURCE_DIR "/shared/metadata/skillswap/categories.json"),
                                slurp(CJ_SOURCE_DIR "/shared/metadata/demo/skillswap.demo.json"));
    CHECK(c.warnings.empty());
    CHECK(c.categories.size() == 7 && c.creators.size() == 3 && c.offerings.size() == 6);
    CHECK(c.offerings[0].demo);
    Filter free;
    free.pricing = (int)Pricing::Free;
    CHECK(filterOfferings(c, free).size() == 2);
    Filter drawing;
    drawing.category = "drawing";
    auto d = filterOfferings(c, drawing);
    CHECK(d.size() == 3);
    // Free offerings keep their place; price never reorders the list.
    CHECK(d[0]->pricing == Pricing::Free);
    CHECK(priceLabel(c.offerings[2]) == "\xC2\xA3" "12");
    CHECK(priceLabel(c.offerings[1]) == "Pay what you can (suggested \xC2\xA3" "5)");
    Catalogue bad = loadCatalogue("{}", R"({"creators":[{"id":"a","name":"A"}],"offerings":[
        {"id":"x","creator":"nobody"},{"id":"y","creator":"a","pricing":"fixed"}]})");
    CHECK(bad.offerings.size() == 1 && bad.offerings[0].pricing == Pricing::Free && bad.warnings.size() == 2);
}


static void testMediaLibrary() {
    namespace ml = cj::medialib;
    using ml::Kind;
    CHECK(ml::kindOf("Cover.PNG") == Kind::Picture && ml::kindOf("intro.mp4") == Kind::Video && ml::kindOf("song.mp3") == Kind::Music);
    CHECK(ml::kindOf("Starting soon.nrdesign") == Kind::Design && ml::kindOf("notes.txt") == Kind::Other && ml::kindOf("folder.v2/README") == Kind::Other);
    ml::Library lib;
    CHECK(lib.storedName("C:\\Pics\\cover.png") == "cover.png" && lib.storedName("a:b?.png") == "a_b_.png" && lib.storedName("") == "file");
    const std::string a = lib.add("cover.png", "Envoy cover", Kind::Picture, "Books/Alvora", "2026-11-15");
    CHECK(!a.empty() && lib.storedName("Cover.png") == "Cover (2).png");   // names are case-insensitive on Windows
    CHECK(lib.add("cover.png", "again", Kind::Picture, "", "2026-11-16").empty());   // the stored name is taken
    const std::string b = lib.add("Cover (2).png", "Grey Wanderer cover", Kind::Picture, "Books\\Alvora\\", "2026-11-16");
    const std::string c = lib.add("rain.wav", "Rain", Kind::Sound, "Sounds", "2026-11-16");
    const std::string d = lib.add("Starting soon.nrdesign", "Starting soon", Kind::Design, " Overlays / Holding ", "2026-11-14");
    CHECK(lib.item(b)->folder == "Books/Alvora" && lib.item(d)->folder == "Overlays/Holding");
    // Tags: clean, sorted, no repeats.
    CHECK(lib.addTag(a, " Alvora ") && lib.addTag(a, "COVER") && !lib.addTag(a, "alvora") && !lib.addTag(a, "  "));
    CHECK(lib.item(a)->tags == (std::vector<std::string>{"alvora", "cover"}));
    lib.addTag(b, "alvora");
    CHECK(lib.tags().at("alvora") == 2 && lib.removeTag(a, "Cover") && !lib.removeTag(a, "cover"));
    // Search: every word somewhere; kind, folder (and inside it) and tag filters; newest first.
    auto names = [](const std::vector<const ml::Item*>& v) {
        std::vector<std::string> n;
        for (auto* i : v) n.push_back(i->name);
        return n;
    };
    CHECK(names(lib.search({"cover"})) == (std::vector<std::string>{"Grey Wanderer cover", "Envoy cover"}));
    CHECK(names(lib.search({"alvora grey"})) == (std::vector<std::string>{"Grey Wanderer cover"}));
    CHECK(names(lib.search({"", (int)Kind::Sound})) == (std::vector<std::string>{"Rain"}));
    CHECK(lib.search({"", -1, "Books"}).size() == 2 && lib.search({"", -1, "Book"}).empty());
    CHECK(lib.search({"", -1, "", "ALVORA"}).size() == 2 && lib.search({}).size() == 4);
    // Folders: listed with their parents; rename moves what's inside; remove moves it up.
    lib.addFolder("Empty/Inner");
    const auto fs = lib.folders();
    CHECK(std::find(fs.begin(), fs.end(), "Books") != fs.end() && std::find(fs.begin(), fs.end(), "Empty/Inner") != fs.end() &&
          std::find(fs.begin(), fs.end(), "Empty") != fs.end());
    CHECK(lib.renameFolder("Books", "Writing/Books") && lib.item(a)->folder == "Writing/Books/Alvora");
    CHECK(!lib.renameFolder("Writing", "Writing/Inside") && !lib.renameFolder("Nowhere", "X"));
    CHECK(lib.removeFolder("Writing/Books") && lib.item(a)->folder == "Writing/Alvora");
    CHECK(lib.removeFolder("Sounds") && lib.item(c)->folder.empty());
    // Saved and read back; junk entries skipped; ids carry on after the highest.
    ml::Library back;
    CHECK(ml::Library::fromJson(lib.toJson(), back) && back.items() == lib.items() && back.folders() == lib.folders());
    ml::Library junk;
    CHECK(ml::Library::fromJson(R"({"format":"northray-media-library","items":[{"id":"m9","file":"x.png","kind":"picture"},{"id":"m9","file":"y.png"},
        {"id":"bad","file":"z.png"},{"id":"m3","file":"x.png"},{"id":"m4","file":"song.mp3","kind":"nope","tags":["A","a",3]}]})",
                                  junk));
    CHECK(junk.items().size() == 2 && junk.item("m4")->kind == Kind::Music && junk.item("m4")->tags == (std::vector<std::string>{"a"}));
    CHECK(junk.add("new.png", "New", Kind::Picture, "", "2026-11-16") == "m10");
    CHECK(!ml::Library::fromJson("{}", junk) && !ml::Library::fromJson("nope", junk));
    CHECK(lib.remove(c) && !lib.remove(c) && lib.items().size() == 3);
}

static void testTemplatePacks() {
    namespace ml = cj::medialib;
    using ml::Kind;
    std::map<std::string, std::string> disk = {{"cover.png", std::string("\x89PNG fake cover", 15)}, {"Starting soon.nrdesign", "{\"elements\":[]}"},
                                               {"bed.mp3", std::string(200000, 'x')}, {"other.png", "other"}};
    auto read = [&](const std::string& f, std::string& b) {
        auto it = disk.find(f);
        if (it == disk.end()) return false;
        b = it->second;
        return true;
    };
    ml::Library lib;
    const auto a = lib.add("cover.png", "Cover", Kind::Picture, "Kit/Pictures", "2026-11-16");
    lib.addTag(a, "alvora");
    lib.add("Starting soon.nrdesign", "Starting soon", Kind::Design, "Kit", "2026-11-16");
    lib.add("bed.mp3", "Music bed", Kind::Music, "Kit/Sounds", "2026-11-16");
    lib.add("other.png", "Not in the kit", Kind::Picture, "Elsewhere", "2026-11-16");
    lib.add("missing.png", "Missing file", Kind::Picture, "Kit", "2026-11-16");
    ml::PackResult pr;
    const std::string pack = ml::buildPack(lib, "Kit", "Aurora stream kit", read, pr);
    CHECK(pr.ok && pr.count == 3 && !pack.empty() && pack.size() < 20000);   // compressed; the missing file left out
    CHECK(ml::packName(pack) == "Aurora stream kit" && ml::packName("not a zip").empty());
    // Into another library: fresh names where taken, folders kept under the one chosen, tags kept.
    ml::Library other;
    other.add("cover.png", "Their own cover", Kind::Picture, "", "2026-11-01");
    std::map<std::string, std::string> written;
    auto write = [&](const std::string& f, const std::string& b) {
        written[f] = b;
        return true;
    };
    const auto rr = ml::readPack(other, pack, "Packs/Aurora stream kit", "2026-11-17", write);
    CHECK(rr.ok && rr.count == 3 && other.items().size() == 4);
    CHECK(written.count("cover (2).png") && written["cover (2).png"] == disk["cover.png"] && written["bed.mp3"].size() == 200000);
    const auto found = other.search({"cover", (int)Kind::Picture, "Packs"});
    CHECK(found.size() == 1 && found[0]->folder == "Packs/Aurora stream kit/Pictures" && found[0]->tags == (std::vector<std::string>{"alvora"}));
    CHECK(other.search({"starting"}).size() == 1 && other.search({"starting"})[0]->folder == "Packs/Aurora stream kit");
    // Everything, when no folder is given; nothing for an empty folder.
    CHECK(!ml::buildPack(lib, "", "All", read, pr).empty() && pr.count == 4);
    CHECK(ml::buildPack(lib, "Nowhere", "x", read, pr).empty() && !pr.ok && !pr.error.empty());
    // A hostile pack: entries outside files/, a path in the file name, a junk manifest.
    {
        mz_zip_archive z{};
        mz_zip_writer_init_heap(&z, 0, 0);
        const std::string evil = "evil";
        mz_zip_writer_add_mem(&z, "files/0-a.png", evil.data(), evil.size(), 0);
        mz_zip_writer_add_mem(&z, "../../outside.txt", evil.data(), evil.size(), 0);
        const std::string man = R"({"format":"northray-pack","name":"Bad","items":[{"entry":"../../outside.txt","file":"x.txt"},
            {"entry":"files/0-a.png","file":"..\\..\\Windows\\a.png","name":"A","folder":"../../up"},{"entry":"files/none.png"},7]})";
        mz_zip_writer_add_mem(&z, "pack.json", man.data(), man.size(), 0);
        void* buf;
        size_t size;
        mz_zip_writer_finalize_heap_archive(&z, &buf, &size);
        const std::string bad(static_cast<const char*>(buf), size);
        mz_free(buf);
        mz_zip_writer_end(&z);
        ml::Library l3;
        written.clear();
        const auto r3 = ml::readPack(l3, bad, "", "2026-11-17", write);
        CHECK(r3.ok && r3.count == 1 && written.size() == 1 && written.count("a.png") && l3.items()[0].folder == "up");
    }
    CHECK(!ml::readPack(other, "junk", "", "d", write).ok && !ml::readPack(other, pack.substr(0, 100), "", "d", write).ok);
}

int main() {
    testSkillSwap();
    testTime();
    testRegistry();
    testMediaLibrary();
    testTemplatePacks();
    std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
