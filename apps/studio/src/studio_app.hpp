// NorthRay Writing Studio: the Windows application.
//
// Milestone 1 is the Author Library: worlds, series and books, organised
// without ever copying files. The window is three panes (library sidebar,
// page, properties) under a workspace bar whose later workspaces (Writing,
// Typesetting, ...) are visible but switched off until they exist.
#pragma once

#include <optional>

#include "cjui/window.hpp"
#include "cjui/icons.hpp"
#include "studio/conlang.hpp"
#include "studio/cover.hpp"
#include "studio/ebook.hpp"
#include "studio/artwork.hpp"
#include "studio/import.hpp"
#include "studio/bible_export.hpp"
#include "studio/library.hpp"
#include "studio/entry_paste.hpp"
#include "studio/fonts.hpp"
#include "studio/typeset.hpp"
#include "studio/publish.hpp"
#include "studio/review.hpp"
#include "studio/sync.hpp"
#include "web_pane.hpp"

#include <atomic>
#include <deque>
#include <map>
#include <thread>
#include <set>
#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <vector>

namespace nrs {

using namespace cjui;
using studio::Id;

enum class View { Worlds, AllBooks, World, Series, Independent, Trash, Book, Settings, Artwork, FromGitHub };
// The workspace tabs along the top. Import is the preview shown before an import.
enum class Workspace { Library, Writing, Import, Typesetting, Preview, Publishing, Worldbuilding, Planning, Review };

inline constexpr float kWriteSideW = 230, kWriteBarH = 52, kViewerHeadH = 64;   // Writing's outline and chapter bar (0.70)
inline constexpr float kTopH = 46, kSideW = 260, kPanelW = 330, kStatusH = 28, kRailW = 76;
inline constexpr UINT WM_APP_IMPORT = WM_APP + 20;  // an import file has been read (lParam: Plan*)
inline constexpr UINT WM_APP_TYPESET = WM_APP + 21; // a book has been typeset (wParam: generation, lParam: typeset::Book*)
inline constexpr UINT WM_APP_EDITION = WM_APP + 22;
inline constexpr UINT WM_APP_SYNC_CALL = WM_APP + 23; // the sync thread asks for something done on the window's thread (lParam: std::function<void()>*)
inline constexpr UINT WM_APP_SYNC_DONE = WM_APP + 24; // a sync has finished (lParam: SyncResult*) // an edition has been typeset for Publishing (same parameters)

// Writing commands (menus, the outline's right-click menu).
enum WCmd {
    WNewChapterAfter = 5000, WNewSceneInside, WNewPart, WRename, WMoveUp, WMoveDown, WTrash, WRestore,
    WNewChapter, WNewScene, WPrev, WNext, WLibrary,
    WKindBase = 5100, WStatusBase = 5200, WIntoBase = 5300,
};

// What the properties panel is editing.
struct Focus {
    enum Kind { None, World, Series, Book, Node, Artwork, Picture, Edition, Entry, Plan, Snapshot, Goal, Comment, Board, BoardItem, Cover, Language, Share, Relation, Font, Thread, Beat, Structure, Sync } kind = None;
    // Thread, Beat (0.50): a plot thread or a structure beat (id), of a book (contextSeries). Structure: one of the
    // author's own (id), of the book whose plan is open (contextSeries).
    // Language: a constructed language (id); contextSeries "w:<word>" for one of its words.
    // Board: an image board (id). BoardItem: a picture on one (id; contextSeries: the board).
    // Picture: a picture in the open chapter's text (id: the chapter, contextSeries: its position).
    Id id;
    Id contextSeries;  // for a book shown inside a series: its number and kind there
    bool operator==(const Focus& o) const { return kind == o.kind && id == o.id && contextSeries == o.contextSeries; }
};

enum Field {
    FName, FSubtitle, FAuthor, FPenName, FGenre, FLanguage, FIsbn, FEdition, FTags, FAccent, FNumber,
    FCaption, FAlt, FDescription, FNotes, FSynopsis, FSearch, FTarget, FStoryDay, FWhen,
    FSection, FBookNote, FTemplate, FFind, FRepo, FToken, FTopSearch, FieldCount
};
// The fields that take several lines.
inline bool isMultiField(int f) { return f == FDescription || f == FNotes || f == FSynopsis || f == FSection || f == FBookNote || f == FTemplate; }

struct Settings {
    std::string libraryFolder;
    bool gallery = true;
    bool showPanel = true;
    bool showSide = true;      // the outline / sidebar on the left (F3)
    int sort = 0;          // 0 title, 1 series order, 2 author, 3 status, 4 recently changed
    int theme = 0;         // 0 system, 1 light, 2 dark
    std::vector<std::string> collapsedWorlds;
    std::string units = "in";  // in, cm, mm, px
    // Ebook preview
    std::string ebookDevice = "ereader", ebookTheme = "white", ebookFont = "book";
    double ebookScale = 1.0;
    // Where you were: reopened on the next start.
    std::string lastBook, lastNode;
    bool lastInWriting = false;
    bool writingDetails = false;
    bool typesetDesign = true;    // Typesetting's design choices shown in a card (0.71)
    bool previewChoices = true;   // Preview's device and reader's settings shown in a card (0.71)   // Writing shows the chapter's details in a card (0.70; F4)
    bool entryList = false;   // Worldbuilding shows entries as a list of rows rather than cards (0.59)
    // Books written in lately, newest first, each with the chapter it was on (0.58): NorthRay Launcher's Jump back in.
    std::vector<std::pair<std::string, std::string>> recentBooks;
    // The editor's writing view (font, size, width, focus, typewriter, spellcheck, hints, names), kept as the editor sends it.
    std::string writingView = "{}";   // JSON
    // Backups: also copied to this folder (another drive, a USB stick, a synced folder), if set.
    std::string backupFolder;
};

// A group of keyboard shortcuts ("Library" -> {"Ctrl+N", "New book"}, ...).
using ShortcutGroup = std::pair<std::wstring, std::vector<std::pair<std::wstring, std::wstring>>>;

// WM_COPYDATA's dwData for "open this book" from a second launch (NorthRay Launcher's Jump back in, 0.58).
inline constexpr ULONG_PTR kOpenBookMessage = 0x4E52424B;   // "NRBK"

class StudioApp : public AppWindow {
public:
    struct PageTab { std::wstring label; int count = -1; bool on = false; std::function<void()> act; Color dot{}; bool hasDot = false; };
    // A row of tabs (the chosen one in strong ink over the accent); tabs that don't fit before `right` go in "More".
    // Returns the row's height.
    float pageTabs(const std::string& id, float x, float y, float right, const std::vector<PageTab>& tabs, const std::vector<PageTab>& more = {});
    struct ToolItem {
        std::wstring label;
        std::function<void()> act;
        int kind = 0;   // 0 an item, 1 a line between groups, 2 a small heading
        bool checked = false, enabled = true;
        std::vector<ToolItem> sub;
        static ToolItem item(std::wstring l, std::function<void()> a, bool checked = false) { ToolItem t; t.label = std::move(l); t.act = std::move(a); t.checked = checked; return t; }
        static ToolItem line() { ToolItem t; t.kind = 1; return t; }
        static ToolItem heading(std::wstring l) { ToolItem t; t.kind = 2; t.label = std::move(l); return t; }
        static ToolItem menu(std::wstring l, std::vector<ToolItem> s) { ToolItem t; t.label = std::move(l); t.sub = std::move(s); return t; }
    };
    bool create(HINSTANCE inst, int show);
    // Command line: WritingStudio.exe [--import] <file> opens the import preview for that file.
    void handleCommandLine();
    // Opens a book at a chapter for NorthRay Launcher (0.58): from the command line, or from a second launch passed on
    // by WM_COPYDATA. `library` is the library folder the ids belong to ("" = this one).
    void openFromOutside(const std::string& book, const std::string& chapter, const std::string& library);
    void noteRecentBook();
    HACCEL accelerators() const { return accel_; }
    // Keyboard handling for the text fields (Tab order, Enter, Escape). True if handled.
    bool preTranslate(const MSG& msg);

protected:
    void drawFrame(float W, float H) override;
    bool onKeyDown(WPARAM vk, bool ctrl, bool shift) override;
    void onTimer(UINT_PTR id) override;
    void onSystemThemeChanged() override { applyTheme(); }
    LRESULT onMessage(UINT msg, WPARAM wp, LPARAM lp, bool& handled) override;
    void onDestroy() override;
    bool onMouseDown(float x, float y) override;
    void onMouseMove(float x, float y, bool leftDown) override;
    bool onMouseUp(float x, float y) override;
    bool onRightClick(float x, float y) override;
    bool onWheel(float x, float y, float lines) override;
    const char* navPrefix() const override { return "side:"; }

private:
    // Library
    bool openLibrary(const std::string& folder, bool interactive);
    void chooseLibraryFolder();
    void exportLibrary();
    void backupNow();
    std::string defaultLibraryFolder() const;
    void loadSettings();
    void saveSettings();
    std::wstring stateDir() const;

