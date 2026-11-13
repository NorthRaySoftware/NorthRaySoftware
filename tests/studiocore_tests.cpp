// Tests for the Writing & Publishing Studio's library core. Each test works in
// a fresh temporary folder and reopens the library to prove what was saved.
#include "studio/artwork.hpp"
#include "studio/ebook.hpp"
#include "studio/document.hpp"
#include "studio/import.hpp"
#include "studio/library.hpp"
#include "studio/publish.hpp"
#include "studio/review.hpp"
#include "miniz.h"
#include "studio/typeset.hpp"
#include "importutil.hpp"
#include "sqlite3.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
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
    fs::remove_all(dir);
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
        CHECK(lib->mentionsOf(*lib->entry(grey)).size() == 1 && lib->mentionsOf(*lib->entry(grey))[0].count == 2);  // "Grey Wanderer" and "Wanderer"
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

int main() {
    testReview();
    testWorldbuilding();
    testPublish();
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
