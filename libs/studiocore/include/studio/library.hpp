// Writing & Publishing Studio: the Author Library.
//
// Author Library -> World -> Series -> Book. Series and worlds are optional:
// a book can sit in a world without a series, or outside any world. Books join
// series through Membership rows, so organising never copies or moves files.
//
// Everything is stored in one SQLite database inside the library folder (see
// docs/studio/FORMAT.md). Each public mutation is a single transaction: it
// either happens completely or not at all. Portable C++; no Windows UI code.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <map>
#include <string>
#include <vector>

struct sqlite3;

namespace studio {

using Id = std::string;          // random UUID, lower-case hex with dashes
using Time = std::int64_t;       // seconds since 1970-01-01 UTC

enum class BookStatus { Idea, Planning, Drafting, Revising, Editing, ReadyToPublish, Published, OnHold };
enum class SeriesStatus { Planned, Ongoing, Complete, OnHold };
enum class Instalment { Novel, Novella, Companion, ShortStory, Collection };

const char* toString(BookStatus s);
const char* toString(SeriesStatus s);
const char* toString(Instalment k);
const char* label(BookStatus s);       // "Ready to publish"
const char* label(SeriesStatus s);
const char* label(Instalment k);
std::optional<BookStatus> bookStatusFrom(const std::string& s);
std::optional<SeriesStatus> seriesStatusFrom(const std::string& s);
std::optional<Instalment> instalmentFrom(const std::string& s);

struct World {
    Id id;
    std::string name;
    std::string description;
    std::string accent;      // "#RRGGBB" or empty
    std::string notes;
    std::string bannerBlob;  // content hash in objects/, or empty
    std::string logoBlob;
    double order = 0;
    Time created = 0, modified = 0, deleted = 0;  // deleted != 0: in the Trash
};

struct Series {
    Id id;
    Id worldId;              // empty: a series outside any world
    std::string name;
    std::string description;
    SeriesStatus status = SeriesStatus::Planned;
    std::string notes;
    std::string coverBlob;
    std::string logoBlob;
    double order = 0;
    Time created = 0, modified = 0, deleted = 0;
};

struct Book {
    Id id;
    Id worldId;              // empty: an independent book
    std::string title;
    std::string subtitle;
    std::string author;
    std::string penName;
    std::string synopsis;
    std::string genre;
    std::string language = "en-GB";
    std::string isbn;
    std::string edition;
    BookStatus status = BookStatus::Idea;
    std::vector<std::string> tags;
    std::string coverBlob;
    std::string trim;            // print trim size id (studio/artwork.hpp); empty = the default
    std::int64_t wordCount = 0;  // filled in by the manuscript editor (Milestone 2)
    double order = 0;            // position among the world's standalone books
    Time created = 0, modified = 0, deleted = 0;
};

// A book's place in a series. Reading order and chronological order are
// independent; `number` is free text ("1", "2.5") and empty when unnumbered.
struct Membership {
    Id seriesId;
    Id bookId;
    double readingOrder = 0;
    double chronoOrder = 0;
    std::string number;
    Instalment kind = Instalment::Novel;
};

// ------------------------------------------------------------ manuscripts
// A book's text is a tree: Part -> Chapter -> Scene, every level optional.
// Parts sit at the top; chapters at the top or in a part; scenes anywhere
// above them. Text lives in chapters and scenes (a chapter with scenes can
// still have an opening of its own).
enum class NodeKind { Part, Chapter, Scene };
enum class NodeStatus { Outline, Draft, Revised, Final };
const char* toString(NodeKind k);
const char* toString(NodeStatus s);
const char* label(NodeKind k);
const char* label(NodeStatus s);
std::optional<NodeKind> nodeKindFrom(const std::string& s);
std::optional<NodeStatus> nodeStatusFrom(const std::string& s);

struct ManuscriptNode {
    Id id;
    Id bookId;
    Id parentId;                 // empty: top level of the book
    NodeKind kind = NodeKind::Chapter;
    std::string title;
    std::string synopsis;
    NodeStatus status = NodeStatus::Draft;
    double order = 0;
    std::int64_t wordCount = 0;  // of this node's own text
    Id povId;                    // planning (6): the viewpoint character (a worldbuilding entry), or empty
    std::int64_t targetWords = 0;   // planning (6): how long it's meant to be; 0 = no target
    Time created = 0, modified = 0, deleted = 0;
};

// One saved version of a node's text, kept so changes can be undone later.
struct NodeVersion {
    std::int64_t id = 0;
    Time saved = 0;
    std::int64_t wordCount = 0;
    std::string reason;          // "autosave", "before import", "before restore"
};

// For bulk creation (imports).
struct NewNode {
    NodeKind kind = NodeKind::Chapter;
    std::string title;
    std::string content;         // document JSON (see studio/document.hpp); empty = empty document
    std::vector<NewNode> children;
};

// A picture in the artwork library. Each stored picture is listed once (by
// its content), however many chapters use it.
struct Artwork {
    Id id;
    std::string blob;            // SHA-256 of the file in objects/
    std::string name;
    std::string kind = "illustration";   // see ArtworkKind in studio/artwork.hpp
    int width = 0, height = 0;   // pixels
    std::string format;          // png, jpeg, ...
    std::string source;          // where it came from ("Imported from novel.epub", a file name)
    std::string notes;
    Time created = 0, modified = 0, deleted = 0;
};

// One edition of a book (Milestone 7): paperback, hardback, large print or
// ebook, each with its own ISBN. Print editions can have their own trim size,
// design and paper; empty means "the book's own" (its trim and Typesetting design).
struct Edition {
    Id id;
    Id bookId;
    std::string kind = "paperback";   // "paperback", "hardback", "large-print", "ebook"
    std::string name;                 // "" = the kind's name
    std::string isbn;
    std::string trim;                 // print: trim size id, "" = the book's
    std::string design;               // print: a preset id, "" = the book's Typesetting design
    std::string paper = "white";      // print: "white" or "cream"
    bool bleed = false;               // print: 0.125in bleed in the PDF
    bool colour = false;              // print: pictures in colour (CMYK)
    double order = 0;
    Time created = 0, modified = 0;
    bool print() const { return kind != "ebook"; }
};
const char* editionKindLabel(const std::string& kind);   // "Paperback", ...

// A worldbuilding entry (Milestone 8): a character, place, group, thing, piece
// of lore or event. It belongs to a world, or (world empty) to one independent book.
struct Entry {
    Id id;
    Id worldId;
    Id bookId;                        // only when worldId is empty
    std::string kind = "character";   // "character", "place", "group", "thing", "lore", "event"
    std::string name;
    std::vector<std::string> aliases; // other names it goes by (also found in the text)
    std::string summary;              // a line or two
    std::string details;              // "Label: value" lines (Age: 34, Eyes: grey, ...)
    std::string notes;
    std::string imageBlob;
    double order = 0;
    Time created = 0, modified = 0, deleted = 0;
};
const std::vector<std::string>& entryKinds();          // in display order
const char* entryKindLabel(const std::string& kind);   // "Character"
const char* entryKindPlural(const std::string& kind);  // "Characters"

// A relationship between two entries, read from `from`: "Ada" Member of "The Guild".
struct EntryLink {
    Id from, to;
    std::string label;
};
// The relationships offered, each with how it reads from the other side.
struct Relation { const char* label; const char* reverse; };
const std::vector<Relation>& relations();
std::string reverseRelation(const std::string& label);

// Where an entry's name (or another name it goes by) appears in the manuscripts.
struct Mention {
    Id bookId, nodeId;
    int count = 0;
    std::string snippet;              // the first appearance, with some text either side
};

// A snapshot of a whole book's manuscript at one moment (Milestone 9), to
// compare against later. Kept until removed (autosaved versions are pruned).
struct Snapshot {
    Id id, bookId;
    std::string name;
    Time taken = 0;
    std::int64_t words = 0;
    int parts = 0;                    // chapters, scenes and parts saved
};
struct SnapshotNode {
    Id nodeId;
    NodeKind kind = NodeKind::Chapter;
    std::string title;
    std::string content;              // document JSON as it was
    std::int64_t words = 0;
};

// A comment on part of a chapter's or scene's text (Milestone 9). The text it's
// on carries a "comment" mark with the same id.
struct Comment {
    Id id;                            // 16 hex digits, made by the editor
    Id nodeId;
    std::string body;
    std::string quote;                // the text it was made on, as it was then
    Time created = 0, modified = 0;
    Time resolved = 0;                // 0 = open
};

struct OpenResult {
    bool ok = false;
    bool created = false;          // a new, empty library was made
    std::string error;
    std::vector<std::string> notes;  // e.g. sync-folder warning, backup made
};

class Library {
public:
    // Opens the library folder, creating it (and an empty library) if needed.
    // Makes a rotating backup and checks the database's integrity.
    static std::unique_ptr<Library> open(const std::string& folder, OpenResult& result);
    ~Library();
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;

