// Tests for the Writing & Publishing Studio's library core. Each test works in
// a fresh temporary folder and reopens the library to prove what was saved.
#include "studio/artwork.hpp"
#include "studio/bible_export.hpp"
#include "studio/conlang.hpp"
#include "studio/cover.hpp"
#include "studio/ebook.hpp"
#include "studio/document.hpp"
#include "studio/entry_paste.hpp"
#include "studio/fonts.hpp"
#include "studio/family_tree.hpp"
#include "studio/import.hpp"
#include "studio/library.hpp"
#include "studio/planning.hpp"
#include "studio/sync.hpp"
#include "studio/publish.hpp"
#include "studio/review.hpp"
#include "miniz.h"
#include "studio/typeset.hpp"
#include "importutil.hpp"
#include "sqlite3.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <fstream>
#include <nlohmann/json.hpp>
#include <functional>
#include <map>
#include <string>

namespace fs = std::filesystem;
using namespace studio;

static int g_failed = 0, g_passed = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (cond) ++g_passed;                                                       \
        else { ++g_failed; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

static std::string tempLibrary(const char* name) {
    fs::path p = fs::temp_directory_path() / ("studio-test-" + std::string(name) + "-" + newId().substr(0, 8));
    fs::remove_all(p);
    return p.string();
}

static std::unique_ptr<Library> openOk(const std::string& dir) {
    OpenResult r;
    auto lib = Library::open(dir, r);
    CHECK(r.ok);
    if (!r.ok) std::printf("  open error: %s\n", r.error.c_str());
    return lib;
}

static void testCreateAndReopen() {
    const std::string dir = tempLibrary("create");
    {
        OpenResult r;
        auto lib = Library::open(dir, r);
        CHECK(r.ok && r.created);
        CHECK(lib->worlds().empty() && lib->books().empty());  // the library begins empty
        CHECK(lib->schemaVersion() == Library::kSchemaVersion);

        const Id alvora = lib->createWorld("  Alvora ");
        const Id chronicles = lib->createSeries("The Alvora Chronicles", alvora);
        const Id b1 = lib->createBook("Book One", {}, chronicles);  // world comes from the series
        const Id b2 = lib->createBook("Book Two", {}, chronicles);
        const Id lonely = lib->createBook("A Standalone Tale", alvora);
        const Id indie = lib->createBook("Nonfiction Manuscript", {});
        CHECK(!alvora.empty() && !chronicles.empty() && !b1.empty() && !lonely.empty() && !indie.empty());
        CHECK(lib->world(alvora)->name == "Alvora");               // trimmed
        CHECK(lib->book(b1)->worldId == alvora);
        CHECK(lib->membership(chronicles, b2)->number == "2");      // numbered in order
        CHECK(lib->booksInSeries(chronicles).size() == 2);
        CHECK(lib->standaloneBooksIn(alvora).size() == 1);
        CHECK(lib->standaloneBooksIn({}).size() == 1);              // independent books

        Book b = *lib->book(b1);
        b.subtitle = "The Envoy of Telvana";
        b.penName = "C. J. Philip";
        b.status = BookStatus::Drafting;
        b.tags = {"fantasy", " fantasy ", "", "dragons"};
        b.isbn = "978-0-00-000000-0";
        CHECK(lib->updateBook(b));
    }
    {
        auto lib = openOk(dir);  // everything survived closing and reopening
        CHECK(lib->worlds().size() == 1);
        CHECK(lib->series().size() == 1);
        CHECK(lib->books().size() == 4);
        const Book* b = nullptr;
        for (auto& x : lib->books()) if (x.title == "Book One") b = &x;
        CHECK(b && b->subtitle == "The Envoy of Telvana" && b->status == BookStatus::Drafting);
        CHECK(b && b->tags.size() == 2 && b->tags[0] == "fantasy" && b->tags[1] == "dragons");  // deduplicated, trimmed
        CHECK(fs::exists(fs::path(dir) / "backups"));
        int backups = 0;
        for (auto& e : fs::directory_iterator(fs::path(dir) / "backups")) (void)e, ++backups;
        CHECK(backups >= 1);  // reopening made a backup copy
    }
    fs::remove_all(dir);
}

static void testOrdering() {
    const std::string dir = tempLibrary("order");
    auto lib = openOk(dir);
    const Id w = lib->createWorld("Zehndar");
    const Id s = lib->createSeries("Main Series", w);
    const Id a = lib->createBook("A", {}, s), b = lib->createBook("B", {}, s), c = lib->createBook("C", {}, s);
    CHECK(lib->reorderSeriesBooks(s, {c, a, b}, false));
    auto order = lib->booksInSeries(s);
    CHECK(order.size() == 3 && order[0]->id == c && order[1]->id == a && order[2]->id == b);
    // Automatic numbering follows the new reading order...
    CHECK(lib->membership(s, c)->number == "1" && lib->membership(s, a)->number == "2" && lib->membership(s, b)->number == "3");
    // Chronological order is independent of reading order.
    CHECK(lib->reorderSeriesBooks(s, {b, c, a}, true));
    auto chrono = lib->booksInSeries(s, true);
    CHECK(chrono[0]->id == b && chrono[2]->id == a);
    CHECK(lib->booksInSeries(s)[0]->id == c);
    const Id w2 = lib->createWorld("Vesper"), w3 = lib->createWorld("Alex");
    CHECK(lib->reorderWorlds({w3, w, w2}));
    CHECK(lib->liveWorlds()[0]->id == w3 && lib->liveWorlds()[2]->id == w2);
    Membership m = *lib->membership(s, a);
    m.number = "";            // unnumbered companion story
    m.kind = Instalment::Companion;
    CHECK(lib->updateMembership(m));
    // ...but once numbers are customised, reordering leaves them alone.
    CHECK(lib->reorderSeriesBooks(s, {a, c, b}, false));
    CHECK(lib->membership(s, a)->number.empty() && lib->membership(s, c)->number == "1");
    CHECK(lib->reorderSeriesBooks(s, {c, a, b}, false));
    lib.reset();
    auto again = openOk(dir);
    CHECK(again->booksInSeries(s)[0]->id == c);
    CHECK(again->booksInSeries(s, true)[0]->id == b);
    CHECK(again->liveWorlds()[0]->id == w3);
    CHECK(again->membership(s, a)->number.empty() && again->membership(s, a)->kind == Instalment::Companion);
    again.reset();
    fs::remove_all(dir);
}

static void testMoving() {
    const std::string dir = tempLibrary("move");
    auto lib = openOk(dir);
    const Id alvora = lib->createWorld("Alvora"), zehndar = lib->createWorld("Zehndar");
    const Id chron = lib->createSeries("Chronicles", alvora), extra = lib->createSeries("Additional Series", alvora);
    const Id zmain = lib->createSeries("Main Series", zehndar);
    const Id book = lib->createBook("Crossover", {}, chron);
    CHECK(lib->addBookToSeries(book, extra));               // one book, two series, no copy
    CHECK(lib->books().size() == 1);
    CHECK(lib->seriesOfBook(book).size() == 2);

    // Moving to another world leaves the old world's series; the UI is warned first.
    CHECK(lib->seriesLostByMove(book, zehndar).size() == 2);
    CHECK(lib->moveBookToWorld(book, zehndar));
    CHECK(lib->book(book)->worldId == zehndar && lib->seriesOfBook(book).empty());
    CHECK(lib->standaloneBooksIn(zehndar).size() == 1);

    // Adding it to a series in another world moves it there.
    CHECK(lib->addBookToSeries(book, chron));
    CHECK(lib->book(book)->worldId == alvora && lib->seriesOfBook(book).size() == 1);

    CHECK(lib->makeStandalone(book));
    CHECK(lib->seriesOfBook(book).empty() && lib->book(book)->worldId == alvora);

    // A series moves with the books that lived in its old world.
    CHECK(lib->addBookToSeries(book, chron));
    const Id other = lib->createBook("Stays put", zehndar);
    CHECK(lib->addBookToSeries(other, zmain));
    CHECK(lib->moveSeriesToWorld(chron, zehndar));
    CHECK(lib->seriesById(chron)->worldId == zehndar && lib->book(book)->worldId == zehndar);
    CHECK(lib->seriesIn(alvora).size() == 1 && lib->seriesIn(zehndar).size() == 2);

    // Independent book: out of any world.
    CHECK(lib->moveBookToWorld(book, {}));
    CHECK(lib->book(book)->worldId.empty() && lib->seriesOfBook(book).empty());
    lib.reset();
    fs::remove_all(dir);
}

static void testTrash() {
    const std::string dir = tempLibrary("trash");
    auto lib = openOk(dir);
    const Id w = lib->createWorld("Alvora");
    const Id s = lib->createSeries("Chronicles", w);
    const Id b1 = lib->createBook("One", {}, s), b2 = lib->createBook("Two", {}, s);

    CHECK(lib->trashSeries(s));
    CHECK(lib->seriesIn(w).empty());
    CHECK(lib->standaloneBooksIn(w).size() == 2);   // books aren't lost with their series
    CHECK(lib->restore(s));
    CHECK(lib->booksInSeries(s).size() == 2);        // and the series comes back whole

    CHECK(lib->trashBook(b1));
    CHECK(lib->booksInSeries(s).size() == 1 && lib->liveBooks().size() == 1);
    CHECK(lib->restore(b1));
    CHECK(lib->booksInSeries(s).size() == 2);

    CHECK(lib->trashWorld(w));
    CHECK(lib->liveWorlds().empty() && lib->liveBooks().empty());
    CHECK(lib->trashCounts().worlds == 1);
    // Restoring a book whose world is still in the Trash makes it independent.
    CHECK(lib->restore(b2));
    CHECK(lib->book(b2)->worldId.empty() && lib->liveBooks().size() == 1);

    CHECK(lib->emptyTrash());
    CHECK(lib->worlds().empty() && lib->series().empty());
    CHECK(lib->books().size() == 1 && lib->books()[0].id == b2);
    CHECK(lib->memberships().empty());
    lib.reset();
    auto again = openOk(dir);
    CHECK(again->books().size() == 1 && again->worlds().empty());
    again.reset();
    fs::remove_all(dir);
}

static void testBlobsExportAndSafety() {
    const std::string dir = tempLibrary("blobs");
    auto lib = openOk(dir);
    CHECK(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const std::string png = std::string("\x89PNG\r\n\x1a\n", 8) + std::string(5000, 'x');
    const std::string h1 = lib->storeBlob(png), h2 = lib->storeBlob(png);
    CHECK(!h1.empty() && h1 == h2);                       // stored once
    CHECK(fs::file_size(fs::path(lib->blobPath(h1))) == png.size());
    CHECK(lib->blobPath("../../etc/passwd").empty());      // only real hashes map to files

    const Id w = lib->createWorld("Alvora");
    const Id b = lib->createBook("Book", w);
    Book x = *lib->book(b);
    x.coverBlob = h1;
    CHECK(lib->updateBook(x));
    const std::string out = (fs::path(dir) / "export" / "library.json").string();
    std::string err;
    CHECK(lib->exportJson(out, err));
    nlohmann::json j;
    { std::ifstream f(out); j = nlohmann::json::parse(f, nullptr, false); }  // closed before it's replaced (Windows locks open files)
    CHECK(j.is_object() && j["format"] == "studio-library-export");
    CHECK(j["worlds"].size() == 1 && j["books"].size() == 1 && j["books"][0]["coverBlob"] == h1);

    // Atomic write replaces an existing file completely.
    CHECK(atomicWrite(out, "second", err));
    std::string body;
    { std::ifstream g(out); body.assign((std::istreambuf_iterator<char>(g)), {}); }
    CHECK(body == "second");

    CHECK(looksLikeSyncFolder("C:\\Users\\caleb\\OneDrive\\Documents\\Library"));
    CHECK(!looksLikeSyncFolder("C:\\Users\\caleb\\Writing Studio"));

    // Bad input is refused, not stored.
    CHECK(lib->createSeries("x", "no-such-world").empty());
    CHECK(!lib->addBookToSeries(b, "no-such-series"));
    Book unnamed = *lib->book(b);
    unnamed.title = "   ";
    CHECK(lib->updateBook(unnamed) && lib->book(b)->title == "Book");  // an empty title keeps the old one
    lib.reset();
    fs::remove_all(dir);
}

static void testNewerFormatRefused() {
    const std::string dir = tempLibrary("newer");
    { auto lib = openOk(dir); lib->createWorld("Kept"); }
    {
        // A library written by a future version of the Studio...
        sqlite3* db = nullptr;
        sqlite3_open((fs::path(dir) / "library.db").string().c_str(), &db);
        sqlite3_exec(db, "UPDATE meta SET value = '99' WHERE key = 'schema_version'", nullptr, nullptr, nullptr);
        sqlite3_close(db);
    }
    OpenResult r;
    auto lib = Library::open(dir, r);
    CHECK(!r.ok && !lib && r.error.find("newer version") != std::string::npos);  // ...is refused, never rewritten
    {
        sqlite3* db = nullptr;
        sqlite3_open((fs::path(dir) / "library.db").string().c_str(), &db);
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db, "SELECT count(*) FROM worlds", -1, &st, nullptr);
        CHECK(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 1);  // data untouched
        sqlite3_finalize(st);
        sqlite3_close(db);
    }
    // A damaged file is reported, not opened.
    const std::string bad = tempLibrary("damaged");
    fs::create_directories(bad);
    { std::ofstream f(fs::path(bad) / "library.db", std::ios::binary); f << std::string(8192, 'Z'); }
    OpenResult r2;
    auto lib2 = Library::open(bad, r2);
    CHECK(!r2.ok && !lib2);
    fs::remove_all(dir);
    fs::remove_all(bad);
}


static void testDocument() {
    using namespace studio::doc;
    CHECK(countWords("Hello, world!") == 2);
    CHECK(countWords("It's a well-known fact—really.") == 5);   // it's, a, well-known, fact, really
    CHECK(countWords("  —  ...  ") == 0);
    CHECK(countWords("Café naïve 2026") == 3);
    CHECK(countWords("") == 0);

    Builder b;
    CHECK(b.empty());
    b.heading(1); b.text("Chapter One");
    b.paragraph("first"); b.text("The "); b.text("storm", Builder::Em); b.text(" came.");
    b.sceneBreak(); b.sceneBreak();               // doubled: kept once
    b.paragraph(); b.text("Later.");
    b.beginQuote(); b.paragraph("epigraph"); b.text("A quote."); b.endQuote();
    b.beginList(true); b.listItem(); b.text("One"); b.listItem(); b.text("Two"); b.endList();
    b.sceneBreak();                               // trailing: dropped
    const std::string j = b.toJson();
    CHECK(!b.empty());
    CHECK(b.firstHeadingText() == "Chapter One");
    CHECK(wordCount(j) == 2 + 3 + 1 + 2 + 2);
    nlohmann::json d = nlohmann::json::parse(j);
    CHECK(d["type"] == "doc");
    CHECK(d["content"][1]["attrs"]["style"] == "first");
    CHECK(d["content"][1]["content"][1]["marks"][0]["type"] == "em");
    int breaks = 0;
    for (auto& x : d["content"]) breaks += x["type"] == "scene_break";
    CHECK(breaks == 1);
    CHECK(d["content"].back()["type"] == "ordered_list");
    CHECK(plainText(j).find("* * *") != std::string::npos);
    CHECK(sanitise(j) == nlohmann::json::parse(j).dump());   // already clean: unchanged

    // Sanitising: unknown blocks/marks/styles, unsafe links, bad images.
    std::vector<std::string> problems;
    const std::string dirty = R"J({"type":"doc","content":[
      {"type":"table","content":[{"type":"text","text":"cell"}]},
      {"type":"paragraph","attrs":{"style":"weird"},"content":[{"type":"text","text":"x","marks":[{"type":"em"},{"type":"blink"},{"type":"link","attrs":{"href":"javascript:alert(1)"}}]}]},
      {"type":"image","attrs":{"blob":"nope"}},
      {"type":"script"}]})J";
    nlohmann::json c = nlohmann::json::parse(sanitise(dirty, &problems));
    CHECK(c["content"].size() == 2);
    CHECK(c["content"][0]["type"] == "paragraph" && c["content"][0]["content"][0]["text"] == "cell");
    CHECK(c["content"][1]["attrs"]["style"] == "body");
    CHECK(c["content"][1]["content"][0]["marks"].size() == 1);
    CHECK(!problems.empty());
    CHECK(sanitise("not json") == emptyDoc());
    CHECK(sanitise("") == emptyDoc());
}

static std::string para(const std::string& text) {
    studio::doc::Builder b;
    b.paragraph(); b.text(text);
    return b.toJson();
}

static void testManuscript() {
    const std::string dir = tempLibrary("manuscript");
    Id book, p1, c1, c2, s1, s2;
    {
        auto lib = openOk(dir);
        book = lib->createBook("Novel", {});
        p1 = lib->createNode(book, {}, NodeKind::Part);
        c1 = lib->createNode(book, p1, NodeKind::Chapter);
        c2 = lib->createNode(book, p1, NodeKind::Chapter, "The Road");
        s1 = lib->createNode(book, c1, NodeKind::Scene);
        s2 = lib->createNode(book, c1, NodeKind::Scene, "", s1);
        CHECK(lib->node(p1)->title == "Part 1");
        CHECK(lib->node(c1)->title == "Chapter 1");
        CHECK(lib->node(c2)->title == "The Road");
        CHECK(lib->node(s2)->title == "Scene 2");
        // Nesting rules.
        CHECK(lib->createNode(book, s1, NodeKind::Scene).empty());
        CHECK(lib->createNode(book, c1, NodeKind::Part).empty());
        CHECK(lib->createNode(book, p1, NodeKind::Part).empty());
        CHECK(!lib->createNode(book, {}, NodeKind::Scene).empty());   // a loose scene at book level is fine
        // Reading order.
        auto all = lib->manuscriptOf(book);
        CHECK(all.size() == 6 && all[0]->id == p1 && all[1]->id == c1 && all[2]->id == s1 && all[3]->id == s2 && all[4]->id == c2);
        // Text and counts.
        CHECK(lib->saveNodeContent(s1, para("One two three.")));
        CHECK(lib->saveNodeContent(c2, para("Four five.")));
        CHECK(lib->node(s1)->wordCount == 3);
        CHECK(lib->book(book)->wordCount == 5);
        // Moving: c2 before c1; s2 into c2; not a chapter into a scene; not a part into itself.
        CHECK(lib->moveNode(c2, p1, 0));
        CHECK(lib->childrenOf(book, p1)[0]->id == c2);
        CHECK(lib->moveNode(s2, c2, -1));
        CHECK(lib->childrenOf(book, c2).size() == 1);
        CHECK(!lib->moveNode(c1, s1, 0));
        CHECK(!lib->moveNode(p1, p1, 0));
        // Trash and restore take the subtree together.
        CHECK(lib->trashNode(c1));
        CHECK(lib->node(s1)->deleted != 0);
        CHECK(lib->book(book)->wordCount == 2);
        CHECK(lib->trashedNodesOf(book).size() == 1);
        CHECK(lib->restore(c1));
        CHECK(lib->node(s1)->deleted == 0 && lib->book(book)->wordCount == 5);
    }
    {
        // Everything survives reopening.
        auto lib = openOk(dir);
        CHECK(lib->schemaVersion() == Library::kSchemaVersion);
        CHECK(lib->node(c2)->title == "The Road");
        CHECK(lib->childrenOf(book, p1)[0]->id == c2);
        CHECK(lib->book(book)->wordCount == 5);
        CHECK(studio::doc::plainText(lib->nodeContent(s1)) == "One two three.");
        // Versions: the first save after a while keeps the old text.
        CHECK(lib->nodeVersions(s1).empty());   // the first save came straight after creation (empty text isn't kept)
        CHECK(lib->saveNodeContent(s1, para("Rewritten completely.")));
        auto v = lib->nodeVersions(s1);
        CHECK(v.size() == 1 && v[0].wordCount == 3 && v[0].reason == "autosave");
        CHECK(lib->saveNodeContent(s1, para("Rewritten again, soon after.")));
        CHECK(lib->nodeVersions(s1).size() == 1);           // within ten minutes: no extra version
        CHECK(lib->restoreNodeVersion(s1, v[0].id));
        CHECK(studio::doc::plainText(lib->nodeContent(s1)) == "One two three.");
        CHECK(lib->nodeVersions(s1).size() == 2 && lib->nodeVersions(s1)[0].reason == "before restore");
        // Bulk add (an import), in one go.
        NewNode part{NodeKind::Part, "Book Two", "", {{NodeKind::Chapter, "Arrival", para("Hello there."), {}},
                                                       {NodeKind::Chapter, "", para("More words here."), {}}}};
        auto top = lib->addNodes(book, {}, {part});
        CHECK(top.size() == 1);
        CHECK(lib->childrenOf(book, top[0]).size() == 2);
        CHECK(lib->childrenOf(book, top[0])[1]->title == "Chapter");
        CHECK(lib->book(book)->wordCount == 5 + 2 + 3);
        CHECK(lib->addNodes(book, s1, {part}).empty());     // a part can't go in a scene: nothing added
        // Export includes the text.
        std::string err;
        CHECK(lib->exportJson((fs::path(dir) / "x.json").string(), err));
        std::ifstream f(fs::path(dir) / "x.json");
        nlohmann::json ex = nlohmann::json::parse(f);
        CHECK(ex["nodes"].size() == lib->nodes().size());
        // Emptying the Trash removes trashed nodes for good; trashing the book takes its text with it.
        CHECK(lib->trashNode(c2));
        CHECK(lib->emptyTrash());
        CHECK(lib->node(c2) == nullptr && lib->node(s2) == nullptr);
        CHECK(lib->trashBook(book) && lib->emptyTrash());
        CHECK(lib->nodes().empty());
    }
    fs::remove_all(dir);
}

static void testUpgradeFromVersion1() {
    // A library made by Milestone 1 (schema 1, no manuscript tables) opens and upgrades.
    const std::string dir = tempLibrary("upgrade");
    fs::create_directories(dir);
    {
        sqlite3* db = nullptr;
        sqlite3_open((fs::path(dir) / "library.db").string().c_str(), &db);
        sqlite3_exec(db,
                     "CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
                     "INSERT INTO meta VALUES ('format','studio-library'),('schema_version','1');"
                     "CREATE TABLE worlds (id TEXT PRIMARY KEY, name TEXT NOT NULL, description TEXT NOT NULL DEFAULT '', accent TEXT NOT NULL DEFAULT '', notes TEXT NOT NULL DEFAULT '', banner_blob TEXT NOT NULL DEFAULT '', logo_blob TEXT NOT NULL DEFAULT '', sort_order REAL NOT NULL DEFAULT 0, created INTEGER NOT NULL, modified INTEGER NOT NULL, deleted INTEGER NOT NULL DEFAULT 0);"
                     "CREATE TABLE books (id TEXT PRIMARY KEY, world_id TEXT, title TEXT NOT NULL, subtitle TEXT NOT NULL DEFAULT '', author TEXT NOT NULL DEFAULT '', pen_name TEXT NOT NULL DEFAULT '', synopsis TEXT NOT NULL DEFAULT '', genre TEXT NOT NULL DEFAULT '', language TEXT NOT NULL DEFAULT 'en-GB', isbn TEXT NOT NULL DEFAULT '', edition TEXT NOT NULL DEFAULT '', status TEXT NOT NULL DEFAULT 'idea', tags TEXT NOT NULL DEFAULT '[]', cover_blob TEXT NOT NULL DEFAULT '', word_count INTEGER NOT NULL DEFAULT 0, sort_order REAL NOT NULL DEFAULT 0, created INTEGER NOT NULL, modified INTEGER NOT NULL, deleted INTEGER NOT NULL DEFAULT 0);"
                     "INSERT INTO books(id,title,created,modified) VALUES ('b1','Old Book',1,1);",
                     nullptr, nullptr, nullptr);
        sqlite3_close(db);
    }
    auto lib = openOk(dir);
    CHECK(lib && lib->schemaVersion() == Library::kSchemaVersion);
    CHECK(lib && lib->book("b1") && lib->book("b1")->title == "Old Book");
    CHECK(lib && !lib->createNode("b1", {}, NodeKind::Chapter).empty());
    lib.reset();
    fs::remove_all(dir);
}

static std::string fixture(const char* name) {
    for (const char* base : {"tests/fixtures/", "../tests/fixtures/", "../../tests/fixtures/", "fixtures/"}) {
        const std::string p = std::string(base) + name;
        if (fs::exists(p)) return p;
    }
    return std::string("tests/fixtures/") + name;
}

static const studio::import::Section* findSection(const studio::import::Plan& p, const std::string& title) {
    for (auto& s : p.sections) if (s.title == title) return &s;
    return nullptr;
}

static void testImportEpub() {
    using namespace studio::import;
    auto plan = readFile(fixture("novel.epub"));
    CHECK(plan.ok);
    if (!plan.ok) { std::printf("  %s\n", plan.error.c_str()); return; }
    CHECK(plan.format == Format::Epub);
    CHECK(plan.meta.title == "The Glass Harbour" && plan.meta.subtitle == "A Novel" && plan.meta.author == "Ada Example");
    CHECK(plan.meta.isbn == "9780000000002" && plan.meta.description == "A quiet town.");
    CHECK(plan.meta.coverType == "image/png" && !plan.meta.coverBytes.empty());
    CHECK(plan.images.size() == 2);
    // Structure: front matter excluded; two parts; continuation file joined; deep TOC entries don't split.
    auto* title = findSection(plan, "The Glass Harbour");
    CHECK(title && title->role == Role::TitlePage && !title->include);
    CHECK(findSection(plan, "Contents") && !findSection(plan, "Contents")->include);
    auto* p1 = findSection(plan, "Part One: Arrival");
    CHECK(p1 && p1->role == Role::Part && p1->include);
    auto* c1 = findSection(plan, "Chapter 1: The Storm");
    CHECK(c1 && c1->level == 1 && c1->role == Role::Chapter);
    CHECK(!findSection(plan, "Deep entry"));
    CHECK(findSection(plan, "About the Author") && findSection(plan, "About the Author")->role == Role::AboutAuthor);
    CHECK(findSection(plan, "Acknowledgements") && findSection(plan, "Acknowledgements")->role == Role::Acknowledgements);
    CHECK(!findSection(plan, "Cover"));   // the cover page became the book cover
    if (c1) {
        const std::string text = studio::doc::plainText(c1->content);
        CHECK(text.find("Secret text") == std::string::npos);           // display:none
        CHECK(text.find("alert") == std::string::npos);                 // scripts never come across
        CHECK(text.find("carries on in a second file") != std::string::npos);
        CHECK(text.rfind("The sea remembers.", 0) == 0);                // repeated chapter headings removed
        nlohmann::json d = nlohmann::json::parse(c1->content);
        CHECK(d["content"][0]["type"] == "blockquote");
        CHECK(d["content"][0]["content"][0]["attrs"]["style"] == "epigraph");
        CHECK(d["content"][0]["content"][1]["attrs"]["style"] == "attribution");
        CHECK(d["content"][1]["attrs"]["style"] == "first");            // CSS text-indent: 0
        CHECK(d["content"][1]["content"][1]["text"] == "storm");        // italic run without the spaces around it
        CHECK(d["content"][1]["content"][3]["marks"][0]["type"] == "em");  // CSS class italic
        int breaks = 0;
        for (auto& b : d["content"]) breaks += b["type"] == "scene_break";
        CHECK(breaks == 2);                                             // "* * *" and <hr>
    }
    auto* c3 = findSection(plan, "Chapter 3");
    CHECK(c3 && c3->content.find("\"image\"") != std::string::npos);

    // Into a library.
    const std::string dir = tempLibrary("import");
    {
        auto lib = openOk(dir);
        const Id book = lib->createBook("", {});
        auto ids = apply(*lib, book, plan);
        CHECK(ids.size() == 4);   // Part One, Part Two, Acknowledgements, About the Author
        auto top = lib->childrenOf(book, {});
        CHECK(top.size() == 4 && top[0]->kind == NodeKind::Part && top[0]->title == "Part One: Arrival");
        CHECK(lib->childrenOf(book, top[0]->id).size() == 2);
        CHECK(lib->book(book)->title == "The Glass Harbour" && lib->book(book)->author == "Ada Example");
        CHECK(!lib->book(book)->coverBlob.empty());
        CHECK(lib->book(book)->wordCount == plan.includedWords());
        // The map image is stored and referenced.
        bool found = false;
        for (auto& [hash, bytes] : plan.images)
            if (fs::exists(lib->blobPath(hash))) found = true;
        CHECK(found);
        // Existing details are never overwritten.
        Book b = *lib->book(book);
        b.author = "Someone Else";
        lib->updateBook(b);
        apply(*lib, book, plan);
        CHECK(lib->book(book)->author == "Someone Else");
    }
    fs::remove_all(dir);

    // EPUB 2, NCX, one file split by anchors; font obfuscation is fine.
    auto single = readFile(fixture("single.epub"));
    CHECK(single.ok && single.sections.size() == 3);
    CHECK(single.ok && single.sections[2].title == "Three" && single.meta.author == "B. Writer");
    // Refusals.
    auto adobe = readFile(fixture("drm-adobe.epub"));
    CHECK(!adobe.ok && adobe.error.find("Adobe DRM") != std::string::npos);
    auto enc = readFile(fixture("drm-encrypted.epub"));
    CHECK(!enc.ok && enc.error.find("encrypted") != std::string::npos);
    auto notEpub = readFile(fixture("not-epub.zip"));
    CHECK(!notEpub.ok);
    // A zip bomb is refused, not unpacked; the rest still imports.
    auto bomb = readFile(fixture("bomb.epub"));
    CHECK(bomb.ok && bomb.sections.size() == 3);
    bool warned = false;
    for (auto& w : bomb.warnings) if (w.find("too large") != std::string::npos) warned = true;
    CHECK(warned);
}

static void testImportText() {
    using namespace studio::import;
    const std::string txt =
        "THE LONG ROAD\nby A. Writer\n\n"
        "CHAPTER ONE\nThe Beginning\n\nIt was a cold day. The wind\nblew hard.\n\nShe walked on.\n\n* * *\n\nNight fell.\n\n"
        "Chapter 2\n\nMore story here.\n\nEpilogue\n\nThe end of it all.\n";
    auto p = readText(txt, "road.txt", false);
    CHECK(p.ok && p.approximate);
    CHECK(p.sections.size() == 4);
    if (p.sections.size() == 4) {
        CHECK(p.sections[0].role == Role::TitlePage && !p.sections[0].include);
        CHECK(p.sections[1].title == "CHAPTER ONE: The Beginning");
        CHECK(studio::doc::plainText(p.sections[1].content).rfind("It was a cold day. The wind blew hard.", 0) == 0);
        CHECK(p.sections[1].content.find("scene_break") != std::string::npos);
        CHECK(p.sections[2].title == "Chapter 2");
        CHECK(p.sections[3].role == Role::Epilogue);
    }
    const std::string md = "# Part One\n\n# Chapter 1\n\nSome *italic* and **bold** and snake_case and [a link](https://x.org).\n\n"
                           "> A quote.\n\n# Chapter 2\n\nMore.\n";
    auto m = readText(md, "book.md", true);
    CHECK(m.ok && m.sections.size() == 3);
    if (m.sections.size() == 3) {
        CHECK(m.sections[0].role == Role::Part && m.sections[1].level == 1);
        nlohmann::json d = nlohmann::json::parse(m.sections[1].content);
        CHECK(d["content"][0]["content"][1]["marks"][0]["type"] == "em");
        CHECK(d["content"][0]["content"][3]["marks"][0]["type"] == "strong");
        CHECK(studio::doc::plainText(m.sections[1].content).find("snake_case") != std::string::npos);
        CHECK(d["content"][1]["type"] == "blockquote");
    }
    auto none = readText("Just one paragraph with no headings at all.", "notes.txt", false);
    CHECK(none.ok && none.sections.size() == 1 && none.sections[0].title == "notes" && !none.warnings.empty());
    CHECK(!readText("   \n\n  ", "empty.txt", false).ok);
    // HTML file split at its h1 headings.
    auto h = readHtml("<html><head><title>T</title></head><body><h1>One</h1><p>a b</p><h1>Two</h1><p>c</p></body></html>", "x.html");
    CHECK(h.ok && h.sections.size() == 2 && h.sections[1].title == "Two");
}

static void testPlanHandOver() {
    using namespace studio::import;
    auto plan = readFileSafely(fixture("novel.epub"));
    CHECK(plan.ok);
    const std::string dir = tempLibrary("plan");
    std::string err;
    CHECK(savePlan(plan, dir, err));
    auto back = loadPlan(dir);
    CHECK(back.ok && back.sections.size() == plan.sections.size() && back.images.size() == plan.images.size());
    CHECK(back.meta.title == plan.meta.title && back.meta.coverBytes == plan.meta.coverBytes);
    bool same = back.sections.size() == plan.sections.size();
    for (size_t i = 0; same && i < plan.sections.size(); ++i)
        same = back.sections[i].content == plan.sections[i].content && back.sections[i].role == plan.sections[i].role &&
               back.sections[i].include == plan.sections[i].include && back.sections[i].level == plan.sections[i].level;
    CHECK(same);
    // A tampered image is dropped, not trusted.
    { std::ofstream f(fs::path(dir) / (plan.images.begin()->first + ".img"), std::ios::binary); f << "not the image"; }
    CHECK(loadPlan(dir).images.size() == plan.images.size() - 1);
    // Missing or broken results are an error, not a crash.
    CHECK(!loadPlan(dir + "-missing").ok);
    { std::ofstream f(fs::path(dir) / "plan.json"); f << "{\"sections\": 5}"; }
    CHECK(!loadPlan(dir).ok);
    // Errors pass through too.
    auto bad = readFileSafely(fixture("drm-adobe.epub"));
    CHECK(savePlan(bad, dir, err) && !loadPlan(dir).ok && loadPlan(dir).error == bad.error);
    fs::remove_all(dir);
}