    // Commands
    void buildMenu();
    void command(int id);
    void newWorld();
    void newSeries(const Id& worldId);
    void newBook(const Id& worldId, const Id& seriesId);
    void trashFocused();
    void undoTrash();
    void emptyTrash();
    void renameFocused();
    void chooseImage(Focus::Kind kind, const Id& id, int which);  // which: 0 cover/banner, 1 logo
    void contextMenuFor(const std::string& hitId, float x, float y);
    void drawDropLine(float x, float y, float len, bool vertical);
    cjui::Color povColour(const Id& id) const;
    void quickJump();
    // The workspaces' rail and the top bar's search (UI pass, 0.64; studio_pages.cpp, studio_search.cpp).
    struct Action {
        std::string id;
        std::wstring label;
        std::function<void()> act;
    };
    float actionRow(float x, float y, float maxW, const std::vector<Action>& actions);
    void sideRowFrame(const std::string& key, const cjui::Rect& rr, bool selected, float hover, float scroll);
    float sideSection(float x, float y, float w, const std::wstring& label, std::function<void()> add, const std::string& addId);
    int tabCount() const;
    bool tabActive(int i) const;
    void openTab(int i);
    void drawRail(const cjui::Rect& r);
    struct SearchHit {
        std::wstring label, right;
        int kind = 0;            // 0 chapter or scene, 1 bible entry, 2 book, 3 command, 4 workspace, 5 world
        studio::Id id;
        int cmd = 0;
        cjui::Icon icon = cjui::Icon::None;
    };
    std::vector<SearchHit> searchHits() const;
    void runSearchHit(const SearchHit& h);
    bool searchKey(WPARAM vk);
    void drawSearchResults();
    void focusTopSearch();
    cjui::Rect railRect_, topSearchRect_;
    int topSel_ = 0;
    std::vector<SearchHit> lastSearchHits_;
    void showShortcuts();
    static std::vector<ShortcutGroup> shortcutGroups();
    void drawDropInto(const cjui::Rect& r);
    void moveBookTo(const Id& bookId, const Id& worldId);
    void addToSeries(const Id& bookId, const Id& seriesId);
    void moveSeriesTo(const Id& seriesId, const Id& worldId);
    void popupStatusMenu();
    void popupKindMenu();
    void popupSortMenu(float x, float y);
    void popupFilterMenu(float x, float y);
    void showAbout();
    void openSettings();