    const std::string& folder() const { return folder_; }
    int schemaVersion() const;
    static constexpr int kSchemaVersion = 8;

    // Everything is kept in memory and written through on every change.
    const std::vector<World>& worlds() const { return worlds_; }
    const std::vector<Series>& series() const { return series_; }
    const std::vector<Book>& books() const { return books_; }
    const std::vector<Membership>& memberships() const { return memberships_; }

    const World* world(const Id& id) const;
    const Series* seriesById(const Id& id) const;
    const Book* book(const Id& id) const;

    // Views (Trash excluded unless stated). Ordered as the user arranged them.
    std::vector<const World*> liveWorlds() const;
    std::vector<const Series*> seriesIn(const Id& worldId) const;      // "" = series outside worlds
    std::vector<const Book*> standaloneBooksIn(const Id& worldId) const;  // in no live series; "" = independent
    std::vector<const Book*> booksInSeries(const Id& seriesId, bool chronological = false) const;
    std::vector<const Series*> seriesOfBook(const Id& bookId) const;
    const Membership* membership(const Id& seriesId, const Id& bookId) const;
    std::vector<const Book*> liveBooks() const;
    bool inTrash(const World& w) const { return w.deleted != 0; }
    bool bookVisible(const Book& b) const;      // not trashed, and its world isn't trashed
    bool seriesVisible(const Series& s) const;