static void testImportOffice() {
    using namespace studio::import;
    auto findType = [](const std::string& docJson, const char* type) { return docJson.find(std::string("\"type\":\"") + type + "\"") != std::string::npos; };
    auto m = readFile(fixture("manuscript.docx"));
    CHECK(m.ok && m.format == Format::Docx);
    CHECK(m.meta.title == "The Lighthouse Keeper" && m.meta.author == "Ada Example");
    auto* part = findSection(m, "Part One: The Coast");
    CHECK(part && part->role == Role::Part);
    auto* c1 = findSection(m, "Chapter 1: The Lamp");
    CHECK(c1 && c1->level == 1);
    if (c1) {
        CHECK(findType(c1->content, "blockquote") && findType(c1->content, "bullet_list") && findType(c1->content, "scene_break"));
        CHECK(c1->content.find("\"smallcaps\"") != std::string::npos && c1->content.find("\"em\"") != std::string::npos);
    }
    auto* c2 = findSection(m, "Chapter 2: Strangers");
    CHECK(c2 && findType(c2->content, "image") && c2->content.find("https://example.com/harbour") != std::string::npos);
    CHECK(c2 && c2->content.find("DELETED") == std::string::npos);          // tracked deletions stay out
    auto* notes = findSection(m, "Notes");
    CHECK(notes && notes->role == Role::Notes && notes->level == 0 &&
          studio::doc::plainText(notes->content).find("mending nets") != std::string::npos);
    CHECK(m.images.size() == 1);
    auto p = readFile(fixture("plain.docx"));
    CHECK(p.ok && p.sections.size() == 4);
    CHECK(findSection(p, "CHAPTER ONE: The Storm") && findSection(p, "Epilogue") && findSection(p, "Epilogue")->role == Role::Epilogue);
    CHECK(findSection(p, "CHAPTER ONE: The Storm") && findType(findSection(p, "CHAPTER ONE: The Storm")->content, "scene_break"));
    auto o = readFile(fixture("harbour.odt"));
    CHECK(o.ok && o.format == Format::Odt && o.meta.title == "Harbour Lights" && o.meta.author == "B. Writer");
    auto* oc = findSection(o, "Chapter One");
    CHECK(oc && oc->content.find("\"em\"") != std::string::npos && findType(oc->content, "blockquote") && findType(oc->content, "scene_break"));
    CHECK(findSection(o, "Notes") && studio::doc::plainText(findSection(o, "Notes")->content).find("local custom") != std::string::npos);
}

// Header-only test pictures: enough for imageInfo, which never decodes.
static std::string pngOf(int w, int h) {
    std::string b("\x89PNG\r\n\x1a\n\0\0\0\x0dIHDR", 16);
    for (int v : {w, h}) for (int s = 24; s >= 0; s -= 8) b += (char)((v >> s) & 0xFF);
    b += std::string("\x08\x02\0\0\0", 5);
    return b;
}

static void testArtwork() {
    // Image headers.
    CHECK(imageInfo(pngOf(1800, 1200)).ok && imageInfo(pngOf(1800, 1200)).width == 1800 && imageInfo(pngOf(1800, 1200)).height == 1200);
    {
        std::string gif = "GIF89a";
        gif += std::string("\x40\x01\xf0\x00", 4);  // 320 x 240
        CHECK(imageInfo(gif).ok && imageInfo(gif).width == 320 && imageInfo(gif).height == 240 && imageInfo(gif).format == "gif");
        // JPEG: SOI, an APP0 segment, then SOF0 with 600 x 400.
        std::string jpg("\xFF\xD8\xFF\xE0\x00\x10JFIF\0\x01\x01\0\0\x01\0\x01\0\0", 20);
        jpg += std::string("\xFF\xC0\x00\x11\x08\x01\x90\x02\x58\x03\x01\x22\x00\x02\x11\x01\x03\x11\x01", 19);
        CHECK(imageInfo(jpg).ok && imageInfo(jpg).width == 600 && imageInfo(jpg).height == 400 && imageInfo(jpg).format == "jpeg");
        std::string webp = "RIFF";
        webp += std::string("\0\0\0\0WEBPVP8X\x0a\0\0\0\0\0\0\0", 20);
        webp += std::string("\x1f\x03\x00\xdf\x01\x00", 6);  // 800 x 480 (stored minus one)
        CHECK(imageInfo(webp).ok && imageInfo(webp).width == 800 && imageInfo(webp).height == 480);
        CHECK(!imageInfo("not a picture at all").ok);
        CHECK(!imageInfo(std::string("\x89PNG", 4)).ok);   // truncated
    }
    // Print maths.
    {
        CHECK(std::abs(textWidthInches("6x9in") - 4.75) < 1e-9);
        CHECK(std::string(trimSize("nonsense").id) == "6x9in");   // unknown: the default
        Placement p;
        auto full = printSize(1800, 1200, p, 4.75);
        CHECK(std::abs(full.widthIn - 4.75) < 1e-9 && std::abs(full.ppi - 1800 / 4.75) < 1e-6);
        CHECK(printQuality(full.ppi) == PrintQuality::Good);
        p.cropW = 0.5;                                  // half the picture at full width: half the resolution
        auto cropped = printSize(1800, 1200, p, 4.75);
        CHECK(std::abs(cropped.ppi - 900 / 4.75) < 1e-6 && printQuality(cropped.ppi) == PrintQuality::Soft);
        CHECK(std::abs(cropped.heightIn - 4.75 * 1200 / 900) < 1e-6);   // aspect of the cropped area
        p.width = 0.5;                                  // printed at half width: sharp again
        CHECK(printQuality(printSize(1800, 1200, p, 4.75).ppi) == PrintQuality::Good);
        CHECK(printQuality(printSize(400, 300, {}, 4.75).ppi) == PrintQuality::Poor);
        CHECK(std::abs(sharpWidthFraction(1800, 1, 4.75) - 1.0) < 1e-9);
        CHECK(std::abs(sharpWidthFraction(900, 1, 4.75) - 900 / 300.0 / 4.75) < 1e-9);
        Placement wild;
        wild.cropX = 0.9; wild.cropW = 0.5; wild.width = 7; wild.align = "sideways";
        wild = clampPlacement(wild);
        CHECK(std::abs(wild.cropX - 0.5) < 1e-9 && wild.width == 1 && wild.align == "center");
        CHECK(formatLength(4.75, Unit::Inches) == "4.75 in" && formatLength(1, Unit::Centimetres) == "2.5 cm" &&
              formatLength(1, Unit::Millimetres) == "25 mm" && formatLength(2, Unit::Pixels, 300) == "600 px");
        CHECK(unitFrom("mm") == Unit::Millimetres && unitFrom("?") == Unit::Inches);
    }
    // The text keeps placement, cleaned up.
    {
        const std::string blob(64, 'a');
        const std::string in = R"({"type":"doc","content":[{"type":"image","attrs":{"blob":")" + blob +
                               R"(","alt":"A map","cropX":0.1,"cropY":0,"cropW":0.5,"cropH":2,"width":0.6,"align":"left","evil":"x"}}]})";
        auto out = nlohmann::json::parse(doc::sanitise(in));
        auto& a = out["content"][0]["attrs"];
        CHECK(a["cropX"] == 0.1 && a["cropW"] == 0.5 && a["cropH"] == 1 && a["width"] == 0.6 && a["align"] == "left");
        CHECK(!a.contains("evil"));
        const std::string plain = R"({"type":"doc","content":[{"type":"image","attrs":{"blob":")" + blob + R"("}}]})";
        auto out2 = nlohmann::json::parse(doc::sanitise(plain));
        CHECK(!out2["content"][0]["attrs"].contains("cropX") && !out2["content"][0]["attrs"].contains("width"));
    }
    // The artwork library.
    const std::string dir = tempLibrary("artwork");
    std::string hash;
    Id map;
    {
        auto lib = openOk(dir);
        map = lib->addArtwork(pngOf(2400, 1600), "Map of Alvora", "alvora-map.png");
        CHECK(!map.empty());
        const Artwork* a = lib->artwork(map);
        CHECK(a && a->width == 2400 && a->height == 1600 && a->format == "png" && a->name == "Map of Alvora");
        hash = a ? a->blob : "";
        CHECK(lib->addArtwork(pngOf(2400, 1600), "Same picture again") == map);   // stored and listed once
        CHECK(lib->liveArtworks().size() == 1);
        CHECK(lib->addArtwork("plain text", "not a picture").empty());
        Artwork e = *lib->artwork(map);
        e.kind = "map";
        e.notes = "Drawn for book one";
        CHECK(lib->updateArtwork(e));
        // Used in a chapter.
        const Id book = lib->createBook("Dragonfall", {});
        const Id ch = lib->createNode(book, {}, NodeKind::Chapter);
        CHECK(lib->saveNodeContent(ch, R"({"type":"doc","content":[{"type":"image","attrs":{"blob":")" + hash + R"("}}]})"));
        CHECK(lib->artworkUsage(hash).size() == 1);
        // Trash and restore.
        CHECK(lib->trashArtwork(map) && lib->liveArtworks().empty());
        CHECK(lib->restore(map) && lib->liveArtworks().size() == 1);
        Book b = *lib->book(book);
        b.trim = "a5";
        CHECK(lib->updateBook(b));
    }
    {
        auto lib = openOk(dir);   // everything was saved
        const Artwork* a = lib->artwork(map);
        CHECK(a && a->kind == "map" && a->notes == "Drawn for book one" && !a->deleted);
        CHECK(lib->liveBooks().size() == 1 && lib->liveBooks()[0]->trim == "a5");
        CHECK(lib->trashArtwork(map) && lib->emptyTrash() && lib->artworks().empty());
        CHECK(fs::exists(lib->blobPath(hash)));   // the chapter still shows it: the file stays
    }
    fs::remove_all(dir);

    // A schema-2 library with a picture in its text: upgrading lists the picture.
    const std::string old = tempLibrary("artwork-upgrade");
    {
        auto lib = openOk(old);
        hash = lib->storeBlob(pngOf(640, 480));
        const Id book = lib->createBook("Old", {});
        const Id ch = lib->createNode(book, {}, NodeKind::Chapter);
        lib->saveNodeContent(ch, R"({"type":"doc","content":[{"type":"image","attrs":{"blob":")" + hash + R"("}}]})");
    }
    {
        sqlite3* db = nullptr;
        sqlite3_open((fs::path(old) / "library.db").string().c_str(), &db);
        sqlite3_exec(db, "DROP TABLE artworks; UPDATE meta SET value = '2' WHERE key = 'schema_version';", nullptr, nullptr, nullptr);
        sqlite3_close(db);
    }
    {
        auto lib = openOk(old);
        CHECK(lib && lib->schemaVersion() == studio::Library::kSchemaVersion);
        CHECK(lib && lib->liveArtworks().size() == 1 && lib->artworkForBlob(hash) && lib->artworkForBlob(hash)->width == 640);
    }
    fs::remove_all(old);
}

static std::string appFolder(const char* sub) {
    for (const char* base : {"apps/studio/", "../apps/studio/", "../../apps/studio/"})
        if (fs::exists(std::string(base) + sub)) return std::string(base) + sub;
    return std::string("apps/studio/") + sub;
}

static void testTypeset() {
    using namespace studio::typeset;
    // Hyphenation (Liang's algorithm, British English patterns).
    Hyphenator h;
    CHECK(h.load(appFolder("hyphenation") + "/hyph-en-gb.pat.txt", appFolder("hyphenation") + "/hyph-en-gb.hyp.txt"));
    auto pts = h.points(utf32("university"));                    // from the exceptions list: uni-ver-sity
    CHECK(pts.size() == 2 && pts[0] == 3 && pts[1] == 6);
    pts = h.points(utf32("extraordinarily"));
    CHECK(!pts.empty());
    for (int p : pts) CHECK(p >= 2 && p <= 15 - 3);              // never closer to the ends than 2 and 3 letters
    CHECK(h.points(utf32("cat")).empty());                       // too short
    CHECK(h.points(utf32("abc123def")).empty());                 // not a word
    CHECK(utf8(utf32("Caf\u00e9 \u2014 na\u00efve")) == "Caf\u00e9 \u2014 na\u00efve");

    // A book.
    const std::string dir = tempLibrary("typeset");
    auto lib = openOk(dir);
    const Id book = lib->createBook("The Ledger", {});
    studio::Book bk = *lib->book(book);
    bk.author = "Ada Example";
    lib->updateBook(bk);
    const std::string para = R"({"type":"paragraph","content":[{"type":"text","text":"The harbour master kept a ledger of every vessel that had ever sheltered behind the breakwater, and on winter evenings, when the lamps were trimmed and the tide was running out across the flats, he would read the entries aloud to anyone who cared to listen."}]})";
    std::string doc = R"({"type":"doc","content":[)";
    for (int i = 0; i < 30; ++i) doc += (i ? "," : "") + para;
    doc += "]}";
    const Id c1 = lib->createNode(book, {}, NodeKind::Chapter, "One");
    const Id c2 = lib->createNode(book, {}, NodeKind::Chapter, "Two");
    lib->saveNodeContent(c1, doc);
    lib->saveNodeContent(c2, doc);
    const Resources res{appFolder("fonts"), appFolder("hyphenation")};
    auto tb = typesetBook(*lib, book, preset("classic").design, res);
    CHECK(tb.ok);
    if (!tb.ok) { std::printf("  typeset error: %s\n", tb.error.c_str()); return; }
    CHECK(std::abs(tb.width - 6 * 72) < 0.01 && std::abs(tb.height - 9 * 72) < 0.01);   // the default trim
    CHECK(tb.pages.size() >= 6);
    CHECK(tb.pages[0].kind == Page::Title && tb.pages[1].kind == Page::Copyright && tb.pages[2].kind == Page::Contents);
    // The contents lists both chapters with the pages they open on.
    {
        int firstOpening = -1;
        for (size_t i = 0; i < tb.pages.size(); ++i) if (tb.pages[i].kind == Page::ChapterOpening) { firstOpening = (int)i; break; }
        const std::string want = firstOpening >= 0 ? tb.pages[firstOpening].folio : "?";
        CHECK(want == "1");                              // the story starts at page 1 (front matter is roman)
        CHECK(tb.pages[2].folio == "iii");                // the contents page
        std::string digits;   // the contents page's text, from its glyphs' recorded characters
        for (auto& r : tb.pages[2].runs) for (auto g : r.glyphs) digits += tb.fonts[r.font].toUnicode[g];
        CHECK(firstOpening > 2 && digits.find("One") != std::string::npos && digits.find(want) != std::string::npos);
    }
    // No author, edition or ISBN: no copyright page, just a blank back to the title page.
    {
        Design plainD = preset("classic").design;
        plainD.contents = false;
        auto src = snapshot(*lib, book);
        src.author.clear();
        auto tbp = typesetBook(src, plainD, res);
        CHECK(tbp.ok && tbp.pages[1].kind == Page::Blank && tbp.pages[2].kind == Page::ChapterOpening);
    }
    int openings = 0, withHeads = 0, text = 0;
    for (auto& p : tb.pages) {
        if (p.kind == Page::ChapterOpening) ++openings;
        if (p.kind == Page::Text) ++text;
    }
    CHECK(openings == 2);
    CHECK(text >= 3);
    // Justified lines reach the right margin: most lines on text pages end within a point of it.
    const auto& d = tb.design;
    int lines = 0, full = 0;
    for (size_t pi = 0; pi < tb.pages.size(); ++pi) {
        const Page& p = tb.pages[pi];
        if (p.kind != Page::Text) continue;
        const bool recto = (pi + 1) % 2 == 1;
        const double right = tb.width - (recto ? d.marginOutside : d.marginInside) * 72;
        std::map<int, double> ends;   // baseline -> rightmost glyph end
        for (auto& r : p.runs) {
            if (r.glyphs.empty() || r.y < d.marginTop * 72 || r.y > tb.height - d.marginBottom * 72) continue;   // not heads or folios
            ends[(int)std::lround(r.y)] = std::max(ends[(int)std::lround(r.y)], (double)r.xs.back());
        }
        if (!ends.empty()) ++withHeads;
        for (auto& [y, x] : ends) { ++lines; if (right - x < 12) ++full; }   // last glyph starts within 12 pt of the margin
    }
    
    CHECK(lines > 50 && full >= lines * 0.7);   // each 4-line paragraph has one short last line
    // Running heads on text pages only: something above the text block.
    bool headOnText = false, headOnOpening = false;
    for (auto& p : tb.pages)
        for (auto& r : p.runs)
            if (r.y < d.marginTop * 72) { if (p.kind == Page::Text) headOnText = true; if (p.kind == Page::ChapterOpening) headOnOpening = true; }
    CHECK(headOnText && !headOnOpening);
    // Every glyph used can be copied as text.
    size_t mapped = 0, glyphs = 0;
    for (auto& p : tb.pages) for (auto& r : p.runs) for (auto g : r.glyphs) { ++glyphs; if (tb.fonts[r.font].toUnicode.count(g)) ++mapped; }
    CHECK(glyphs > 1000 && mapped == glyphs);
    // A comic script: pages, panels and balloons numbered; each comic page on a new page.
    {
        const Id cb = lib->createBook("Bone Boys #1", {});
        const Id issue = lib->createNode(cb, {}, NodeKind::Chapter, "Issue One");
        auto para = [](const char* style, const char* text) {
            return std::string(R"({"type":"paragraph","attrs":{"style":")") + style + R"("},"content":[{"type":"text","text":")" + text + R"("}]})";
        };
        const std::string docs = std::string(R"({"type":"doc","content":[)") +
            para("page", "") + "," + para("panel", "Wide shot. A cave mouth under a red moon.") + "," +
            para("caption", "Akunda. The dry season.") + "," + para("dialogue", "TOM: Hey mate, how's life?") + "," +
            para("panel", "Close on Snarf.") + "," + para("dialogue", "Snarf (whisper): Terrible. How's death?") + "," +
            para("sfx", "CLATTER") + "," + para("page", "") + "," + para("panel", "The skeletons, loud.") + "," +
            para("dialogue", "SKELETON: OI!") + "]}";
        CHECK(lib->saveNodeContent(issue, docs));
        auto ts = typesetBook(*lib, cb, preset("comic-script").design, res);
        CHECK(ts.error.empty());
        std::vector<std::string> pageText;
        for (auto& p : ts.pages) {
            std::string t;
            for (auto& r : p.runs) for (auto g : r.glyphs) t += ts.fonts[r.font].toUnicode[g];
            std::string squeezed;   // spaces (and the balloon numbers' no-break spaces) left out
            for (size_t k = 0; k < t.size(); ++k) {
                if (t[k] == ' ') continue;
                if ((unsigned char)t[k] == 0xC2 && k + 1 < t.size() && (unsigned char)t[k + 1] == 0xA0) { ++k; continue; }
                squeezed += t[k];
            }
            pageText.push_back(squeezed);
        }
        int first = -1, second = -1;
        for (size_t i = 0; i < pageText.size(); ++i) {
            if (pageText[i].find("PAGEONE") != std::string::npos) first = (int)i;
            if (pageText[i].find("PAGETWO") != std::string::npos) second = (int)i;
        }
        CHECK(first >= 0 && second > first);
        if (first >= 0) {
            const std::string& t = pageText[first];
            CHECK(t.find("(right,twopanels)") != std::string::npos);                  // page side and panel count
            CHECK(t.find("Panel1") != std::string::npos && t.find("Panel2") != std::string::npos);
            CHECK(t.find("1.CAPTION:Akunda") != std::string::npos);                    // balloons numbered
            CHECK(t.find("2.TOM:Hey") != std::string::npos);
            CHECK(t.find("3.SNARF(WHISPER):Terrible") != std::string::npos);
            CHECK(t.find("SFX:CLATTER") != std::string::npos && t.find("4.SFX") == std::string::npos);
        }
        if (second >= 0) CHECK(pageText[second].find("(left,onepanel)") != std::string::npos && pageText[second].find("1.SKELETON:OI!") != std::string::npos);
        bool courier = false;
        for (auto& f : ts.fonts) if (f.path.find("courier-prime") != std::string::npos) courier = true;
        CHECK(courier);
    }
    // Symbols the book font lacks (point-of-view marks) come from the symbol fonts, not as empty boxes.
    {
        const Id sb = lib->createBook("Marks", {});
        const Id sc = lib->createNode(sb, {}, NodeKind::Chapter, "2. \u2727 The Canyon");
        CHECK(lib->saveNodeContent(sc, R"({"type":"doc","content":[{"type":"paragraph","content":[{"type":"text","text":"\ud83d\udde1 Tom rode on. \u2727 Sarrah watched."}]}]})"));
        auto ts = typesetBook(*lib, sb, preset("classic").design, res);
        CHECK(ts.error.empty());
        std::string fromSymbols;
        bool notdef = false;
        for (auto& p : ts.pages)
            for (auto& r : p.runs) {
                for (auto g : r.glyphs) if (g == 0) notdef = true;
                if (ts.fonts[r.font].path.find("symbols") != std::string::npos)
                    for (auto g : r.glyphs) fromSymbols += ts.fonts[r.font].toUnicode[g];
            }
        CHECK(!notdef);
        CHECK(fromSymbols.find("\u2727") != std::string::npos && fromSymbols.find("\U0001F5E1") != std::string::npos);
        std::string err;
        CHECK(writePdf(ts, dir + "/marks.pdf", err));
    }
    // Pixels and PDF.
    int textPage = 0;
    for (size_t i = 0; i < tb.pages.size(); ++i) if (tb.pages[i].kind == Page::Text) { textPage = (int)i; break; }
    auto bm = renderPage(tb, textPage, 72);
    CHECK(bm.width == 432 && bm.height == 648);
    size_t dark = 0;
    for (size_t i = 0; i < bm.rgba.size(); i += 4) if (bm.rgba[i] < 128) ++dark;
    CHECK(dark > 500);
    std::string err;
    const std::string pdfPath = (fs::path(dir) / "book.pdf").string();
    CHECK(writePdf(tb, pdfPath, err));
    std::ifstream f(pdfPath, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();   // Windows won't delete an open file
    CHECK(bytes.rfind("%PDF-1.7", 0) == 0 && bytes.find("/FontFile2") != std::string::npos && bytes.find("/ToUnicode") != std::string::npos);
    CHECK(bytes.size() > 7 && bytes.compare(bytes.size() - 6, 6, "%%EOF\n") == 0);
    // Chapter styles: "Chapter One" above the title, a numeral, a drop cap.
    {
        Design dn = preset("classic").design;
        dn.chapterStyle = "number";
        auto tn = typesetBook(*lib, book, dn, res);
        std::string opening;
        for (auto& p : tn.pages)
            if (p.kind == Page::ChapterOpening) { for (auto& r : p.runs) for (auto g : r.glyphs) opening += tn.fonts[r.font].toUnicode[g]; break; }
        CHECK(opening.find("CHAPTER") != std::string::npos || opening.find("Chapter") != std::string::npos);
        Design dc = preset("airy").design;
        CHECK(dc.dropCap && dc.chapterStyle == "numeral");
        auto tc = typesetBook(*lib, book, dc, res);
        float biggest = 0;
        for (auto& p : tc.pages) if (p.kind == Page::ChapterOpening) for (auto& r : p.runs) biggest = std::max(biggest, r.size);
        CHECK(tc.ok && biggest > dc.fontSize * 2.5);     // the numeral and the drop cap are large
        Design dr = dc;
        dr.frontRoman = false;
        auto tr = typesetBook(*lib, book, dr, res);
        CHECK(tr.ok && tr.pages[2].folio.empty());       // arabic throughout: no number printed on the contents page
    }
    // Print-ready PDF/X-1a: PDF 1.3, black text, no RGB, an output intent, trim and bleed boxes.
    {
        std::string err;
        PdfOptions xo;
        xo.printReady = true;
        const std::string x = pdfBytes(tb, err, xo);
        CHECK(x.rfind("%PDF-1.3", 0) == 0);
        CHECK(x.find("/GTS_PDFXVersion (PDF/X-1:2001)") != std::string::npos && x.find("/GTS_PDFXConformance (PDF/X-1a:2001)") != std::string::npos);
        CHECK(x.find("/OutputIntents") != std::string::npos && x.find("/S /GTS_PDFX") != std::string::npos);
        CHECK(x.find("/Trapped /False") != std::string::npos && x.find("/ID [<") != std::string::npos && x.find("/ModDate") != std::string::npos);
        CHECK(x.find("DeviceRGB") == std::string::npos);
        CHECK(x.find("/TrimBox [0 0 432 648]") != std::string::npos);
        xo.bleed = true;
        const std::string xb = pdfBytes(tb, err, xo);
        CHECK(xb.find("/MediaBox [0 0 441 666]") != std::string::npos && xb.find("/BleedBox") != std::string::npos);
        CHECK(xb.find("/TrimBox [0 9 432 657]") != std::string::npos && xb.find("/TrimBox [9 9 441 657]") != std::string::npos);  // right- and left-hand pages
        // The ordinary PDF is unchanged: PDF 1.7, no PDF/X keys.
        const std::string plainPdf = pdfBytes(tb, err);
        CHECK(plainPdf.rfind("%PDF-1.7", 0) == 0 && plainPdf.find("GTS_PDFX") == std::string::npos);
    }
    // Pre-flight.
    {
        const Source src = snapshot(*lib, book);
        auto has = [](const std::vector<publish::Check>& cs, publish::Check::Level l, const std::string& t) {
            for (auto& c : cs) if (c.level == l && c.title.find(t) != std::string::npos) return true;
            return false;
        };
        auto cs = publish::preflight(src, &tb);
        CHECK(has(cs, publish::Check::Advice, "No ISBN") && has(cs, publish::Check::Advice, "No cover"));
        CHECK(has(cs, publish::Check::Ok, "Fonts embedded"));
        Source bad = src;
        bad.isbn = "978-0-306-40615-8";
        CHECK(has(publish::preflight(bad, nullptr), publish::Check::Problem, "ISBN"));
        bad.isbn = "978-0-306-40615-7";
        CHECK(has(publish::preflight(bad, nullptr), publish::Check::Ok, "ISBN"));
    }
    // Editions: a paperback and an ebook to start with, each with its own ISBN.
    {
        lib->setSetting("publish:" + book, R"({"paper":1,"bleed":true})");
        CHECK(lib->editionsOf(book).empty() && lib->ensureEditions(book));
        auto eds = lib->editionsOf(book);
        CHECK(eds.size() == 2 && eds[0]->kind == "paperback" && eds[1]->kind == "ebook");
        CHECK(eds[0]->paper == "cream" && eds[0]->bleed && !eds[1]->print());   // the earlier publishing settings carried over
        CHECK(lib->ensureEditions(book) && lib->editionsOf(book).size() == 2);   // only once
        studio::Edition pb = *eds[0], eb = *eds[1];
        pb.isbn = "978-0-306-40615-7";
        eb.isbn = "978-1-86197-876-9";
        CHECK(lib->updateEdition(pb) && lib->updateEdition(eb));
        const Id lp = lib->addEdition(book, "large-print");
        CHECK(!lp.empty() && lib->edition(lp)->design == "large-print");
        const Source ps = publish::editionSource(*lib, *lib->edition(pb.id));
        CHECK(ps.isbn == pb.isbn && ps.editionKind == "paperback" && ps.isbns.size() == 2);
        CHECK(publish::editionDesign(*lib, *lib->edition(lp)).fontSize > preset("classic").design.fontSize + 2);
        // The copyright page lists both ISBNs.
        auto te = typesetBook(ps, publish::editionDesign(*lib, *lib->edition(pb.id)), res);
        std::string notice;
        for (auto& p : te.pages)
            if (p.kind == Page::Copyright) for (auto& r : p.runs) for (auto g : r.glyphs) notice += te.fonts[r.font].toUnicode[g];
        CHECK(notice.find("(paperback)") != std::string::npos && notice.find("(ebook)") != std::string::npos);
        // An ebook gets no print checks; two editions can't share an ISBN.
        const Source es = publish::editionSource(*lib, *lib->edition(eb.id));
        auto has = [](const std::vector<publish::Check>& cs, publish::Check::Level l, const std::string& t) {
            for (auto& c : cs) if (c.level == l && c.title.find(t) != std::string::npos) return true;
            return false;
        };
        CHECK(!has(publish::preflight(es, nullptr), publish::Check::Problem, "same ISBN"));
        studio::Edition dup = *lib->edition(lp);
        dup.isbn = pb.isbn;
        CHECK(lib->updateEdition(dup));
        CHECK(has(publish::preflight(publish::editionSource(*lib, *lib->edition(lp)), nullptr), publish::Check::Problem, "same ISBN"));
        CHECK(lib->removeEdition(lp) && lib->editionsOf(book).size() == 2 && !lib->edition(lp));
        // They're kept in the library.
        CHECK(lib->schemaVersion() == studio::Library::kSchemaVersion);
        lib.reset();
        lib = openOk(dir);
        CHECK(lib->editionsOf(book).size() == 2 && lib->edition(eb.id) && lib->edition(eb.id)->isbn == eb.isbn);
    }
    // Another trim size changes the page.
    Design a5 = preset("classic").design;
    a5.trim = "a5";
    auto tb2 = typesetBook(*lib, book, a5, res);
    CHECK(tb2.ok && std::abs(tb2.width - 148 / 25.4 * 72) < 0.01);
    // Missing fonts: a clear error, not a crash.
    auto tb3 = typesetBook(*lib, book, a5, Resources{dir + "/nowhere", dir + "/nowhere"});
    CHECK(!tb3.ok && !tb3.error.empty());
    lib.reset();
    fs::remove_all(dir);
}

// PDF-style lines (headings found from type size): the title page stays front
// matter, and "CHAPTER ONE" joins the title under it.
static void testPdfLikeHeadings() {
    using studio::import::Line;
    std::vector<Line> lines;
    auto add = [&](const std::string& t, bool heading, bool starts = true) {
        Line l;
        l.text = t;
        l.heading = heading;
        l.headingLevel = heading ? 1 : 0;
        l.startsParagraph = starts;
        lines.push_back(l);
    };
    add("The Lighthouse Keeper", true);
    add("Ada Example", true);
    add("CHAPTER ONE", true);
    add("The Lighthouse", true);
    add("The lamp had burned for a hundred years.", false);
    add("CHAPTER TWO", true);
    add("Strangers", true);
    add("Nobody came in winter.", false);
    studio::import::Plan plan;
    studio::import::detail::buildFromLines(lines, plan, false, "Fallback");
    CHECK(plan.sections.size() == 3);
    if (plan.sections.size() == 3) {
        CHECK(plan.sections[0].role == studio::import::Role::TitlePage && !plan.sections[0].include);
        CHECK(plan.sections[1].title == "CHAPTER ONE: The Lighthouse" && plan.sections[1].include);
        CHECK(plan.sections[2].title == "CHAPTER TWO: Strangers");
    }
    // Without any "Chapter" lines, large-type headings are the chapters themselves.
    std::vector<Line> plain;
    for (const char* t : {"The Storm", "Rain came.", "The Calm", "It passed."}) {
        Line l;
        l.text = t;
        l.heading = t[0] == 'T';
        l.startsParagraph = true;
        plain.push_back(l);
    }
    studio::import::Plan p2;
    studio::import::detail::buildFromLines(plain, p2, false, "Fallback");
    int chapters = 0;
    for (auto& s : p2.sections) if (s.role == studio::import::Role::Chapter) ++chapters;
    CHECK(chapters == 2);
}

static void testEbookHtml() {
    using namespace studio;
    typeset::Source src;
    src.title = "Harbour";
    typeset::Source::Section part;
    part.kind = NodeKind::Part;
    part.title = "Part One";
    part.content = R"({"type":"doc","content":[]})";
    src.sections.push_back(part);
    typeset::Source::Section ch;
    ch.title = "Tom & Jerry <1>";
    const std::string blob(64, 'b');
    ch.content = R"J({"type":"doc","content":[
        {"type":"paragraph","content":[{"type":"text","text":"Once "},{"type":"text","marks":[{"type":"em"}],"text":"upon"},{"type":"text","marks":[{"type":"smallcaps"}],"text":" a time"}]},
        {"type":"paragraph","content":[{"type":"text","text":"a < b & \"c\""}]},
        {"type":"scene_break"},
        {"type":"paragraph","content":[{"type":"text","marks":[{"type":"link","attrs":{"href":"javascript:alert(1)"}}],"text":"bad"},{"type":"text","marks":[{"type":"link","attrs":{"href":"https://example.com"}}],"text":"good"}]},
        {"type":"image","attrs":{"blob":")J" + blob + R"J(","alt":"A map","caption":"The bay","cropX":0.1,"cropW":0.5,"width":0.6}},
        {"type":"paragraph","attrs":{"style":"verse"},"content":[{"type":"text","text":"line one"},{"type":"hard_break"},{"type":"text","text":"line two"}]},
        {"type":"paragraph"}]})J";
    src.sections.push_back(ch);
    typeset::Source::Section scene;
    scene.kind = NodeKind::Scene;
    scene.content = R"({"type":"doc","content":[{"type":"paragraph","content":[{"type":"text","text":"Later."}]}]})";
    src.sections.push_back(scene);
    auto c = ebook::build(src, [](const std::string& h) { return "img/" + h.substr(0, 4); });
    CHECK(c.chapters.size() == 2);                                   // the scene page joins its chapter
    if (c.chapters.size() != 2) return;
    CHECK(c.chapters[0].part && c.chapters[0].html.find("<section class=\"part\">") == 0);
    const std::string& h = c.chapters[1].html;
    CHECK(h.find("<h1>Tom &amp; Jerry &lt;1&gt;</h1>") != std::string::npos);
    CHECK(h.find("<p class=\"first\">Once <em>upon</em><span class=\"sc\"> a time</span></p>") != std::string::npos);  // first after the title
    CHECK(h.find("a &lt; b &amp; &quot;c&quot;") != std::string::npos);
    CHECK(h.find("<hr class=\"scene\"/>") != std::string::npos);
    CHECK(h.find("javascript") == std::string::npos && h.find("<a href=\"https://example.com\">good</a>") != std::string::npos);
    CHECK(h.find("style=\"width:60%\"") != std::string::npos && h.find("data-crop=\"0.1 0 0.5 1\"") != std::string::npos);
    CHECK(h.find("src=\"img/bbbb\" alt=\"A map\"") != std::string::npos && h.find("<figcaption>The bay</figcaption>") != std::string::npos);
    CHECK(h.find("line one<br/>line two") != std::string::npos);
    CHECK(h.find("empty") == std::string::npos);                     // the editor's empty last line is dropped
    CHECK(h.find("Later.") != std::string::npos);
    CHECK(ebook::stylesheet().find("hr.scene") != std::string::npos);
}