    // Writing workspace (studio_writing.cpp)
    void openWriting(const Id& bookId);
    // focusEditor: put the cursor in the text once it's loaded (not when
    // moving through the outline with the keyboard, or naming a new chapter).
    void selectNode(const Id& nodeId, bool focusEditor = true);
    void stepNode(int delta, bool chaptersOnly, bool focusEditor);  // previous / next in the outline
    void restorePlace();
    void rememberPlace();
    void ensureEditor();
    void sendLoad();
    bool isComic(const Id& book) const { return lib_ && lib_->setting("format:" + book) == "comic"; }
    void showInWriting(const Id& book, const Id& node, std::vector<std::string> words);
    void changeNameEverywhere(const std::string& from, const std::string& to);
    // A name from the text into the bible: asks what kind, never makes a second copy, relinks the editor.
    Id addNameToBible(const Id& bookId, const std::string& name);
    void drawNewNames(const Rect& c, float& y);   // Writing's panel: names in this chapter not in the bible yet
    std::string contextWord_, pendingBibleName_;  // the word right-clicked in the text; the one to add
    std::vector<studio::review::NameIssue> liveNames_;
    std::string liveNamesKey_;
    unsigned long long liveNamesAt_ = 0;
    void editorMessage(const std::string& json);
    void addNode(studio::NodeKind kind);
    void addSceneBreak();
    void nodeContextMenu(const Id& nodeId);
    void writingCommand(int id);
    void moveNodeBy(const Id& nodeId, int delta);
    void drawBinder(const Rect& r);
    void drawNodePanel(const Rect& r, const Rect& c, float& y);
    void placeEditor();
    void drawWritingBar(const Rect& r);   // the chapter's name, status and words over the page, with its tools (0.70)
    std::vector<ToolItem> chapterTools();
    void sendView();                  // the writing view and the bible's names, to the editor
    // Style rules belong to the book's world, or to the book itself when it's in no world.
    struct StyleScope { std::string key; std::string name, label; };
    StyleScope styleScope() const;
    // Constructed languages (studio_languages.cpp).
    std::string conlangKey(const Id& world, const Id& book) const;
    const std::vector<studio::conlang::Language>& conlangs(const Id& world, const Id& book) const;
    void saveConlangs(std::vector<studio::conlang::Language> langs);   // for the Worldbuilding scope shown
    void openLanguages();
    void newLanguage();
    void addLanguageWord();
    void importLanguages();
    void exportLanguage(const std::string& id);
    void removeLanguage(const std::string& id);
    void loadLanguageFields();
    void commitLanguageFields(const Focus& f);
    void drawLanguages(const Rect& r);
    void drawLanguagePanel(const Rect& r);
    mutable std::map<std::string, std::pair<std::string, std::vector<studio::conlang::Language>>> conlangCache_;   // key -> (stored JSON, read)
    void styleFileExport(const std::string& json);
    void styleFileImport();
    bool focusMode() const;           // Writing in focus mode: no side panels
    void backupTick();                // every 30 minutes: a backup if anything changed
    void mirrorBackups();             // copy new backups to settings_.backupFolder
    void chooseBackupFolder();
    long long changesAtBackup_ = 0;
    std::int64_t nodeTotalWords(const Id& nodeId) const;
    std::wstring nodeLabel(const studio::ManuscriptNode& n) const;
    // Import (studio_writing.cpp)
    void startImport(const Id& intoBook);   // asks for a file
    void importPath(const std::wstring& path, const Id& intoBook);
    void importFinished(studio::import::Plan* plan);
    void doImport();
    void cancelImport();
    float pageImport(const Rect& r);
    std::wstring exeDir() const;
    void goTo(View v, const Id& id = {});
    void setFocus(const Focus& f);
    void applyTheme();