    // Creating. Names are trimmed; an empty name gets a sensible default.
    Id createWorld(const std::string& name);
    Id createSeries(const std::string& name, const Id& worldId);
    Id createBook(const std::string& title, const Id& worldId, const Id& seriesId = {});

    // Editing: replaces the stored fields (id, timestamps and order are kept).
    bool updateWorld(const World& w);
    bool updateSeries(const Series& s);
    bool updateBook(const Book& b);
    bool updateMembership(const Membership& m);

    // Ordering: the given ids, in their new order.
    bool reorderWorlds(const std::vector<Id>& ids);
    bool reorderSeries(const std::vector<Id>& ids);
    bool reorderStandaloneBooks(const std::vector<Id>& ids);
    bool reorderSeriesBooks(const Id& seriesId, const std::vector<Id>& bookIds, bool chronological);

    // Moving. A book belongs to the world of any series it's in, so adding it to
    // a series in another world moves it there, and leaves series it no longer fits.
    // seriesLostByMove() lists those series so the UI can ask first.
    std::vector<const Series*> seriesLostByMove(const Id& bookId, const Id& toWorldId) const;
    bool addBookToSeries(const Id& bookId, const Id& seriesId);
    bool removeBookFromSeries(const Id& bookId, const Id& seriesId);  // keeps the book
    bool makeStandalone(const Id& bookId);                           // leaves every series
    bool moveBookToWorld(const Id& bookId, const Id& worldId);
    // Moves the series and the member books that were in its old world.
    bool moveSeriesToWorld(const Id& seriesId, const Id& worldId);

    // Trash: nothing is lost until the Trash is emptied.
    bool trashWorld(const Id& id);
    bool trashSeries(const Id& id);
    bool trashBook(const Id& id);
    bool restore(const Id& id);
    struct TrashCounts { int worlds = 0, series = 0, books = 0, entries = 0; };
    TrashCounts trashCounts() const;
    bool emptyTrash();