// EPUB: the container, the package, the contents, pictures cropped ahead of time.
static std::string unzipped(const std::string& zip, const char* name) {
    mz_zip_archive za{};
    if (!mz_zip_reader_init_mem(&za, zip.data(), zip.size(), 0)) return {};
    size_t n = 0;
    void* d = mz_zip_reader_extract_file_to_heap(&za, name, &n, 0);
    std::string out = d ? std::string(static_cast<const char*>(d), n) : std::string();
    mz_free(d);
    mz_zip_reader_end(&za);
    return out;
}

static void testPublish() {
    using namespace studio;
    CHECK(publish::validIsbn("978-0-306-40615-7") && !publish::validIsbn("978-0-306-40615-8"));
    CHECK(publish::validIsbn("0-306-40615-2") && !publish::validIsbn("0-306-40615-3") && !publish::validIsbn("hello"));
    CHECK(std::abs(publish::spineInches(300, publish::Paper::White) - 0.6756) < 0.001);
    CHECK(std::abs(publish::spineInches(300, publish::Paper::Cream) - 0.75) < 0.001);

    const std::string dir = (fs::temp_directory_path() / "studio-publish-test").string();
    fs::remove_all(dir);
    fs::create_directories(dir);
    // A real 200 x 100 picture: left half red, right half blue.
    std::vector<unsigned char> px(200 * 100 * 3);
    for (int y = 0; y < 100; ++y) for (int x = 0; x < 200; ++x) { auto* p = &px[(y * 200 + x) * 3]; p[0] = x < 100 ? 220 : 10; p[1] = 20; p[2] = x < 100 ? 10 : 220; }
    size_t pngLen = 0;
    void* png = tdefl_write_image_to_png_file_in_memory(px.data(), 200, 100, 3, &pngLen);
    const std::string blob(64, 'a');
    { std::ofstream(dir + "/pic.png", std::ios::binary).write(static_cast<const char*>(png), (std::streamsize)pngLen); }
    { std::ofstream(dir + "/cover.png", std::ios::binary).write(static_cast<const char*>(png), (std::streamsize)pngLen); }
    mz_free(png);

    typeset::Source src;
    src.id = "book-1";
    src.title = "Harbour & Home";
    src.subtitle = "A Novel";
    src.author = "Ada Example";
    src.language = "en-GB";
    src.isbn = "978-0-306-40615-7";
    src.synopsis = "A harbour master's ledger.";
    src.coverPath = dir + "/cover.png";
    src.picturePaths[blob] = dir + "/pic.png";
    src.pictureSizes[blob] = {200, 100};
    typeset::Source::Section part;
    part.kind = NodeKind::Part;
    part.title = "Part One";
    part.content = R"({"type":"doc","content":[]})";
    src.sections.push_back(part);
    typeset::Source::Section ch;
    ch.title = "The Ledger";
    ch.words = 5;
    ch.content = R"J({"type":"doc","content":[{"type":"paragraph","content":[{"type":"text","text":"It began \u0007at dusk."}]},{"type":"scene_break"},
        {"type":"image","attrs":{"blob":")J" + blob + R"J(","alt":"The red half","cropX":0,"cropW":0.5}},
        {"type":"image","attrs":{"blob":")J" + blob + R"J(","alt":"All of it"}}]})J";
    src.sections.push_back(ch);
    typeset::Source::Section ch2 = ch;
    ch2.title = "Low Tide";
    src.sections.push_back(ch2);

    publish::EpubOptions o;
    o.modified = "2026-11-13T09:30:00Z";
    const auto r = publish::buildEpub(src, o);
    CHECK(r.ok && r.chapters == 3 && r.pictures == 2);   // the same crop is one file, used twice
    const std::string& z = r.bytes;
    CHECK(z.size() > 60 && z.compare(0, 4, "PK\x03\x04") == 0 && z.compare(30, 8, "mimetype") == 0 && z.compare(38, 20, "application/epub+zip") == 0);
    CHECK(unzipped(z, "META-INF/container.xml").find("OEBPS/content.opf") != std::string::npos);
    const std::string opf = unzipped(z, "OEBPS/content.opf");
    CHECK(opf.find("<dc:identifier id=\"bookid\">urn:isbn:9780306406157</dc:identifier>") != std::string::npos);
    CHECK(opf.find("<dc:title id=\"title\">Harbour &amp; Home</dc:title>") != std::string::npos);
    CHECK(opf.find("<meta property=\"dcterms:modified\">2026-11-13T09:30:00Z</meta>") != std::string::npos);
    CHECK(opf.find("properties=\"nav\"") != std::string::npos && opf.find("properties=\"cover-image\"") != std::string::npos);
    CHECK(opf.find("schema:accessibilityFeature\">alternativeText") != std::string::npos);
    CHECK(opf.find("<spine toc=\"ncx\">") != std::string::npos && opf.find("<itemref idref=\"c3\"/>") != std::string::npos);
    const std::string nav = unzipped(z, "OEBPS/nav.xhtml");
    CHECK(nav.find("<li><a href=\"chapter-001.xhtml\">Part One</a><ol>") != std::string::npos);   // chapters inside their part
    CHECK(nav.find("epub:type=\"bodymatter\" href=\"chapter-002.xhtml\"") != std::string::npos);
    CHECK(unzipped(z, "OEBPS/toc.ncx").find("<navPoint id=\"n3\" playOrder=\"3\">") != std::string::npos);
    const std::string c2 = unzipped(z, "OEBPS/chapter-002.xhtml");
    CHECK(c2.rfind("<?xml", 0) == 0 && c2.find("epub:type=\"chapter\"") != std::string::npos);
    CHECK(c2.find("data-crop") == std::string::npos && c2.find("role=\"separator\"") != std::string::npos);
    CHECK(c2.find('\x07') == std::string::npos);                     // no control characters in XML
    // The cropped picture is the left half, already cut.
    const size_t at = c2.find("src=\"images/");
    CHECK(at != std::string::npos);
    if (at != std::string::npos) {
        const std::string file = c2.substr(at + 5, c2.find('"', at + 5) - at - 5);
        const ImageInfo info = imageInfo(unzipped(z, ("OEBPS/" + file).c_str()));
        CHECK(info.ok && info.width == 100 && info.height == 100);
    }
    CHECK(!unzipped(z, "OEBPS/images/cover.png").empty() && !unzipped(z, "OEBPS/cover.xhtml").empty());
    // Without an ISBN: a stable identifier of its own.
    src.isbn.clear();
    const auto r2 = publish::buildEpub(src, o), r3 = publish::buildEpub(src, o);
    CHECK(unzipped(r2.bytes, "OEBPS/content.opf").find("urn:uuid:") != std::string::npos && r2.bytes == r3.bytes);
    // Writing it.
    CHECK(publish::writeEpub(src, dir + "/book.epub", o).ok && fs::file_size(dir + "/book.epub") == r2.bytes.size());
    typeset::Source none;
    CHECK(!publish::buildEpub(none).ok);
    // The cover fills the screen through an SVG of the picture's own size.
    CHECK(unzipped(z, "OEBPS/cover.xhtml").find("viewBox=\"0 0 200 100\"") != std::string::npos && opf.find("cover.xhtml\" media-type=\"application/xhtml+xml\" properties=\"svg\"") != std::string::npos);

    // Styled like the printed book: numbered chapter openings, lead-in, drop cap, the scene break, the typeface inside.
    {
        typeset::Design d;
        d.chapterStyle = "number";
        d.leadIn = true;
        d.dropCap = true;
        d.sceneBreak = "\u2042";
        d.justify = false;
        publish::EpubOptions so = o;
        so.style = ebook::styleFrom(d);
        so.fontsFolder = appFolder("fonts");
        so.fontId = "crimson-text";
        typeset::Source s2 = src;
        typeset::Source::Section pro;
        pro.title = "Prologue";
        pro.content = R"({"type":"doc","content":[{"type":"paragraph","content":[{"type":"text","text":"Before."}]}]})";
        s2.sections.insert(s2.sections.begin() + 1, pro);
        s2.sections[2].content = R"J({"type":"doc","content":[{"type":"paragraph","content":[{"type":"text","text":"\u201CWhere were you?\u201D she asked."}]},{"type":"scene_break"},{"type":"paragraph","content":[{"type":"text","text":"Later."}]}]})J";
        const auto e = publish::buildEpub(s2, so);
        CHECK(e.ok);
        const std::string pr = unzipped(e.bytes, "OEBPS/chapter-002.xhtml"), c1 = unzipped(e.bytes, "OEBPS/chapter-003.xhtml"),
                          c2b = unzipped(e.bytes, "OEBPS/chapter-004.xhtml");
        CHECK(pr.find("<h1>Prologue</h1>") != std::string::npos);                                    // never numbered
        CHECK(c1.find("<span class=\"chapnum\">Chapter One</span><span class=\"chaptitle\">The Ledger</span>") != std::string::npos);
        CHECK(c2b.find("<span class=\"chapnum\">Chapter Two</span>") != std::string::npos);
        CHECK(c1.find("<p class=\"first opening\"><span class=\"dropcap\">\u201CW</span><span class=\"leadin\">here were you?\u201D</span> she asked.") != std::string::npos);
        CHECK(c1.find(">\u2042</div>") != std::string::npos);
        const std::string css = unzipped(e.bytes, "OEBPS/style.css");
        CHECK(css.find("@font-face { font-family: \"NR Book\"; font-weight: normal; font-style: italic; src: url(\"fonts/CrimsonText-Italic.ttf\"); }") != std::string::npos);
        CHECK(css.find("font-family: \"NR Book\", serif") != std::string::npos && css.find("text-align: left") != std::string::npos);
        CHECK(css.find("content: \"\\2042 \"") != std::string::npos);
        CHECK(unzipped(e.bytes, "OEBPS/fonts/CrimsonText-Regular.ttf").size() > 10000 && !unzipped(e.bytes, "OEBPS/fonts/OFL.txt").empty());
        // A world's own typeface (0.40), found in the second of the font folders, with a licence that isn't the OFL.
        {
            const fs::path wf = fs::temp_directory_path() / ("studio-worldfont-" + newId().substr(0, 8));
            const fs::path srcf = wf / "src";
            fs::create_directories(srcf);
            fs::copy_file(fs::path(appFolder("fonts")) / "gentium-book-plus" / "GentiumBookPlus-Regular.ttf", srcf / "G.ttf");
            { std::ofstream(srcf / "LICENSE.txt") << "Licence words."; }
            const auto ir = studio::fonts::importFiles((wf / "mine").string(), {(srcf / "G.ttf").string()}, {"gentium-book-plus"});
            CHECK(ir.added.size() == 1);
            publish::EpubOptions wo = so;
            wo.fontsFolder = studio::fonts::joinFolders({appFolder("fonts"), (wf / "mine").string()});
            wo.fontId = "gentium-book-plus-2";
            const auto we = publish::buildEpub(s2, wo);
            CHECK(we.ok && unzipped(we.bytes, "OEBPS/fonts/LICENSE.txt") == "Licence words." && unzipped(we.bytes, "OEBPS/fonts/OFL.txt").empty());
            CHECK(unzipped(we.bytes, "OEBPS/style.css").find("-Regular.ttf") != std::string::npos);
            CHECK(unzipped(we.bytes, "OEBPS/content.opf").find("fonts/LICENSE.txt") != std::string::npos);
            fs::remove_all(wf);
        }
        const std::string opf2 = unzipped(e.bytes, "OEBPS/content.opf");
        CHECK(opf2.find("media-type=\"font/ttf\"") != std::string::npos);
        CHECK(unzipped(e.bytes, "OEBPS/copyright.xhtml").find("Set in Crimson Text") != std::string::npos);
        // Lead-in alone: the first three words.
        ebook::Style ls;
        ls.leadIn = true;
        CHECK(ebook::markOpening("<p class=\"first\">The wind came over the hill.</p>", ls) ==
              "<p class=\"first opening\"><span class=\"leadin\">The wind came</span> over the hill.</p>");
        ls.dropCap = true;
        CHECK(ebook::markOpening("<p class=\"first\">A wind came over.</p>", ls) ==
              "<p class=\"first opening\"><span class=\"leadin\">A wind came</span> over.</p>");   // a one-letter word: no drop cap
        CHECK(ebook::chapterHeading("Chapter 7: Rain", 7, "", "Rain", [] { ebook::Style n; n.chapterStyle = "numeral"; return n; }()) ==
              "<h1 class=\"numeral\"><span class=\"numeral\">7</span><span class=\"chaptitle\">Rain</span></h1>\n");
    }
    fs::remove_all(dir);
}

// Ebook covers drawn in the Studio.
static void testCovers() {
    using namespace studio;
    cover::Spec sp;
    sp.title = "The Glass Harbour";
    sp.subtitle = "A Novel";
    sp.author = "Ada Example";
    sp.series = "Book One of the Tides";
    for (const auto& l : cover::layouts()) {
        sp.layout = l.id;
        const auto r = cover::render(sp, appFolder("fonts"), 400, 640);
        CHECK(r.ok && r.width == 400 && r.height == 640);
        const ImageInfo info = imageInfo(r.png);
        CHECK(info.ok && info.format == "png" && info.width == 400 && info.height == 640);
        if (std::getenv("COVER_DUMP")) {
            cover::Spec dp = sp;
            if (std::getenv("COVER_PICTURE") && std::string(l.id) != "minimal") dp.picturePath = std::getenv("COVER_PICTURE");
            const auto big = cover::render(dp, appFolder("fonts"));
            std::ofstream(std::string(std::getenv("COVER_DUMP")) + "/" + l.id + ".png", std::ios::binary) << big.png;
        }
    }
    // Different words, a different picture.
    sp.layout = "classic";
    const auto a = cover::render(sp, appFolder("fonts"), 400, 640);
    sp.title = "Low Tide";
    const auto b = cover::render(sp, appFolder("fonts"), 400, 640);
    CHECK(a.ok && b.ok && a.png != b.png);
    // A picture that can't be read is an error, not a blank cover.
    sp.layout = "picture";
    sp.picturePath = "/nonexistent/picture.png";
    CHECK(!cover::render(sp, appFolder("fonts"), 400, 640).ok);
    // A print cover: back, spine and front, with bleed; a PDF with the trim marked.
    {
        cover::Spec ps;
        ps.title = "The Glass Harbour";
        ps.author = "Ada Example";
        ps.series = "Book One of the Tides";
        cover::Wrap wp;
        wp.trimW = 5;
        wp.trimH = 8;
        wp.spine = 0.6;
        wp.dpi = 80;
        wp.backText = "A harbour master keeps a ledger of every ship.\n\nOne night a ship comes in that isn't in it.";
        const auto png = cover::renderWrap(ps, wp, appFolder("fonts"), false);
        CHECK(png.ok && png.width == 868 && png.height == 660 && png.spineText);
        if (std::getenv("COVER_DUMP")) {
            cover::Wrap big = wp;
            big.dpi = 150;
            if (std::getenv("COVER_PICTURE")) { ps.layout = "picture"; ps.picturePath = std::getenv("COVER_PICTURE"); }
            std::ofstream(std::string(std::getenv("COVER_DUMP")) + "/wrap.png", std::ios::binary) << cover::renderWrap(ps, big, appFolder("fonts"), false).png;
            ps.layout = "classic";
            ps.picturePath.clear();
        }
        const auto pdf = cover::renderWrap(ps, wp, appFolder("fonts"), true);
        CHECK(pdf.ok && pdf.pdf.rfind("%PDF-1.4", 0) == 0 && pdf.pdf.find("/TrimBox [9.000 9.000 ") != std::string::npos);
        CHECK(pdf.pdf.find("/MediaBox [0 0 781.200 594.000]") != std::string::npos);
        wp.spine = 0.1;   // too thin for words
        CHECK(!cover::renderWrap(ps, wp, appFolder("fonts"), false).spineText);
    }
    // Kept as JSON.
    sp.background = "#102030";
    const cover::Spec back = cover::fromJson(cover::toJson(sp));
    CHECK(back.layout == "picture" && back.title == "Low Tide" && back.background == "#102030" && back.series == sp.series);
}

// Tidying a world's bible: copies merged, a book's entries moved in, kinds suggested.
static void testEntryPages() {
    using namespace studio;
    const std::string dir = tempLibrary("pages");
    Id world, book, ada, ada2;
    {
        auto lib = openOk(dir);
        world = lib->createWorld("Alvora");
        book = lib->createBook("Shards", world);
        ada = lib->createEntry(world, {}, "character", "Ada");
        CHECK(lib->entryTemplate(world, "character").size() == 4);
        CHECK(lib->entryTemplate(world, "character")[0] == "Appearance & personality");
        Entry e = *lib->entry(ada);
        e.setSection("Backstory & goals", "Raised by smiths.");
        e.setSection("Odd habits", "Hums.");
        e.gallery.push_back({"abc", "At sixteen"});
        e.gallery.push_back({"def", ""});
        e.setBookNote(book, "Hides the shard.");
        e.pronunciation = "AH-dah";
        e.setRevealedIn("section:Backstory & goals", book);
        e.setRevealedIn("detail:Age", book);
        e.setRevealedIn("detail:Age", "");
        CHECK(lib->updateEntry(e));
        // A world's own template: reordered, one renamed away.
        CHECK(lib->setEntryTemplate(world, "character", parseEntryTemplate("- Backstory & goals\n\nLooks\nBackstory & goals\n")));
        CHECK(lib->entryTemplate(world, "character").size() == 2);
        ada2 = lib->createEntry(world, {}, "character", "Ada Smith");
        Entry f = *lib->entry(ada2);
        f.setSection("Looks", "Tall.");
        f.setBookNote(book, "Wakes in chapter 2.");
        f.gallery.push_back({"abc", ""});
        f.gallery.push_back({"ghi", ""});
        f.mapBlob = "mapblob";
        f.pins = {{ada, 0.25, 0.75}, {ada2, 2.0, -1.0}};
        lib->updateEntry(f);
    }
    {
        auto lib = openOk(dir);   // read back from disk
        const Entry* e = lib->entry(ada);
        CHECK(e && e->section("Backstory & goals") == "Raised by smiths." && e->gallery.size() == 2 && e->gallery[0].caption == "At sixteen");
        CHECK(e && e->bookNote(book) == "Hides the shard.");
        CHECK(e && e->pronunciation == "AH-dah" && e->revealedIn("section:Backstory & goals") == book && e->revealedIn("detail:Age").empty());
        CHECK(e && e->revealed.size() == 1);
        const auto secs = lib->entrySections(*e);
        // Template first (Backstory & goals, Looks), then the extra one it has.
        CHECK(secs.size() == 3 && secs[0].first == "Backstory & goals" && secs[1].first == "Looks" && secs[2].first == "Odd habits");
        CHECK(lib->setEntryTemplate(world, "character", defaultEntryTemplate("character")));
        CHECK(lib->setting("entry-template:" + world + ":character").empty() && lib->entryTemplate(world, "character").size() == 4);
        CHECK(lib->mergeEntries(ada, ada2));
        const Entry* m = lib->entry(ada);
        CHECK(m && m->section("Looks") == "Tall." && m->gallery.size() == 3);
        CHECK(m && m->mapBlob == "mapblob" && m->pins.size() == 2 && m->pins[0].x == 0.25 && m->pins[1].x == 1.0 && m->pins[1].y == 0.0);
        CHECK(m && m->bookNote(book).find("Hides the shard.") != std::string::npos && m->bookNote(book).find("Wakes in chapter 2.") != std::string::npos);
        Entry n = *m;
        n.setBookNote(book, "");
        CHECK(n.bookNotes.empty());
    }
}

// A family tree as a picture (0.45), and in the bible's PDF.
// The program's own typefaces (0.46): each opens with a regular face, and the bible website copies one in.
// CJ's Alvora shapes (0.47): one Smith family however it's reached, one Crystalline family with Zah'ra's brother, and
// partners who aren't married labelled as partners.
static void testFamilyFounders() {
    const std::string dir = tempLibrary("famfounders");
    auto lib = openOk(dir);
    if (!lib) return;
    const Id world = lib->createWorld("Alvora");
    auto person = [&](const char* name) { return lib->createEntry(world, {}, "character", name); };
    const Id iris = person("Iris"), violet = person("Violet Smith"), ralph = person("Ralph Smith"), maria = person("Maria Susan Smith");
    const Id tom = person("Thomas Terrance Smith"), astrid = person("Astrid Mira Smith"), lillia = person("Lillia Smith");
    const Id sahra = person("Sah'ra Jane Crystalline"), zia = person("Zia Smith"), ben = person("Benithic Aku");
    const Id kion = person("Kion R'kal Crystalline"), zahra = person("Zah'ra Nara Crystalline"), mahlos = person("Mah'los Sinclair Crystalline");
    const Id liah = person("Li'ah Anne Crystalline");
    // Iris and Violet sisters, Violet and Ralph brother and sister; Ralph married to Maria.
    lib->addLink(iris, violet, "Sibling of");
    lib->addLink(violet, ralph, "Sibling of");
    lib->addLink(ralph, maria, "Married to");
    for (const Id& c : {tom, astrid}) { lib->addLink(ralph, c, "Parent of"); lib->addLink(maria, c, "Parent of"); }
    lib->addLink(tom, astrid, "Twin of");
    lib->addLink(ralph, lillia, "Adoptive parent of");
    lib->addLink(tom, sahra, "Partner of");
    lib->addLink(astrid, ben, "Partner of");
    lib->addLink(tom, zia, "Parent of");
    lib->addLink(sahra, zia, "Parent of");
    lib->addLink(kion, zahra, "Married to");
    for (const Id& c : {sahra, liah}) { lib->addLink(kion, c, "Parent of"); lib->addLink(zahra, c, "Parent of"); }
    lib->addLink(mahlos, zahra, "Sibling of");
    int smiths = 0, crystallines = 0;
    for (auto& g : lib->families(world)) {
        if (g.surname == "Smith") ++smiths;
        if (g.surname == "Crystalline") {
            ++crystallines;
            bool hasMahlos = false, hasLiah = false;
            for (auto& p : lib->familyLayout(g.root, 150, true).people) { hasMahlos |= p.id == mahlos; hasLiah |= p.id == liah; }
            CHECK(hasMahlos && hasLiah);
        }
        CHECK(g.name.find("(") == std::string::npos);   // no families told apart: none are doubled
    }
    CHECK(smiths == 1 && crystallines == 1);
    // Partners are partners; a marriage is a marriage.
    for (auto& g : lib->families(world))
        for (auto& p : lib->familyLayout(g.root, 150, true).people) {
            if (p.id == ben) CHECK(p.inLaw && p.joined == "partner");
            if (g.surname == "Smith" && p.id == sahra) CHECK(p.inLaw && p.joined == "partner");
        }
    const auto rl = lib->familyLayout(ralph, 150, true);
    for (auto& p : rl.people) if (p.id == maria && p.inLaw) CHECK(p.joined == "married");
}

static void testBuiltInFonts() {
    namespace ts = studio::typeset;
    namespace bible = studio::bible;
    const std::string fonts = appFolder("fonts");
    std::set<std::string> names;
    for (auto& f : ts::fontFamilies(fonts)) {
        names.insert(f.name);
        CHECK(!studio::fonts::familyFolder(fonts, f.id).empty());
    }
    for (const char* n : {"EB Garamond", "Libre Baskerville", "Lora", "Alegreya", "Spectral", "Source Sans 3", "Caveat", "Dancing Script",
                          "Cinzel", "Uncial Antiqua", "IM Fell English", "MedievalSharp", "Noto Sans Runic", "Special Elite", "Crimson Text"})
        CHECK(names.count(n) == 1);
    for (const char* id : {"garamond", "cinzel", "caveat", "elite", "uncial"}) {
        bool listed = false;
        for (auto& c : bible::fonts()) if (std::string(c.id) == id) listed = true;
        CHECK(listed);
    }
    const std::string dir = tempLibrary("builtinfonts");
    auto lib = openOk(dir);
    if (!lib) return;
    const Id world = lib->createWorld("Alvora");
    lib->createEntry(world, {}, "character", "Tom Smith");
    bible::Look look;
    look.headings = "cinzel";
    look.body = "garamond";
    std::string err;
    int pages = 0;
    CHECK(bible::exportWebsite(*lib, {world, {}, false, {}}, dir + "/site", err, &pages, look, fonts));
    std::ifstream css(dir + "/site/style.css");
    const std::string c((std::istreambuf_iterator<char>(css)), {});
    CHECK(c.find("font-family:\"Cinzel\"") != std::string::npos || c.find("\"Cinzel\"") != std::string::npos);
    CHECK(c.find("EB Garamond") != std::string::npos);
    bool copied = false;
    for (auto& e : std::filesystem::recursive_directory_iterator(dir + "/site")) if (e.path().filename().string().rfind("EBGaramond-", 0) == 0) copied = true;
    CHECK(copied);
}

static void testFamilyKinds() {
    namespace ft = studio::familytree;
    namespace pa = studio::paste;
    const std::string dir = tempLibrary("famkinds");
    auto lib = openOk(dir);
    if (!lib) return;
    const Id world = lib->createWorld("Alvora");
    auto person = [&](const char* name, const char* details) {
        const Id id = lib->createEntry(world, {}, "character", name);
        Entry e = *lib->entry(id);
        e.details = details;
        lib->updateEntry(e);
        return id;
    };
    const Id ralph = person("Ralph Smith", "Gender: Male");
    const Id maria = person("Maria Smith", "Gender: Female");
    const Id tom = person("Tom Smith", "Gender: Male");
    const Id astrid = person("Astrid Smith", "Gender: Female");
    const Id lillia = person("Lillia Smith", "Gender: Female");
    const Id jen = person("Jen Holt", "Gender: Female");        // Ralph's second partner
    const Id rose = person("Rose Smith", "Gender: Female");      // Ralph and Jen's daughter
    const Id dag = person("Dag Holt", "Gender: Male");           // Jen's son from before
    const Id ana = person("Ana Bright", "Gender: Female");
    const Id bo = person("Bo Bright", "Gender: Male");           // Ana's twin, no parents in the bible
    for (const Id& c : {tom, astrid}) { lib->addLink(ralph, c, "Parent of"); lib->addLink(maria, c, "Parent of"); }
    lib->addLink(tom, astrid, "Twin of");
    lib->addLink(lillia, ralph, "Adopted child of");
    lib->addLink(maria, lillia, "Adoptive parent of");
    lib->addLink(ralph, jen, "Married to");
    lib->addLink(ralph, maria, "Married to");   // his late first wife
    lib->addLink(ralph, rose, "Parent of");
    lib->addLink(jen, rose, "Parent of");
    lib->addLink(ralph, dag, "Step-parent of");
    lib->addLink(jen, dag, "Parent of");
    lib->addLink(ana, bo, "Twin of");
    // Kinds read back, either way round, and siblings worked out.
    const auto tf = lib->familyOf(tom);
    CHECK(tf.how.at(astrid) == "twin");
    CHECK(tf.how.at(lillia) == "adoptive");
    CHECK(tf.how.at(rose) == "half");
    CHECK(tf.how.at(ralph).empty());
    const auto lf = lib->familyOf(lillia);
    CHECK(lf.how.at(ralph) == "adoptive" && lf.how.at(maria) == "adoptive");
    const auto rf = lib->familyOf(ralph);
    CHECK(rf.how.at(dag) == "step" && rf.how.at(lillia) == "adoptive" && rf.how.at(tom).empty());
    CHECK(lib->familyOf(dag).how.at(rose) == "step" || lib->familyOf(dag).how.at(rose) == "half");
    // The layout: parent kinds, twins side by side, the Brights joined by their own link.
    const auto lay = lib->familyLayout(ralph, 150, true);
    CHECK(lay.parentHow.at({ralph, lillia}) == "adoptive" && lay.parentHow.at({ralph, dag}) == "step" && lay.parentHow.at({ralph, tom}).empty());
    float xt = -1, xa = -1;
    for (auto& p : lay.people) { if (p.id == tom) xt = p.x; if (p.id == astrid) xa = p.x; }
    CHECK(std::fabs(xt - xa) < 1.3f);
    bool twins = false;
    for (auto& sb : lay.siblings) if (sb.how == "twin" && sb.sharedParents) twins = true;
    CHECK(twins);
    const auto bl = lib->familyLayout(ana, 150, true);
    CHECK(bl.siblings.size() == 1 && bl.siblings[0].how == "twin" && !bl.siblings[0].sharedParents);
    // The bible's PDF names each relative by what they are (0.48.1).
    {
        namespace bible = studio::bible;
        const auto src = bible::printableSource(*lib, bible::Options{world, {}, false, {}}, "Bible", "A. Writer");
        std::string tomPage, ralphPage, lilliaPage;
        for (auto& sec : src.sections) {
            if (sec.title == "Tom Smith") tomPage = sec.content;
            if (sec.title == "Ralph Smith") ralphPage = sec.content;
            if (sec.title == "Lillia Smith") lilliaPage = sec.content;
        }
        CHECK(tomPage.find("Twin sister: ") != std::string::npos && tomPage.find("Half-sister: ") != std::string::npos &&
              tomPage.find("Adoptive sister: ") != std::string::npos && tomPage.find("Father: ") != std::string::npos &&
              tomPage.find("Brothers and sisters") == std::string::npos);
        CHECK(ralphPage.find("Wives: ") != std::string::npos && ralphPage.find("Stepson: ") != std::string::npos &&
              ralphPage.find("Adopted daughter: ") != std::string::npos);
        CHECK(lilliaPage.find("Adoptive father: ") != std::string::npos && lilliaPage.find("Adoptive mother: ") != std::string::npos);
        // And the website's box (0.48.2).
        std::string err;
        int pages = 0;
        CHECK(bible::exportWebsite(*lib, bible::Options{world, {}, false, {}}, dir + "/site", err, &pages));
        bool twin = false, wives = false;
        for (auto& f : std::filesystem::directory_iterator(dir + "/site")) {
            if (!f.is_regular_file() || f.path().extension() != ".html") continue;
            std::ifstream in(f.path());
            const std::string h((std::istreambuf_iterator<char>(in)), {});
            if (h.find("<title>Tom Smith") != std::string::npos) twin = h.find("<td>Twin sister</td>") != std::string::npos && h.find("<td>Half-sister</td>") != std::string::npos;
            if (h.find("<title>Ralph Smith") != std::string::npos) wives = h.find("<td>Wives</td>") != std::string::npos && h.find("<td>Stepson</td>") != std::string::npos;
        }
        CHECK(twin && wives);
    }
    // Reverse wordings of the new relationships.
    CHECK(studio::reverseRelation("Twin of") == "Twin of" && studio::reverseRelation("Adoptive parent of") == "Adopted child of" &&
          studio::reverseRelation("Stepchild of") == "Step-parent of" && studio::reverseRelation("Foster sibling of") == "Foster sibling of");
    // Pasted wordings.
    CHECK(pa::relationFor("character", "Twin sister") == "Twin of");
    CHECK(pa::relationFor("character", "Half-brothers") == "Half-sibling of");
    CHECK(pa::relationFor("character", "Stepmother") == "Stepchild of");
    CHECK(pa::relationFor("character", "Stepson") == "Step-parent of");
    CHECK(pa::relationFor("character", "Adoptive father") == "Adopted child of");
    CHECK(pa::relationFor("character", "Adopted daughter") == "Adoptive parent of");
    CHECK(pa::relationFor("character", "Foster brother") == "Foster sibling of");
    CHECK(pa::relationFor("character", "Former stepsister") == "Step-sibling of");
    CHECK(pa::relationFor("character", "Brother") == "Sibling of" && pa::relationFor("character", "Parents") == "Child of");
    // The blank page's family lines, filled in and pasted, make links of each kind.
    {
        std::string page = pa::blankPage("character", {}, {});
        CHECK(page.find("Twin: ") != std::string::npos && page.find("Stepchildren: ") != std::string::npos);
        auto fill = [&](const std::string& label, const std::string& who) {
            const size_t at = page.find("\n" + label + ": \n");
            CHECK(at != std::string::npos);
            if (at != std::string::npos) page.insert(at + label.size() + 3, who);
        };
        page.replace(page.find("Name: "), 6, "Name: Cara Bright");
        fill("Twin", "Ana Bright");
        fill("Adoptive parents", "Ralph Smith");
        fill("Step-siblings", "Dag Holt");
        fill("Foster children", "Kip Rowan");   // not in the bible: made
        const Id cara = lib->createEntry(world, {}, "character", "Cara Bright");
        pa::Report rep;
        CHECK(pa::apply(*lib, cara, pa::parse(page), {}, rep));
        auto linked = [&](const Id& other, const char* label) {
            for (auto& l : lib->linksOf(cara)) if ((l.to == other || l.from == other) && l.label == label) return true;
            return false;
        };
        CHECK(linked(ana, "Twin of") && linked(ralph, "Adopted child of") && linked(dag, "Step-sibling of"));
        const Entry* kip = nullptr;
        for (const Entry* x : lib->entriesOf(world)) if (x->name == "Kip Rowan") kip = x;
        CHECK(kip && linked(kip->id, "Foster parent of"));
        const auto cf = lib->familyOf(cara);
        CHECK(cf.how.count(ana) && cf.how.at(ana) == "twin" && cf.how.at(ralph) == "adoptive" && cf.how.at(dag) == "step");
    }
    // Importing with "replace all their text": what the page doesn't say goes; the picture stays (0.46).
    {
        Entry a = *lib->entry(ana);
        a.summary = "Old summary.";
        a.details = "Gender: Female\nEyes: green";
        a.notes = "Old notes.";
        a.aliases = {"Annie"};
        a.imageBlob = "abc123";
        a.setSection("History", "Old history.");
        lib->updateEntry(a);
        lib->addLink(ana, ralph, "Friend of");
        const std::vector<pa::Batch> pages{{"character", "Ana Bright", "Name: Ana Bright\nSummary\nA new summary.\n\nFamily\nTwin: Bo Bright\n"}};
        pa::Import imp;
        CHECK(pa::importPages(*lib, world, {}, pages, {}, imp, true));
        const Entry* n = lib->entry(ana);
        CHECK(n && n->summary == "A new summary." && n->details.empty() && n->notes.empty() && n->aliases.empty() && n->section("History").empty());
        CHECK(n && n->imageBlob == "abc123");
        bool friendLink = false, twin = false;
        for (auto& l : lib->linksOf(ana)) { friendLink |= l.label == "Friend of"; twin |= l.label == "Twin of"; }
        CHECK(!friendLink && twin);
        CHECK(imp.updated.size() == 1 && imp.before[0].summary == "Old summary." && imp.linksBefore[0].size() >= 2);
    }
    // Two families of one surname are told apart.
    const Id old1 = person("Hal Smith", "Gender: Male");
    const Id kid1 = person("Ned Smith", "Gender: Male");
    lib->addLink(old1, kid1, "Parent of");
    int smiths = 0;
    std::set<std::string> names;
    for (auto& g : lib->families(world)) { if (g.surname == "Smith") { ++smiths; names.insert(g.name); } }
    CHECK(smiths == 2 && names.size() == 2 && names.count("The Smith family (Hal's)") == 1);
    for (auto& g : lib->families(world)) if (g.root == maria || g.root == jen) CHECK(g.people >= 8 && g.name.find("Smith") != std::string::npos);
    CHECK(ft::title(*lib, old1, true) == "The Smith family (Hal's)");
    // The picture draws with kinds (and says so in its key).
    const std::string fonts = appFolder("fonts");
    const auto pic = ft::render(*lib, ralph, fonts);
    CHECK(pic.ok && pic.people >= 8);
    if (const char* out = std::getenv("STUDIO_TREE_PNG")) std::ofstream(std::string(out) + "-kinds.png", std::ios::binary) << pic.png;
}