    // Drawing
    void drawTopBar(const Rect& r);
    void drawSidebar(const Rect& r);
    void drawStatusBar(const Rect& r);
    void drawPanel(const Rect& r);
    float pageAllBooks(const Rect& r);
    float pageWorlds(const Rect& r);   // the start page: a card for each world (0.38)
    float pageWorld(const Rect& r);
    float pageSeries(const Rect& r);
    float pageIndependent(const Rect& r);
    float pageTrash(const Rect& r);
    float pageSettings(const Rect& r);
    float pageArtwork(const Rect& r);
    // Artwork (studio_artwork.cpp)
    void drawArtworkPanel(const Rect& c, float& y);
    void drawPicturePanel(const Rect& c, float& y);
    void addArtworkFiles();
    void insertArtwork(const Id& artworkId);
    void pictureMessage(const std::string& json);
    void sendPicture();
    double bookTextWidth(const Id& bookId) const;   // inches, from the book's trim size
    std::wstring length(double inches) const;       // in the chosen units
    Rect fitImage(const Rect& box, int w, int h) const;
    void popupTrimMenu();
    // Ebook preview (studio_preview.cpp)
    void openPreview(const Id& bookId);
    void ensurePreview();
    void sendPreview();
    void sendPreviewSettings();
    void previewMessage(const std::string& json);
    void drawPreviewSidebar(const Rect& r);
    void drawPreviewPanel(const Rect& r);
    // Review (studio_review.cpp)
    void openReview(const Id& bookId, int section = -1);
    void reviewFocus();
    void showReviewSection(int section);
    bool reviewAgainst(std::wstring& label, std::string& content) const;
    void compareWith(const Id& nodeId, const std::string& against);
    std::string row_is_new_guard(const Id& nodeId, const Id& snapshotId) const;
    // Comments (studio_comments.cpp)
    void sendComments();
    void commentMessage(const std::string& text);
    void showComment(const Id& id);
    void setCommentResolved(const Id& id, bool resolved);
    void deleteComment(const Id& id);
    void drawCommentPanel(const Rect& c, float& y);
    void drawNodeComments(const Rect& c, float& y, const studio::ManuscriptNode& n);
    float drawReviewComments(const Rect& m, float y);
    void makeReviewCopy();
    void importReaderComments();
    bool focusCommentField_ = false;
    bool showResolvedComments_ = false;
    Id pendingComment_;
    std::vector<std::string> pendingFind_;   // words to find and highlight once the chapter is in the editor
    void drawReviewSidebar(const Rect& r);
    void drawReviewPage(const Rect& r);
    void drawReviewPanel(const Rect& r);
    std::int64_t dailyGoal() const;
    int writingStreak() const;   // days in a row with any words written (to today, or yesterday if none yet today)
    std::wstring todayLine() const;
    std::int64_t wordsToday() const;
    // Today's words, the goal and the streak, in summary.json beside the settings, for NorthRay
    // Launcher's home page, and the book being written (title, series line, cover file) for NorthRay Capture.
    // Written only when they change.
    void writeSummary();
    std::string seriesLine(const Id& bookId) const;   // "Book Three of The Alvora Chronicles", or ""
    std::string lastSummary_;
    // Planning (studio_plan.cpp)
    void openPlanning(const Id& bookId);
    void selectCard(const Id& nodeId);   // "" = the book itself
    float planHeader(const Rect& m, const std::wstring& sub);   // Planning's header, tabs and filters (0.69)
    void drawPlanThreads(const Rect& r);
    std::vector<ToolItem> planTools();
    void drawPlanBoard(const Rect& r);
    void drawPlanPanel(const Rect& r);
    // Planning's other views (studio_plot.cpp): the timeline and story structure.
    void drawPlanTimeline(const Rect& r);
    void drawPlanStructure(const Rect& r);
    void planPanelExtras(const Rect& c, float& y);     // the timeline/structure parts of the panel
    struct StoryTime { bool placed = false; double day = 0; std::string when; };
    std::map<Id, StoryTime> storyTimes(const Id& bookId) const;
    void setStoryTime(const Id& bookId, const Id& nodeId, const StoryTime& t);
    std::string timelineUnit(const Id& bookId) const;
    void setTimelineUnit(const Id& bookId, const std::string& unit);
    void outlineFromStructure();
    // Plot threads, beats and your own structures in the panel, and the plan as text (0.50, studio_plot.cpp).
    void loadPlanFields();
    void commitPlanFields(const Focus& f);
    void drawThreadPanel(const Rect& c, float& y);
    void drawBeatPanel(const Rect& c, float& y);
    void drawOwnStructurePanel(const Rect& c, float& y);
    void cardThreads(const Rect& c, float& y);          // a card's threads, to tick
    void newThread(const Id& tagNode);
    void selectThread(const std::string& id);
    void selectBeat(const std::string& id);
    void importPlanText();
    void exportPlanText();
    bool nodeShown(const studio::ManuscriptNode* n) const;
    void chooseViewpoint(const Id& nodeId);              // a menu of the world's characters (0.51)
    void choosePlace(const Id& nodeId);                  // a menu of the bible's places and realms (0.52)
    void addToCast(const Id& nodeId);                    // who's present: a menu of characters and creatures (0.59)
    void cardCast(const Rect& c, float& y);              // the WHO'S THERE part of the planning panel
    void zoomTimeline(float factor, float atX);
    std::string planPref(const Id& book, const char* key, const std::string& fallback) const;   // in "plan:<book>"
    void setPlanPref(const Id& book, const char* key, const std::string& value);   // passes the status, viewpoint and thread filters
    bool planMouseDown(float x, float y);
    bool planMouseMove(float x, float y, bool leftDown);
    bool planMouseUp(float x, float y);
    Color statusColour(studio::NodeStatus s) const;
    std::int64_t bookTarget(const Id& bookId) const;
    void setBookTarget(const Id& bookId, std::int64_t words);
    // Worldbuilding (studio_world.cpp)
    bool worldbuildingScope(Id& worldId, Id& bookId) const;
    void openWorldbuilding(const Id& worldId, const Id& bookId);
    void openEntry(const Id& entryId);   // "" = back to the cards
    void newEntry(const std::string& kind);   // "" = ask which kind
    void addRelationship();
    void addMembers();
    float drawCreatureTree(const Rect& m, float y);
    float drawPlacesOutline(const Rect& m, float y);
    void setPlaceIn(const Id& place, const Id& parent);
    // Family trees (studio_world.cpp): a character's family on their page, the whole tree as a page.
    float drawFamilySection(const Rect& m, float y, const studio::Entry& e);
    void drawFamilyTree(const Rect& r);
    void drawFamilyPanel(const Rect& r);
    void familyChipMenu(const Id& who, const Id& other, int role);
    std::wstring kinWord(const Id& other, int role, const std::string& how);
    void addFamilyMember(const Id& who, int relation);   // 0 parent, 1 partner, 2 sibling, 3 child, 4 former partner
    void openFamilyTree(const Id& root, bool bloodline = false);   // a person's tree, or a family from its founder
    void saveFamilyTreePicture();
    void zoomFamilyTree(float factor);                              // about the middle of the page; 0 = fit
    // Maps (studio_world.cpp): a place's or realm's map picture, with entries pinned on it.
    float drawMapSection(const Rect& m, float y, const studio::Entry& e);
    void chooseMapPicture(const Id& entryId);
    // Checks (studio_world.cpp): names spelt like a bible name, facts that disagree, entries never named.
    void drawChecks(const Rect& r);
    void checkIsFine(const std::string& key);
    void drawGlossary(const Rect& r);   // every name in the bible, A to Z, with how it's said
    void shareBible();                  // a website or a printable PDF: everything, or a reader's guide
    void pinMenu(const Id& mapEntry, const Id& pinned);
    void openMap(const Id& entryId);
    void openLastWriting();
    float drawPlaceSection(const cjui::Rect& m, float y, const studio::Entry& e);
    void drawMapView(const cjui::Rect& r);
    bool mapMouseDown(float x, float y);
    bool mapMouseMove(float x, float y, bool leftDown);
    bool mapMouseUp(float x, float y);
    bool mapWheel(float x, float y, float lines);
    void pinOnMenu(const Id& mapEntry);
    float drawKindSuggestions(const Rect& m, float y);
    void addGalleryPictures();
    void galleryMenu(size_t index);
    void makeMainPicture(size_t index);                    // 0.60: dropped on the main picture
    void addPictureToBoard(size_t index, const Id& board); // 0.60: dropped on one of its boards
    void newEntryBoard();
    void moveBoardToEntry(const std::string& boardId);
    Id noteBookFor(const studio::Entry& e) const;
    void setKindOf(const Id& entry, const Id& parent);   // parent "" = a species of its own
    void addKindUnder(const Id& parent);
    void entryKindMenu(const Id& entry);
    void tidyBibles();
    void drawWorldStrip(const Rect& r);   // a bible tool's way back, and the tools menu (0.68)
    std::vector<ToolItem> bibleTools();
    std::vector<ToolItem> entryTools(const studio::Entry& e);
    void entryKindChoice();
    void entryKindOfChoice();
    void entryFacetChoice(const std::string& key);
    void entrySpoilerChoice(const std::string& key);
    void entryNoteBookChoice();
    void trashOpenEntry();
    void startEntryEdit(const std::string& key, int field);
    void drawWorldPage(const Rect& r);
    void drawWorldPanel(const Rect& r);
    // Text with the bible's names as links to their pages (wiki-style). Returns its height.
    float drawLinkedText(const std::string& text, const Rect& r, Font f, Color c, const Id& self, const std::string& hitPrefix);
    void openEntryChoice(const std::vector<Id>& ids);   // one entry, or a menu when several share a name
    // Image boards (studio_boards.cpp): pictures arranged freely, kept with the world or independent book.
    void drawBoard(const Rect& r);
    void drawBoardPanel(const Rect& r);
    void newBoard();
    void addBoardPictures();
    nlohmann::json boardsData() const;
    void saveBoards(const nlohmann::json& j);
    bool boardMouseDown(float x, float y);
    bool boardMouseMove(float x, float y, bool leftDown);
    bool boardMouseUp(float x, float y);
    // Publishing (studio_publish.cpp)
    void openPublishing(const Id& bookId);
    const studio::Edition* currentEdition() const;
    void chooseEdition(const Id& editionId);
    void editionChanged();
    void startEditionTypeset();
    void editionTypesetFinished(int generation, studio::typeset::Book* book);
    void addEditionMenu();
    void removeCurrentEdition();
    void refreshChecks();
    void exportEditionPdf(bool printReady);
    void exportEpub();
    std::wstring askSavePath(const wchar_t* kind, const wchar_t* pattern, const wchar_t* ext, const std::wstring& name, const wchar_t* title);
    void drawPublishSidebar(const Rect& r);
    void drawPublishPage(const Rect& r);
    void drawPublishPanel(const Rect& r);
    // The ebook cover designer and the ebook's look (studio_cover.cpp).
    studio::ebook::Style ebookStyleFor(const Id& book, const Id& edition, std::string* fontId);
    void setEbookLook(const std::string& key, bool value);
    void openCoverDesigner();
    void closeCoverDesigner();
    void renderCoverPreview();
    void chooseCoverPicture();
    void useDesignedCover();
    void exportPrintCover();
    void saveCoverDesign();
    bool coverWrap(studio::cover::Wrap& out) const;   // a print edition's measurements (false until its pages are set)
    void drawCoverDesigner(const Rect& r);
    void drawCoverPanel(const Rect& r);
    // Typesetting (studio_typeset.cpp)
    void openTypesetting(const Id& bookId);
    void prepareTypeset(const Id& bookId);   // the book's design loaded and its pages set (any workspace)
    void startTypeset();
    void typesetFinished(int generation, studio::typeset::Book* book);
    void designChanged();
    void drawTypesetSidebar(const Rect& r);
    void drawTypesetPages(const Rect& r);
    void drawTypesetPanel(const Rect& r);
    void exportPdf();
    void showSpread(int spread);
    void showPage(int index);       // the spread or page with that page on it
    void turnPages(int delta);      // next / previous spread (or page)
    int spreadOfPage(int pageIndex) const;
    int spreadCount() const;
    studio::typeset::Resources typesetResources(const Id& book = {}) const;
    // The font folders a book sees ("app fonts|its world's own", 0.40), and a world's (or an independent book's) own.
    std::string fontFoldersFor(const Id& book) const;
    std::string worldFontFolders(const Id& world, const Id& book) const;
    std::string ownFontsFolder(const Id& world, const Id& book) const;
    float pageBook(const Rect& r);
    float libraryTabs(const Rect& page);
    float workspaceHeader(const Rect& m, const std::wstring& kicker, const std::wstring& title, const std::string& tabsId,
                          const std::vector<PageTab>& tabs, const std::wstring& toolsLabel, std::function<std::vector<ToolItem>()> tools,
                          const std::wstring& sub);
    float reviewHeader(const Rect& m, const std::wstring& sub);
    void drawTypesetHead(const Rect& r);
    void drawPreviewHead(const Rect& r);
    void viewerHead(const Rect& r, const std::wstring& kicker, const std::wstring& title, const std::wstring& note);
    float toggleButton(const std::string& id, float right, float y, const std::wstring& label, bool on, std::function<void()> flip,
                       const std::wstring& tip = {});   // its width; drawn leftwards from `right`
    int bookTab_ = 0;   // the book page's tab (0.69)
    float pageEmpty(const Rect& r);
    Id fallbackBook() const;
    bool chooserBox(const std::string& id, const cjui::Rect& b, const std::wstring& value, std::function<void()> act);
    float viewSwitch(const std::string& id, float left, float x, float y, float right);
    void quietEdit(const std::string& id, const Rect& section, float x, float y, const std::wstring& label, std::function<void()> act);
    float emptyState(const Rect& area, cjui::Icon icon, const std::wstring& line, const std::wstring& label, std::function<void()> act);
    float bookCollection(const Rect& r, const std::vector<const studio::Book*>& books, const std::string& listKey,
                         const Id& seriesId, bool chronological, bool reorderable);
    void bookCard(const Rect& r, const studio::Book& b, const Id& seriesId, const std::string& hitId);
    void bookRow(const Rect& r, const studio::Book& b, const Id& seriesId, const std::string& hitId, bool header);
    void drawCover(const Rect& r, const studio::Book& b);
    Color accentFor(const studio::Book& b) const;
    Color accentOfWorld(const Id& worldId) const;
    float pageHeader(const Rect& r, const std::wstring& kicker, const std::wstring& title, const std::wstring& sub);
    // The second redesign (0.68): cards on warm paper, a page's tabs along its top, a tools menu per workspace, and
    // editing where the text is shown.
    void card(const Rect& r, float radius = 12, float lift = 1.0f);
    // A menu with its top-left (or with `right`, its top-right) corner at (x, y) in the window.
    void showTools(const std::vector<ToolItem>& items, float x, float y, bool right = false);
    // A button with a drawn chevron that opens a menu under it; its width.
    float menuButton(const std::string& id, float x, float y, const std::wstring& label, std::function<std::vector<ToolItem>()> items,
                     bool primary = false);
    // Editing in place: a click on the text puts a box there; Enter or a click elsewhere keeps it, Esc puts it back.
    void startInline(const std::string& key, int field, std::function<void()> revert, bool big = false);
    bool editingInline(const std::string& key) const { return ie_.field >= 0 && ie_.key == key; }
    void inlineBox(const Rect& r);   // the box for the edit going on, with its field in it
    float inlineHeight(float minH) const;   // how tall a box the text being edited needs
    void editableArea(const std::string& id, const Rect& r, const std::wstring& tip, std::function<void()> start);
    void settleInline();   // after a frame: focus the new box; end the edit when the focus has gone
    void endInline(bool keep);
    float toolbar(const Rect& r, bool withSearch);
    bool smallButton(const std::string& id, float x, float y, const std::wstring& label, std::function<void()> act, float* w = nullptr);
    std::vector<const studio::Book*> filteredBooks(std::vector<const studio::Book*> in) const;
    std::wstring seriesLine(const studio::Book& b) const;
    Color bookStatusColour(studio::BookStatus s) const;
    float statusTag(float x, float y, studio::BookStatus s, float maxW);   // returns its width
    std::wstring wordsText(std::int64_t n) const;                           // "12,340 words"
    float booksTotals(float x, float y, const std::vector<const studio::Book*>& books, float maxW);