    // Manuscripts. Lists are live nodes in the user's order.
    const std::vector<ManuscriptNode>& nodes() const { return nodes_; }
    const ManuscriptNode* node(const Id& id) const;
    std::vector<const ManuscriptNode*> childrenOf(const Id& bookId, const Id& parentId) const;
    // Depth-first, in reading order.
    std::vector<const ManuscriptNode*> manuscriptOf(const Id& bookId) const;
    std::vector<const ManuscriptNode*> trashedNodesOf(const Id& bookId) const;  // top-most trashed nodes
    static bool canContain(std::optional<NodeKind> parent, NodeKind child);  // nullopt = book level
    // Creates after `afterId` among its siblings (or at the end). An empty
    // title gets "Chapter 3" etc. Returns "" if the nesting isn't allowed.
    Id createNode(const Id& bookId, const Id& parentId, NodeKind kind, const std::string& title = {}, const Id& afterId = {});
    bool updateNode(const ManuscriptNode& n);           // title, synopsis, status, kind, viewpoint, target
    bool moveNode(const Id& id, const Id& newParentId, int index);  // index among live siblings; -1 = end
    bool trashNode(const Id& id);                       // with everything inside it
    // Text. saveNodeContent sanitises, updates word counts (node and book) and
    // keeps a version every few minutes of editing (newest kVersionsKept).
    std::string nodeContent(const Id& id) const;        // document JSON; empty document if none
    bool saveNodeContent(const Id& id, const std::string& docJson);
    std::vector<NodeVersion> nodeVersions(const Id& id) const;  // newest first
    std::string nodeVersionContent(std::int64_t versionId) const;
    bool restoreNodeVersion(const Id& id, std::int64_t versionId);  // the current text is kept as a version first
    bool restoreNodeText(const Id& id, const std::string& docJson); // the same, from any text (a snapshot's)
    static constexpr int kVersionsKept = 40;
    static constexpr Time kVersionInterval = 10 * 60;
    // Adds a whole tree (an import) in one transaction, at the end of `parentId`.
    // Returns the ids of the top-level nodes created.
    std::vector<Id> addNodes(const Id& bookId, const Id& parentId, const std::vector<NewNode>& tree);
    std::int64_t bookWordCount(const Id& bookId) const;

    // Images and other binary files, stored once by content (SHA-256).
    std::string storeBlob(const std::string& bytes);  // returns the hash, "" on failure
    std::string blobPath(const std::string& hash) const;

    // Artwork library.
    const std::vector<Artwork>& artworks() const { return artworks_; }
    std::vector<const Artwork*> liveArtworks() const;          // newest first
    const Artwork* artwork(const Id& id) const;
    const Artwork* artworkForBlob(const std::string& hash) const;
    // Stores the picture and lists it (or returns the existing entry for the same
    // picture, bringing it out of the Trash). Returns "" if it isn't a picture.
    Id addArtwork(const std::string& bytes, const std::string& name, const std::string& source = {});
    // Lists a picture that's already stored (imports, older libraries).
    Id registerArtwork(const std::string& hash, const std::string& name, const std::string& source = {});
    bool updateArtwork(const Artwork& a);                      // name, kind, notes
    bool trashArtwork(const Id& id);                           // restore() brings it back
    // Live chapters and scenes whose text uses the picture.
    std::vector<const ManuscriptNode*> artworkUsage(const std::string& hash) const;

