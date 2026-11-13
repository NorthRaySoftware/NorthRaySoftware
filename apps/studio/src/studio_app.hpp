// NorthRay Writing Studio: the Windows application.
//
// Milestone 1 is the Author Library: worlds, series and books, organised
// without ever copying files. The window is three panes (library sidebar,
// page, properties) under a workspace bar whose later workspaces (Writing,
// Typesetting, ...) are visible but switched off until they exist.
#pragma once

#include "cjui/window.hpp"
#include "studio/artwork.hpp"
#include "studio/import.hpp"
#include "studio/library.hpp"
#include "studio/typeset.hpp"
#include "studio/publish.hpp"
#include "studio/review.hpp"
#include "web_pane.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace nrs {

using namespace cjui;
using studio::Id;

enum class View { AllBooks, World, Series, Independent, Trash, Book, Settings, Artwork };
// The workspace tabs along the top. Import is the preview shown before an import.
enum class Workspace { Library, Writing, Import, Typesetting, Preview, Publishing, Worldbuilding, Planning, Review };

inline constexpr float kTopH = 46, kSideW = 260, kPanelW = 330, kStatusH = 28;
inline constexpr UINT WM_APP_IMPORT = WM_APP + 20;  // an import file has been read (lParam: Plan*)
inline constexpr UINT WM_APP_TYPESET = WM_APP + 21; // a book has been typeset (wParam: generation, lParam: typeset::Book*)
inline constexpr UINT WM_APP_EDITION = WM_APP + 22; // an edition has been typeset for Publishing (same parameters)

// Writing commands (menus, the outline's right-click menu).
enum WCmd {
    WNewChapterAfter = 5000, WNewSceneInside, WNewPart, WRename, WMoveUp, WMoveDown, WTrash, WRestore,
    WNewChapter, WNewScene, WPrev, WNext, WLibrary,
    WKindBase = 5100, WStatusBase = 5200, WIntoBase = 5300,
};

// What the properties panel is editing.
struct Focus {
    enum Kind { None, World, Series, Book, Node, Artwork, Picture, Edition, Entry, Plan, Snapshot, Goal, Comment } kind = None;
    // Picture: a picture in the open chapter's text (id: the chapter, contextSeries: its position).
    Id id;
    Id contextSeries;  // for a book shown inside a series: its number and kind there
    bool operator==(const Focus& o) const { return kind == o.kind && id == o.id && contextSeries == o.contextSeries; }
};

enum Field {
    FName, FSubtitle, FAuthor, FPenName, FGenre, FLanguage, FIsbn, FEdition, FTags, FAccent, FNumber,
    FCaption, FAlt, FDescription, FNotes, FSynopsis, FSearch, FTarget, FieldCount
};

struct Settings {
    std::string libraryFolder;
    bool gallery = true;
    bool showPanel = true;
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
};

class StudioApp : public AppWindow {
public:
    bool create(HINSTANCE inst, int show);
    // Command line: WritingStudio.exe [--import] <file> opens the import preview for that file.
    void handleCommandLine();
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
    void editorMessage(const std::string& json);
    void addNode(studio::NodeKind kind);
    void addSceneBreak();
    void nodeContextMenu(const Id& nodeId);
    void writingCommand(int id);
    void moveNodeBy(const Id& nodeId, int delta);
    void drawBinder(const Rect& r);
    void drawNodePanel(const Rect& r, const Rect& c, float& y);
    void placeEditor();
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
    bool focusCommentField_ = false;
    bool showResolvedComments_ = false;
    Id pendingComment_;
    void drawReviewSidebar(const Rect& r);
    void drawReviewPage(const Rect& r);
    void drawReviewPanel(const Rect& r);
    std::int64_t dailyGoal() const;
    // Planning (studio_plan.cpp)
    void openPlanning(const Id& bookId);
    void selectCard(const Id& nodeId);   // "" = the book itself
    void drawPlanSidebar(const Rect& r);
    void drawPlanBoard(const Rect& r);
    void drawPlanPanel(const Rect& r);
    Color statusColour(studio::NodeStatus s) const;
    std::int64_t bookTarget(const Id& bookId) const;
    void setBookTarget(const Id& bookId, std::int64_t words);
    // Worldbuilding (studio_world.cpp)
    bool worldbuildingScope(Id& worldId, Id& bookId) const;
    void openWorldbuilding(const Id& worldId, const Id& bookId);
    void openEntry(const Id& entryId);   // "" = back to the cards
    void newEntry(const std::string& kind);   // "" = ask which kind
    void addRelationship();
    void drawWorldSidebar(const Rect& r);
    void drawWorldPage(const Rect& r);
    void drawWorldPanel(const Rect& r);
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
    studio::typeset::Resources typesetResources() const;
    float pageBook(const Rect& r);
    float pageEmpty(const Rect& r);
    float bookCollection(const Rect& r, const std::vector<const studio::Book*>& books, const std::string& listKey,
                         const Id& seriesId, bool chronological, bool reorderable);
    void bookCard(const Rect& r, const studio::Book& b, const Id& seriesId, const std::string& hitId);
    void bookRow(const Rect& r, const studio::Book& b, const Id& seriesId, const std::string& hitId, bool header);
    void drawCover(const Rect& r, const studio::Book& b);
    Color accentFor(const studio::Book& b) const;
    Color accentOfWorld(const Id& worldId) const;
    float pageHeader(const Rect& r, const std::wstring& kicker, const std::wstring& title, const std::wstring& sub);
    float toolbar(const Rect& r, bool withSearch);
    bool smallButton(const std::string& id, float x, float y, const std::wstring& label, std::function<void()> act, float* w = nullptr);
    std::vector<const studio::Book*> filteredBooks(std::vector<const studio::Book*> in) const;
    std::wstring seriesLine(const studio::Book& b) const;

    // Native text fields in the properties panel
    void createFields();
    void layoutFont();
    void placeField(Field f, const Rect& r);
    void hideUnplacedFields();
    void loadFields();
    void commitFields();
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
        enum Kind { ToWorld, ToIndependent, ToSeries, ReorderSeriesBooks, ReorderStandalone, ReorderSeriesList, ReorderWorlds, NodeRow } kind;
        Rect rect;
        Id id;              // world, series, or the item at this position
        Id listOwner;       // series id / world id the list belongs to
        bool horizontal = false;
        bool chrono = false;
        std::vector<Id> list;  // the list being reordered, in current order
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
    HBRUSH fieldBrush_ = nullptr;
    HACCEL accel_ = nullptr;

    struct DragState {
        bool armed = false, active = false;
        enum Kind { Book, Series, World, Node } kind = Book;
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
    Id currentNode_;               // the chapter/scene in the editor
    WebPane editor_;
    bool editorStarted_ = false;
    std::wstring editorNote_;      // why the editor isn't available, if it isn't
    bool loadPending_ = false;     // send the current node once the page is ready
    std::int64_t liveWords_ = -1, selectionWords_ = 0;
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
        std::map<Id, std::vector<Id>> appearances;   // node -> entries found in its text
        float scroll = 0, content = 0, sideScroll = 0, sideContent = 0, panelScroll = 0, panelContent = 0;
    } pl_;
    // Worldbuilding
    struct WorldState {
        Id world, book;            // whose entries: a world's, or an independent book's
        std::string kind;          // the kind shown ("" = everything)
        Id entry;                  // the entry open ("" = the cards)
        std::vector<studio::Mention> mentions;
        Id mentionsFor;
        std::string mentionsKey;   // the names they were found for
        float scroll = 0, content = 0, sideScroll = 0, sideContent = 0, panelScroll = 0, panelContent = 0;
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