    // Native text fields in the properties panel
    void createFields();
    void layoutFont();
    void placeField(Field f, const Rect& r);
    void hideUnplacedFields();
    void loadFields();
    void commitFields();
public:
    // Pasting (0.37, studio_paste.cpp).
    bool pastedIntoField(HWND field, const std::wstring& text);   // true: taken as a whole bible page
    bool isListField(HWND field) const;
private:
    void watchFieldPastes();
    // Share the bible (studio_share.cpp).
    void loadShare();
    void saveShare();
    void drawShare(const Rect& r);
    void drawSharePreview(const Rect& r);
    void drawSharePanel(const Rect& r);
    void loadShareFields();
    void commitShareFields();
    void makeSharedBible();

    // Keeping a world in a GitHub repository (0.61, studio_sync.cpp). The network part runs on syncThread_, one sync
    // at a time from syncQueue_; whatever touches the library is handed back to the window's thread
    // (WM_APP_SYNC_CALL). The token lives in Windows Credential Manager, never in the library or settings.
    struct SyncJob {
        enum Kind { Check, Sync } kind = Sync;
        Id world;                  // empty: opening a world from the repository
        std::string repo;          // "owner/name"
        std::string folder;        // the library it's for
        bool quiet = false;        // automatic: no message unless something needs saying
    };
    struct SyncResult {
        SyncJob job;
        bool ok = false;
        std::string error;
        studio::sync::Report report;
        studio::sync::State state;
        studio::sync::RepoInfo info;
    };
    friend class SyncLocal;
    void openSyncPage();
    void drawSync(const Rect& r);
    void drawSyncPanel(const Rect& r);
    float pageFromGitHub(const Rect& r);
    float pageField(const Rect& r, float y, const std::wstring& label, Field f);
    void startSyncSetup(const Id& world);
    void queueSync(const SyncJob& job);
    void runNextSync();
    void syncFinished(SyncResult* res);
    void syncTick();
    void syncAllSoon();
    void stopSyncing(const Id& world);
    void replaceSyncToken(const Id& world);
    void finishClosing();
    std::thread syncThread_;
    std::atomic<bool> syncBusy_{false}, syncCancel_{false};
    std::atomic<size_t> syncSent_{0}, syncToSend_{0};   // files sent so far in this sync, for the Sync page
    std::deque<SyncJob> syncQueue_;
    Id syncRunning_;                                   // the world being synced ("-" while opening one)
    std::map<Id, std::wstring> syncNote_;              // the last sync's outcome, per world
    std::map<Id, std::vector<std::string>> syncConflicts_, syncSkipped_;
    std::map<Id, bool> syncFailed_;
    std::map<Id, long long> syncChangesAt_;            // changesMade() after the last sync
    std::map<Id, ULONGLONG> syncTriedAt_;              // GetTickCount64() of the last try
    std::wstring openNote_;                            // the Open from GitHub page's message
    bool closing_ = false;
    // The world's own kinds of relationship (studio_relations.cpp).
    void openRelations(const std::string& select);
    void newRelation();
    void drawRelations(const Rect& r);
    void drawRelationsPanel(const Rect& r);
    void loadRelationFields();
    void commitRelationFields(const Focus& f);
    // A world's own typefaces (studio_fonts.cpp, 0.40).
    void openFonts(const std::string& select);
    void importFonts();
    void drawFonts(const Rect& r);
    void drawFontsPanel(const Rect& r);
    void loadFontFields();
    void commitFontFields(const Focus& f);
    float drawFontSample(const std::string& folders, const std::string& id, const std::string& style, const std::string& text, float x,
                         float y, float px, float maxW);
    std::map<std::string, studio::fonts::Sample> fontSamples_;
    // A font file's URL in the app's web pages: the library's fonts host or the app's (0.41); "" if neither.
    std::string fontUrl(const std::string& path) const;
    // The world's typefaces for the editor: [{id, name, faces: {regular, italic, bold, boldItalic}}].
    nlohmann::json editorFonts(const Id& book) const;   // preview lines, by family, style, colour, size and text
    bool pasteFromClipboard(bool replaceAll = false);   // Ctrl+V on a bible page or the cards; replaceAll: the page's text all replaced (0.46)
    void pasteEntryPage(const Id& entryId, const std::string& text, bool created, bool replaceAll = false);
    bool undoEntryPaste();
    void copyBlankPage();     // a template for the open entry's kind, on the clipboard
    void resetBible();        // every entry to the Trash, or everything (studio_paste.cpp)
    bool undoBibleReset();
    void importManyPages(bool fromClipboard);
    void copyBlankPageFor(const std::string& kind);
    void copyBlankPageText(const std::string& kind, const std::vector<std::string>& sections);   // many bible pages from a file or the clipboard (0.39)
    void importPagesText(const std::string& text, const std::wstring& from, const std::string& baseFolder = {});
    bool undoPagesImport();
    void setFieldText(Field f, const std::string& utf8);
    std::string fieldText(Field f) const;
    float panelField(const Rect& r, float y, const std::wstring& label, Field f, float h = 30);
    // Details panel building blocks, shared by worlds, series, books, chapters and scenes.
    void panelHeading(const Rect& c, float& y, const std::wstring& kicker, bool first);
    void panelChoice(const std::string& id, const Rect& c, float& y, const std::wstring& label, const std::wstring& value,
                     std::function<void()> act);
    float panelImage(const Rect& r, float y, const std::wstring& label, const std::string& blob, std::function<void()> choose,
                     std::function<void()> clear, bool wide);