static void testFamilyTreePicture() {
    namespace ft = studio::familytree;
    const std::string dir = tempLibrary("famtreepic");
    auto lib = openOk(dir);
    if (!lib) return;
    const Id world = lib->createWorld("Alvora");
    auto person = [&](const char* name, const char* details) {
        const Id id = lib->createEntry(world, {}, "character", name);
        Entry e = *lib->entry(id);
        e.details = details;
        lib->updateEntry(e);
        return id;
    };
    const Id tobias = person("Tobias Vale", "Gender: Male\nAge: 61");
    const Id maren = person("Maren Ashdown", "Gender: Female\nAge: 58");
    const Id ada = person("Ada Vale", "Gender: Female\nAge: 34\nTitle: Harbourmaster");
    const Id zorn = person("Zornov Vale of the Long Harbour Road", "Gender: Male\nBorn: Highcliff");
    const Id wren = person("Wren Holt", "Gender: Female");
    const Id kit = person("Kit Vale", "Gender: Non-binary\nAge: 9");
    const Id yon = person("Yonvaan Akumes Yerasuun", "Gender: Male");
    const Id zih = person("Zih'ah Saara Yerasuun", "Gender: Female");
    const Id aal = person("Aaliyah Ne'liya Yerasuun", "Gender: Female");
    lib->addLink(tobias, maren, "Married to");
    for (const Id& c : {ada, zorn}) { lib->addLink(tobias, c, "Parent of"); lib->addLink(maren, c, "Parent of"); }
    lib->addLink(zorn, wren, "Partner of");
    lib->addLink(zorn, kit, "Parent of");
    lib->addLink(wren, kit, "Parent of");
    // Two parents who aren't partners.
    lib->addLink(yon, aal, "Parent of");
    lib->addLink(zih, aal, "Parent of");
    CHECK(ft::title(*lib, tobias, true) == "The Vale family" && ft::title(*lib, tobias, false) == "Tobias Vale's family tree");
    const std::string fonts = appFolder("fonts");
    const auto p = ft::render(*lib, tobias, fonts);
    CHECK(p.ok && p.png.compare(1, 3, "PNG") == 0 && p.width > 600 && p.height > 300 && p.people >= 6);
    const auto y = ft::render(*lib, yon, fonts);
    CHECK(y.ok && y.people == 3);
    ft::Options turn;
    turn.turnWide = true;
    turn.heading = false;
    const auto t = ft::render(*lib, tobias, fonts, turn);
    CHECK(t.ok && (t.turned ? t.height > t.width : true));
    CHECK(!ft::render(*lib, "nobody", fonts).ok);
    if (const char* out = std::getenv("STUDIO_TREE_PNG")) {
        std::ofstream(std::string(out) + "-vale.png", std::ios::binary) << p.png;
        std::ofstream(std::string(out) + "-yerasuun.png", std::ios::binary) << y.png;
    }
    // In the bible's PDF: a Family trees part with a chapter per family, its picture written to the folder.
    bible::Options o{world, {}, false, {}};
    o.treeFonts = fonts;
    o.treeFolder = dir + "/trees";
    const auto src = bible::printableSource(*lib, o, "The Alvora Bible", "A. Writer");
    int part = -1, trees = 0;
    for (size_t i = 0; i < src.sections.size(); ++i) {
        if (src.sections[i].kind == NodeKind::Part && src.sections[i].title == "Family trees") part = (int)i;
        if (part >= 0 && (int)i > part && src.sections[i].kind == NodeKind::Chapter && src.sections[i].content.find("\"image\"") != std::string::npos) ++trees;
    }
    CHECK(part >= 0 && trees == 2);
    bool files = false;
    for (auto& [blob, path] : src.picturePaths) if (path.find("familytree-") != std::string::npos && fs::exists(path)) files = true;
    CHECK(files);
    const auto book = typeset::typesetBook(src, typeset::presets()[0].design, {fonts, appFolder("hyphenation")});
    CHECK(book.ok);
    // On the website: a Family trees page, linked from the header, with the pictures.
    {
        std::string err;
        int pages = 0;
        CHECK(bible::exportWebsite(*lib, o, dir + "/site", err, &pages, {}, fonts));
        std::ifstream in(dir + "/site/families.html");
        const std::string html((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(html.find("The Vale family") != std::string::npos && html.find("images/familytree-") != std::string::npos);
        std::ifstream home(dir + "/site/index.html");
        const std::string h((std::istreambuf_iterator<char>(home)), std::istreambuf_iterator<char>());
        CHECK(h.find("families.html") != std::string::npos);
        int pngs = 0;
        for (auto& e : fs::directory_iterator(dir + "/site/images")) if (e.path().filename().string().rfind("familytree-", 0) == 0) ++pngs;
        CHECK(pngs == 2);
    }
    // Not in a reader's guide.
    bible::Options g = o;
    g.readersGuide = true;
    const auto gs = bible::printableSource(*lib, g, "Guide", "A. Writer");
    CHECK(std::none_of(gs.sections.begin(), gs.sections.end(), [](auto& x) { return x.title == "Family trees"; }));
}

static void testFamilyTree() {
    using namespace studio;
    const std::string dir = tempLibrary("family");
    auto lib = openOk(dir);
    const Id world = lib->createWorld("Alvora");
    auto person = [&](const char* n) { return lib->createEntry(world, {}, "character", n); };
    const Id tom = person("Tom Wren"), mae = person("Mae Wren"), ada = person("Ada Wren"), ben = person("Ben Wren");
    const Id cal = person("Cal Holt"), ivy = person("Ivy Holt"), gran = person("Old Wren"), stray = person("Nobody");
    lib->addLink(tom, ada, "Parent of");
    lib->addLink(ben, mae, "Child of");     // the other way round
    lib->addLink(tom, ben, "Parent of");
    lib->addLink(mae, tom, "Married to");
    lib->addLink(mae, ada, "Parent of");
    lib->addLink(ada, cal, "Partner of");
    lib->addLink(ada, ivy, "Parent of");
    lib->addLink(cal, ivy, "Parent of");
    lib->addLink(gran, tom, "Parent of");
    lib->addLink(ada, ben, "Parent of"); lib->removeLink(ada, ben, "Parent of");
    lib->addLink(ivy, gran, "Child of"); lib->removeLink(ivy, gran, "Child of");
    // A loop by mistake doesn't hang anything.
    const Id x = person("Loop A"), y = person("Loop B");
    lib->addLink(x, y, "Parent of");
    lib->addLink(y, x, "Parent of");

    const auto f = lib->familyOf(ada);
    CHECK(f.parents.size() == 2 && f.partners.size() == 1 && f.partners[0] == cal && f.children.size() == 1 && f.children[0] == ivy);
    CHECK(f.siblings.size() == 1 && f.siblings[0] == ben);    // shares a parent, never linked as siblings
    const auto lay = lib->familyLayout(ada);
    CHECK(lay.people.size() == 7 && !lay.cut);
    CHECK(lay.minGeneration == -2 && lay.maxGeneration == 1);
    std::map<Id, Library::FamilyLayout::Person> at;
    for (auto& p : lay.people) at[p.id] = p;
    CHECK(at[gran].generation == -2 && at[tom].generation == -1 && at[ivy].generation == 1 && at[cal].generation == 0);
    CHECK(std::fabs(at[tom].x - at[mae].x) == 1.0f);         // a couple side by side
    CHECK(std::fabs(at[ada].x - at[cal].x) == 1.0f);
    // No two people of a generation in one place.
    for (auto& a : lay.people) for (auto& b : lay.people)
        if (a.id != b.id && a.generation == b.generation) CHECK(std::fabs(a.x - b.x) >= 0.99f);
    // Ivy sits under her parents; Tom and Mae over their children.
    const float kids = (at[ada].x + at[ben].x) / 2, parents = (at[tom].x + at[mae].x) / 2;
    CHECK(std::fabs(kids - parents) < 1.01f);
    CHECK(lay.couples.size() == 2 && lay.parentChild.size() == 7);
    CHECK(lib->familyLayout(stray).people.size() == 1);
    CHECK(lib->familyLayout(x).people.size() == 2);
    {
        bool calInLaw = false, maeInLaw = false;
        for (auto& p : lay.people) { if (p.id == cal) calInLaw = p.inLaw; if (p.id == mae) maeInLaw = p.inLaw; }
        CHECK(calInLaw && !maeInLaw);   // Mae is Ada's mother here, not someone married in
    }
    auto fams = lib->families(world);
    CHECK(fams.size() == 1 && fams[0].people == 7 && fams[0].surname == "Wren" && fams[0].root == gran);   // the loop has no founder
    // Two families joined by a marriage stay two trees (0.38.3): Cal's parents start the Holts.
    const Id paHolt = person("Pa Holt"), maHolt = person("Ma Holt"), aunt = person("Rue Holt");
    lib->addLink(paHolt, cal, "Parent of");
    lib->addLink(maHolt, cal, "Parent of");
    lib->addLink(paHolt, maHolt, "Married to");
    lib->addLink(aunt, cal, "Sibling of");
    fams = lib->families(world);
    CHECK(fams.size() == 2);
    for (auto& g : fams) {
        const auto t = lib->familyLayout(g.root, 400, true);
        std::set<Id> ids;
        for (auto& p : t.people) ids.insert(p.id);
        if (g.surname == "Wren") CHECK(!ids.count(paHolt) && !ids.count(aunt) && ids.count(cal));          // Cal married in; his family isn't drawn
        else CHECK(g.surname == "Holt" && ids.count(paHolt) && ids.count(maHolt) && ids.count(ivy) && !ids.count(tom) && !ids.count(gran));
    }
    {   // Two founders with a child but not marked as partners: one family, not two.
        const Id f1 = person("Yon Yerasuun"), f2 = person("Zih Yerasuun"), kid = person("Aali Yerasuun");
        lib->addLink(f1, kid, "Parent of");
        lib->addLink(kid, f2, "Child of");
        int yer = 0;
        for (auto& g : lib->families(world)) if (g.surname == "Yerasuun") { ++yer; CHECK(g.people == 3); }
        CHECK(yer == 1);
    }
    {   // Ada's own tree: her lines up, not Cal's.
        const auto mine = lib->familyLayout(ada);
        std::set<Id> ids;
        for (auto& p : mine.people) ids.insert(p.id);
        CHECK(ids.count(gran) && ids.count(cal) && !ids.count(paHolt) && !ids.count(aunt));
        const auto his = lib->familyLayout(cal);
        std::set<Id> hids;
        for (auto& p : his.people) hids.insert(p.id);
        CHECK(hids.count(paHolt) && hids.count(aunt) && hids.count(ada) && !hids.count(tom) && !hids.count(gran));
    }
    // Limited: the rest left out, and said so.
    CHECK(lib->familyLayout(ada, 3).cut && lib->familyLayout(ada, 3).people.size() == 3);
}

static void testContinuity() {
    using namespace studio;
    const std::string dir = tempLibrary("continuity");
    auto lib = openOk(dir);
    const Id world = lib->createWorld("Alvora");
    const Id book = lib->createBook("Dragonfall", world);
    const Id ada = lib->createEntry(world, {}, "character", "Ada Wren");
    const Id tom = lib->createEntry(world, {}, "character", "Tom");
    const Id never = lib->createEntry(world, {}, "place", "Nowhere Fen");
    {
        Entry e = *lib->entry(ada);
        e.details = "Eyes: grey\nHair: black\nAge: 34\nGender: Female\nGender: Woman";
        lib->updateEntry(e);
        Entry t = *lib->entry(tom);
        t.details = "Eyes: brown";
        lib->updateEntry(t);
    }
    const Id ch = lib->createNode(book, {}, NodeKind::Chapter, "One");
    CHECK(lib->saveNodeContent(ch, para("Ada Wren turned her bright blue eyes to the sea. Her black hair was wet. "
                                        "Ada was a thirty-four-year-old harbourmaster. Tom looked at Ada with his green eyes. "
                                        "Tom was twenty years old.")));
    const auto c = review::continuityChecks(*lib, world);
    int inBible = 0, eyes = 0, hair = 0, age = 0;
    for (auto& f : c.facts) {
        if (f.inBible) { ++inBible; CHECK(f.entry == ada && f.label == "Gender"); continue; }
        if (f.label == "Eyes") { ++eyes; CHECK(f.entry == ada && f.other == "blue eyes" && f.nodeId == ch); }
        if (f.label == "Hair") ++hair;
        if (f.label == "Age") ++age;
    }
    CHECK(inBible == 1);
    CHECK(eyes == 1);          // Tom's green eyes are in a sentence naming Ada too: left alone
    CHECK(hair == 0 && age == 0);   // black hair and 34 agree
    CHECK(c.unused.size() == 1 && c.unused[0] == never);
    CHECK(!c.facts.empty() && !c.facts[0].key().empty());
}

static void testBibleExport() {
    using namespace studio;
    namespace ts = studio::typeset;
    const std::string dir = tempLibrary("bibleexport");
    auto lib = openOk(dir);
    const Id world = lib->createWorld("Alvora");
    const Id b1 = lib->createBook("Book One", world), b2 = lib->createBook("Book Two", world);
    const Id ada = lib->createEntry(world, {}, "character", "Ada Wren");
    const Id tom = lib->createEntry(world, {}, "character", "Tom Wren");
    const Id ghost = lib->createEntry(world, {}, "character", "The Ghost");
    const Id cliff = lib->createEntry(world, {}, "place", "Highcliff");
    lib->addLink(tom, ada, "Parent of");
    lib->addLink(ada, cliff, "Lives in");
    {
        Entry e = *lib->entry(ada);
        e.summary = "Harbourmaster of Highcliff <and> keeper.";
        e.details = "Age: 34\nSecret: Tom's heir";
        e.pronunciation = "AH-dah";
        e.aliases = {"Captain Wren"};
        e.setSection("Backstory & goals", "Grew up in Highcliff.\n\nWants the ledger.");
        e.setRevealedIn("detail:Secret", b2);
        e.setBookNote(b1, "Finds the ledger.");
        e.notes = "Private author note.";
        lib->updateEntry(e);
        Entry g = *lib->entry(ghost);
        g.setRevealedIn("entry", b2);
        lib->updateEntry(g);
        Entry c = *lib->entry(cliff);
        c.mapBlob = lib->storeBlob(std::string("\x89PNG\r\n\x1a\n\0\0\0\rIHDR\0\0\0\x10\0\0\0\x08", 24));
        c.pins = {{ada, 0.5, 0.25}, {ghost, 0.1, 0.1}};
        lib->updateEntry(c);
    }
    CHECK(bible::booksInOrder(*lib, world, {}).size() == 2);
    CHECK(bible::pictureSize(std::string("\x89PNG\r\n\x1a\n\0\0\0\rIHDR\0\0\0\x10\0\0\0\x08", 24)) == std::make_pair(16, 8));
    auto read = [](const std::string& p) { std::ifstream f(p, std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); };
    // The author's whole bible.
    {
        const std::string out = dir + "/site-all";
        std::string err;
        int pages = 0;
        CHECK(bible::exportWebsite(*lib, {world, {}, false, {}}, out, err, &pages));
        CHECK(pages == 8);
        const std::string a = read(out + "/e-" + ada + ".html");
        CHECK(a.find("Ada Wren") != std::string::npos && a.find("AH-dah") != std::string::npos);
        CHECK(a.find("&lt;and&gt;") != std::string::npos);                          // escaped
        CHECK(a.find("<a href=\"e-" + cliff + ".html\">Highcliff</a>") != std::string::npos);   // linked
        CHECK(a.find("s heir") != std::string::npos && a.find("Private author note.") != std::string::npos && a.find("Finds the ledger.") != std::string::npos);
        CHECK((a.find("<td>Parent</td>") != std::string::npos || a.find("Mother") != std::string::npos || a.find("Father") != std::string::npos) && a.find("Lives in") != std::string::npos);
        const std::string c = read(out + "/e-" + cliff + ".html");
        CHECK(c.find("class=\"pin\"") != std::string::npos && c.find("The Ghost") != std::string::npos && c.find("left:50.00%;top:25.00%") != std::string::npos);
        CHECK(read(out + "/glossary.html").find("Captain Wren") != std::string::npos);
        CHECK(read(out + "/index.html").find("Characters") != std::string::npos);
        CHECK(std::filesystem::exists(out + "/style.css"));
    }
    // A reader's guide for Book One: no spoilers from Book Two, no author's notes.
    {
        const std::string out = dir + "/site-guide";
        std::string err;
        int pages = 0;
        CHECK(bible::exportWebsite(*lib, {world, {}, true, b1}, out, err, &pages));
        CHECK(pages == 7);   // the Ghost isn't met until Book Two
        CHECK(!std::filesystem::exists(out + "/e-" + ghost + ".html"));
        const std::string a = read(out + "/e-" + ada + ".html");
        CHECK(a.find("s heir") == std::string::npos && a.find("Private author note.") == std::string::npos && a.find("Finds the ledger.") == std::string::npos);
        CHECK(a.find("reader's guide") != std::string::npos && a.find("Age") != std::string::npos);
        CHECK(read(out + "/e-" + cliff + ".html").find("The Ghost") == std::string::npos);
        CHECK(read(out + "/glossary.html").find("Ghost") == std::string::npos);
    }
    // Printable: a part per kind, a chapter per entry, the glossary at the back; and it typesets.
    {
        auto src = bible::printableSource(*lib, {world, {}, false, {}}, "The Alvora Bible", "A. Writer");
        int parts = 0, chapters = 0;
        for (auto& sec : src.sections) (sec.kind == NodeKind::Part ? parts : chapters)++;
        CHECK(parts == 2 && chapters == 5 && src.sections.back().title == "Glossary" && src.sections.back().matter == 2);
        bool sawSecret = false;
        for (auto& sec : src.sections) if (sec.content.find("Tom's heir") != std::string::npos) sawSecret = true;
        CHECK(sawSecret);
        const ts::Resources res{appFolder("fonts"), appFolder("hyphenation")};
        const ts::Book book = ts::typesetBook(src, ts::presets()[0].design, res);
        CHECK(book.pages.size() >= 8);
        std::string err;
        const std::string pdf = ts::pdfBytes(book, err);
        CHECK(pdf.rfind("%PDF", 0) == 0);
        auto guide = bible::printableSource(*lib, {world, {}, true, b1}, "Guide", "A. Writer");
        bool leak = false;
        for (auto& sec : guide.sections) if (sec.content.find("Tom's heir") != std::string::npos || sec.title == "The Ghost") leak = true;
        CHECK(!leak && guide.subtitle == "A reader's guide to Book One");
    }
    // Languages and the look (0.38): a dictionary page and a part in the PDF; themes, typefaces and choices kept.
    {
        studio::conlang::Language L;
        L.id = "lang1";
        L.name = "Telvani";
        L.notes = "The old tongue.";
        L.words = {{"veyth'ar", "light", "noun", "VAY-thar", ""}, {"sel-amar", "river", "noun", "", "poetic"}};
        lib->setSetting("conlangs:world:" + world, studio::conlang::toJson({L}));
        CHECK(bible::languagesOf(*lib, world, {}).size() == 1);
        bible::Look look;
        look.theme = "night";
        look.headings = "crimson";
        look.tagline = "Where the light <broke>";
        look.accent = "#123456";
        CHECK(bible::lookFromJson(bible::toJson(look)).tagline == look.tagline && bible::lookFromJson(bible::toJson(look)).theme == "night");
        CHECK(bible::lookFromJson("{\"theme\":\"nope\",\"accent\":\"red\",\"mode\":\"x\"}").theme == "parchment");
        CHECK(bible::lookFromJson("{\"accent\":\"red\"}").accent.empty() && bible::lookFromJson("junk").mode == "auto");
        const std::string out = dir + "/site-look";
        std::string err;
        int pages = 0;
        CHECK(bible::exportWebsite(*lib, {world, {}, false, {}}, out, err, &pages, look, appFolder("fonts")));
        CHECK(pages == 9);   // and a page for each of the two kinds
        const std::string langs = read(out + "/languages.html");
        CHECK(langs.find("Telvani") != std::string::npos && langs.find("veyth'ar") != std::string::npos && langs.find("VAY-thar") != std::string::npos);
        CHECK(langs.find("class=\"filter\"") != std::string::npos);
        const std::string css = read(out + "/style.css");
        CHECK(css.find("#0b1120") != std::string::npos && css.find("#123456") != std::string::npos && css.find("@font-face") != std::string::npos);
        CHECK(std::filesystem::exists(out + "/fonts/crimson-text/CrimsonText-Regular.ttf") && std::filesystem::exists(out + "/fonts/crimson-text/OFL.txt"));
        CHECK(read(out + "/search.js").find("Ada Wren") != std::string::npos && std::filesystem::exists(out + "/site.js"));
        const std::string home = read(out + "/index.html");
        CHECK(home.find("Where the light &lt;broke&gt;") != std::string::npos && home.find("class=\"card") != std::string::npos && home.find("languages.html") != std::string::npos);
        // Without languages, glossary, family or pictures.
        bible::Options o{world, {}, false, {}};
        o.languages = o.glossary = o.family = o.pictures = false;
        bible::Look plain;
        plain.cards = false;
        plain.search = false;
        const std::string out2 = dir + "/site-plain";
        CHECK(bible::exportWebsite(*lib, o, out2, err, &pages, plain));
        CHECK(pages == 7 && !std::filesystem::exists(out2 + "/languages.html") && !std::filesystem::exists(out2 + "/glossary.html"));
        CHECK(read(out2 + "/e-" + ada + ".html").find("Lives in") == std::string::npos && !std::filesystem::exists(out2 + "/search.js"));
        CHECK(read(out2 + "/e-" + cliff + ".html").find("class=\"pin\"") == std::string::npos);
        // In the PDF: a Languages part with a chapter per language.
        auto src = bible::printableSource(*lib, {world, {}, false, {}}, "The Alvora Bible", "A. Writer");
        bool part = false, chapter = false;
        for (auto& sec : src.sections) {
            part |= sec.kind == NodeKind::Part && sec.title == "Languages";
            chapter |= sec.title == "Telvani" && sec.content.find("veyth'ar") != std::string::npos && sec.content.find("light") != std::string::npos;
        }
        CHECK(part && chapter);
        auto noLang = bible::printableSource(*lib, o, "x", "y");
        CHECK(std::none_of(noLang.sections.begin(), noLang.sections.end(), [](auto& sec) { return sec.title == "Languages" || sec.title == "Glossary"; }));
    }
}

static void testBibleTidy() {
    using namespace studio;
    const std::string dir = tempLibrary("tidy");
    {
        auto lib = openOk(dir);
        const Id world = lib->createWorld("Alvora");
        const Id shards = lib->createBook("Shards of Light", {});   // on its own, with its own entries
        const Id inBook = lib->createEntry({}, shards, "character", "Thomas Terrance Smith");
        Entry e = *lib->entry(inBook);
        e.details = "Gender: Male\nThe blacksmith's son.";
        e.aliases = {"Tom"};
        lib->updateEntry(e);
        const Id inWorld = lib->createEntry(world, {}, "character", "Thomas Terrance Smith");
        Entry w = *lib->entry(inWorld);
        w.details = "Gender: Man\nHas a scar.";
        w.aliases = {"Scion"};
        lib->updateEntry(w);
        const Id astrid = lib->createEntry(world, {}, "character", "Astrid");
        CHECK(lib->addLink(astrid, inBook, "Sibling of") || true);   // a link to the book's copy (from before)
        // 3 copies of Zah’mes, one with a straight apostrophe.
        const Id z1 = lib->createEntry(world, {}, "character", "Zah’mes");
        lib->createEntry(world, {}, "character", "Zah’mes");
        lib->createEntry(world, {}, "character", "Zah'mes");
        // Bluebolt: a character and a creature (a Wyvern).
        const Id dragon = lib->createEntry(world, {}, "creature", "Dragon");
        const Id wyvern = lib->createEntry(world, {}, "creature", "Wyvern");
        lib->addLink(wyvern, dragon, "Kind of");
        const Id boltC = lib->createEntry(world, {}, "creature", "Bluebolt");
        lib->addLink(boltC, wyvern, "Kind of");
        const Id boltP = lib->createEntry(world, {}, "character", "Bluebolt");
        // A place and a character of one name stay apart.
        lib->createEntry(world, {}, "place", "Lamentis");
        lib->createEntry(world, {}, "character", "Lamentis");
        CHECK(lib->findEntryNamed(world, {}, "zah'MES", "character") != nullptr);

        // Moving the book in tidies the bible.
        CHECK(lib->moveBookToWorld(shards, world));
        const auto all = lib->entriesOf(world);
        auto count = [&](const char* n) { int c = 0; for (auto* x : all) if (x->name == n) ++c; return c; };
        CHECK(count("Thomas Terrance Smith") == 1 && count("Zah’mes") + count("Zah'mes") == 1 && count("Bluebolt") == 1 && count("Lamentis") == 2);
        const Entry* tom = lib->findEntryNamed(world, {}, "Thomas Terrance Smith");
        CHECK(tom && tom->aliases.size() == 2);                          // Tom and Scion
        CHECK(tom && tom->details.find("Has a scar.") != std::string::npos && tom->details.find("The blacksmith's son.") != std::string::npos);
        CHECK(tom && (tom->details.find("Gender: Man") == std::string::npos) != (tom->details.find("Gender: Male") == std::string::npos));   // one gender line
        bool sibling = false;
        for (auto& l : lib->linksOf(tom->id)) if (l.from == astrid && l.label == "Sibling of") sibling = true;
        CHECK(sibling);                                                  // the link followed the merge
        const Entry* bolt = lib->findEntryNamed(world, {}, "Bluebolt");
        CHECK(bolt && bolt->id == boltP && bolt->kind == "character");
        bool kindOf = false;
        for (auto& l : lib->linksOf(boltP)) if (l.from == boltP && l.to == wyvern && l.label == "Kind of") kindOf = true;
        CHECK(kindOf);                                                   // still listed under Wyvern
        CHECK(lib->entry(z1) != nullptr);
        CHECK(!lib->trashedEntries().empty());                           // merged copies can be brought back
        CHECK(lib->tidyBible(world).merged == 0);                        // nothing left to do

        // Kinds that look wrong.
        const Id demon = lib->createEntry(world, {}, "character", "Demon");
        const Id zornov = lib->createEntry(world, {}, "character", "Zornov");
        Entry zo = *lib->entry(zornov);
        zo.details = "Race: Demon";
        lib->updateEntry(zo);
        const Id flux = lib->createEntry(world, {}, "character", "Fluxians");
        const auto sug = lib->kindSuggestions(world);
        auto suggested = [&](const Id& id, const char* kind) {
            for (auto& s2 : sug) if (s2.entry == id && s2.kind == kind) return true;
            return false;
        };
        CHECK(suggested(demon, "race") && suggested(flux, "race") && !suggested(zornov, "race"));
    }
    std::filesystem::remove_all(dir);
}

// Worldbuilding: entries, relationships, where they appear, the Trash.
static void testWorldbuilding() {
    using namespace studio;
    const std::string dir = tempLibrary("world");
    {
        auto lib = openOk(dir);
        const Id world = lib->createWorld("Alvora");
        const Id book = lib->createBook("Dragonfall", world);
        const Id other = lib->createBook("Elsewhere", {});
        const Id ch = lib->createNode(book, {}, NodeKind::Chapter, "One");
        lib->saveNodeContent(ch, R"({"type":"doc","content":[{"type":"paragraph","content":[{"type":"text","text":"Maren walked to Highcliff. The Grey Wanderer watched Maren's back; Marenna was elsewhere. Café owners waved."}]}]})");
        const Id ada = lib->createEntry(world, {}, "character", "Maren");
        const Id town = lib->createEntry(world, {}, "place", "Highcliff");
        const Id grey = lib->createEntry(world, {}, "character", "Wanderer");
        const Id lone = lib->createEntry({}, other, "lore", "Rule of Three");
        CHECK(!ada.empty() && !town.empty() && !lone.empty());
        CHECK(lib->createEntry({}, {}, "character", "Nobody").empty());           // needs a world or a book
        CHECK(lib->entriesOf(world).size() == 3 && lib->entriesOf({}, other).size() == 1);
        CHECK(lib->entriesOf(world)[0]->name == "Highcliff");                      // by name
        Entry e = *lib->entry(grey);
        e.name = "Wanderer";
        e.aliases = {"Grey Wanderer", " ", "Wanderer", "the Grey"};
        e.kind = "nonsense";
        CHECK(lib->updateEntry(e));
        CHECK(lib->entry(grey)->aliases.size() == 2 && lib->entry(grey)->kind == "character");   // blanks, repeats and its own name dropped
        // Mentions: whole names only, possessives count, "Marenna" doesn't.
        auto ms = lib->mentionsOf(*lib->entry(ada));
        CHECK(ms.size() == 1 && ms[0].count == 2 && ms[0].nodeId == ch);
        CHECK(ms.size() == 1 && ms[0].snippet.find("Maren walked") != std::string::npos);
        CHECK(lib->mentionsOf(*lib->entry(grey)).size() == 1 && lib->mentionsOf(*lib->entry(grey))[0].count == 1);  // "Grey Wanderer" once, not also "Wanderer"
        // A character's full name: its parts stand for them too; titles and joining words alone don't.
        CHECK((nameParts("Maren Ada Holt") == std::vector<std::string>{"Maren", "Maren Ada", "Ada Holt", "Holt", "Maren Holt"}));   // not "Ada" alone
        {
            int c = 0;
            CHECK(review::replaceWholeWord("Stienriket and Stienriket's gate, not Stienriketan.", "Stienriket", "Steinriket", c) ==
                  "Steinriket and Steinriket's gate, not Stienriketan." && c == 2);
        }
        {
            Entry z;
            z.kind = "character";
            z.name = "Zah'mes";
            const auto sn = searchNames(z);
            CHECK(std::find(sn.begin(), sn.end(), std::string("Zah\xE2\x80\x99mes")) != sn.end());
        }
        // Relationships: each kind's list reads both ways, and places don't offer family ties.
        for (const char* k : {"character", "race", "creature", "place", "realm", "group", "thing", "lore", "event"})
            for (auto& rc : relationsFor(k)) {
                const std::string back = reverseRelation(rc.label);
                CHECK(reverseRelation(back) == rc.label);
                bool listed = false;
                for (auto& r : relations()) if (std::string(r.label) == rc.label) listed = true;
                CHECK(listed);
            }
        for (auto& rc : relationsFor("place")) CHECK(std::string(rc.label) != "Parent of" && std::string(rc.label) != "Sibling of");
        CHECK((nameParts("Snarf Jill Grave") == std::vector<std::string>{"Snarf", "Snarf Jill", "Jill Grave", "Grave", "Snarf Grave"}));
        {
            const auto d = nameParts("Albus Percival Wulfric Brian Dumbledore");
            auto has = [&](const char* x) { return std::find(d.begin(), d.end(), x) != d.end(); };
            CHECK(has("Albus") && has("Dumbledore") && has("Albus Dumbledore") && has("Albus Brian Dumbledore") && has("Percival Wulfric Brian Dumbledore"));
            CHECK(!has("Percival") && !has("Wulfric") && !has("Brian") && !has("Percival Wulfric") && !has("Albus Percival Wulfric Brian Dumbledore"));
        }
        CHECK((nameParts("Captain Ivo of Brennick") == std::vector<std::string>{"Ivo", "Ivo of Brennick", "Brennick", "Ivo Brennick"}));
        CHECK(nameParts("Highcliff").empty() && (nameParts("the Grey") == std::vector<std::string>{"Grey"}));
        {
            const Id ch2 = lib->createNode(book, {}, NodeKind::Chapter, "Two");
            lib->saveNodeContent(ch2, R"({"type":"doc","content":[{"type":"paragraph","content":[{"type":"text","text":"Tomas Holt met Holt at dawn. Tomas laughed; holt is a wood."}]}]})");
            const Id tomas = lib->createEntry(world, {}, "character", "Tomas Holt");
            auto mt = lib->mentionsOf(*lib->entry(tomas));
            CHECK(mt.size() == 1 && mt[0].nodeId == ch2 && mt[0].count == 3);   // "Tomas Holt", "Holt", "Tomas"; not "holt"
            {
                const Id ch3 = lib->createNode(book, {}, NodeKind::Chapter, "Three");
                lib->saveNodeContent(ch3, R"({"type":"doc","content":[{"type":"paragraph","content":[{"type":"text","text":"Jill came by. Snarf waved. GRAVE! Snarf Jill Grave bowed."}]}]})");
                const Id snarf = lib->createEntry(world, {}, "character", "Snarf Jill Grave");
                auto sm = lib->mentionsOf(*lib->entry(snarf));
                CHECK(sm.size() == 1 && sm[0].count == 3);   // "Snarf", "GRAVE", the full name; not "Jill" alone
                CHECK(lib->trashEntry(snarf) && lib->trashNode(ch3));
            }
            const Id inn = lib->createEntry(world, {}, "place", "Blue Anchor");
            auto mi = lib->mentionsOf(*lib->entry(inn));
            CHECK(mi.empty());                                                   // places aren't split into parts
            // Names in a piece of text (links between bible pages): longest first, shared names give every entry.
            const Id maren = lib->createEntry(world, {}, "character", "Maren Holt");
            const std::string t = "Tomas Holt and Maren Holt sailed; Holt steered past Highcliff.";
            const auto spans = findNames(t, lib->entriesOf(world));
            std::vector<std::string> found;
            for (auto& sp : spans) found.push_back(t.substr(sp.start, sp.end - sp.start) + "=" + std::to_string(sp.ids.size()));
            CHECK((found == std::vector<std::string>{"Tomas Holt=1", "Maren Holt=1", "Holt=2", "Highcliff=1"}));
            CHECK(lib->trashEntry(maren));
            CHECK(lib->trashEntry(tomas) && lib->trashEntry(inn) && lib->trashNode(ch2) && lib->emptyTrash());
        }
        CHECK(lib->mentionsOf(*lib->entry(lone)).empty());
        // Relationships, read from either side.
        CHECK(lib->addLink(ada, town, "Lives in") && lib->addLink(ada, town, "Lives in"));
        CHECK(!lib->addLink(ada, ada, "Friend of"));
        CHECK(lib->linksOf(town).size() == 1 && reverseRelation("Lives in") == "Home of" && reverseRelation("Custom") == "Custom");
        // The Trash.
        CHECK(lib->trashEntry(town) && lib->entriesOf(world).size() == 2 && lib->linksOf(ada).empty());
        CHECK(lib->trashCounts().entries == 1 && lib->trashedEntries().size() == 1);
        CHECK(lib->restore(town) && lib->entriesOf(world).size() == 3 && lib->linksOf(ada).size() == 1);
        CHECK(lib->trashEntry(lone) && lib->emptyTrash() && !lib->entry(lone));
        // Planning: viewpoint and target are kept with the chapter; who appears where, in one pass.
        ManuscriptNode n = *lib->node(ch);
        n.povId = ada;
        n.targetWords = 3000;
        CHECK(lib->updateNode(n) && lib->node(ch)->povId == ada && lib->node(ch)->targetWords == 3000);
        n.targetWords = -5;
        CHECK(lib->updateNode(n) && lib->node(ch)->targetWords == 0);
        n.targetWords = 2500;
        CHECK(lib->updateNode(n));
        auto ap = lib->appearancesIn(book);
        CHECK(ap.count(ch) && ap[ch].size() == 3);                                 // Maren, Highcliff, the Wanderer
        CHECK(ap.count(ch) && (ap[ch][0] == ada || ap[ch][0] == grey));          // the most mentioned first (twice each)
        // Kept per chapter (0.35): asked again it's the same; new text, or a renamed entry, is read again.
        CHECK(lib->appearancesIn(book)[ch] == ap[ch]);
        CHECK(lib->saveNodeContent(ch, para("Only Highcliff today.")));
        ap = lib->appearancesIn(book);
        CHECK(ap.count(ch) && ap[ch].size() == 1 && ap[ch][0] == town);
        {
            Entry t = *lib->entry(town);
            t.name = "Lowcliff";
            CHECK(lib->updateEntry(t));
        }
        ap = lib->appearancesIn(book);
        CHECK(!ap.count(ch) || ap[ch].empty());
        {
            Entry t = *lib->entry(town);
            t.name = "Highcliff";
            CHECK(lib->updateEntry(t));
        }
        CHECK(lib->schemaVersion() == studio::Library::kSchemaVersion);
    }
    {
        auto lib = openOk(dir);   // kept
        CHECK(lib->liveBooks().size() == 2);
        Id world;
        for (auto& w : lib->worlds()) world = w.id;
        CHECK(lib->entriesOf(world).size() == 3);
        Id maren;
        for (auto* e : lib->entriesOf(world)) if (e->name == "Maren") maren = e->id;
        CHECK(lib->linksOf(maren).size() == 1 && lib->linksOf(maren)[0].label == "Lives in");
        bool kept = false;
        for (const studio::Book* b : lib->liveBooks())
            for (auto* nd : lib->manuscriptOf(b->id)) if (nd->povId == maren && nd->targetWords == 2500) kept = true;
        CHECK(kept);
        // A world emptied from the Trash takes its entries with it.
        CHECK(lib->trashWorld(world) && lib->trashedEntries().empty() && lib->emptyTrash() && !lib->entry(maren));
    }
    fs::remove_all(dir);
}

// Editorial: comparing texts, snapshots, the writing log, name checks.
static void testReview() {
    using namespace studio;
    using review::DiffPart;
    // Comparing.
    {
        auto d = review::diffText("The cat sat on the mat.\n\nIt was warm.", "The black cat sat on the mat.\n\nIt was warm.\n\nThe end.");
        std::string added, removed, same;
        for (auto& p : d) (p.kind == DiffPart::Added ? added : p.kind == DiffPart::Removed ? removed : same) += p.text;
        CHECK(added.find("black") != std::string::npos && added.find("The end.") != std::string::npos && removed.empty());
        CHECK(same.find("cat sat on the mat.") != std::string::npos && same.find("It was warm.") != std::string::npos);
        auto st = review::stats(d);
        CHECK(st.added == 3 && st.removed == 0);
        auto d2 = review::diffText("One two three.", "One four three.");
        auto s2 = review::stats(d2);
        CHECK(s2.added == 1 && s2.removed == 1);
        CHECK(!review::stats(review::diffText("Same.\n\nText.", "Same.\n\nText.")).changed());
        CHECK(review::stats(review::diffText("", "New words here.")).added == 3);
        CHECK(review::stats(review::diffText("Gone now.", "")).removed == 2);
        // Long texts still compare.
        std::string a, b;
        for (int i = 0; i < 400; ++i) { a += "Paragraph " + std::to_string(i) + " says something.\n\n"; b += "Paragraph " + std::to_string(i) + (i % 50 ? " says something.\n\n" : " says otherwise.\n\n"); }
        auto s3 = review::stats(review::diffText(a, b));
        CHECK(s3.added == 8 && s3.removed == 8);
    }
    CHECK(review::editDistance(U"maren", U"marren", 1) == 1 && review::editDistance(U"maren", U"karen", 1) == 1 && review::editDistance(U"ada", U"zed", 1) > 1);

    const std::string dir = tempLibrary("review");
    auto lib = openOk(dir);
    const Id world = lib->createWorld("Alvora");
    const Id book = lib->createBook("Dragonfall", world);
    const Id ch = lib->createNode(book, {}, NodeKind::Chapter, "One");
    auto docOf = [](const std::string& t) {
        return std::string(R"({"type":"doc","content":[{"type":"paragraph","content":[{"type":"text","text":")") + t + R"("}]}]})";
    };
    CHECK(lib->saveNodeContent(ch, docOf("Maren walked to the harbour.")));
    // The writing log counts words added today.
    const std::string today = Library::localDay(now());
    CHECK(lib->wordsWritten(book, today)[today] == 5 && lib->wordsWritten({}, today)[today] == 5);
    // A snapshot keeps the text as it was.
    const Id snap = lib->takeSnapshot(book, "First draft");
    CHECK(!snap.empty() && lib->snapshotsOf(book).size() == 1 && lib->snapshotsOf(book)[0].words == 5 && lib->snapshotsOf(book)[0].parts == 1);
    CHECK(lib->saveNodeContent(ch, docOf("Maren walked slowly to the harbour. Then Marren saw Tobias, and Tobias waved; Tobias was late.")));
    CHECK(lib->wordsWritten(book, today)[today] > 5);
    auto sn = lib->snapshotNodes(snap);
    CHECK(sn.size() == 1 && sn[0].nodeId == ch && doc::plainText(sn[0].content) == "Maren walked to the harbour.");
    auto cmp = review::stats(review::diffText(doc::plainText(sn[0].content), doc::plainText(lib->nodeContent(ch))));
    CHECK(cmp.added > 5 && cmp.removed == 0);
    CHECK(lib->renameSnapshot(snap, "Before revision") && lib->snapshotsOf(book)[0].name == "Before revision");
    // Many saves later, autosaved versions are pruned; the snapshot isn't.
    for (int i = 0; i < 3; ++i) CHECK(lib->saveNodeContent(ch, docOf("Version " + std::to_string(i) + ".")));
    CHECK(lib->snapshotNodes(snap).size() == 1);
    // Names: a near miss of an entry's name, and a name used often that isn't in the bible.
    CHECK(lib->saveNodeContent(ch, docOf("Maren walked slowly to the harbour. Then Marren saw Tobias, and Tobias waved; Tobias was late. The Harbour was quiet.")));
    lib->createEntry(world, {}, "character", "Maren");
    auto issues = review::nameChecks(*lib, book);
    bool variant = false, unknown = false;
    for (auto& is : issues) {
        if (is.kind == review::NameIssue::Variant && is.name == "Marren" && is.like == "Maren") variant = true;
        if (is.kind == review::NameIssue::Unknown && is.name == "Tobias" && is.count == 3) unknown = true;
        CHECK(is.name != "Harbour" && is.name != "Then" && is.name != "Maren");   // written in lower case too; sentence start; known
    }
    CHECK(variant && unknown);
    CHECK(!issues.empty() && issues[0].kind == review::NameIssue::Variant);
    // A curly apostrophe in the text matches a straight one in the bible; word forms aren't misspellings;
    // and changing a name everywhere keeps the old text as a version.
    {
        CHECK(lib->saveNodeContent(ch, docOf("Then Zah\u2019mes came, and Zah\u2019mes left; Zah\u2019mes slept. The Fluxian fleet and the Elven blades. Stienriket fell.")));
        lib->createEntry(world, {}, "character", "Zah'mes");
        lib->createEntry(world, {}, "group", "Fluxians");
        lib->createEntry(world, {}, "group", "Elves");
        lib->createEntry(world, {}, "place", "Steinriket");
        bool zah = false, flux = false, elven = false, stein = false;
        for (auto& is : review::nameChecks(*lib, book)) {
            if (is.name.rfind("Zah", 0) == 0) zah = true;
            if (is.name == "Fluxian") flux = true;
            if (is.name == "Elven") elven = true;
            if (is.name == "Stienriket" && is.like == "Steinriket") stein = true;
        }
        CHECK(!zah && !flux && !elven && stein);
        std::vector<Id> changed;
        CHECK(review::replaceName(*lib, book, "Stienriket", "Steinriket", &changed) == 1 && changed.size() == 1);
        CHECK(doc::plainText(lib->nodeContent(ch)).find("Steinriket fell.") != std::string::npos);
        CHECK(!lib->nodeVersions(ch).empty());
        // Every use, whatever's around it: curly quotes, a possessive, dashes, an ellipsis; never inside another word.
        CHECK(lib->saveNodeContent(ch, docOf("\u201CMarren, wait!\u201D Marren\u2019s boat. Marren\u2014no. \u2026Marren\u2026 (Marren) Marrenson \u00C9Marren Marren\u00E9.")));
        int count = 0;
        const std::string t = review::replaceWholeWord(doc::plainText(lib->nodeContent(ch)), "Marren", "Maren", count);
        CHECK(count == 5);
        CHECK(t.find("\u201CMaren, wait!\u201D Maren\u2019s boat. Maren\u2014no. \u2026Maren\u2026 (Maren) Marrenson \u00C9Marren Marren\u00E9.") != std::string::npos);
        changed.clear();
        CHECK(review::replaceName(*lib, book, "Marren", "Maren", &changed) == 5 && changed.size() == 1);
        CHECK(doc::plainText(lib->nodeContent(ch)).find("Maren\u2019s boat") != std::string::npos);
    }
    // Front matter: kept, never numbered, and the story's first chapter is still Chapter 1.
    {
        const Id fm = lib->createNode(book, {}, NodeKind::Chapter, "Pronunciation Guide");
        ManuscriptNode u = *lib->node(fm);
        u.matter = 1;
        CHECK(lib->updateNode(u) && lib->node(fm)->matter == 1);
        CHECK(lib->moveNode(fm, {}, 0));
        auto src = typeset::snapshot(*lib, book);
        bool found = false;
        for (auto& sec : src.sections) if (sec.title == "Pronunciation Guide") found = sec.matter == 1;
        CHECK(found);
        u = *lib->node(fm);
        u.kind = NodeKind::Part;
        CHECK(lib->updateNode(u) && lib->node(fm)->matter == 0);   // only chapters can be front matter
    }
    // Review copies: one HTML file for a reader; their comments come back onto the right words.
    {
        const Id rc = lib->createNode(book, {}, NodeKind::Chapter, "Review");
        CHECK(lib->saveNodeContent(rc, docOf("The wind came off the sea. The wind </script> was cold. Then the wind dropped.")));
        std::ifstream tf(appFolder("web") + "/review-copy.html", std::ios::binary);
        std::stringstream ts;
        ts << tf.rdbuf();
        std::string err;
        const std::string html = review::makeReviewCopy(*lib, book, ts.str(), err);
        CHECK(err.empty() && html.find("Dragonfall") != std::string::npos && html.find("{{DATA}}") == std::string::npos);
        const size_t d0 = html.find("id=\"data\">"), d1 = html.find("</script>", d0);
        CHECK(d0 != std::string::npos && d1 != std::string::npos);
        const std::string payload = html.substr(d0 + 10, d1 - d0 - 10);
        CHECK(payload.find("</") == std::string::npos);                       // nothing can close the script early
        const auto pj = nlohmann::json::parse(payload, nullptr, false);
        CHECK(pj.is_object() && pj["chapters"].size() >= 1 && pj["book"] == book);
        // The reader commented on the second "wind" (told apart by what's around it), on words since
        // changed, and on the whole chapter.
        const nlohmann::json file = {{"format", "nrcomments"}, {"book", book}, {"reader", "Jo"}, {"comments", {
            {{"node", rc}, {"quote", "wind"}, {"prefix", "The "}, {"suffix", " </script> was"}, {"text", "Which wind?"}},
            {{"node", rc}, {"quote", "a purple heron"}, {"prefix", ""}, {"suffix", ""}, {"text", "Lovely."}},
            {{"node", rc}, {"quote", ""}, {"text", "Great chapter."}},
            {{"node", "gone"}, {"quote", "x"}, {"text", "?"}}}}};
        const auto rep = review::importReviewComments(*lib, book, file.dump());
        CHECK(rep.error.empty() && rep.reader == "Jo" && rep.placed == 1 && rep.unplaced == 2 && rep.skipped == 1);
        const std::string after = lib->nodeContent(rc);
        const auto dj = nlohmann::json::parse(after);
        // The mark sits on the second "wind" only.
        std::string marked;
        int seen = 0;
        for (auto& t : dj["content"][0]["content"]) {
            if (t.contains("marks")) marked += t["text"].get<std::string>();
            ++seen;
        }
        CHECK(marked == "wind" && seen >= 3);
        CHECK(doc::plainText(after).find("The wind came off the sea. The wind </script> was cold.") != std::string::npos);   // text unchanged
        int fromJo = 0;
        for (auto* c : lib->commentsOf(rc)) if (c->body.rfind("Jo: ", 0) == 0) ++fromJo;
        CHECK(fromJo == 3);
        CHECK(!review::importReviewComments(*lib, book, R"({"format":"nrcomments","book":"other","comments":[]})").error.empty());
        std::vector<Id> ids;
        for (auto* c : lib->commentsOf(rc)) ids.push_back(c->id);
        for (auto& id : ids) lib->removeComment(id, true);   // the comment tests below count the book's
    }
    CHECK(lib->removeSnapshot(snap) && lib->snapshotsOf(book).empty());
    // Comments: a mark in the text, the comment beside it.
    {
        const std::string cid = "0123456789abcdef";
        const std::string withMark = R"({"type":"doc","content":[{"type":"paragraph","content":[{"type":"text","text":"Maren "},{"type":"text","marks":[{"type":"comment","attrs":{"id":")" + cid + R"("}},{"type":"comment","attrs":{"id":"fedcba9876543210"}},{"type":"em"},{"type":"comment","attrs":{"id":"<bad>"}}],"text":"walked slowly"},{"type":"text","text":" home."}]}]})";
        CHECK(lib->saveNodeContent(ch, withMark));
        const std::string stored = lib->nodeContent(ch);
        CHECK(stored.find(cid) != std::string::npos && stored.find("fedcba9876543210") != std::string::npos && stored.find("<bad>") == std::string::npos);
        CHECK(lib->addComment(ch, cid, "walked slowly") && lib->updateComment(cid, "Too slow? Cut it."));
        CHECK(!lib->addComment(ch, "not-hex!", "x") && !lib->addComment("nowhere", "aaaaaaaaaaaaaaaa", "x"));
        CHECK(lib->addComment(ch, "fedcba9876543210", "walked slowly"));
        // Comments never reach what's published: the ebook has the words, not the marks.
        const std::string html = ebook::documentHtml(stored, [](const std::string&) { return std::string(); });
        CHECK(html.find("walked slowly") != std::string::npos && html.find(cid) == std::string::npos && html.find("comment") == std::string::npos);
        CHECK(html.find("<em>walked slowly</em>") != std::string::npos);
        CHECK(lib->commentsOf(ch).size() == 2 && lib->commentsInBook(book).size() == 2 && lib->comment(cid)->body == "Too slow? Cut it.");
        CHECK(lib->commentAnchored(*lib->comment(cid)));
        // Resolving takes the mark out of the text (when the editor isn't doing it), and keeps the comment.
        CHECK(lib->resolveComment(cid, true, true) && lib->comment(cid)->resolved != 0);
        CHECK(lib->nodeContent(ch).find(cid) == std::string::npos && lib->nodeContent(ch).find("fedcba9876543210") != std::string::npos);
        CHECK(!lib->commentAnchored(*lib->comment(cid)) && lib->nodeContent(ch).find("\"em\"") != std::string::npos);
        CHECK(lib->resolveComment(cid, false, false) && lib->comment(cid)->resolved == 0);
        CHECK(lib->removeComment("fedcba9876543210", true) && !lib->comment("fedcba9876543210") && lib->commentsOf(ch).size() == 1);
        CHECK(lib->nodeContent(ch).find("comment") == std::string::npos);
        lib.reset();
        lib = openOk(dir);
        CHECK(lib->comment(cid) && lib->comment(cid)->body == "Too slow? Cut it." && lib->schemaVersion() == Library::kSchemaVersion);
    }
    lib.reset();
    fs::remove_all(dir);
}

// Backups: a complete copy that opens as a library, its path reported, and changes counted.
static void testBackup() {
    const std::string dir = tempLibrary("backup");
    auto lib = openOk(dir);
    const long long before = lib->changesMade();
    const Id b = lib->createBook("Backed Up", {});
    CHECK(lib->changesMade() > before);
    std::string err, made;
    CHECK(lib->backup(err, 3, &made));
    CHECK(!made.empty() && fs::exists(fs::path(made)));
    // The copy opens as a library and holds the book.
    const std::string copyDir = tempLibrary("backup-copy");
    fs::create_directories(copyDir);
    fs::copy_file(fs::path(made), fs::path(copyDir) / "library.db");
    {
        auto copy = openOk(copyDir);
        CHECK(copy && copy->book(b) && copy->book(b)->title == "Backed Up");
    }
    // Only the newest `keep` are kept.
    for (int i = 0; i < 5; ++i) CHECK(lib->backup(err, 3));
    int n = 0;
    for (auto& e : fs::directory_iterator(fs::path(dir) / "backups")) if (e.path().extension() == ".db") ++n;
    CHECK(n == 3);
    lib.reset();
    fs::remove_all(dir);
    fs::remove_all(copyDir);
}

static void testConlang() {
    namespace cl = studio::conlang;
    std::vector<cl::Language> out;
    std::string err;
    // CSV with a header (columns in another order, quotes, a comma inside a meaning, accents, a BOM).
    const std::string csv = "\xEF\xBB\xBF" "English,Conword,Part of speech,IPA\r\n"
                            "light,veyth'ar,noun,/veɪθ.ar/\r\n"
                            "\"river, stream\",sel-amar,noun,\r\n"
                            "\"to \"\"see\"\"\",ómë,verb,\r\n"
                            ",,,\r\n"
                            "plural,-in,suffix,\r\n"
                            "again,ve-,prefix,\r\n";
    CHECK(cl::importFile("C:\\words\\Telvani.csv", csv, out, err) && out.size() == 1);
    const cl::Language t = out[0];   // a copy: `out` is used again below
    CHECK(t.name == "Telvani" && t.words.size() == 5);
    CHECK(t.words[0].word == "veyth'ar" && t.words[0].meaning == "light" && t.words[0].part == "noun" && t.words[0].pronunciation == "/veɪθ.ar/");
    CHECK(t.words[1].meaning == "river, stream" && t.words[2].word == "ómë" && t.words[2].meaning == "to \"see\"");
    const cl::SpellData sd = cl::spellData(t);
    auto has = [](const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); };
    CHECK(has(sd.words, "veyth'ar") && has(sd.words, "sel-amar") && has(sd.words, "sel") && has(sd.words, "amar") && has(sd.words, "ómë"));
    CHECK(has(sd.suffixes, "in") && has(sd.prefixes, "ve") && !has(sd.words, "-in") && !has(sd.words, "ve-"));
    // Semicolons, no header: word, meaning, part.
    CHECK(cl::importFile("dwarvish.csv", "khaz;mountain;noun\nbaruk;axe;noun\n", out, err) && out[0].words.size() == 2 && out[0].words[1].meaning == "axe" &&
          out[0].words[1].part == "noun" && out[0].name == "dwarvish");
    // TSV.
    CHECK(cl::importFile("x.tsv", "word\tgloss\nalpha\tfirst\n", out, err) && out[0].words.size() == 1 && out[0].words[0].meaning == "first");
    // SFM (Toolbox / Lexique Pro).
    const std::string sfm = "\\_sh v3.0  400  MDF 4.0\n\n\\lx veyth'ar\n\\ps n\n\\ge light\n\\de the light of dawn\n\\nt common in songs\n\n\\lx amar\n\\ge water\n"
                            "\\ge river\n";
    CHECK(cl::importFile("lexicon.db", sfm, out, err) && out[0].words.size() == 2);
    CHECK(out[0].words[0].word == "veyth'ar" && out[0].words[0].part == "n" && out[0].words[0].meaning == "light" && out[0].words[0].notes == "common in songs");
    CHECK(out[0].words[1].meaning == "water; river");
    CHECK(cl::importFile("noext", sfm, out, err) && out[0].words.size() == 2);   // sniffed
    // Plain lists.
    CHECK(cl::importFile("list.txt", "# my words\nkhaz = mountain\nbaruk - axe\nzirak\nmenu: gate\n", out, err) && out[0].words.size() == 4);
    CHECK(cl::importFile("h.txt", "Word = Meaning\nkhaz = mountain\n", out, err) && out[0].words.size() == 1);
    CHECK(cl::importFile("list.txt", "# my words\nkhaz = mountain\nbaruk - axe\nzirak\nmenu: gate\n", out, err));
    CHECK(out[0].words[0].meaning == "mountain" && out[0].words[1].meaning == "axe" && out[0].words[2].word == "zirak" && out[0].words[3].meaning == "gate");
    // UTF-16 with a BOM.
    {
        std::string u16 = "\xFF\xFE";
        for (char c : std::string("khaz\r\n")) u16 += c, u16 += '\0';
        CHECK(cl::importFile("u.txt", u16, out, err) && out[0].words.size() == 1 && out[0].words[0].word == "khaz");
    }
    // Our own file, round trip; and a file with several languages.
    cl::Language back;
    {
        std::vector<cl::Language> again;
        CHECK(cl::importFile("Telvani.nrlang", cl::exportJson(t), again, err) && again.size() == 1);
        back = again[0];
        CHECK(back.name == "Telvani" && back.words == t.words);
    }
    CHECK(cl::importFile("two.json", R"({"languages":[{"name":"A","words":["x","y"]},{"name":"B","words":[{"word":"z","meaning":"zed"}]}]})", out, err) &&
          out.size() == 2 && out[1].words[0].meaning == "zed");
    // CSV export reads back the same.
    {
        std::vector<cl::Language> again;
        CHECK(cl::importFile("Telvani.csv", cl::exportCsv(t), again, err) && again[0].words == t.words);
    }
    // Nothing readable.
    CHECK(!cl::importFile("empty.csv", "", out, err) && !err.empty());
    CHECK(!cl::importFile("bad.nrlang", "{nope", out, err));
    CHECK(!cl::importFile("none.json", R"({"name":"X","words":[]})", out, err));
    // Kept in settings: ids, names required, junk skipped.
    std::vector<cl::Language> langs{t};
    langs[0].id = cl::newId({});
    cl::Language d;
    d.name = "Dwarvish";
    d.id = cl::newId(langs);
    langs.push_back(d);
    CHECK(langs[0].id == "lang1" && langs[1].id == "lang2");
    const auto kept = cl::fromJson(cl::toJson(langs));
    CHECK(kept == langs);
    CHECK(cl::fromJson("junk").empty() && cl::fromJson(R"({"languages":[{"name":""},{"name":"Ok","id":"a"},{"name":"Dup","id":"a"}]})").size() == 2);
    // Merging: new words added, gaps filled, nothing doubled.
    cl::Language m;
    m.words = {{"khaz", "", "", "", ""}};
    cl::Language inc;
    inc.words = {{"KHAZ", "mountain", "noun", "", ""}, {"baruk", "axe", "", "", ""}};
    CHECK(cl::merge(m, inc) == 1 && m.words.size() == 2 && m.words[0].meaning == "mountain" && m.words[0].word == "khaz");
}

static std::string pasteFixture(const char* name) {
    std::ifstream f(fixture((std::string("paste/") + name).c_str()), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// A world's own typefaces (0.40): imported into the library, grouped by family, used by covers, the bible website and
// the font lists, while the app's own stay first; fonts that can't be printed refused with a reason.
static void testWorldFonts() {
    namespace nf = studio::fonts;
    const std::string dir = tempLibrary("fonts");
    auto lib = openOk(dir);
    if (!lib) return;
    const Id world = lib->createWorld("Alvora");
    lib->createEntry(world, {}, "character", "Ada Wren");
    // Source files, as an author's download folder would have them.
    const fs::path src = fs::path(dir) / "download";
    fs::create_directories(src);
    const fs::path app = appFolder("fonts");
    for (const char* s : {"Regular", "Italic", "Bold", "BoldItalic"})
        fs::copy_file(app / "crimson-text" / (std::string("CrimsonText-") + s + ".ttf"), src / (std::string("Crimson ") + s + ".ttf"));
    fs::copy_file(app / "crimson-text" / "OFL.txt", src / "OFL.txt");
    { std::ofstream(src / "Fancy.otf", std::ios::binary) << std::string("OTTO\0\x0b\0\x80\0\x03\0\x30", 12) << std::string(200, '\0'); }
    { std::ofstream(src / "web.woff", std::ios::binary) << "wOFF" << std::string(200, '\0'); }
    { std::ofstream(src / "notes.ttf", std::ios::binary) << "not a font at all, just words"; }

    // What a file is.
    {
        std::ifstream in(app / "crimson-text" / "CrimsonText-BoldItalic.ttf", std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const nf::FaceInfo info = nf::inspect(bytes);
        CHECK(info.problem.empty() && info.family == "Crimson Text" && info.style == "BoldItalic" && info.weight >= 600 && !info.variable);
        CHECK(nf::inspect("OTTO" + std::string(40, '\0')).problem.find("PostScript") != std::string::npos);
        CHECK(nf::inspect("wOF2" + std::string(40, '\0')).problem.find("web font") != std::string::npos);
    }

    const std::string mine = nf::scopeFolder(dir, world, {});
    CHECK(mine.find("world-" + world) != std::string::npos && nf::scopeFolder(dir, {}, "b1").find("book-b1") != std::string::npos);
    std::vector<std::string> files;
    for (auto& e : fs::directory_iterator(src)) if (e.path().extension() != ".txt") files.push_back(e.path().string());
    std::sort(files.begin(), files.end());
    std::vector<std::string> bundled;
    for (auto& f : typeset::fontFamilies(app.string())) bundled.push_back(f.id);
    const nf::ImportResult r = nf::importFiles(mine, files, bundled);
    CHECK(r.added.size() == 1 && r.added[0] == "Crimson Text");
    CHECK(r.problems.size() == 3);   // the CFF font, the web font, the text file
    const auto fams = nf::familiesIn(mine);
    CHECK(fams.size() == 1);
    std::string id;
    if (fams.size() == 1) {
        id = fams[0].id;
        CHECK(id == "crimson-text-2" && fams[0].name == "Crimson Text" && fams[0].styles.size() == 4 && fams[0].styles[0] == "Regular" && fams[0].licence);
    }
    // The font lists: the app's first, the world's after; a family is found in whichever folder has it.
    const std::string list = nf::joinFolders({app.string(), mine});
    CHECK(nf::folders(list).size() == 2 && nf::folders("a||b|").size() == 2);
    bool appFirst = false, worldToo = false;
    for (auto& f : typeset::fontFamilies(list)) {
        if (f.id == "crimson-text") appFirst = true;
        if (f.id == id && f.name == "Crimson Text") worldToo = true;
    }
    CHECK(appFirst && worldToo);
    CHECK(nf::familyFolder(list, "crimson-text").find("apps") != std::string::npos && nf::familyFolder(list, id).find("world-") != std::string::npos);
    CHECK(nf::familyFolder(list, "../crimson-text").empty() && nf::familyFolder(list, "nothing").empty());
    // A cover in the world's own typeface (and the symbols still found in the app's folder).
    {
        cover::Spec sp;
        sp.title = "Shards of Light \xE2\x9C\xA7";
        sp.author = "A. Writer";
        sp.fontId = id;
        const auto c = cover::render(sp, list, 400, 640);
        CHECK(c.ok && !c.png.empty());
    }
    // The bible website with it for headings: the files and the licence copied, @font-face written.
    {
        bible::Look look;
        look.headings = "family:" + id;
        const std::string out = dir + "/site";
        std::string err;
        int pages = 0;
        CHECK(bible::exportWebsite(*lib, {world, {}, false, {}}, out, err, &pages, look, list));
        std::ifstream in(out + "/style.css");
        const std::string css((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(css.find("\"NR " + id + "\"") != std::string::npos && css.find("fonts/" + id + "/") != std::string::npos);
        bool ttf = false;
        for (auto& e : fs::directory_iterator(out + "/fonts/" + id)) if (e.path().extension() == ".ttf") ttf = true;
        CHECK(ttf && fs::exists(out + "/fonts/" + id + "/OFL.txt"));
    }
    // A preview line.
    {
        const nf::Sample sm = nf::sample(list, id, "Italic", "The light broke", 28, 0x203040);
        bool ink = false;
        for (size_t i = 3; i < sm.rgba.size(); i += 4) if (sm.rgba[i] > 128) { ink = true; break; }
        CHECK(sm.w > 100 && sm.h > 20 && ink && sm.rgba[0] == 0x20);
        CHECK(nf::sample(list, "nothing", "Regular", "x", 20, 0).w == 0);
    }
    // Importing the same family again replaces its styles in place (no second family); renaming; removing.
    {
        const nf::ImportResult again = nf::importFiles(mine, {files[0]}, bundled);
        CHECK(again.added.size() == 1 && nf::familiesIn(mine).size() == 1 && nf::familiesIn(mine)[0].styles.size() == 4);
        CHECK(nf::renameFamily(mine, id, "Crimson (Alvora)") && nf::familiesIn(mine)[0].name == "Crimson (Alvora)");
        std::string err;
        CHECK(nf::removeFamily(mine, id, err) && nf::familiesIn(mine).empty());
        CHECK(!nf::removeFamily(mine, id, err) && !err.empty());
    }
    // A world typeface on words in a chapter (runes, handwriting: 0.41): kept when saved, set in that face on the pages,
    // in a span with its own @font-face in the ebook; a missing one falls back to the book's with a warning.
    {
        const nf::ImportResult again = nf::importFiles(mine, {(app / "gentium-book-plus" / "GentiumBookPlus-Italic.ttf").string()}, bundled);
        CHECK(again.added.size() == 1);
        std::string runes;
        for (auto& f : nf::familiesIn(mine)) if (f.name == "Gentium Book Plus") runes = f.id;
        CHECK(runes == "gentium-book-plus-2");
        const std::string docJson = std::string(R"({"type":"doc","content":[{"type":"paragraph","content":[{"type":"text","text":"The door read "},)") +
                                    R"({"type":"text","text":"Veyth ar sel","marks":[{"type":"font","attrs":{"family":")" + runes + R"("}}]},)" +
                                    R"({"type":"text","text":" in old letters.","marks":[{"type":"font","attrs":{"family":"../evil"}}]}]}]})";
        const std::string clean = doc::sanitise(docJson);
        CHECK(clean.find("\"family\":\"" + runes + "\"") != std::string::npos && clean.find("evil") == std::string::npos);
        typeset::Source src;
        src.title = "Runes";
        src.author = "A. Writer";
        typeset::Source::Section sec;
        sec.title = "One";
        sec.content = clean;
        src.sections.push_back(sec);
        typeset::Design d;
        const auto book = typeset::typesetBook(src, d, {list, appFolder("hyphenation")});
        CHECK(book.ok);
        bool usedWorld = false;
        for (auto& f : book.fonts) if (f.path.find("world-") != std::string::npos) usedWorld = true;
        CHECK(usedWorld);
        const std::string gone = std::string(R"({"type":"doc","content":[{"type":"paragraph","content":[{"type":"text","text":"Lost","marks":[{"type":"font","attrs":{"family":"gone-font"}}]}]}]})");
        typeset::Source src2 = src;
        src2.sections[0].content = gone;
        const auto book2 = typeset::typesetBook(src2, d, {list, appFolder("hyphenation")});
        bool warned = false;
        for (auto& w : book2.warnings) if (w.find("gone-font") != std::string::npos) warned = true;
        CHECK(book2.ok && warned);
        CHECK(studio::ebook::fontsUsed(src).size() == 1 && studio::ebook::fontsUsed(src)[0] == runes);
        const auto html = studio::ebook::build(src, [](const std::string&) { return std::string(); });
        CHECK(!html.chapters.empty() && html.chapters.back().html.find("<span class=\"ff ff-" + runes + "\">Veyth ar sel</span>") != std::string::npos);
        const std::string css = studio::ebook::worldFontCss(list, runes, [](const std::string& p) { return "u/" + fs::path(p).filename().string(); });
        CHECK(css.find("NR F " + runes) != std::string::npos && css.find("span.ff-" + runes) != std::string::npos);
        publish::EpubOptions eo;
        eo.fontsFolder = list;   // no book typeface: just the words' one
        const auto ep = publish::buildEpub(src, eo);
        CHECK(ep.ok);
        if (ep.ok) {
            mz_zip_archive za{};
            CHECK(mz_zip_reader_init_mem(&za, ep.bytes.data(), ep.bytes.size(), 0));
            bool font = false, licence = false;
            std::string style;
            for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&za); ++i) {
                char name[512];
                mz_zip_reader_get_filename(&za, i, name, sizeof name);
                const std::string n = name;
                if (n.rfind("OEBPS/fonts/" + runes + "/", 0) == 0 && n.size() > 4 && n.substr(n.size() - 4) == ".ttf") font = true;
                if (n == "OEBPS/fonts/" + runes + "/OFL.txt") licence = true;
                if (n == "OEBPS/style.css") {
                    size_t sz = 0;
                    void* p = mz_zip_reader_extract_to_heap(&za, i, &sz, 0);
                    if (p) { style.assign((const char*)p, sz); mz_free(p); }
                }
            }
            mz_zip_reader_end(&za);
            CHECK(font && licence && style.find("NR F " + runes) != std::string::npos);
        }
        std::string err;
        CHECK(nf::removeFamily(mine, runes, err));
    }
    // Two weights for one style: the one nearer regular kept, the other reported.
    {
        const fs::path two = fs::path(dir) / "two";
        fs::create_directories(two);
        // Gentium's Regular and Crimson's Regular are different families; a second Gentium "regular" is its Bold renamed?
        // Use the same file twice under two names: the same weight, so the first stays and the second is reported.
        fs::copy_file(app / "gentium-book-plus" / "GentiumBookPlus-Regular.ttf", two / "a.ttf");
        fs::copy_file(app / "gentium-book-plus" / "GentiumBookPlus-Regular.ttf", two / "b.ttf");
        const nf::ImportResult g = nf::importFiles(mine, {(two / "a.ttf").string(), (two / "b.ttf").string()}, bundled);
        CHECK(g.added.size() == 1 && g.problems.size() == 1 && nf::familiesIn(mine).size() == 1);
        if (!nf::familiesIn(mine).empty()) CHECK(nf::familiesIn(mine)[0].id == "gentium-book-plus-2" && !nf::familiesIn(mine)[0].licence);
    }
}

static void testEntryPaste() {
    using namespace studio;
    namespace pa = studio::paste;
    // Choice sections in a template.
    {
        const SectionSpec s = sectionSpec("Status: Alive | Dead | Missing | dead");
        CHECK(s.title == "Status" && s.options.size() == 3 && !s.many);
        CHECK(toggleSectionOption(s, "", "Dead") == "Dead" && toggleSectionOption(s, "Dead", "Dead").empty() && toggleSectionOption(s, "Dead", "Alive") == "Alive");
        CHECK(sectionOptionPicked(s, " dead ", "Dead"));
        const SectionSpec m = sectionSpec("Traits (any): Brave | Loyal | Proud");
        CHECK(m.title == "Traits" && m.many && m.options.size() == 3);
        CHECK(toggleSectionOption(m, "", "Proud") == "Proud");
        CHECK(toggleSectionOption(m, "Proud, stubborn", "Brave") == "Brave, Proud, stubborn");
        CHECK(toggleSectionOption(m, "Brave, Proud", "Brave") == "Proud");
        CHECK(sectionSpec("Age, birth & status").options.empty() && sectionSpec("Age, birth & status").title == "Age, birth & status");
        CHECK(sectionSpec("Status: alive").options.empty() && sectionSpec("Only | one:").options.empty());
    }
    // Parsing the two shapes: Markdown (Tom) and plain (Sarrah), and pages without blank lines (Light Magic).
    const pa::Page tom = pa::parse(pasteFixture("tom.txt"));
    CHECK(tom.name == "Thomas Terrance Smith");
    CHECK(tom.aliases.size() == 12 && tom.aliases[0] == "Tom" && tom.aliases.back() == "my master's scion");
    CHECK(tom.pronunciation == "TOM-us TERR-ence SMITH");
    CHECK(tom.summary.rfind("A young blacksmith", 0) == 0);
    CHECK(tom.details.find("Eyes: forest green") != std::string::npos && tom.details.find("Here's") == std::string::npos);
    CHECK(tom.sections.size() == 4 && tom.sections[0].first == "Appearance" && tom.sections[3].first == "Abilities");
    CHECK(tom.sections[2].second.find("\n\n") != std::string::npos);   // History's paragraphs kept
    CHECK(tom.bookNotes.size() == 3 && tom.bookNotes[0].first == "Shards of Light (Book 1)" && tom.bookNotes[2].first == "Book 5");
    CHECK(tom.bookNotes[0].second.rfind("\xE2\x80\xA2 Returning", 0) == 0 && tom.bookNotes[1].second.rfind("Tom stabs", 0) == 0);
    CHECK(tom.relations.size() == 16 && tom.relations[0].first == "Parents");
    CHECK(tom.notes.find("Spoilers:") == 0 && tom.notes.find("chapter 5") != std::string::npos);
    const pa::Page sar = pa::parse(pasteFixture("sarrah.txt"));
    CHECK(sar.name == "Sah\xE2\x80\x99ra Jane Crystalline" && sar.aliases.size() == 14 && sar.pronunciation == "SAH-rah");
    CHECK(sar.sections.size() == 4 && sar.sections[1].first == "Personality");
    CHECK(sar.sections[3].second.find("fireballs, and a flame spiral") != std::string::npos);
    CHECK(sar.bookNotes.size() == 2 && sar.bookNotes[1].first == "Book 5");
    CHECK(sar.relations.size() == 11 && sar.details.find("POV symbol") != std::string::npos);
    const pa::Page light = pa::parse(pasteFixture("light.txt"));
    CHECK(light.sections.size() == 3 && light.sections[0].first == "How it works" && light.sections[2].first == "History");
    CHECK(light.sections[0].second.find("many forms") != std::string::npos);
    CHECK(light.bookNotes.size() == 2 && light.relations.size() == 5);
    const pa::Page tel = pa::parse(pasteFixture("telvana.txt"));
    CHECK(tel.sections.size() == 2 && tel.bookNotes.size() == 1 && tel.bookNotes[0].second.find("\xE2\x80\xA2 In Ursvald") != std::string::npos);
    CHECK(!pa::looksLikePage("Just a sentence.") && pa::looksLikePage("Name: Ada\nSummary\nA smith."));
    CHECK(!pa::looksLikePage("Toby\n- Tommy\n\nThe Watchman\n"));   // a list, not a page
    CHECK(!pa::looksLikePage("Early years\nShe grew up by the sea.\n\nLater\nShe left."));   // a section with its own headings
    CHECK(pa::relationFor("character", "Parents") == "Child of" && pa::relationFor("character", "Best friend") == "Friend of");
    CHECK(pa::relationFor("character", "Haunted by").empty() && pa::relationFor("place", "Owned by") == "Owned by");
    CHECK(guessReverseRelation("Admires") == "Admired by" && guessReverseRelation("Haunted by") == "Haunts" && guessReverseRelation("Guided by") == "Guides");
    CHECK(guessReverseRelation("Deceived by") == "Deceives" && guessReverseRelation("Teaches") == "Taught by" && guessReverseRelation("Guardian of") == "Has guardian");
    CHECK(guessReverseRelation("Watches over") == "Watched over by" && guessReverseRelation("Stabbed by") == "Stabs" && guessReverseRelation("Twin") == "Twin");
    CHECK(guessReverseRelation("Has guardian") == "Guardian of" && guessReverseRelation("Carries") == "Carried by");
    CHECK(pa::relationFor("magic", "Used by") == "Practised by" && pa::relationFor("character", "Used by").empty());

    // Putting it on entries.
    const std::string dir = tempLibrary("paste");
    auto lib = openOk(dir);
    const Id world = lib->createWorld("Alvora");
    const Id b1 = lib->createBook("Shards of Light", world);
    const Id b2 = lib->createBook("The Reign of Zornov", world);
    const Id ralph = lib->createEntry(world, {}, "character", "Ralph Smith");
    const Id jim = lib->createEntry(world, {}, "character", "Jim Armstrong");
    const Id sarrah = lib->createEntry(world, {}, "character", "Sah'ra Jane Crystalline");
    {
        Entry s = *lib->entry(sarrah);
        s.aliases = {"Sarrah"};
        CHECK(lib->updateEntry(s));
    }
    const Id t = lib->createEntry(world, {}, "character", "New character");
    {
        Entry x = *lib->entry(t);
        x.details = "Eyes: blue\nHeight: tall";
        x.setSection("Abilities & powers", "Swordsman.");
        x.setSection("Backstory & goals", "Kept: the paste has no such section.");
        CHECK(lib->updateEntry(x));
    }
    const Id oldFriend = lib->createEntry(world, {}, "character", "Old Friend");
    const Id pupil = lib->createEntry(world, {}, "character", "A Pupil");
    CHECK(lib->addLink(t, oldFriend, "Friend of") && lib->addLink(t, pupil, "Mentor of"));
    pa::Report r;
    CHECK(pa::apply(*lib, t, tom, {b1, b2}, r));
    CHECK(lib->entry(t)->section("Backstory & goals") == "Kept: the paste has no such section.");
    {
        const auto ls = lib->linksOf(t);
        CHECK(std::none_of(ls.begin(), ls.end(), [&](const EntryLink& l) { return l.to == oldFriend; }));   // friends replaced
        CHECK(std::any_of(ls.begin(), ls.end(), [&](const EntryLink& l) { return l.to == pupil && l.label == "Mentor of"; }));   // not named: kept
    }
    const Entry& e = *lib->entry(t);
    CHECK(e.name == "Thomas Terrance Smith" && r.name && e.aliases.size() == 12);
    CHECK(e.details.find("Eyes: forest green") != std::string::npos && e.details.find("Eyes: blue") == std::string::npos);
    CHECK(e.details.find("Height: tall") == std::string::npos);   // the pasted details replace the old ones
    CHECK(e.section("Appearance & personality").find("brown hair") != std::string::npos);
    CHECK(e.section("Appearance & personality").find("warm, loyal") != std::string::npos);
    CHECK(e.section("Abilities & powers").rfind("Tom is a trained", 0) == 0 && e.section("Abilities & powers").find("Swordsman.") == std::string::npos);
    CHECK(e.section("History").find("Silverbrick") != std::string::npos);
    CHECK(e.bookNote(b1).find("Gir-Garhm") != std::string::npos && e.bookNote(b2).rfind("Tom stabs", 0) == 0);
    // A book not in Writing Studio yet: its note is kept under its label until it is (0.56).
    CHECK(e.bookNote("title:Book 5").rfind("Tom is the father", 0) == 0 && e.notes.find("Tom is the father") == std::string::npos && r.unmatchedBooks.size() == 1);
    // Links to entries in the bible (by name or other name); the rest stay as details.
    const auto links = lib->linksOf(t);
    auto linked = [&](const Id& to, const char* label) {
        return std::any_of(links.begin(), links.end(), [&](const EntryLink& l) { return l.from == t && l.to == to && l.label == label; });
    };
    CHECK(linked(ralph, "Child of") && linked(jim, "Friend of") && linked(sarrah, "Partner of"));
    // Those not in the bible yet are made (Maria, Astrid, Celos, Bluebolt, Telvana...) and linked too.
    {
        const Entry* maria = lib->findEntryNamed(world, {}, "Maria Susan Smith");
        CHECK(maria && maria->kind == "character" && linked(maria->id, "Child of"));
        const Entry* home = lib->findEntryNamed(world, {}, "Silverbrick Lane");
        CHECK(home && home->kind == "place" && linked(home->id, "Lives in"));
        CHECK(r.created.size() >= 12 && r.links >= 13 && lib->findEntryNamed(world, {}, "Human", "race"));
    }
    CHECK(e.details.find("Parents:") == std::string::npos && e.details.find("Ralph") == std::string::npos);
    // A relationship the bible doesn't have is one of the world's own, linked like the rest.
    {
        const Entry* mimic = lib->findEntryNamed(world, {}, "the Mimic");
        CHECK(mimic && linked(mimic->id, "Haunted by") && e.details.find("Haunted by") == std::string::npos);
        CHECK(lib->relationReverse(world, {}, "Haunted by") == "Haunts");
        bool listed = false;
        for (auto& c : lib->customRelations(world, {})) listed |= c.label == "Haunted by" && c.reverse == "Haunts";
        CHECK(listed);
    }
    CHECK(r.words().find("the name, 12 other names") == 0);
    // Pasting again changes nothing and adds no links.
    pa::Report again;
    CHECK(pa::apply(*lib, t, tom, {b1, b2}, again));
    CHECK(again.created.empty() && again.links == 0 && again.aliases == 0 && again.sections == 0 && again.details == 0 && again.bookNotes == 0 && !again.summary);
    // A named entry keeps its name; the pasted one becomes another name.
    const Id s2 = lib->createEntry(world, {}, "magic", "Light");
    pa::Report r2;
    CHECK(pa::apply(*lib, s2, light, {b1, b2}, r2));
    CHECK(lib->entry(s2)->name == "Light Magic" && lib->entry(s2)->aliases.size() == 2 && lib->entry(s2)->aliases[0] == "Light");
    CHECK(lib->entry(s2)->section("How it works").find("many forms") != std::string::npos);
    // A choice section from the template is still a section a paste can fill.
    CHECK(lib->setEntryTemplate(world, "place", parseEntryTemplate("Description\nStatus: Thriving | Ruined | Lost\nHistory")));
    const Id tv = lib->createEntry(world, {}, "place", "New place");
    CHECK(lib->entrySections(*lib->entry(tv)).size() == 3 && lib->entrySections(*lib->entry(tv))[1].first == "Status");
    CHECK(lib->entrySectionSpec(*lib->entry(tv), "Status").options.size() == 3);
    pa::Report r3;
    CHECK(pa::apply(*lib, tv, tel, {b1, b2}, r3));
    // A book's title that isn't in the library isn't matched by its number.
    {
        const pa::Page odd = pa::parse("Name: X\nNotes per book\nThe Lost Tome (Book 1). Something.\nBook 2. Other.\n");
        const Id xe = lib->createEntry(world, {}, "thing", "New thing");
        pa::Report rx;
        CHECK(pa::apply(*lib, xe, odd, {b1, b2}, rx));
        CHECK(lib->entry(xe)->bookNote(b1).empty() && lib->entry(xe)->bookNote(b2) == "Other." && rx.unmatchedBooks.size() == 1);
        CHECK(lib->entry(xe)->bookNote("title:The Lost Tome (Book 1)") == "Something.");
    }
    // Aaliyah: names written more fully than the bible's, with "the", a kind of relationship said another way.
    {
        const Id yon = lib->createEntry(world, {}, "character", "Yonvaan Yerasuun");
        const Id ship = lib->createEntry(world, {}, "group", "Grey Wanderer");
        const Id akunda = lib->createEntry(world, {}, "place", "Akunda");
        const Id aa = lib->createEntry(world, {}, "character", "New character");
        const pa::Page ap = pa::parse(pasteFixture("aaliyah.txt"));
        pa::Report ar;
        CHECK(pa::apply(*lib, aa, ap, {b1, b2}, ar));
        const auto ls = lib->linksOf(aa);
        auto has = [&](const Id& to, const char* label) {
            return std::any_of(ls.begin(), ls.end(), [&](const EntryLink& l) { return l.from == aa && l.to == to && l.label == label; });
        };
        CHECK(has(yon, "Child of") && has(ship, "Member of") && has(akunda, "Lives in") && has(sarrah, "Student of"));
        const Entry* ramus = lib->findEntryNamed(world, {}, "Ramus Yerasuun");
        CHECK(ramus && has(ramus->id, "Family"));
        CHECK(lib->findEntryNamed(world, {}, "Human", "race") && lib->findEntryNamed(world, {}, "Mortal world", "realm"));   // made from the details
        CHECK(has(jim, "Admires") && lib->entry(aa)->details.find("Admires") == std::string::npos);
        CHECK(lib->relationReverse(world, {}, "Admires") == "Admired by");
        // Renaming a kind of relationship renames its links; its uses are counted.
        CHECK(lib->relationUses(world, {}, "Admires") == 1);
        CHECK(lib->renameRelation(world, {}, "Admires", {"Looks up to", "Looked up to by"}));
        const auto ls2 = lib->linksOf(aa);
        CHECK(std::any_of(ls2.begin(), ls2.end(), [&](const EntryLink& l) { return l.to == jim && l.label == "Looks up to"; }));
        CHECK(lib->relationReverse(world, {}, "Looks up to") == "Looked up to by" && lib->relationUses(world, {}, "Admires") == 0);
        CHECK(lib->ensureRelation(world, {}, "friend of") == "Friend of" && lib->ensureRelation(world, {}, "looked up to by") == "Looked up to by");
        CHECK(pa::relationFor("character", "Former teacher") == "Student of" && pa::relationFor("character", "Crew of") == "Member of");
        CHECK(pa::relationFor("character", "Grandparents") == "Family" && pa::relationFor("place", "Related to") == "Related to");
    }
    // A blank page: pasting it as it is changes nothing; filled in partly, only what's written goes in.
    {
        const std::string blank = pa::blankPage("character", {"Appearance & personality", "Backstory & goals"}, {"Shards of Light", "The Reign of Zornov"});
        CHECK(blank.find("Name: ") == 0 && blank.find("Shards of Light: ") != std::string::npos && blank.find("Taught by: ") != std::string::npos);
        const pa::Page empty = pa::parse(blank);
        CHECK(empty.name.empty() && empty.aliases.empty() && empty.details.empty() && empty.sections.empty() && empty.relations.empty() &&
              empty.bookNotes.empty() && empty.notes.empty() && empty.summary.empty());
        std::string filled = blank;
        auto put = [&](const std::string& after, const std::string& text) { filled.insert(filled.find(after) + after.size(), text); };
        put("Name: ", "Lia Crystalline");
        put("Eyes: ", "violet");
        put("Backstory & goals\n", "Sarrah's younger sister.");
        put("Shards of Light: ", "Stays in Akunda.");
        put("Siblings: ", "Sah'ra Jane Crystalline");
        const pa::Page fp = pa::parse(filled);
        CHECK(fp.name == "Lia Crystalline" && fp.aliases.empty() && fp.details == "Eyes: violet" && fp.sections.size() == 1 &&
              fp.sections[0].first == "Backstory & goals" && fp.bookNotes.size() == 1 && fp.relations.size() == 1 && fp.relations[0].first == "Siblings");
        CHECK(pa::blankPage("place", {"Description"}, {}).find("Type: ") != std::string::npos);
    }
    // Every kind's blank page (the manual prints them): each section filled in comes back as that section, and the
    // first detail as a detail. Lore has both a Details part and a Details section.
    for (const char* kind : {"character", "race", "creature", "place", "realm", "group", "thing", "lore", "magic", "event"}) {
        std::vector<std::string> secs;
        for (auto& l : defaultEntryTemplate(kind)) { std::string t = l.substr(0, l.find(':')); while (!t.empty() && t.back() == ' ') t.pop_back(); secs.push_back(t); }
        std::string page = pa::blankPage(kind, secs, {"Book 1"});
        auto put = [&](const std::string& after, const std::string& text, size_t from = 0) {
            const size_t at = page.find(after, from);
            CHECK(at != std::string::npos);
            if (at != std::string::npos) page.insert(at + after.size(), text);
        };
        put("Name: ", "Test name");
        put("Summary\n", "A summary.");
        const size_t det = page.find("Details\n") + 8;
        const size_t colon = page.find(": ", det);
        page.insert(colon + 2, "first value");
        size_t from = page.find("Summary\n") + 8;
        for (const auto& t : secs) {
            const size_t at = page.find("\n" + t + "\n", t == "Details" ? page.find("Details\n", det) - 1 : from);
            CHECK(at != std::string::npos);
            if (at == std::string::npos) continue;
            page.insert(at + t.size() + 2, "Text for " + t + ".");
            from = at + 1;
        }
        const pa::Page got = pa::parse(page);
        bool ok = got.name == "Test name" && got.summary == "A summary." && got.details.find("first value") != std::string::npos &&
                  got.details.find('\n') == std::string::npos && got.sections.size() == secs.size() && got.notes.empty();
        for (size_t i = 0; ok && i < secs.size(); ++i) ok = got.sections[i].first == secs[i] && got.sections[i].second == "Text for " + secs[i] + ".";
        if (!ok) std::printf("blank page for %s: %zu sections, details [%s]\n", kind, got.sections.size(), got.details.c_str());
        CHECK(ok);
    }
    // Many pages at once: kinds from headings and Kind: lines, pages from Name: lines and "# Title", pages naming each
    // other linked, a page's own "## Magic" section kept in it, an import repeated changing nothing.
    {
        const std::string many =
            "Here's the bible you asked for.\n\n"
            "# Characters\n\n"
            "Name: Brannoc Vale\nSummary\nA sailor of Highcliff.\n\nFamily\nSiblings: Isolde Vale\n\nRelationships\nLives in: Highcliff\n\n"
            "## Magic\nHe can't do any.\n\n---\n\n"
            "Name: Isolde Vale\nSummary\nHis sister.\n\n"
            "# Places\n\n"
            "# Highcliff\nSummary\nA harbour town.\nDetails\nType: Town\n\nRelationships\nRuled by: Merrow Quill\n\n"
            "Kind: Creature\nName: Grey Harrier\nSummary\nA hawk of the cliffs.\n\n"
            "Name: Skerry Light\nKind: Place\nSummary\nA lighthouse.\n";
        const auto pages = pa::splitPages(many, "thing");
        CHECK(pages.size() == 5);
        if (pages.size() == 5) {
            CHECK(pages[0].name == "Brannoc Vale" && pages[0].kind == "character" && pages[1].kind == "character");
            CHECK(pages[2].name == "Highcliff" && pages[2].kind == "place");
            CHECK(pages[3].name == "Grey Harrier" && pages[3].kind == "creature");
            CHECK(pages[4].name == "Skerry Light" && pages[4].kind == "place");
            CHECK(pages[0].text.find("He can't do any.") != std::string::npos && pages[0].text.find("Here's") == std::string::npos);
        }
        CHECK(pa::looksLikeManyPages(many) && !pa::looksLikeManyPages("Name: One\nSummary\nJust one."));
        CHECK(pa::kindNamed("Magic systems") == "magic" && pa::kindNamed("PEOPLE") == "character" && pa::kindNamed("Sailors").empty());
        pa::Import imp;
        CHECK(pa::importPages(*lib, world, {}, pages, {}, imp));
        CHECK(imp.made.size() == 5 && imp.updated.empty());
        const Entry* bran = lib->findEntryNamed(world, {}, "Brannoc Vale", "character");
        const Entry* isol = lib->findEntryNamed(world, {}, "Isolde Vale", "character");
        const Entry* high = lib->findEntryNamed(world, {}, "Highcliff", "place");
        CHECK(bran && isol && high && lib->findEntryNamed(world, {}, "Grey Harrier", "creature") && lib->findEntryNamed(world, {}, "Skerry Light", "place"));
        if (bran && isol && high) {
            bool sib = false, home = false;
            for (auto& l : lib->linksOf(bran->id)) {
                if ((l.to == isol->id || l.from == isol->id) && (l.label == "Sibling of")) sib = true;
                if (l.to == high->id && l.label == "Lives in") home = true;
            }
            CHECK(sib && home);
            CHECK(bran->section("Magic") == "He can't do any." && high->summary == "A harbour town.");
        }
        CHECK(imp.extra.size() == 1 && lib->findEntryNamed(world, {}, "Merrow Quill"));
        CHECK(!imp.words().empty());
        pa::Import again;
        CHECK(pa::importPages(*lib, world, {}, pages, {}, again));
        CHECK(again.made.empty() && again.updated.size() == 5 && again.unchanged == 5 && again.links == 0 && again.extra.empty());
        // A page that changes one of them replaces just that part; the old entry is kept for undo.
        pa::Import third;
        CHECK(pa::importPages(*lib, world, {}, pa::splitPages("# Places\n\nName: Highcliff\nSummary\nA windy harbour town.\n", "thing"), {}, third));
        CHECK(third.updated.size() == 1 && third.unchanged == 0 && third.before.size() == 1 && third.before[0].summary == "A harbour town.");
        CHECK(lib->findEntryNamed(world, {}, "Highcliff")->summary == "A windy harbour town.");
    }
    // The manual's templates, one after another with a Kind: line each and only the names filled in: one page each, of
    // the right kind (the blank "Realm:" detail lines aren't kind headings).
    {
        std::string all = "Here are my pages.\n\n";
        const std::vector<std::pair<const char*, const char*>> ks = {{"character", "Character"}, {"race", "Race"}, {"creature", "Creature"},
                                                                     {"place", "Place"}, {"realm", "Realm"}, {"group", "Group"}, {"thing", "Thing"},
                                                                     {"lore", "Lore"}, {"magic", "Magic"}, {"event", "Event"}};
        for (auto& [k, label] : ks) {
            std::vector<std::string> secs;
            for (auto& l : defaultEntryTemplate(k)) { std::string t = l.substr(0, l.find(':')); while (!t.empty() && t.back() == ' ') t.pop_back(); secs.push_back(t); }
            std::string page = pa::blankPage(k, secs, {"Book 1", "Book 2"});
            page.insert(page.find("Name: ") + 6, std::string("Test ") + k);
            all += std::string("Kind: ") + label + "\n" + page + "\n";
        }
        const auto pages = pa::splitPages(all, "");
        CHECK(pages.size() == ks.size());
        {
            const pa::Page one = pa::parse("Kind: Place\nName: Skerry Light\nOther names: \n\nSummary\nA lighthouse.\n");
            CHECK(one.name == "Skerry Light" && one.details.empty() && one.summary == "A lighthouse." && one.notes.empty());
        }
        for (size_t i = 0; i < pages.size() && i < ks.size(); ++i) {
            const bool ok = pages[i].kind == ks[i].first && pages[i].name == std::string("Test ") + ks[i].first;
            if (!ok) std::printf("template page %zu: %s %s\n", i, pages[i].kind.c_str(), pages[i].name.c_str());
            CHECK(ok);
        }
    }
    // Pages marked by headings only (Markdown, or a Word document's headings): the shallowest heading that isn't a kind
    // or a part of a page is the pages' titles.
    {
        const auto md = pa::splitPages("My notes.\n\n# Characters\n\n## Brannoc Vale\n\n### Summary\nA sailor.\n\n### Appearance\nWeathered.\n\n"
                                       "## Isolde Vale\n### Summary\nHis sister.\n\n# Places\n## Highcliff\n### Summary\nA town.\n", "");
        CHECK(md.size() == 3);
        if (md.size() == 3) {
            CHECK(md[0].name == "Brannoc Vale" && md[0].kind == "character" && md[2].name == "Highcliff" && md[2].kind == "place");
            const pa::Page b = pa::parse(md[0].text);
            CHECK(b.summary == "A sailor." && b.sections.size() == 1 && b.sections[0].first == "Appearance");
        }
        doc::Builder front, chars;
        front.paragraph();
        front.text("My Alvora bible.");
        chars.heading(2);
        chars.text("Brannoc Vale");
        chars.heading(3);
        chars.text("Summary");
        chars.paragraph();
        chars.text("A sailor.");
        const std::string text = pa::documentPagesText({{"Front matter", front.toJson()}, {"Characters", chars.toJson()}});
        CHECK(text.find("Front matter") == std::string::npos && text.find("# Characters") != std::string::npos && text.find("### Brannoc Vale") != std::string::npos);
        const auto dp = pa::splitPages(text, "");
        CHECK(dp.size() == 1 && dp[0].name == "Brannoc Vale" && dp[0].kind == "character" && pa::parse(dp[0].text).summary == "A sailor.");
    }
    // Starting the bible again, and taking that back.
    {
        const Id w2 = lib->createWorld("Second");
        const Id keep = lib->createEntry(w2, {}, "place", "Elsewhere");
        const std::size_t before = lib->entriesOf(world).size();
        lib->setSetting("conlangs:world:" + world, "{\"languages\":[{\"id\":\"l\",\"name\":\"X\",\"words\":[{\"word\":\"a\"}]}]}");
        Library::BibleReset rs;
        CHECK(lib->resetBible(world, {}, false, rs) && rs.trashed.size() == before && rs.settings.empty());
        CHECK(lib->entriesOf(world).empty() && lib->entriesOf(w2).size() == 1 && lib->entry(keep));
        CHECK(!lib->setting("conlangs:world:" + world).empty());   // entries only: the languages stay
        CHECK(lib->undoBibleReset(rs) && lib->entriesOf(world).size() == before);
        Library::BibleReset all;
        CHECK(lib->resetBible(world, {}, true, all) && lib->entriesOf(world).empty());
        CHECK(lib->setting("conlangs:world:" + world).empty() && lib->entryTemplate(world, "place") == defaultEntryTemplate("place"));
        CHECK(lib->undoBibleReset(all) && lib->entriesOf(world).size() == before && !lib->setting("conlangs:world:" + world).empty());
        CHECK(lib->entryTemplate(world, "place").size() == 3);   // its own template back
    }
    CHECK(lib->entry(tv)->name == "Telvana" && lib->entry(tv)->bookNote(b1).find("Telvana\xE2\x80\x99s cliffs") != std::string::npos);
    lib.reset();
    fs::remove_all(dir);
}

// Notes per book (0.56): titles with a colon, books not added yet, paragraphs.
static void testBookNotes() {
    const std::string dir = tempLibrary("book-notes");
    auto lib = openOk(dir);
    const Id world = lib->createWorld("Alvora");
    const Id b1 = lib->createBook("Shards of Light", world);
    const Id b4 = lib->createBook("Alvora: The Fall of Valona", world);
    const Id tom = lib->createEntry(world, {}, "character", "Tom");
    namespace pa = studio::paste;
    const std::string page =
        "Name: Tom\n"
        "Notes per book\n"
        "Shards of Light: Tom finds the shards.\n"
        "He loses one at the Storm Wall.\n"
        "Later: he finds it again in the ash.\n"
        "\n"
        "A third paragraph.\n"
        "Alvora: The Fall of Valona: Tom leads the defence.\n"
        "The Envoy of Telvana: Tom is only a name in a song.\n"
        "Book 6: Tom's son is born.\n";
    pa::Report r;
    CHECK(pa::apply(*lib, tom, pa::parse(page), {b1, b4}, r));
    const Entry& e = *lib->entry(tom);
    // Each line its own paragraph; "Later:" stays with the note it's in.
    CHECK(e.bookNote(b1) == "Tom finds the shards.\n\nHe loses one at the Storm Wall.\n\nLater: he finds it again in the ash.\n\nA third paragraph.");
    // A colon in the title.
    CHECK(e.bookNote(b4) == "Tom leads the defence.");
    // Books not here yet, kept by their labels; nothing in the notes.
    CHECK(e.bookNote("title:The Envoy of Telvana") == "Tom is only a name in a song." && e.bookNote("title:Book 6") == "Tom's son is born.");
    CHECK(e.notes.empty() && r.unmatchedBooks.size() == 2);
    const auto later = pa::laterBookNotes(e);
    CHECK(later.size() == 2 && later[0].first == "The Envoy of Telvana");
    // Sentence starts aren't titles; titles are.
    {
        const pa::Page p2 = pa::parse("Name: X\nNotes per book\nShards of Light: One.\nIn the end: two.\n");
        const Id x = lib->createEntry(world, {}, "thing", "X");
        pa::Report r2;
        CHECK(pa::apply(*lib, x, p2, {b1, b4}, r2));
        CHECK(lib->entry(x)->bookNote(b1) == "One.\n\nIn the end: two." && r2.unmatchedBooks.empty());
    }
    // The book is added: its note moves onto it; the one still to come waits.
    CHECK(pa::claimLaterBookNotes(*lib, world, {}, {b1, b4}) == 0);
    const Id envoy = lib->createBook("The Envoy of Telvana", world);
    CHECK(pa::claimLaterBookNotes(*lib, world, {}, {b1, b4, envoy}) == 1);
    CHECK(lib->entry(tom)->bookNote(envoy) == "Tom is only a name in a song." && lib->entry(tom)->bookNote("title:The Envoy of Telvana").empty());
    CHECK(pa::laterBookNotes(*lib->entry(tom)).size() == 1);
    // "Book 6" by reading order once there are six.
    std::vector<Id> six = {b1, b4, envoy};
    for (int i = 0; i < 3; ++i) six.push_back(lib->createBook("More " + std::to_string(i), world));
    CHECK(pa::claimLaterBookNotes(*lib, world, {}, six) == 1 && lib->entry(tom)->bookNote(six[5]) == "Tom's son is born.");
    CHECK(pa::laterBookNotes(*lib->entry(tom)).empty());
    // Kept in the library, and pasted again changes nothing.
    pa::Report again;
    CHECK(pa::apply(*lib, tom, pa::parse("Name: Tom\nNotes per book\nShards of Light: Tom finds the shards.\n"), six, again));
    CHECK(lib->entry(tom)->bookNote(b1) == "Tom finds the shards.");
}

// Importing pages that rename, remove and merge entries, and bring their pictures (0.55).
static void testImportControls() {
    const std::string dir = tempLibrary("import-controls");
    auto lib = openOk(dir);
    const Id world = lib->createWorld("Alvora");
    // Pictures beside the import file, one in a folder of its own, and one that isn't a picture.
    const std::string folder = dir + "/import";
    std::filesystem::create_directories(folder + "/art");
    std::ofstream(folder + "/tom.png", std::ios::binary) << pngOf(400, 600);
    std::ofstream(folder + "/art/forge.png", std::ios::binary) << pngOf(800, 500);
    std::ofstream(folder + "/art/storm.png", std::ios::binary) << pngOf(800, 501);
    std::ofstream(folder + "/notes.txt") << "not a picture";
    const std::string absolute = folder + "/art/storm.png";

    // Parsing the new lines.
    const auto pg = paste::parse("Name: Armstrong\nFormerly: Armstong, \"Arm\"\nPicture: tom.png\nGallery:\nart/forge.png | At the forge\n" + absolute +
                                 "\nnotes.txt\nSummary: A smith.\n");
    CHECK((pg.name == "Armstrong" && pg.formerly == std::vector<std::string>{"Armstong", "Arm"} && pg.picture == "tom.png"));
    CHECK(pg.gallery.size() == 3 && pg.gallery[0] == std::make_pair(std::string("art/forge.png"), std::string("At the forge")) &&
          pg.gallery[1].first == absolute && pg.gallery[1].second.empty() && pg.summary == "A smith.");
    CHECK(paste::parse("Name: X\nRemove: yes\n").remove && !paste::parse("Name: X\nRemove: no\n").remove);
    CHECK(paste::parse("Name: X\nMerge into: House Malrok\n").mergeInto == "House Malrok");
    CHECK((paste::parse("Name: X\nMerged from: Sunfire, Old Sunfire\n").mergeFrom == std::vector<std::string>{"Sunfire", "Old Sunfire"}));
    CHECK(paste::parse("Name: X\nGallery: a.png; b.png | B\n").gallery.size() == 2);
    // A heading after the gallery ends it; it isn't a picture's path.
    const auto withHeading = paste::parse("Name: X\nGallery:\nart/a.png\n\nAppearance\nTall and grey.\n");
    CHECK(withHeading.gallery.size() == 1 && withHeading.sections.size() == 1 && withHeading.sections[0].first == "Appearance");
    // A blank page leaves them all empty.
    const auto blank = paste::parse(paste::blankPage("character", {}, {}));
    CHECK(blank.picture.empty() && blank.gallery.empty() && blank.formerly.empty() && !blank.remove);

    // The bible as it was: a misspelt name, a king under his old name, old pages to go and to merge.
    const Id armstong = lib->createEntry(world, {}, "character", "Armstong");
    const Id george = lib->createEntry(world, {}, "character", "Prince George");
    const Id malrokOld = lib->createEntry(world, {}, "group", "House Malrok (old)");
    const Id malrok = lib->createEntry(world, {}, "group", "House Malrok");
    const Id sunfire = lib->createEntry(world, {}, "magic", "Sunfire");
    const Id sunOld = lib->createEntry(world, {}, "magic", "Sun Fire");
    const Id gone = lib->createEntry(world, {}, "place", "Old Keep");
    if (Entry e = *lib->entry(sunOld); true) { e.summary = "The old page."; lib->updateEntry(e); }
    lib->addLink(sunOld, george, "Used by");

    const std::string text =
        "# Characters\n\n"
        "Name: Armstrong\nFormerly: Armstong\nPicture: tom.png\nGallery:\nart/forge.png | At the forge\nmissing.png\n\nSummary: A smith.\n\n"
        "Name: King George\nOther names: Prince George\nSummary: The king.\n\n"
        "# Groups\n\nName: House Malrok (old)\nMerge into: House Malrok\n\n"
        "# Magic\n\nName: Sunfire\nMerged from: Sun Fire\nSummary: Light made fire.\n\n"
        "# Places\n\nName: Old Keep\nRemove: yes\n\nName: Nowhere Hall\nRemove: yes\n\n"
        "Name: Lost Tower\nMerge into: The Tower\n";
    // (Lost Tower doesn't exist: nothing to merge, so it's not found.)
    paste::Import im;
    CHECK(paste::importPages(*lib, world, {}, paste::splitPages(text, "character"), {}, im, false, folder));
    // Renamed rather than made again.
    CHECK(lib->entry(armstong)->name == "Armstrong" && lib->entry(george)->name == "King George" && im.made.empty());
    CHECK(im.renamed.size() == 2 && lib->entry(george)->summary == "The king.");
    // Pictures: the main one, the gallery with its caption, and the missing one reported.
    const Entry* arm = lib->entry(armstong);
    CHECK(!arm->imageBlob.empty() && arm->gallery.size() == 2 && arm->gallery[1].caption == "At the forge");
    CHECK(im.pictures == 2 && im.missingPictures == std::vector<std::string>{"missing.png"});
    CHECK(lib->artworks().size() >= 2);
    // Merged one way and the other; removed; not found.
    CHECK(lib->entry(malrokOld)->deleted && !lib->entry(malrok)->deleted);
    CHECK(lib->entry(sunOld)->deleted && lib->entry(sunfire)->summary == "Light made fire." && im.mergedAway.size() == 2);
    bool linkMoved = false;
    for (auto& l : lib->linksOf(sunfire)) linkMoved = linkMoved || (l.to == george || l.from == george);
    CHECK(linkMoved);
    CHECK(lib->entry(gone)->deleted && im.removed == std::vector<Id>{gone});
    CHECK((im.notFound == std::vector<std::string>{"Nowhere Hall", "Lost Tower"}));
    CHECK(im.words().find("2 entries renamed") != std::string::npos && im.words().find("1 entry removed") != std::string::npos);
    // The same import again: nothing new, nothing renamed.
    paste::Import again;
    CHECK(paste::importPages(*lib, world, {}, paste::splitPages(text, "character"), {}, again, false, folder));
    CHECK(again.made.empty() && again.renamed.empty() && lib->entry(armstong)->gallery.size() == 2);

    // "Merge into" a name nobody has: the entry is renamed to it.
    const Id tower = lib->createEntry(world, {}, "place", "Tower of Ash");
    paste::Import ren;
    CHECK(paste::importPages(*lib, world, {}, paste::splitPages("Name: Tower of Ash\nKind: place\nMerge into: The Ash Tower\n", "place"), {}, ren));
    CHECK(lib->entry(tower)->name == "The Ash Tower" && ren.renamed.size() == 1);

    // A single page pasted onto an entry brings its picture too.
    const Id lone = lib->createEntry(world, {}, "character", "Lone");
    paste::Report rep;
    CHECK(paste::apply(*lib, lone, paste::parse("Name: Lone\nPicture: " + absolute + "\nSummary: Alone.\n"), {}, rep));
    CHECK(rep.picture && !lib->entry(lone)->imageBlob.empty() && rep.words().find("the picture") != std::string::npos);
}

static void testPlanning() {
    const std::string dir = tempLibrary("planning");
    auto lib = openOk(dir);
    const Id world = lib->createWorld("Alvora");
    const Id book = lib->createBook("Shards", world);
    const Id tom = lib->createEntry(world, {}, "character", "Tom Smith");
    if (Entry e = *lib->entry(tom); true) { e.aliases = {"Tommy"}; lib->updateEntry(e); }
    lib->createEntry(world, {}, "character", "Sarrah Valen");

    // Built-in structures keep their old ids, and have the new ones.
    const auto& built = plan::builtInStructures();
    CHECK(plan::findStructure(built, "three-act") && plan::findStructure(built, "kishotenketsu"));
    CHECK(plan::findStructure(built, "fichtean") && plan::findStructure(built, "story-circle") && plan::findStructure(built, "four-act"));

    // Own structures from text: percentages, fractions, none (spread evenly), "Beat:" and "|".
    std::vector<std::string> skipped;
    auto own = plan::parseStructures(
        "Structure: Heist\nAbout: The job.\n0% The crew: who's in\nBeat: 50% The plan | how it should go\n100% The getaway\n\n"
        "Structure: Quick\nOne\nTwo\nThree\n", &skipped);
    CHECK(own.size() == 2);
    if (own.size() == 2) {
        CHECK(own[0].name == "Heist" && own[0].about == "The job." && own[0].beats.size() == 3);
        CHECK(own[0].beats[1].name == "The plan" && own[0].beats[1].about == "how it should go" && std::abs(own[0].beats[1].at - 0.5) < 1e-9);
        CHECK(own[0].beats[0].id == "the-crew" && own[0].id == "own-heist");
        CHECK(own[1].beats.size() == 3 && own[1].beats[0].at == 0.0 && own[1].beats[1].at > 0.3 && own[1].beats[2].at > own[1].beats[1].at && own[1].beats[2].at < 1.0);
        // Round trip.
        const auto again = plan::parseStructures(plan::structureText(own[0]));
        CHECK(again.size() == 1 && again[0].beats.size() == 3 && again[0].beats[2].name == "The getaway" && std::abs(again[0].beats[2].at - 1.0) < 1e-9);
        plan::setOwnStructures(*lib, "world:" + world, own);
        const auto all = plan::allStructures(*lib, "world:" + world);
        CHECK(all.size() == built.size() + 2 && plan::findStructure(all, "own-heist") && plan::findStructure(all, "own-heist")->own);
    }

    // A book's structure: one chapter (before 0.50) still reads; several and notes save.
    lib->setSetting("structure:" + book, R"({"template":"three-act","beats":{"setup":"n1"}})");
    auto bs = plan::bookStructure(*lib, book);
    CHECK(bs.templateId == "three-act" && bs.beats["setup"] == std::vector<Id>{"n1"});
    bs.beats["midpoint"] = {"n2", "n3"};
    bs.notes["midpoint"] = "The ledger.";
    plan::setBookStructure(*lib, book, bs);
    bs = plan::bookStructure(*lib, book);
    CHECK(bs.beats["midpoint"].size() == 2 && bs.notes["midpoint"] == "The ledger.");

    // An outline from text.
    const std::string outline =
        "Part: Part One\n"
        "Chapter: The Forge\nViewpoint: Tom\nStatus: Draft\nTarget: 3,000 words\nThreads: The shards, Love\nWhen: Day 3, at dawn\n"
        "Tom finds the ledger.\nIt names his mother.\n\nA second paragraph.\n"
        "Scene: Night watch\nPOV: Tommy\n"
        "Chapter: The Storm Wall\nViewpoint: Nobody Known\nWhen: the night of the storm\n"
        "Part: Part Two\nChapter: Ashes\n";
    plan::OutlineImport oi;
    CHECK(plan::importOutline(*lib, book, outline, oi));
    CHECK(oi.parts == 2 && oi.chapters == 3 && oi.scenes == 1 && oi.threads == 2);
    CHECK(oi.unknownViewpoints == std::vector<std::string>{"Nobody Known"});
    const auto nodes = lib->manuscriptOf(book);
    CHECK(nodes.size() == 6);
    if (nodes.size() == 6) {
        CHECK(nodes[0]->kind == NodeKind::Part && nodes[1]->title == "The Forge" && nodes[1]->parentId == nodes[0]->id);
        CHECK(nodes[1]->povId == tom && nodes[1]->targetWords == 3000 && nodes[1]->status == NodeStatus::Draft);
        CHECK(nodes[1]->synopsis == "Tom finds the ledger.\nIt names his mother.\n\nA second paragraph.");
        CHECK(nodes[2]->kind == NodeKind::Scene && nodes[2]->parentId == nodes[1]->id && nodes[2]->povId == tom);
        CHECK(nodes[3]->status == NodeStatus::Outline && nodes[3]->povId.empty());
        CHECK(nodes[5]->parentId == nodes[4]->id);
        const auto th = plan::threadsOf(*lib, book);
        CHECK(th.threads.size() == 2 && th.named("the shards") && th.has(nodes[1]->id, th.named("Love")->id));
        const auto tl = nlohmann::json::parse(lib->setting("timeline:" + book));
        CHECK(tl["nodes"][nodes[1]->id]["day"] == 3 && tl["nodes"][nodes[1]->id]["when"] == "at dawn");
        CHECK(tl["nodes"][nodes[3]->id]["when"] == "the night of the storm" && !tl["nodes"][nodes[3]->id].contains("day"));

        std::vector<Id> ids;
        for (auto* n : nodes) ids.push_back(n->id);
        // Out again, and back into another book: the same outline.
        const std::string text = plan::outlineText(*lib, book);
        CHECK(text.find("Chapter: The Forge\nViewpoint: Tom Smith\nStatus: Draft\nTarget: 3000\nThreads: The shards, Love\nWhen: Day 3, at dawn\n") != std::string::npos);
        const Id book2 = lib->createBook("Copy", world);
        plan::OutlineImport o2;
        CHECK(plan::importOutline(*lib, book2, text, o2) && o2.chapters == 3 && o2.unknownViewpoints.empty());
        CHECK(plan::outlineText(*lib, book2) == text);

        // Threads as text: numbers, ranges and titles.
        plan::ThreadImport ti;
        CHECK(plan::importThreads(*lib, book, "Thread: Love\nColour: 123456\nAbout: Tom and Sarrah.\nChapters: 2-3, Night watch, 9\n\nThread: Revenge\nChapters: 1\n", ti));
        CHECK(ti.added == 1 && ti.updated == 1 && ti.unknown == std::vector<std::string>{"9"});
        const auto th2 = plan::threadsOf(*lib, book);
        const plan::Thread* love = th2.named("love");
        CHECK(love && love->colour == "#123456" && love->about == "Tom and Sarrah.");
        CHECK(love && th2.has(ids[3], love->id) && th2.has(ids[5], love->id) && th2.has(ids[2], love->id));
        const std::string tt = plan::threadsText(*lib, book, th2);
        CHECK(tt.find("Thread: Love\nColour: #123456\nAbout: Tom and Sarrah.\nChapters: 1-3, Night watch\n") != std::string::npos);
    }

    // All three in one text: an own structure, an outline that follows it with beats, and threads by chapter number.
    {
        const Id b3 = lib->createBook("All in one", world);
        const std::string scope = plan::structureScope(*lib, b3);
        CHECK(scope == "world:" + world);
        const std::string text =
            "Book structure: Heist\n\nChapter: The job\nBeat: The crew\nThreads: Money\n\nChapter: The vault\nBeat: The plan, Nothing\n\n"
            "Thread: Money\nColour: #00AA00\nChapters: 2\n\n"
            "Structure: Heist\nAbout: Replaced.\n0% The crew: who\n50% The plan: how\n100% The getaway\n";
        plan::PlanningImport pi;
        CHECK(plan::importPlanning(*lib, b3, scope, text, pi));
        CHECK(pi.structuresReplaced == 1 && pi.structuresAdded == 0 && pi.outline.chapters == 2 && pi.outline.structure == "Heist");
        CHECK(pi.outline.beats == 2 && pi.outline.unknownBeats == std::vector<std::string>{"Nothing"});
        CHECK(pi.threads.updated == 0 && pi.threads.tagged == 0);   // the outline's own Threads lines win over a "Chapters:" list (0.62)
        const auto own2 = plan::ownStructures(*lib, scope);
        CHECK(own2.size() == 2 && own2[0].about == "Replaced.");
        const auto bs3 = plan::bookStructure(*lib, b3);
        const auto ch = lib->manuscriptOf(b3);
        CHECK(bs3.templateId == "own-heist" && ch.size() == 2 && bs3.beats.at("the-plan") == std::vector<Id>{ch[1]->id});
        const auto th3 = plan::threadsOf(*lib, b3);
        CHECK(th3.threads.size() == 1 && th3.threads[0].colour == "#00AA00" && th3.has(ch[0]->id, "t1") && !th3.has(ch[1]->id, "t1"));
        const std::string all = plan::planningText(*lib, b3, scope);
        CHECK(all.rfind("Book structure: Heist\n\nChapter: The job\nStatus: Outline\nThreads: Money\nBeat: The crew\n", 0) == 0);
        CHECK(all.find("Thread: Money\nColour: #00AA00\nChapters: 1\n") != std::string::npos && all.find("Structure: Heist\nAbout: Replaced.\n") != std::string::npos);
        // Into another world: the structure comes along.
        const Id w2 = lib->createWorld("Elsewhere");
        const Id b4 = lib->createBook("Copy", w2);
        plan::PlanningImport p2;
        CHECK(plan::importPlanning(*lib, b4, plan::structureScope(*lib, b4), all, p2) && p2.structuresAdded == 1);
        CHECK(plan::planningText(*lib, b4, plan::structureScope(*lib, b4)) == all);
    }

    // Places (0.52): set from an outline, out again, and the bible's events on the timeline.
    {
        const Id forge = lib->createEntry(world, {}, "place", "Elandroth Forge");
        if (Entry e = *lib->entry(forge); true) { e.aliases = {"The Forge"}; lib->updateEntry(e); }
        const Id b5 = lib->createBook("Places", world);
        plan::OutlineImport o5;
        CHECK(plan::importOutline(*lib, b5, "Chapter: One\nPlace: the forge\n\nChapter: Two\nWhere: Nowhere at all\n", o5));
        CHECK(o5.unknownPlaces == std::vector<std::string>{"Nowhere at all"});
        const auto ch5 = lib->manuscriptOf(b5);
        const auto pl5 = plan::nodePlaces(*lib, b5);
        CHECK(ch5.size() == 2 && pl5.size() == 1 && pl5.at(ch5[0]->id) == forge);
        CHECK(plan::outlineText(*lib, b5).find("Chapter: One\nStatus: Outline\nPlace: Elandroth Forge\n") != std::string::npos);
        plan::setNodePlace(*lib, b5, ch5[0]->id, {});
        CHECK(plan::nodePlaces(*lib, b5).empty());
        const Id ev = lib->createEntry(world, {}, "event", "The Burning");
        lib->setSetting("timeline:" + b5, R"({"unit":"week","nodes":{"x":{"day":2}}})");
        CHECK(plan::setTimelineEvent(*lib, b5, ev, 4.0) && plan::timelineEvents(*lib, b5).at(ev) == 4.0);
        CHECK(nlohmann::json::parse(lib->setting("timeline:" + b5))["unit"] == "week");   // the rest kept
        plan::setTimelineEvent(*lib, b5, ev, std::nullopt);
        CHECK(plan::timelineEvents(*lib, b5).empty());
    }

    // A plan for a book that already has its chapters (0.54): matched by title or number and filled in, not added again.
    {
        CHECK(plan::titleNumber("Chapter 12: Ashes") == 12 && plan::titleNumber("12. Ashes") == 12 && plan::titleNumber("Ch. 3") == 3 &&
              plan::titleNumber("7") == 7 && plan::titleNumber("The 12 Kings") == 0 && plan::titleNumber("12th Night") == 0 &&
              plan::titleNumber("Chapterhouse") == 0);
        CHECK(plan::titleCore("Chapter 12: Ashes and Embers") == "ashes and embers" && plan::titleCore("12. Ashes") == "ashes" &&
              plan::titleCore("Chapter 4") == "" && plan::titleCore("Chapter 4 \xE2\x80\x94 The Wall") == "the wall" &&
              plan::titleCore("The Forge") == "the forge");
        const Id b6 = lib->createBook("Shards of Light", world);
        const Id p1 = lib->createNode(b6, {}, NodeKind::Part, "Part One");
        const Id c1 = lib->createNode(b6, p1, NodeKind::Chapter, "Chapter 1: The Forge");
        const Id c2 = lib->createNode(b6, p1, NodeKind::Chapter, "The Storm Wall");
        const Id s2 = lib->createNode(b6, c2, NodeKind::Scene, "Night watch");
        const Id c3 = lib->createNode(b6, p1, NodeKind::Chapter, "Chapter 3");
        const Id front = lib->createNode(b6, {}, NodeKind::Chapter, "Prologue");   // front matter: not counted
        if (ManuscriptNode n = *lib->node(front); true) { n.matter = 1; lib->updateNode(n); }
        if (ManuscriptNode n = *lib->node(c2); true) {
            n.synopsis = "Kept.";
            n.status = NodeStatus::Revised;
            n.targetWords = 5000;
            lib->updateNode(n);
        }
        plan::OutlineImport o6;
        CHECK(plan::importOutline(*lib, b6,
                                  "Part: Part One\n"
                                  "Chapter: The Forge\nViewpoint: Tom\nPlace: The Forge\nWhen: Day 2\nThreads: The shards\nTom finds the ledger.\n"
                                  "Chapter: The Storm Wall\nThreads: Love\n"
                                  "Scene: Night watch\nPOV: Sarrah Valen\nThe watch.\n"
                                  "Scene: Dawn\n"
                                  "Chapter 3: Ashes\nStatus: Draft\nTarget: 4000\nIt burns.\n"
                                  "Chapter: A New One\n",
                                  o6));
        CHECK(o6.updatedParts == 1 && o6.updatedChapters == 3 && o6.updatedScenes == 1 && o6.chapters == 1 && o6.scenes == 1 && o6.parts == 0);
        CHECK(o6.updated.size() == 5 && o6.made.size() == 2);
        // By title, with "Chapter 1:" in front on one side only.
        CHECK(lib->node(c1)->povId == tom && lib->node(c1)->synopsis == "Tom finds the ledger." && lib->node(c1)->title == "Chapter 1: The Forge");
        Id forge;
        for (const Entry* e : lib->entriesOf(world, {})) if (e->name == "Elandroth Forge") forge = e->id;
        CHECK(!forge.empty() && plan::nodePlaces(*lib, b6).at(c1) == forge);
        CHECK(nlohmann::json::parse(lib->setting("timeline:" + b6))["nodes"][c1]["day"] == 2);
        // The plan replaces what was there (0.62): no synopsis or target in it, none kept; the status stays when it gives none.
        CHECK(lib->node(c2)->synopsis.empty() && lib->node(c2)->status == NodeStatus::Revised && lib->node(c2)->targetWords == 0);
        const auto th6 = plan::threadsOf(*lib, b6);
        CHECK(th6.has(c1, th6.named("The shards")->id) && th6.has(c2, th6.named("Love")->id));
        // A scene by title within its chapter; a new scene goes under it.
        CHECK(lib->node(s2)->synopsis == "The watch." && !lib->node(s2)->povId.empty());
        const auto kids = lib->childrenOf(b6, c2);
        CHECK(kids.size() == 2 && kids[1]->title == "Dawn");
        // By number among the story's chapters (the prologue isn't one), keeping its title.
        CHECK(lib->node(c3)->synopsis == "It burns." && lib->node(c3)->status == NodeStatus::Draft && lib->node(c3)->targetWords == 4000 &&
              lib->node(c3)->title == "Chapter 3");
        // The new chapter is added under the part.
        const auto all6 = lib->manuscriptOf(b6);
        bool newOne = false;
        for (auto* n : all6) newOne = newOne || (n->title == "A New One" && n->parentId == p1 && n->status == NodeStatus::Outline);
        CHECK(newOne);
        // The same plan again changes nothing in number: everything is matched.
        plan::OutlineImport o7;
        CHECK(plan::importOutline(*lib, b6, plan::outlineText(*lib, b6), o7));
        // (The plan replaces the book's: the empty prologue it didn't have went to the Trash, 0.62.)
        CHECK(o6.trashed == 1 && o7.made.empty() && o7.updatedChapters == 4 && o7.trashed == 0 && lib->manuscriptOf(b6).size() == all6.size());
        // A number that names another chapter's title goes to that chapter, not the number.
        plan::OutlineImport o8;
        CHECK(plan::importOutline(*lib, b6, "Chapter 1: The Storm Wall\nRenumbered.\n", o8));
        CHECK(o8.updatedChapters == 1 && lib->node(c2)->synopsis == "Renumbered." && lib->node(c1)->synopsis == "Tom finds the ledger.");
    }

    // Markdown headings: two levels are chapters and scenes.
    const auto md = plan::parseOutline("# The Forge\nSome words.\n## Night watch\n# The Storm\n");
    CHECK(md.size() == 3 && md[0].kind == NodeKind::Chapter && md[1].kind == NodeKind::Scene && md[0].synopsis == "Some words.");
    const auto md3 = plan::parseOutline("# One\n## Two\n### Three\n");
    CHECK(md3.size() == 3 && md3[0].kind == NodeKind::Part && md3[2].kind == NodeKind::Scene);
}

// 0.57: former partners, engaged couples, triplets, creature families, the missing-gender warning, more wordings.
static void testFamilyAndWordings() {
    const std::string dir = tempLibrary("family-057");
    auto lib = openOk(dir);
    const Id world = lib->createWorld("Alvora");
    namespace pa = studio::paste;
    const std::string text =
        "# Lysa\nDetails\nGender: Female\n\nFamily\nChildren: Lyra\nEx-husband: Ralos\n\n"
        "# Ralos\nDetails\nGender: Male\n\nFamily\nChildren: Lyra\n\n"
        "# Lyra\nDetails\nGender: Female\n\n"
        "# Elliot\nDetails\nGender: Male\n\nFamily\nFiancee: Emma\n\n"
        "# Zia\nDetails\nGender: Female\n\nFamily\nParents: Ralos\nTriplets: Jimmy, Johney\n\n"
        "# Jimmy\nDetails\nGender: Male\n\nFamily\nParents: Ralos\n\n"
        "# Johney\nDetails\nGender: Male\n\nFamily\nParents: Ralos\n\n"
        "Creatures\n# Ra'min\nSummary\nA wyvern.\n\nFamily\nTwin: K'alis\n\n"
        "Things\n# Sunblade\nRelationships\nUsed by: Elliot\nOrigin of: The Old Song\n\n"
        "Groups\n# The Wardens\nRelationships\nFounded: Highwatch\nMentor of: Elliot\n\n"
        "Places\n# Highwatch\nRelationships\nScene of: The Siege\nFounded by: The Wardens\n";
    const auto pages = pa::splitPages(text, "character");
    pa::Import imp;
    CHECK(pa::importPages(*lib, world, {}, pages, {}, imp));
    auto named = [&](const char* n) { const Entry* e = lib->findEntryNamed(world, {}, n); return e ? e->id : Id{}; };
    const Id lysa = named("Lysa"), ralos = named("Ralos"), lyra = named("Lyra"), elliot = named("Elliot"), emma = named("Emma");
    // A former partner: Lyra's father, not Lysa's partner.
    const auto fl = lib->familyOf(lysa);
    CHECK(fl.partners.empty() && fl.formerPartners.size() == 1 && fl.formerPartners[0] == ralos);
    const auto lay = lib->familyLayout(lyra);
    CHECK(lay.couples.empty());
    {
        const auto ly = lib->familyLayout(lysa);
        bool former = false;
        for (auto& p : ly.people) if (p.id == ralos && p.joined == "former") former = true;
        CHECK(former && ly.couples.empty());
    }
    // Engaged: a partner, drawn as a couple, called engaged.
    const auto fe = lib->familyOf(elliot);
    CHECK(fe.partners.size() == 1 && fe.partners[0] == emma && fe.partnerHow.at(emma) == "engaged");
    CHECK(lib->familyLayout(elliot).couples.size() == 1);
    // Triplets: Zia linked to both makes Jimmy and Johney triplets too, though they share a parent.
    const Id zia = named("Zia"), jimmy = named("Jimmy"), johney = named("Johney");
    const auto fj = lib->familyOf(jimmy);
    CHECK(fj.how.count(johney) && fj.how.at(johney) == "triplet" && fj.how.at(zia) == "triplet");
    {
        // Side by side in the tree.
        const auto t = lib->familyLayout(zia);
        std::map<Id, float> x;
        for (auto& p : t.people) x[p.id] = p.x;
        const float lo = std::min({x[zia], x[jimmy], x[johney]}), hi = std::max({x[zia], x[jimmy], x[johney]});
        CHECK(hi - lo < 2.6f && (x[lyra] < lo || x[lyra] > hi));   // the three together, their half-sister to one side
    }
    // Creatures have family: the twin wyverns.
    const Entry* ramin = lib->findEntryNamed(world, {}, "Ra'min", "creature");
    const Entry* kalis = lib->findEntryNamed(world, {}, "K'alis");
    CHECK(ramin && kalis && kalis->kind == "creature");
    if (ramin && kalis) {
        const auto fr = lib->familyOf(ramin->id);
        CHECK(fr.siblings.size() == 1 && fr.how.at(kalis->id) == "twin");
    }
    // The gender warning: Emma has family but no gender; Lysa has both.
    CHECK(std::find(imp.noGender.begin(), imp.noGender.end(), emma) != imp.noGender.end());
    CHECK(std::find(imp.noGender.begin(), imp.noGender.end(), lysa) == imp.noGender.end());
    CHECK(std::find(imp.noGender.begin(), imp.noGender.end(), named("Sunblade")) == imp.noGender.end());
    // The wordings are the bible's own, none of the world's.
    auto linked = [&](const Id& a, const char* label) {
        for (const EntryLink& l : lib->linksOf(a)) if (l.from == a && l.label == label) return true;
        return false;
    };
    CHECK(linked(named("Sunblade"), "Used by") && linked(named("Sunblade"), "Origin of"));
    CHECK(linked(named("The Wardens"), "Founded") && linked(named("The Wardens"), "Mentor of"));
    CHECK(linked(named("Highwatch"), "Scene of") && lib->linksOf(named("Highwatch")).size() == 2);   // "Founded by" is the Wardens' "Founded"
    CHECK(lib->customRelations(world, {}).empty());
    CHECK(reverseRelation("Used by") == "Used" && reverseRelation("Engaged to") == "Engaged to" && isFamilyRelation("Former partner of"));
    // Words the importer knows.
    CHECK(pa::relationFor("character", "ex-wife") == "Former partner of" && pa::relationFor("character", "divorced from") == "Former partner of");
    CHECK(pa::relationFor("character", "fiancé") == "Engaged to" && pa::relationFor("character", "late husband") == "Married to");
    CHECK(pa::relationFor("character", "triplet sister") == "Triplet of" && pa::relationFor("creature", "twins") == "Twin of");
    // A "Used by" the world made itself before is no longer listed as its own.
    lib->setCustomRelations(world, {}, {{"Used by", "Uses"}, {"Admires", "Admired by"}});
    const auto own = lib->customRelations(world, {});
    CHECK(own.size() == 1 && own[0].label == "Admires");
}

// 0.59: who's present, chosen in Planning; creature families in the bible.
static void testCastAndCreatureFamily() {
    const std::string dir = tempLibrary("cast-059");
    auto lib = openOk(dir);
    const Id world = lib->createWorld("Alvora");
    const Id book = lib->createBook("Dragonfall", world);
    const Id tom = lib->createEntry(world, {}, "character", "Tom Smith");
    const Id ann = lib->createEntry(world, {}, "character", "Ann Smith");
    const Id ramin = lib->createEntry(world, {}, "creature", "Ra'min");
    const Id kalis = lib->createEntry(world, {}, "creature", "K'alis");
    lib->addLink(ramin, kalis, "Twin of");
    const Id ch = lib->createNode(book, {}, NodeKind::Chapter, "The Forge");
    const Id sc = lib->createNode(book, ch, NodeKind::Scene, "Night watch");
    namespace plan = studio::plan;
    CHECK(plan::castOf(*lib, book, ch).empty());
    CHECK(plan::setNodeCast(*lib, book, ch, {tom, tom}));
    CHECK(plan::setNodeCast(*lib, book, sc, {ramin, tom}));
    const auto c = plan::castOf(*lib, book, ch);
    CHECK(c.size() == 2 && c[0] == tom && c[1] == ramin);   // the chapter's, then its scenes', no repeats
    CHECK(plan::castOf(*lib, book, sc).size() == 2);
    // Through an outline: written out, and read back into a book (a first name only one has; unknown names reported).
    const std::string text = plan::outlineText(*lib, book);
    CHECK(text.find("Characters: Tom Smith\n") != std::string::npos && text.find("Characters: Ra'min, Tom Smith\n") != std::string::npos);
    plan::OutlineImport oi;
    CHECK(plan::importOutline(*lib, book, "Chapter: The Forge\nCharacters: Ann, K'alis, Nobody\n", oi));
    const auto c2 = plan::nodeCast(*lib, book);
    CHECK(c2.at(ch).size() == 2 && c2.at(ch)[0] == ann && c2.at(ch)[1] == kalis);
    CHECK(oi.unknownCast.size() == 1 && oi.unknownCast[0] == "Nobody");
    CHECK(plan::setNodeCast(*lib, book, sc, {}));
    CHECK(plan::nodeCast(*lib, book).count(sc) == 0);
    // A deleted entry drops out.
    lib->trashEntry(kalis);
    CHECK(plan::castOf(*lib, book, ch).size() == 1);
    lib->restore(kalis);
    // Creatures' families are among the families (the twin wyverns).
    bool wyverns = false;
    for (auto& g : lib->families(world))
        if (g.root == ramin || g.root == kalis) wyverns = true;
    CHECK(wyverns);
    // The PDF lists a creature's family (0.59).
    {
        namespace bible = studio::bible;
        const auto src = bible::printableSource(*lib, bible::Options{world, {}, false, {}}, "Bible", "A. Writer");
        std::string page;
        for (auto& sec : src.sections) if (sec.title == "Ra'min") page = sec.content;
        CHECK(page.find("Family") != std::string::npos && page.find("K'alis") != std::string::npos);
    }
}

// Sexuality, "From", and books counted within their own series (0.60).
static void testSexualityFromSeries() {
    const std::string dir = tempLibrary("studio-060");
    auto lib = openOk(dir);
    const Id world = lib->createWorld("Alvora");
    const Id chron = lib->createSeries("The Alvora Chronicles", world);
    const Id envoy = lib->createSeries("Envoy", world);
    const Id a1 = lib->createBook("Shards of Light", world);
    const Id a2 = lib->createBook("Dragonfall", world);
    const Id e1 = lib->createBook("The Envoy of Telvana", world);
    CHECK(lib->addBookToSeries(a1, chron) && lib->addBookToSeries(a2, chron) && lib->addBookToSeries(e1, envoy));
    CHECK(lib->membership(chron, a2)->number == "2" && lib->membership(envoy, e1)->number == "1");
    namespace pa = studio::paste;
    std::vector<const Book*> books = {lib->book(a1), lib->book(a2), lib->book(e1)};
    // The series named picks its own Book N; with none named, the one series that has that number.
    CHECK(pa::bookForLabel(books, "Alvora Chronicles Book 2", lib.get()) == lib->book(a2));
    CHECK(pa::bookForLabel(books, "Book 1 of the Envoy series", lib.get()) == lib->book(e1));
    CHECK(pa::bookForLabel(books, "Book 2", lib.get()) == lib->book(a2));
    CHECK(pa::bookForLabel(books, "Book 1", lib.get()) == nullptr);   // both series have a Book 1: not guessed
    CHECK(pa::bookForLabel(books, "Envoy Book 2", lib.get()) == nullptr);
    // Through a pasted page: notes land on the series' own book.
    const Id tom = lib->createEntry(world, {}, "character", "Tom");
    pa::Report r;
    CHECK(pa::apply(*lib, tom, pa::parse("Name: Tom\nGender: Male\nSexuality: Bisexual\nRelationships\nFrom: Highcliff\nLives in: Valona\n"
                                           "Notes per book\nAlvora Chronicles Book 2: Tom flies.\nEnvoy Book 1: Tom is a song.\n"),
                    {a1, a2, e1}, r));
    const Entry& t = *lib->entry(tom);
    CHECK(t.bookNote(a2) == "Tom flies." && t.bookNote(e1) == "Tom is a song." && t.bookNote(a1).empty());
    CHECK(t.details.find("Sexuality: Bisexual") != std::string::npos);
    // "From" is its own relationship, beside "Lives in", each with its own place.
    bool from = false, lives = false;
    for (const EntryLink& l : lib->linksOf(tom)) {
        const Entry* o = lib->entry(l.to == tom ? l.from : l.to);
        if (l.label == "From" && o && o->name == "Highcliff") from = true;
        if (l.label == "Lives in" && o && o->name == "Valona") lives = true;
    }
    CHECK(from && lives);
    CHECK(studio::reverseRelation("From") == "Hometown of" && studio::reverseRelation("Hometown of") == "From");
    // The blank page offers both, and the sexuality line.
    const std::string blank = pa::blankPage("character", {}, {});
    CHECK(blank.find("Sexuality:") != std::string::npos && blank.find("From:") != std::string::npos && blank.find("Lives in:") != std::string::npos);
    // The bible holds everything (0.60): a later book's note, where it's in the cast, its boards, what's on a map.
    {
        Entry u = *lib->entry(tom);
        u.setBookNote("title:Book 9", "Tom grows old.");
        lib->updateEntry(u);
        const Id ch = lib->createNode(a2, {}, NodeKind::Chapter, "The Forge");
        CHECK(studio::plan::setNodeCast(*lib, a2, ch, {tom}));
        CHECK(lib->setSetting("boards:world:" + world, R"({"boards":[{"id":"b1","name":"Tom's armour","entry":")" + tom +
                                                           R"(","items":[{"id":"i1","blob":"nothere","caption":"Plate"}]}]})"));
        namespace bible = studio::bible;
        const auto src = bible::printableSource(*lib, bible::Options{world, {}, false, {}}, "Bible", "A. Writer");
        std::string page;
        for (auto& sec : src.sections) if (sec.title == "Tom") page = sec.content;
        CHECK(page.find("Book 9 (still to come)") != std::string::npos && page.find("Tom grows old.") != std::string::npos);
        CHECK(page.find("Appears in") != std::string::npos && page.find("The Forge") != std::string::npos);
        const std::string site = dir + "/site060";
        std::string err;
        CHECK(bible::exportWebsite(*lib, bible::Options{world, {}, false, {}}, site, err));
        std::string html;
        for (auto& f : std::filesystem::directory_iterator(site))
            if (f.path().filename().string().rfind("e-", 0) == 0) {
                std::ifstream in(f.path());
                const std::string t((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                if (t.find("<h1>Tom</h1>") != std::string::npos) html = t;
            }
        CHECK(html.find("Book 9") != std::string::npos && html.find("Appears in") != std::string::npos && html.find("The Forge") != std::string::npos);
        CHECK(html.find("Sexuality") != std::string::npos && html.find("Bisexual") != std::string::npos);
    }
}

// A fake GitHub (0.61): the git data endpoints Writing Studio uses, over a MemoryRemote-like store.
struct FakeGitHub {
    std::map<std::string, std::string> blobs;                 // sha -> bytes
    std::map<std::string, std::map<std::string, std::string>> trees;   // tree sha -> path -> blob sha
    std::map<std::string, std::string> commitTree;            // commit -> tree
    std::string ref;                                          // the branch's commit ("" = empty repository)
    std::string token = "ghp_secret_token_123";
    bool isPrivate = true;
    size_t maxTreeEntries = 0;   // like GitHub timing out on a big tree: 422 above this many entries (0: no limit)
    int treePosts = 0;
    int n = 0, requests = 0;
    bool sawToken = true;
    std::string fresh() { return studio::sync::sha1Hex("x" + std::to_string(++n)); }
    studio::sync::HttpResponse operator()(const std::string& method, const std::string& url,
                                          const std::vector<std::pair<std::string, std::string>>& headers, const std::string& body) {
        using nlohmann::json;
        ++requests;
        bool auth = false;
        for (auto& [k, v] : headers) if (k == "Authorization" && v == "Bearer " + token) auth = true;
        if (!auth) { sawToken = false; return {401, R"({"message":"Bad credentials"})", ""}; }
        const std::string base = "https://api.github.com/repos/me/alvora";
        if (url.rfind(base, 0) != 0) return {404, "{}", ""};
        std::string path = url.substr(base.size());
        const json in = json::parse(body.empty() ? "{}" : body, nullptr, false);
        if (method == "GET" && path.empty()) return {200, json{{"private", isPrivate}, {"default_branch", "main"}}.dump(), ""};
        if (method == "GET" && path == "/git/ref/heads/main") {
            if (ref.empty()) return {409, R"({"message":"Git Repository is empty."})", ""};
            return {200, json{{"object", {{"sha", ref}}}}.dump(), ""};
        }
        if (method == "GET" && path.rfind("/git/commits/", 0) == 0) return {200, json{{"tree", {{"sha", commitTree[path.substr(13)]}}}}.dump(), ""};
        if (method == "GET" && path.rfind("/git/trees/", 0) == 0) {
            const std::string t = path.substr(11, path.find('?') - 11);
            json arr = json::array();
            for (auto& [p, sha] : trees[t]) arr.push_back({{"path", p}, {"type", "blob"}, {"sha", sha}, {"mode", "100644"}});
            return {200, json{{"tree", arr}, {"truncated", false}}.dump(), ""};
        }
        if (method == "GET" && path.rfind("/git/blobs/", 0) == 0) {
            auto it = blobs.find(path.substr(11));
            if (it == blobs.end()) return {404, "{}", ""};
            std::string b64 = studio::sync::base64Encode(it->second);
            for (size_t i = 60; i < b64.size(); i += 61) b64.insert(i, "\n");   // GitHub wraps its base64
            return {200, json{{"content", b64}, {"encoding", "base64"}}.dump(), ""};
        }
        if (method == "PUT" && path == "/contents/README.md" && ref.empty()) {
            const std::string bytes = studio::sync::base64Decode(in.value("content", ""));
            const std::string sha = studio::sync::gitBlobSha(bytes);
            blobs[sha] = bytes;
            const std::string t = fresh();
            trees[t] = {{"README.md", sha}};
            ref = fresh();
            commitTree[ref] = t;
            return {201, "{}", ""};
        }
        if (method == "POST" && path == "/git/blobs") {
            const std::string bytes = studio::sync::base64Decode(in.value("content", ""));
            const std::string sha = studio::sync::gitBlobSha(bytes);
            blobs[sha] = bytes;
            return {201, json{{"sha", sha}}.dump(), ""};
        }
        if (method == "POST" && path == "/git/trees") {
            ++treePosts;
            if (maxTreeEntries && in["tree"].size() > maxTreeEntries)
                return {422, R"({"message":"Sorry, your request timed out.  It's likely that your input was too large to process."})", ""};
            auto files = trees[in.value("base_tree", "")];
            for (auto& e : in["tree"]) {
                if (e["sha"].is_null()) files.erase(e.value("path", ""));
                else files[e.value("path", "")] = e.value("sha", "");
            }
            const std::string t = fresh();
            trees[t] = files;
            return {201, json{{"sha", t}}.dump(), ""};
        }
        if (method == "POST" && path == "/git/commits") {
            const std::string c = fresh();
            commitTree[c] = in.value("tree", "");
            return {201, json{{"sha", c}}.dump(), ""};
        }
        if (method == "PATCH" && path == "/git/refs/heads/main") {
            ref = in.value("sha", "");
            return {200, "{}", ""};
        }
        return {404, "{}", ""};
    }
};

static void testWorldSync() {
    namespace sy = studio::sync;
    // Hashes and links.
    CHECK(sy::sha1Hex("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(sy::gitBlobSha("") == "e69de29bb2d1d6434b8b29ae775ad8c2e48c5391");
    CHECK(sy::gitBlobSha("hello\n") == "ce013625030ba8dba906f756967f9e9ca394464a");
    for (const std::string s : {std::string(""), std::string("a"), std::string("ab"), std::string("abc"), std::string("\x00\xff\x10", 3)})
        CHECK(sy::base64Decode(sy::base64Encode(s)) == s);
    CHECK(sy::base64Encode("hello") == "aGVsbG8=");
    std::string o, r;
    CHECK(sy::parseRepoLink("https://github.com/CJ/alvora-bible", o, r) && o == "CJ" && r == "alvora-bible");
    CHECK(sy::parseRepoLink("github.com/CJ/alvora.git", o, r) && r == "alvora");
    CHECK(sy::parseRepoLink("git@github.com:CJ/alvora.git", o, r) && o == "CJ");
    CHECK(sy::parseRepoLink(" CJ/alvora ", o, r) && sy::parseRepoLink("https://github.com/CJ/alvora/tree/main", o, r) && r == "alvora");
    CHECK(!sy::parseRepoLink("https://gitlab.com/CJ/alvora", o, r) && !sy::parseRepoLink("alvora", o, r));
    CHECK(sy::validPath("books/b1/nodes/n1.json") && sy::validPath("objects/ab/" + std::string(62, 'c')));
    CHECK(!sy::validPath("../world.json") && !sy::validPath("books/../x.json") && !sy::validPath("objects/ab/xyz") && !sy::validPath("README.md"));

    // A world written on the first computer.
    auto a = openOk(tempLibrary("sync-a"));
    const Id world = a->createWorld("Alvora");
    const Id series = a->createSeries("The Alvora Chronicles", world);
    const Id book = a->createBook("Dragonfall", world);
    CHECK(a->addBookToSeries(book, series));
    const Id ch1 = a->createNode(book, {}, NodeKind::Chapter, "The Forge");
    const std::string text1 = R"({"type":"doc","content":[{"type":"paragraph","content":[{"type":"text","text":"Tom lit the forge."}]}]})";
    CHECK(a->saveNodeContent(ch1, text1));
    const Id tom = a->createEntry(world, {}, "character", "Tom");
    const Id cliff = a->createEntry(world, {}, "place", "Highcliff");
    CHECK(a->addLink(tom, cliff, "Lives in"));
    const Id art = a->addArtwork(pngOf(3, 2), "Tom", "Bible");
    {
        Entry u = *a->entry(tom);
        u.imageBlob = a->artwork(art)->blob;
        u.summary = "A smith.";
        CHECK(a->updateEntry(u));
    }
    CHECK(a->setSetting("boards:world:" + world, R"({"boards":[]})"));
    CHECK(a->setSetting("unrelated", "stays here"));
    const auto files = a->worldFiles(world);
    CHECK(files.count("world.json") && files.count("series/" + series + ".json") && files.count("books/" + book + "/book.json"));
    CHECK(files.count("books/" + book + "/nodes/" + ch1 + ".json") && files.count("entries/" + tom + ".json") && files.count("links/" + tom + ".json"));
    CHECK(files.count("memberships/" + series + "+" + book + ".json") && files.count("artworks/" + art + ".json"));
    const std::string blob = a->artwork(art)->blob;
    CHECK(files.count("objects/" + blob.substr(0, 2) + "/" + blob.substr(2)));
    bool boardsFile = false, unrelatedFile = false;
    for (auto& [p, f] : files) {
        CHECK(sy::validPath(p));
        if (p.rfind("settings/", 0) == 0 && f.bytes.find("boards:world:") != std::string::npos) boardsFile = true;
        if (f.bytes.find("unrelated") != std::string::npos) unrelatedFile = true;
    }
    CHECK(boardsFile && !unrelatedFile);
    CHECK(a->worldFiles(world).at("entries/" + tom + ".json").bytes == files.at("entries/" + tom + ".json").bytes);   // the same bytes each time

    // To the repository, then onto a second computer.
    sy::MemoryRemote repo;
    sy::State sa, sb;
    sy::Report rep;
    std::string err;
    CHECK(sy::syncWorld(*a, world, repo, sa, rep, err));
    CHECK(rep.sent == (int)files.size() && rep.received == 0 && repo.commits == 1);
    CHECK(sy::syncWorld(*a, world, repo, sa, rep, err) && rep.sent == 0 && rep.received == 0 && repo.commits == 1);   // nothing new
    auto b = openOk(tempLibrary("sync-b"));
    CHECK(sy::syncWorld(*b, {}, repo, sb, rep, err));
    CHECK(rep.world == world && rep.received == (int)files.size());
    CHECK(b->world(world) && b->world(world)->name == "Alvora" && b->book(book) && b->book(book)->title == "Dragonfall");
    CHECK(b->nodeContent(ch1).find("Tom lit the forge.") != std::string::npos);
    CHECK(b->entry(tom) && b->entry(tom)->summary == "A smith." && b->linksOf(tom).size() == 1);
    CHECK(b->membership(series, book) && b->membership(series, book)->number == "1");
    CHECK(b->setting("boards:world:" + world) == R"({"boards":[]})" && b->setting("unrelated").empty());
    CHECK(b->artworkForBlob(blob) && std::filesystem::exists(b->blobPath(blob)));
    CHECK(sy::loadState(*b, world).files.size() == files.size());
    // Opening it again in the same library is refused.
    sy::State again;
    CHECK(!sy::syncWorld(*b, {}, repo, again, rep, err) && err.find("already") != std::string::npos);

    // Changed on one computer, then the other: each gets the other's.
    const std::string text2 = R"({"type":"doc","content":[{"type":"paragraph","content":[{"type":"text","text":"Tom banked the forge."}]}]})";
    CHECK(b->saveNodeContent(ch1, text2));
    const Id ch2 = b->createNode(book, {}, NodeKind::Chapter, "The Storm Wall");
    {
        Entry u = *a->entry(cliff);
        u.summary = "A harbour town.";
        CHECK(a->updateEntry(u));
    }
    CHECK(sy::syncWorld(*b, world, repo, sb, rep, err) && rep.sent >= 2 && rep.received == 0);
    CHECK(sy::syncWorld(*a, world, repo, sa, rep, err));
    CHECK(a->nodeContent(ch1).find("banked") != std::string::npos && a->node(ch2) && a->node(ch2)->title == "The Storm Wall");
    CHECK(rep.conflicts.empty());
    CHECK(sy::syncWorld(*b, world, repo, sb, rep, err) && b->entry(cliff)->summary == "A harbour town.");

    // Changed on both: the newer is kept, the other kept as a saved version, and both end up the same.
    const std::string textA = R"({"type":"doc","content":[{"type":"paragraph","content":[{"type":"text","text":"Written on A."}]}]})";
    const std::string textB = R"({"type":"doc","content":[{"type":"paragraph","content":[{"type":"text","text":"Written on B."}]}]})";
    CHECK(a->saveNodeContent(ch1, textA) && b->saveNodeContent(ch1, textB));
    CHECK(sy::syncWorld(*a, world, repo, sa, rep, err));
    CHECK(sy::syncWorld(*b, world, repo, sb, rep, err) && rep.conflicts.size() == 1);
    CHECK(sy::syncWorld(*a, world, repo, sa, rep, err));
    CHECK(a->nodeContent(ch1) == b->nodeContent(ch1));
    const bool aWon = b->nodeContent(ch1).find("Written on A.") != std::string::npos;
    bool keptOther = false;
    for (auto& v : b->nodeVersions(ch1))
        if (b->nodeVersionContent(v.id).find(aWon ? "Written on B." : "Written on A.") != std::string::npos && v.reason == "From another computer") keptOther = true;
    CHECK(keptOther);

    // Links added on both sides are all kept; an entry gone for good on one goes on the other.
    const Id smithy = a->createEntry(world, {}, "place", "The Smithy");
    CHECK(sy::syncWorld(*a, world, repo, sa, rep, err) && sy::syncWorld(*b, world, repo, sb, rep, err));
    CHECK(a->addLink(tom, smithy, "Owns") && b->addLink(tom, smithy, "Works at"));
    CHECK(sy::syncWorld(*a, world, repo, sa, rep, err) && sy::syncWorld(*b, world, repo, sb, rep, err) && sy::syncWorld(*a, world, repo, sa, rep, err));
    CHECK(a->linksOf(tom).size() == 3 && b->linksOf(tom).size() == 3);
    CHECK(a->trashEntry(smithy) && a->emptyTrash());
    CHECK(sy::syncWorld(*a, world, repo, sa, rep, err) && sy::syncWorld(*b, world, repo, sb, rep, err));
    CHECK(!b->entry(smithy) && b->linksOf(tom).size() == 1);

    // Another computer syncing in between: the sync goes again and loses nothing.
    struct Racy : sy::MemoryRemote {
        std::function<void()> before;
        bool commit(const sy::RemoteState& parent, const std::map<std::string, std::optional<std::string>>& changes, const std::string& m,
                    std::string& c, bool& moved, std::string& e) override {
            if (before) { auto f = before; before = nullptr; f(); }
            return sy::MemoryRemote::commit(parent, changes, m, c, moved, e);
        }
    } racy;
    racy.files = repo.files;
    racy.commitId = repo.commitId;
    {
        Entry u = *a->entry(tom);
        u.summary = "A smith of Highcliff.";
        CHECK(a->updateEntry(u));
    }
    racy.before = [&] {
        sy::State sc = sb;
        sy::Report rc;
        std::string ec;
        CHECK(b->setSetting("plan:" + book, R"({"targetWords":90000})"));
        CHECK(sy::syncWorld(*b, world, racy, sc, rc, ec) && rc.sent == 1);
        sb = sc;
    };
    CHECK(sy::syncWorld(*a, world, racy, sa, rep, err));
    CHECK(a->setting("plan:" + book) == R"({"targetWords":90000})" && racy.files.at("entries/" + tom + ".json").find("A smith of Highcliff.") != std::string::npos);

    // A repository holding something else is refused; bad files in it are ignored.
    {
        sy::MemoryRemote odd;
        odd.files["notes.txt"] = "hello";
        auto c = openOk(tempLibrary("sync-c"));
        sy::State sc;
        CHECK(!sy::syncWorld(*c, {}, odd, sc, rep, err) && err.find("doesn't hold") != std::string::npos);
        sy::MemoryRemote evil = repo;
        evil.files["../../escape.json"] = "{}";
        evil.files["entries/zzz.json"] = R"({"id":"zzz","world_id":"another","kind":"character","name":"Spy"})";
        CHECK(!sy::syncWorld(*c, {}, evil, sc, rep, err) && err.find("another world") != std::string::npos);
        CHECK(!c->world(world));   // nothing half-written
    }

    // Through the fake GitHub: an empty repository started, a world sent and read back, the token only in its header.
    {
        FakeGitHub gh;
        auto http = [&gh](const std::string& m, const std::string& u, const std::vector<std::pair<std::string, std::string>>& h, const std::string& body) {
            return gh(m, u, h, body);
        };
        sy::GitHubRemote remote("me", "alvora", gh.token, http);
        sy::RepoInfo info;
        CHECK(remote.info(info, err) && info.isPrivate && remote.branch() == "main");
        auto d = openOk(tempLibrary("sync-d"));
        const Id w2 = d->createWorld("Telvana");
        const Id b2 = d->createBook("The Envoy of Telvana", w2);
        const Id n2 = d->createNode(b2, {}, NodeKind::Chapter, "Arrival");
        CHECK(d->saveNodeContent(n2, text1));
        const Id pic = d->addArtwork(pngOf(5, 5), "Map", "Bible");
        const Id ent = d->createEntry(w2, {}, "place", "Telvana");
        {
            Entry u = *d->entry(ent);
            u.imageBlob = d->artwork(pic)->blob;
            CHECK(d->updateEntry(u));
        }
        sy::State sd;
        CHECK(sy::syncWorld(*d, w2, remote, sd, rep, err));
        CHECK(!gh.ref.empty() && gh.trees[gh.commitTree[gh.ref]].count("README.md") && gh.trees[gh.commitTree[gh.ref]].count("world.json"));
        auto e2 = openOk(tempLibrary("sync-e"));
        sy::GitHubRemote remote2("me", "alvora", gh.token, http);
        sy::State se;
        CHECK(sy::syncWorld(*e2, {}, remote2, se, rep, err) && rep.world == w2);
        CHECK(e2->nodeContent(n2) == d->nodeContent(n2) && e2->entry(ent) && std::filesystem::exists(e2->blobPath(d->artwork(pic)->blob)));
        CHECK(gh.sawToken);
        // A wrong token: refused, said plainly, and the token isn't in the message.
        // A big world on a GitHub that times out on big trees (CJ's 422, 23 Nov 2026): built in batches instead.
        {
            FakeGitHub big;
            big.maxTreeEntries = 7;
            auto bhttp = [&big](const std::string& m, const std::string& u, const std::vector<std::pair<std::string, std::string>>& h,
                                const std::string& body) { return big(m, u, h, body); };
            sy::GitHubRemote rb("me", "alvora", big.token, bhttp);
            auto g = openOk(tempLibrary("sync-g"));
            const Id w3 = g->createWorld("Alvora");
            for (int i = 0; i < 40; ++i) g->createEntry(w3, {}, "character", "Person " + std::to_string(i));
            const size_t want = g->worldFiles(w3).size();
            CHECK(want > 40);
            sy::State sg;
            CHECK(sy::syncWorld(*g, w3, rb, sg, rep, err));
            CHECK(big.treePosts > (int)(want / 7));
            CHECK(big.trees[big.commitTree[big.ref]].size() == want + 1);   // every file, and the README
            auto h = openOk(tempLibrary("sync-h"));
            sy::GitHubRemote rb2("me", "alvora", big.token, bhttp);
            sy::State sh;
            CHECK(sy::syncWorld(*h, {}, rb2, sh, rep, err) && h->entriesOf(w3).size() == 40);
        }
        sy::GitHubRemote bad("me", "alvora", "wrong_token_456", http);
        sy::State sf;
        auto f2 = openOk(tempLibrary("sync-f"));
        CHECK(!sy::syncWorld(*f2, {}, bad, sf, rep, err) && err.find("token") != std::string::npos && err.find("wrong_token_456") == std::string::npos);
    }
}

// A plan replaces the book's plan (0.62): CJ's book had "1. 🗡 Steel and Snow" and a plan said "Steel and Snow", so
// every chapter was added again. The same plan now finds them, and tidies up the copies.
static void testPlanReplaces() {
    const std::string dir = tempLibrary("plan-062");
    auto lib = openOk(dir);
    const Id world = lib->createWorld("Alvora");
    const Id book = lib->createBook("Shards of Light", world);
    namespace plan = studio::plan;
    CHECK(plan::matchKey("1. \xF0\x9F\x97\xA1 Steel and Snow") == "steel and snow" && plan::matchKey("Steel and Snow") == "steel and snow");
    CHECK(plan::matchKey("2. \xE2\x9C\xA7 The Canyon") == "the canyon" && plan::matchKey("13. Chapter 13") == "#13" && plan::matchKey("Chapter 13") == "#13");
    CHECK(plan::matchKey("19. \xF0\x9F\x97\xA1") == "#19" && plan::matchKey("Sah'ra's Café") == "sah'ra's caf\xC3\xA9");
    const std::string words = R"({"type":"doc","content":[{"type":"paragraph","content":[{"type":"text","text":"Snow fell on the forge."}]}]})";
    const Id soul = lib->createNode(book, {}, NodeKind::Part, "I. Soul");
    const Id c1 = lib->createNode(book, soul, NodeKind::Chapter, "1. \xF0\x9F\x97\xA1 Steel and Snow");
    const Id c2 = lib->createNode(book, soul, NodeKind::Chapter, "2. \xE2\x9C\xA7 The Canyon");
    const Id c13 = lib->createNode(book, soul, NodeKind::Chapter, "13. Chapter 13");
    const Id extra = lib->createNode(book, soul, NodeKind::Chapter, "An old draft");
    for (const Id& c : {c1, c2, c13, extra}) CHECK(lib->saveNodeContent(c, words));
    // The mess: a second part with empty copies, as the old import made.
    const Id soul2 = lib->createNode(book, {}, NodeKind::Part, "I. Soul");
    const Id d1 = lib->createNode(book, soul2, NodeKind::Chapter, "Steel and Snow");
    const Id d2 = lib->createNode(book, soul2, NodeKind::Chapter, "The Canyon");
    const Id d13 = lib->createNode(book, soul2, NodeKind::Chapter, "Chapter 13");
    const std::string text =
        "Part: I. Soul\n\n"
        "Chapter: The Canyon\nStatus: Draft\nThreads: Tom and Sarrah\nSarrah hunts sherds.\n\n"
        "Chapter: Steel and Snow\nThreads: Someone watching\nTom flies to Gir-Garhm.\n\n"
        "Chapter: Chapter 13\nSarrah meets Ruby.\n\n"
        "Thread: Someone watching\nColour: #7F8C8D\nAbout: A figure on the ridge.\nA figure on the ridge.\nChapters: 1, 3, 7, 11\n\n"
        "Thread: Tom and Sarrah\nAbout: From a rescue to a kiss.\n";
    plan::PlanningImport pi;
    CHECK(plan::importPlanning(*lib, book, plan::structureScope(*lib, book), text, pi));
    const auto& o = pi.outline;
    CHECK(o.made.empty() && o.updatedParts == 1 && o.updatedChapters == 3);
    // The written chapters took the plan, kept their titles, and stand in the plan's order in the part with writing.
    CHECK(lib->node(c2)->synopsis == "Sarrah hunts sherds." && lib->node(c1)->synopsis == "Tom flies to Gir-Garhm." && lib->node(c13)->synopsis == "Sarrah meets Ruby.");
    CHECK(lib->node(c1)->title == "1. \xF0\x9F\x97\xA1 Steel and Snow");
    const auto kids = lib->childrenOf(book, soul);
    CHECK(kids.size() == 4 && kids[0]->id == c2 && kids[1]->id == c1 && kids[2]->id == c13 && kids[3]->id == extra);
    // The empty copies and their part are in the Trash; the written chapter the plan doesn't have stays, and is named.
    CHECK(lib->node(d1)->deleted && lib->node(d2)->deleted && lib->node(d13)->deleted && lib->node(soul2)->deleted);
    CHECK(o.trashed >= 1 && o.keptWithWriting == std::vector<std::string>{"An old draft"});
    // Threads: each chapter's from the plan (the "Chapters:" numbers, from the old copy, aren't used), descriptions once.
    const auto th = plan::threadsOf(*lib, book);
    const plan::Thread* watching = th.named("Someone watching");
    CHECK(watching && watching->about == "A figure on the ridge." && th.has(c1, watching->id) && !th.has(c2, watching->id) && !th.has(c13, watching->id));
    CHECK(th.named("Tom and Sarrah") && th.has(c2, th.named("Tom and Sarrah")->id));
    // Again: nothing more is made or thrown away.
    plan::PlanningImport again;
    CHECK(plan::importPlanning(*lib, book, plan::structureScope(*lib, book), text, again));
    CHECK(again.outline.made.empty() && again.outline.trashed == 0 && plan::threadsOf(*lib, book).named("Someone watching")->about == "A figure on the ridge.");
}

int main() {
    testPlanReplaces();
    testWorldSync();
    testSexualityFromSeries();
    testEntryPaste();
    testWorldFonts();
    testConlang();
    testBackup();
    testReview();
    testWorldbuilding();
    testPublish();
    testImportControls();
    testBookNotes();
    testFamilyAndWordings();
    testCastAndCreatureFamily();
    testPlanning();
    testCovers();
    testBibleTidy();
    testEntryPages();
    testFamilyTree();
    testFamilyTreePicture();
    testFamilyKinds();
    testBuiltInFonts();
    testFamilyFounders();
    testContinuity();
    testBibleExport();
    testEbookHtml();
    testPdfLikeHeadings();
    testTypeset();
    testArtwork();
    testImportOffice();
    testPlanHandOver();
    testImportEpub();
    testImportText();
    testDocument();
    testManuscript();
    testUpgradeFromVersion1();
    testCreateAndReopen();
    testOrdering();
    testMoving();
    testTrash();
    testBlobsExportAndSafety();
    testNewerFormatRefused();
    std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