    // Editions of a book, in order. A book with none gets a paperback and an
    // ebook the first time ensureEditions() is called (the paperback takes the
    // book's ISBN and the publishing settings it had).
    std::vector<const Edition*> editionsOf(const Id& bookId) const;
    const Edition* edition(const Id& id) const;
    Id addEdition(const Id& bookId, const std::string& kind);
    bool updateEdition(const Edition& e);
    bool removeEdition(const Id& id);                         // gone for good (it holds settings, not writing)
    bool ensureEditions(const Id& bookId);
    // Worldbuilding: entries of a world, or of an independent book (worldId empty).
    std::vector<const Entry*> entriesOf(const Id& worldId, const Id& bookId = {}) const;   // live, by name
    std::vector<const Entry*> trashedEntries() const;
    const Entry* entry(const Id& id) const;
    Id createEntry(const Id& worldId, const Id& bookId, const std::string& kind, const std::string& name);
    bool updateEntry(const Entry& e);
    bool trashEntry(const Id& id);                     // restore() brings it back
    std::vector<EntryLink> linksOf(const Id& entryId) const;   // both directions, as stored
    bool addLink(const Id& from, const Id& to, const std::string& label);
    bool removeLink(const Id& from, const Id& to, const std::string& label);
    // Searches the manuscripts of the entry's world (or its book) for its names.
    std::vector<Mention> mentionsOf(const Entry& e) const;
    // For planning: which entries appear in each chapter and scene of a book
    // (node id -> entry ids, most mentioned first). One pass over the text.
    std::map<Id, std::vector<Id>> appearancesIn(const Id& bookId) const;
    // Comments (schema 8).
    std::vector<const Comment*> commentsOf(const Id& nodeId) const;     // in the order made
    std::vector<const Comment*> commentsInBook(const Id& bookId) const; // in manuscript order
    const Comment* comment(const Id& id) const;
    bool addComment(const Id& nodeId, const Id& commentId, const std::string& quote);
    bool updateComment(const Id& id, const std::string& body);
    // Resolving or removing also takes its mark out of the stored text when
    // `stripText` is set (the editor does it itself when the chapter is open).
    bool resolveComment(const Id& id, bool resolved, bool stripText);
    bool removeComment(const Id& id, bool stripText);
    bool commentAnchored(const Comment& c) const;                       // its mark is still in the text
    // Snapshots of a book (schema 7).
    Id takeSnapshot(const Id& bookId, const std::string& name);
    std::vector<Snapshot> snapshotsOf(const Id& bookId) const;    // newest first
    std::vector<SnapshotNode> snapshotNodes(const Id& snapshotId) const;   // in manuscript order
    bool renameSnapshot(const Id& snapshotId, const std::string& name);
    bool removeSnapshot(const Id& snapshotId);
    // Words added while writing, per day (local "YYYY-MM-DD"), from the editor's saves;
    // imports don't count. bookId "" = every book.
    std::map<std::string, std::int64_t> wordsWritten(const Id& bookId, const std::string& fromDay) const;
    static std::string localDay(Time t);
    // Small per-library settings (a book's typesetting design, ...), kept in the database.
    std::string setting(const std::string& key) const;
    bool setSetting(const std::string& key, const std::string& value);

    // A whole-library JSON file (without images) for moving or archiving.
    bool exportJson(const std::string& path, std::string& error) const;
    // Makes a backup copy of the database now. Keeps the newest `keep`.
    bool backup(std::string& error, int keep = 10);

    const std::string& lastError() const { return error_; }

private:
    Library() = default;
    bool load();
    bool exec(const char* sql);
    bool begin();
    bool commit();
    void rollback();
    bool saveWorld(const World& w);
    bool saveSeries(const Series& s);
    bool saveBook(const Book& b);
    bool saveMembership(const Membership& m);
    bool deleteMembership(const Id& seriesId, const Id& bookId);
    bool moveBookToWorldTx(Book& b, const Id& worldId);
    bool saveNode(const ManuscriptNode& n);
    bool updateBookWordCountTx(const Id& bookId);
    bool addVersionTx(const Id& nodeId, const std::string& content, std::int64_t words, const std::string& reason);
    ManuscriptNode* nodeMut(const Id& id);
    bool loadNodes();
    void collectSubtree(const Id& id, std::vector<Id>& out) const;
    World* worldMut(const Id& id);
    Series* seriesMut(const Id& id);
    Book* bookMut(const Id& id);
    double nextOrder(double maxSoFar) const { return maxSoFar + 1; }

    std::string folder_;
    sqlite3* db_ = nullptr;
    std::vector<World> worlds_;
    std::vector<Series> series_;
    std::vector<Book> books_;
    std::vector<Membership> memberships_;
    std::vector<ManuscriptNode> nodes_;
    std::vector<Artwork> artworks_;
    std::vector<Edition> editions_;
    std::vector<Entry> entries_;
    std::vector<Comment> comments_;
    bool loadComments();
    bool saveComment(const Comment& c);
    bool stripCommentMark(const Id& nodeId, const Id& commentId);
    std::vector<EntryLink> links_;
    bool loadEntries();
    bool saveEntry(const Entry& e);
    Entry* entryMut(const Id& id);
    bool loadEditions();
    bool saveEdition(const Edition& e);
    bool loadArtworks();
    bool saveArtwork(const Artwork& a);
    Artwork* artworkMut(const Id& id);
    bool registerArtworksInText();     // lists pictures used in text that aren't listed yet
    mutable std::string error_;
};

// Helpers shared with the app.
std::string newId();
Time now();
std::string sha256Hex(const std::string& bytes);
// Write-to-temp, flush, then replace: the target is never left half-written.
bool atomicWrite(const std::string& path, const std::string& bytes, std::string& error);
// True when the path looks like it's inside a cloud-sync folder.
bool looksLikeSyncFolder(const std::string& path);

}  // namespace studio