    // Drag and drop
    struct Drop {
        enum Kind { ToWorld, ToIndependent, ToSeries, ReorderSeriesBooks, ReorderStandalone, ReorderSeriesList, ReorderWorlds, NodeRow, KindOf,
                    MainPicture, PictureToBoard } kind;
        Rect rect;
        Id id;              // world, series, or the item at this position
        Id listOwner;       // series id / world id the list belongs to
        bool horizontal = false;
        bool chrono = false;
        std::vector<Id> list;  // the list being reordered, in current order
        bool into = false;     // NodeRow: always inside the target (a part's column, 0.51)
    };
    void addDrop(const Drop& d) { drops_.push_back(d); }
    const Drop* dropAt(float x, float y) const;
    void applyDrop(const Drop& d, float x, float y);

    std::unique_ptr<studio::Library> lib_;
    Settings settings_;
    std::wstring libraryNote_;

    View view_ = View::AllBooks;
    Id viewId_;
    Focus focus_, loadedFocus_;
    bool chrono_ = false;          // series page: chronological order
    std::string search_;
    std::string find_;          // Worldbuilding's "Find in the bible" (0.43)
    int statusFilter_ = -1;        // -1 all, else BookStatus
    std::string tagFilter_;
    float sideScroll_ = 0, sideContent_ = 0;
    float panelScroll_ = 0, panelContent_ = 0;
    Rect sideRect_, panelRect_, pageRect_;
    std::vector<Id> recentlyTrashed_;

    HWND fields_[FieldCount] = {};
    bool placed_[FieldCount] = {};
    RECT fieldRects_[FieldCount] = {};
    bool suppressChange_ = false;
    bool dirty_ = false;
    HFONT fieldFont_ = nullptr;
    HFONT titleFont_ = nullptr;   // a name being edited in place, in the page title's size (0.68)
    struct InlineEdit { int field = -1; std::string key; bool placed = false, focused = false, big = false; std::function<void()> revert; };
    InlineEdit ie_;
    std::map<std::string, float> areaH_;   // how tall each editable piece of text was last drawn
    Color gold_{};   // the kicker labels' warm gold (0.68)
    bool panelAsCard_ = false;   // a details column drawn as a card on the page (0.68)
    HBRUSH fieldBrush_ = nullptr;
    HACCEL accel_ = nullptr;

    struct DragState {
        bool armed = false, active = false;
        enum Kind { Book, Series, World, Node, Entry, Picture } kind = Book;   // Picture: an entry's picture (id = its index)
        Id id;
        float sx = 0, sy = 0;
        std::wstring label;
    } drag_;
    std::vector<Drop> drops_, lastDrops_;
    std::string lastClickId_;
    DWORD lastClickTime_ = 0;

    // Writing
    Workspace ws_ = Workspace::Library;
    Id writingBook_;               // the book open in the Writing workspace
    int focusFieldSoon_ = -1;      // a field to put the cursor in once the panel has placed it
    Id currentNode_;               // the chapter/scene in the editor
    WebPane editor_;
    bool editorStarted_ = false;
    std::wstring editorNote_;      // why the editor isn't available, if it isn't
    bool loadPending_ = false;     // send the current node once the page is ready
    std::int64_t liveWords_ = -1, selectionWords_ = 0;
    // A writing sprint in the editor, passed on to NorthRay Capture (0.53): running now, and the last one to end.
    std::map<Id, std::vector<Id>> castCache_;   // who appears where, for the summary when Planning hasn't worked it out
    struct { bool running = false; std::int64_t end = 0, words = 0; int minutes = 0;
             std::int64_t lastAt = 0, lastWords = 0; int lastMinutes = 0; bool lastFinished = false; } sprint_;
    float binderScroll_ = 0, binderContent_ = 0;
    bool showNodeTrash_ = false;
    bool closePending_ = false;    // closing: waiting for the editor's last save
    bool editorDirty_ = false;     // typing not yet saved
    bool saveFailed_ = false;      // the last save didn't work
    bool loadFocus_ = true;        // focus the editor after the next load
    // Import
    std::unique_ptr<studio::import::Plan> importPlan_;
    Id importTarget_;              // book to add to; empty = a new book
    bool importUseDetails_ = true;
    bool importBusy_ = false;
    std::wstring importFile_;
    Workspace beforeImport_ = Workspace::Library;
    // Ebook preview
    WebPane preview_;
    bool previewStarted_ = false;
    std::wstring previewNote_;
    struct PreviewState {
        Id book;
        int chapter = 0, page = 0, pages = 1;
        std::vector<std::pair<std::string, bool>> chapters;   // title, is a part
        std::int64_t words = 0;
        bool sent = false;
        float sideScroll = 0, sideContent = 0;
    } pv_;
    // Typesetting
    struct TypesetState {
        Id book;
        studio::typeset::Design design;
        std::string presetId = "classic";
        std::shared_ptr<studio::typeset::Book> result;
        int generation = 0;        // bumped for every new setting
        bool busy = false, again = false;
        int spread = 0;            // 0: the first page alone on the right; n: pages 2n-1 and 2n
        int page = 0;              // the page shown in single-page view (kept in step with spread)
        bool single = false;       // one page at a time instead of spreads
        int zoom = 0;              // 0 fit, 1 actual size, 2 twice actual size
        bool guides = false;       // margins and the baseline grid
        float panX = 0, panY = 0;  // scrolled, when zoomed in past the window
        std::vector<std::wstring> drawnKeys;   // page bitmaps in the current spread
        float sideScroll = 0, sideContent = 0, panelScroll = 0, panelContent = 0;
    } ts_;
    // Review
    struct ReviewRow {
        Id node;
        std::wstring title;
        studio::review::DiffStats st;
        bool isNew = false, gone = false;
    };
    struct DiffWord { float x, y, w; std::wstring text; int kind; };
    struct ReviewState {
        Id book;
        int section = 0;           // 0 changes, 1 snapshots, 2 names, 3 goals
        Id node;                   // the chapter compared
        std::string against;       // "v:<version id>" or "s:<snapshot id>"
        std::vector<studio::review::DiffPart> diff;
        studio::review::DiffStats diffStats;
        std::string diffKey;
        std::vector<DiffWord> words;
        float diffLayoutW = -1, diffHeight = 0;
        Id snapshot;               // the snapshot selected
        std::vector<ReviewRow> bookCompare;
        Id bookCompareFor;
        std::vector<studio::review::NameIssue> names;
        bool namesDone = false;
        bool allComments = false;  // comments: resolved ones too
        float scroll = 0, content = 0, sideScroll = 0, sideContent = 0;
    } rv_;
    // Planning
    struct PlanState {
        Id book;
        Id node;                   // the selected card ("" = the book)
        int status = -1;           // filter: a NodeStatus, or -1 for everything
        std::string pov;           // filter (0.49.3): a viewpoint character's id, "-" for none, "" for anyone
        std::string thread;        // filter (0.50): a plot thread's id, or "" for any
        int cardEdit = -1;         // 0.51: the field (FName, FSynopsis) being edited on the selected card, or -1
        bool cardEditFocused = false;
        float boardX = 0, boardW = 0;   // the corkboard's columns: scrolled along, and how wide they are
        std::map<Id, std::vector<Id>> appearances;   // node -> entries found in its text
        float scroll = 0, content = 0, sideScroll = 0, sideContent = 0, panelScroll = 0, panelContent = 0;
        int view = 0;              // 0 corkboard, 1 timeline, 2 structure, 3 threads (0.69)
        Rect selCard{};            // where the selected card was drawn on the corkboard (0.69: its pop-over goes beside it)
        bool targetOpen = false;   // the book's target, in the details card (0.69)
        // Timeline: where it is, and a card being dragged along it.
        float tlScrollX = 0, tlContentW = 0;
        Rect tlArea{};
        double tlDay0 = 0, tlPx = 80;    // the day at tlArea's left edge (before scrolling), pixels per day
        // A drag on the timeline (0.52 kinds): 0 a placed card, 1 a card not placed yet, 2 a placed story event, 3 one not yet.
        struct { bool armed = false, active = false; Id id; int kind = 0; float sx = 0, sy = 0, mx = 0, my = 0; double startDay = 0, day = 0; } tlDrag;
        float tlZoom = 0;          // 0.52: pixels per unit, or 0 to fit the story
        float tlOrigin = 0, tlX0 = 0;   // where the first day is drawn before scrolling, and now
        Rect tlDropArea{};         // where a card or event can be dropped onto the timeline
        std::string beat;          // Structure: the beat selected
    } pl_;
    // Worldbuilding
    // Share the bible (0.38): the choices, kept with the world.
    struct ShareState {
        bool pdf = false, guide = false;
        Id upTo;
        bool pictures = true, family = true, languages = true, glossary = true;
        studio::bible::Look look;
        int preset = 0;
        std::string font;
    };
    struct PasteUndo {
        Id entry;
        studio::Entry before;
        std::vector<studio::EntryLink> added, removed;
        std::vector<Id> made;
        bool created = false;   // the entry was made by the paste
    };
    struct WorldState {
        Id world, book;            // whose entries: a world's, or an independent book's
        std::string kind;          // the kind shown ("" = everything)
        std::string facet;         // with kind character or place: the gender or type shown ("" = all, "-" = not set)
        std::string groupBy = "gender";   // Characters split by "gender", "race" or "none"
        bool placesOutline = false;
        std::map<Id, int> placeKids;   // places directly inside each place, for the cards' "N inside" (0.48.5)
        std::vector<std::pair<std::wstring, float>> anchors;   // an entry page's sections and where they start (0.48.4), for the jump row       // Places as an outline (0.48.3): continents > nations > cities, dragged to nest
        std::set<Id> collapsed;    // creature tree: families closed
        Id entry;                  // the entry open ("" = the cards)
        std::string section;       // the page section being written in the panel
        Id noteBook;               // whose notes the panel shows ("" = the book open in Writing, or the first)
        bool editTemplate = false; // the panel edits the kind's page template
        Id boardEntry;             // the board open belongs to this entry (back goes to it)
        int picture = -1;          // the gallery picture selected on the entry's page
        bool spoilersOpen = false; // the entry's spoilers, in a card at the top of its page (0.68)
        Id familyRoot;             // the family tree shown ("" = none), drawn from this person
        Id pinning;                // placing this entry's pin on the open map: the next click on it says where
        // The map viewer (0.48): a place's or realm's map over the whole page, zoomed and moved about, pins dragged.
        Id mapView;
        float mapZoom = 1, mapCX = 0.5f, mapCY = 0.5f;   // 1 = the whole map fits; the map's point at the board's centre
        int mapDrag = 0;                                  // 1 moving the map, 2 a pin
        Id mapPin;                                        // the pin pressed
        bool mapMoved = false;
        float mapDownX = 0, mapDownY = 0, mapStartCX = 0, mapStartCY = 0, mapPinX = 0, mapPinY = 0;
        cjui::Rect mapBoard, mapImage;                    // where the board and the picture were drawn
        bool checks = false;       // the Checks page
        bool glossary = false;     // the Glossary page
        bool languages = false;    // the Languages page (constructed languages)
        std::string language;      // the language shown there
        studio::review::Continuity continuity;           // worked out for the Checks page
        std::vector<std::pair<Id, studio::review::NameIssue>> spellings;   // book, name spelt like a bible name
        std::string checksKey;
        float treeX = 0, treeW = 0;   // the tree scrolled across (Shift and the wheel), and its width
        float treeZoom = 0;           // Ctrl + wheel (0.35); 0 = fit the page
        bool treePan = false;         // dragging the tree about by its background
        bool treeBloodline = false;   // the tree is a family from its founder (else one person's tree) (0.38.3)
        Id treeSelected;              // the person picked on the tree (shown in the panel, lines lit)
        Id treeLastClick;             // for double-clicks: re-centre the tree on someone
        DWORD treeLastClickAt = 0;
        float panX = 0, panY = 0, panTreeX = 0, panScroll = 0;
        std::vector<studio::Mention> mentions;
        Id mentionsFor;
        std::string mentionsKey;   // the names they were found for
        float scroll = 0, content = 0, sideScroll = 0, sideContent = 0, panelScroll = 0, panelContent = 0;
        std::vector<const studio::Entry*> scopeEntries;   // this frame's entries, for links
        // The bible by book: each entry's first book (in series reading order), worked out from the text
        // when anything in the library changes.
        std::vector<Id> bookOrder;
        std::map<Id, size_t> firstBook;   // entry -> index into bookOrder
        std::string firstBookKey;
        std::string kindSugKey;                          // kind suggestions, kept until the library changes (0.59)
        std::vector<studio::Library::KindSuggestion> kindSug;
        // Image boards
        std::string board;         // the board open ("" = none)
        std::string item;          // the picture selected on it
        struct { bool armed = false, active = false, resize = false; std::string item; float sx = 0, sy = 0, ox = 0, oy = 0, ow = 0; } boardDrag;
        float boardOriginX = 0, boardOriginY = 0;   // where the board's (0, 0) was drawn
        std::map<std::string, std::pair<float, float>> boardDragPos;   // live position while dragging
        float boardDragW = 0;
        std::optional<PasteUndo> pasteUndo;   // the last page pasted onto an entry, to put back (0.37)
        std::optional<studio::Library::BibleReset> resetUndo;   // the last reset, to take back (0.38.1)
        std::optional<studio::paste::Import> importUndo;        // the last pages imported, to take back (0.39)
        std::int64_t claimedAt = -1;   // changesMade() when kept book notes were last looked at (0.56)
        bool relationsPage = false;           // the Relationships page (0.38.2)
        bool fontsPage = false;               // the Fonts page (0.40)
        bool kindsOpen = false;               // "Check these kinds" opened from its one-line note (0.43)
        // Big bibles (0.59): the families, kept until the library changes; all of them shown or only the biggest;
        // the sidebar's lists of values opened past their first few.
        std::string famKey;
        std::vector<studio::Library::FamilyGroup> fams;
        bool allFamilies = false;
        std::set<std::string> facetsOpen;
        bool share = false;                   // the Share the bible page (0.38)
        bool syncPage = false;                // the Sync with GitHub page (0.61)
        std::string shareKey;                 // whose choices are loaded
        ShareState shareState;
    } wb_;
    // Publishing
    struct PublishState {
        Id book;
        Id edition;                // the edition being published
        std::shared_ptr<studio::typeset::Book> result;   // its pages (print editions)
        int generation = 0;
        bool busy = false, again = false;
        std::vector<studio::publish::Check> checks;
        std::wstring lastExport;
        float sectionY[3] = {0, 0, 0};   // where each step starts on the page
        float scroll = 0, content = 0, sideScroll = 0, sideContent = 0, panelScroll = 0, panelContent = 0;
    } pb_;
    struct CoverDesigner {
        bool open = false;
        Id book;
        studio::cover::Spec spec;
        std::string pictureBlob;
        std::wstring preview;     // the preview picture's file
        int serial = 0;
        std::string error;
        bool print = false;       // a print edition's cover: back, spine and front
        Id edition;
    } cv_;
    // The picture selected in the editor, and dragging its crop.
    struct PictureSel {
        int pos = -1;
        std::string blob, alt, caption;
        studio::Placement place;
    } pic_;
    struct CropDrag {
        int mode = 0;             // 0 none, 1 move, 2 top-left, 3 top-right, 4 bottom-left, 5 bottom-right
        float sx = 0, sy = 0;
        studio::Placement start;
    } crop_;
    Rect cropBox_;                // where the picture is drawn in the panel (0 width: not shown)
};

}  // namespace nrs
