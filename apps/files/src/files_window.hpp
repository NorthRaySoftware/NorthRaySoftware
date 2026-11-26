// A NorthRay Files window: the night-sky sidebar (places and drives), the
// toolbar with the breadcrumb address bar, the folder's contents in Details or
// Icons view, and the status bar. Drawn with cjui like the other NorthRay apps.
//
// Version 0.1 only reads: it lists, sorts, selects and opens. Nothing here
// changes, moves or deletes a file; file operations come in 0.2 as their own
// service, called from the commands below.
#pragma once

#include "settings.hpp"
#include "shell.hpp"
#include "folder_sizes.hpp"
#include "operations.hpp"
#include "thumbnails.hpp"

#include "nrf/batchrename.hpp"
#include "nrf/labels.hpp"
#include "nrf/listing.hpp"

#include "cjui/menu.hpp"
#include "cjui/window.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace nrf {

bool isTextName(const std::wstring& name);   // files_window.cpp: shown as text in previews

class FilesWindow : public cjui::AppWindow {
public:
    static constexpr const wchar_t* kClassName = L"NorthRayFiles";

    FilesWindow(Settings& settings, std::wstring settingsPath);
    ~FilesWindow() override;
    bool create(HINSTANCE inst, int show, const std::wstring& startPath);
    // Keys for the address field; true if the message was used.
    bool preTranslate(MSG& msg);

protected:
    void drawFrame(float W, float H) override;
    bool onKeyDown(WPARAM vk, bool ctrl, bool shift) override;
    void onTimer(UINT_PTR id) override;
    void onSystemThemeChanged() override { applyTheme(); }
    void onDestroy() override;
    LRESULT onMessage(UINT msg, WPARAM wp, LPARAM lp, bool& handled) override;
    bool onMouseDown(float x, float y) override;
    bool onRightClick(float x, float y) override;
    bool onWheel(float x, float y, float deltaLines) override;
    // Ctrl and the wheel over the files in Icons or Gallery size the tiles; elsewhere they size the interface (suite 0.172).
    bool ownZoomAt(float x, float y) override { return settings_.view != ViewMode::Details && !here_.thisPC && listClip_.contains(x, y); }
    void onMouseMove(float x, float y, bool leftDown) override;
    bool onMouseUp(float x, float y) override;
    const char* navPrefix() const override { return "tb:"; }

private:
    // Where the window is: a folder, or This PC (the drives).
    struct Location {
        bool thisPC = false;
        std::wstring path;
        std::wstring query;       // a search in `path` (and its sub-folders) when not empty
        bool everywhere = false;  // the search is of the whole PC (the Windows Search index)
        std::string collection;   // a collection (0.5) instead of a folder
        bool bin = false;         // the Recycle Bin (0.6)
        bool operator==(const Location& o) const {
            return thisPC == o.thisPC && path == o.path && query == o.query && everywhere == o.everywhere && collection == o.collection && bin == o.bin;
        }
        bool searching() const { return !query.empty(); }
    };

    // Tabs and panes (0.3). Each tab is a View: where it is, its history, its listing, selection and
    // scroll. The active tab's view lives in the window's own fields (here_, shown_, sel_...); the others
    // wait in tabs_, and swapView() trades one in and out to draw it or give it a listing.
    struct View {
        Location here;
        std::vector<Location> backStack, forwardStack;
        std::vector<Entry> listed, shown;
        Selection sel;
        std::wstring selectAfterLoad;
        bool loading = false, loadedOnce = false, partial = false, quiet = false;
        ListError error = ListError::None;
        DWORD errorCode = 0;
        std::uint64_t request = 0;
        std::shared_ptr<std::atomic<std::uint64_t>> latest = std::make_shared<std::atomic<std::uint64_t>>(0);
        DWORD loadStarted = 0;
        std::vector<Entry> pending;
        std::set<std::wstring> selectNames;
        cjui::Rect listClip{};
        float listTop = 0, tileW = 156, listX = 0;
        int columns = 1;
        ScrollState scroll{};
        bool searchLimited = false, searchNoIndex = false, searchReplace = false;
        std::uint64_t searchFolders = 0;
        // Its view and sort (0.9: each folder remembers its own), swapped with settings_'s while it shows.
        ViewMode viewMode = ViewMode::Details;
        int sortKey = 0;
        bool sortDescending = false;
    };
    View newView() const;
    void applyFolderView();      // a plain folder: its remembered view and sort, else the usual ones
    void rememberFolderView();   // after the view or sort changes
    static bool plainFolder(const Location& l);
    void swapView(View& v);
    View* viewForRequest(std::uint64_t request);
    const Location& tabLocation(int pane, int tab) const;
    void activate(int pane, int tab);
    void newTab(Location at);   // by value: it may be the active tab's own location
    void closeTab(int pane, int tab);
    void setTwoPanes(bool on);
    void transferToOtherPane(bool move);
    void drawTabs(const cjui::Rect& r, int pane);
    int paneAt(float x, float y) const;
    std::uint64_t nextRequest() { return ++nextRequest_; }
    std::vector<View> tabs_[2];
    int activeTab_[2] = {0, 0};
    int pane_ = 0;                       // the active pane
    bool twoPanes_ = false;
    int swappedIn_ = 0;
    int swappedPane_ = -1;               // while drawing the other pane: whose tab is in the window's fields                  // >0 while another tab's view is swapped in (drawing it, giving it a listing)
    std::uint64_t nextRequest_ = 0;
    cjui::Rect paneRect_[2]{};
    std::string idPrefix_;               // hit ids of the pane being drawn ("" or "p1:")

    // Pinned folders and dragging (0.3)
    void pinFolder(const std::wstring& path);
    void unpinFolder(const std::wstring& path);
    void sidebarMenu(const std::wstring& pinnedPath, POINT screen);
    std::vector<std::pair<std::wstring, cjui::Rect>> pinnedRects_;
    // The folder tree (the sidebar's drives, opening like folders).
    const std::vector<std::wstring>& treeChildren(const std::wstring& path);
    void drawTree(const cjui::Rect& r, float& y, float bottom);
    std::map<std::wstring, std::vector<std::wstring>> treeKids_;   // lower-case path -> sub-folder names
    std::set<std::wstring> treeOpen_;                              // lower-case paths shown open
    std::wstring treeFollows_;                                     // the folder the tree last opened its way to
    bool treeReveal_ = false;
    float treeScroll_ = 0, treeContent_ = 0;
    cjui::Rect treeRect_{};
    cjui::Rect sidebarRect_{};
    float sideScroll_ = 0, sideContent_ = 0, sideViewH_ = 0;   // the sidebar above the tree, when it doesn't fit
    struct Drag { bool armed = false, active = false, external = false; float sx = 0, sy = 0, x = 0, y = 0; std::vector<std::wstring> paths; int fromPane = 0, clickRow = -1; } drag_;
    std::wstring dropTargetFolder(float x, float y, int* pane = nullptr);   // a folder row under the pointer, or a pane's folder

    // Navigation
    void go(const Location& to, bool remember = true, const std::wstring& select = L"");
    void goPath(const std::wstring& path, bool remember = true) { go({false, path}, remember); }
    void back();
    void forward();
    void up();
    void refresh();
    void open(std::size_t row);
    void openSelection();
    void showHere();   // the listing arrived or the view settings changed: filter, sort, keep the selection

    // Data from the worker threads
    void receive(ListBatch* b);
    void receive(DriveBatch* b);

    // The address field (typing a path)
    void beginEditingAddress();
    void endEditingAddress(bool go);

    // Commands
    void showMenu();
    void showItemMenu(POINT screen);
    void setView(ViewMode v);
    void sortBy(SortKey k);
    void toggleHidden();
    void toggleProtected();
    void applyTheme();
    void copyPaths();
    // File actions (0.2): through the Windows Shell's own operations (operations.hpp).
    void copyOrCut(bool cut);
    void paste(const std::wstring& into = L"");
    void deleteSelection(bool permanently);
    void newFolder();
    void beginRename();
    void endRename(bool commit);
    void reloadQuietly(std::vector<std::wstring> select = {});   // the folder changed: list it again, keeping the place
    void moreOptions(POINT screen);                                // Windows' own right-click menu
    void recycleBinMenu(POINT screen);
    DWORD dropAt(const std::vector<std::wstring>& paths, float x, float y, bool ctrl, bool shift, bool doIt);
    void binCommand(bool restore);   // the selection in the Recycle Bin: put back, or deleted for good
    cjui::Rect binRect_{};
    void updateRecycleBin();
    std::vector<std::wstring> selectedPaths() const;
    std::vector<std::wstring> namesHere() const;
    void ensureVisible(std::size_t row);
    void typeAhead(wchar_t c);
    std::wstring rowPath(std::size_t row) const;

    // Labels, tags and collections (0.5)
    labels::Store labels_;
    std::wstring labelsPath_;
    std::vector<std::pair<std::wstring, int>> tagChoices_;   // the tags as the last menu listed them
    std::vector<std::pair<std::string, cjui::Rect>> collectionRects_;
    void loadLabels();
    void saveLabels();
    void noteMoved(const std::vector<std::wstring>& sources, const std::wstring& destFolder);
    void noteRemoved(const std::vector<std::wstring>& paths);
    void setColourOnSelection(labels::Colour c);
    void toggleTagOnSelection(const std::wstring& tag);
    void newTagForSelection();
    void addSelectionToCollection(const std::string& id);
    void newCollectionFromSelection(bool empty = false);
    void openCollection(const std::string& id, bool inNewTab = false);
    void collectionMenu(const std::string& id, POINT screen);
    void openLabelSearch(const std::wstring& query);
    void tagsMenu();
    float drawPreviewLabels(float x, float y, float w, const std::wstring& path);
    void drawGalleryTile(const cjui::Rect& t, std::size_t row, int px, float fade);
    // Asking for a name (a new tag or collection): a small card over the window.
    void askText(const std::wstring& title, const std::wstring& initial, std::function<void(const std::wstring&)> done);
    void endAsk(bool commit);
    void drawAsk(float W, float H);
    HWND ask_ = nullptr;
    bool asking_ = false;
    std::wstring askTitle_;
    std::function<void(const std::wstring&)> askDone_;
    cjui::Rect askCard_{};
    // Batch rename (0.7): rules for many names at once, previewed (batch_rename.cpp).
    void openBatchRename();
    void closeBatchRename();
    std::vector<batch::Change> batchPlan();
    void doBatchRename();
    bool undoBatchRename();
    void drawBatchRename(float W, float H);
    struct BatchRename {
        bool open = false;
        std::wstring folder;
        std::vector<batch::File> files;
        batch::Rules rules;
        HWND fields[5]{};   // pattern, find, replace, start, digits
        int focus = 0;
    } batch_;
    std::vector<std::wstring> batchOthers_;
    std::vector<batch::Step> undoBatch_;   // the last batch, backwards
    std::wstring undoBatchFolder_;
    cjui::Rect batchCard_{};
    // Quick look (Space, 0.8; quick_look.cpp) and pictures' pixel sizes (the status bar too).
    bool quickLook_ = false;
    cjui::Rect quickCard_{};
    mutable std::map<std::wstring, std::pair<unsigned, unsigned>> pictureSizes_;
    void openQuickLook();
    bool quickLookKey(WPARAM vk);
    void drawQuickLook(float W, float H);
    std::pair<unsigned, unsigned> pictureSize(const std::wstring& path) const;
    void requestTextPreview(const std::wstring& key, const std::wstring& path);
    void subfolderMenu(const std::wstring& path, POINT screen);
    // The sidebar's edge, dragged to resize it (0.8).
    bool sidebarDrag_ = false;
    // Details column edges in the header this frame (0.9): drag one to size the column right of it.
    struct ColEdge { float x, y0, y1, right, nameLeft; int col; };
    std::vector<ColEdge> colEdges_;
    int colDrag_ = -1;          // 0 date, 1 type (or folder), 2 size
    float colDragRight_ = 0, colDragMax_ = 0;
    const ColEdge* colEdgeAt(float x, float y) const;
    float* colWidth(int col);
    float sidebarEdge_ = 0;

    // The preview pane (0.4)
    void togglePreview();
    void drawPreview(const cjui::Rect& r);
    cjui::Rect previewRect_{};
    enum PreviewState { PreviewNone, PreviewLoading, PreviewText, PreviewNotText };
    PreviewState previewState_ = PreviewNone;
    std::wstring previewKey_, previewText_;
    bool previewTruncated_ = false;

    // Searching (0.4)
    void searchTyped();                     // the search box changed: narrow at once, search again after a moment
    void runSearch(const std::wstring& text, bool everywhere);
    void endSearch();                       // back to the folder
    void focusSearch();
    void showSearchText();                  // the box shows the current tab's search
    void drawSearchBox(const cjui::Rect& r);
    float drawSearchBar(const cjui::Rect& r);   // above the results: what's searched, how many, where else to look
    HWND search_ = nullptr;
    bool settingSearch_ = false;
    cjui::Rect searchRect_{};
    bool searchLimited_ = false, searchNoIndex_ = false;
    bool searchReplace_ = false;            // the next results replace what's shown (searching again)
    void startListing();                    // reads here_ (a folder, This PC or a search) on a worker
    std::wstring nameFor(const Location& l) const;
    std::uint64_t searchFolders_ = 0;
    std::wstring searchFolderOf(std::size_t row) const;   // where a result is, below the searched folder

    // Drawing
    void drawSidebar(const cjui::Rect& r);
    void drawToolbar(const cjui::Rect& r);
    void drawAddress(const cjui::Rect& r);
    void drawContent(const cjui::Rect& r);
    void drawDetails(const cjui::Rect& r);
    void drawIcons(const cjui::Rect& r);
    void drawThisPC(const cjui::Rect& r);
    void drawMessage(const cjui::Rect& r, const std::wstring& title, const std::wstring& text, bool withRetry);
    std::wstring statusText() const;
    void drawStatusNote(const cjui::Rect& content);
    void navButton(const std::string& id, const cjui::Rect& r, int icon, bool enabled, std::function<void()> act);
    void drawIcon(int index, const cjui::Rect& r, float opacity = 1.0f);
    void drawRowBackground(const cjui::Rect& r, std::size_t row, float radius);
    std::wstring dateText(std::uint64_t fileTime) const;

    Settings& settings_;
    std::wstring settingsPath_;
    std::vector<Place> places_;

    Location here_;
    std::vector<Location> backStack_, forwardStack_;
    std::vector<Entry> listed_;           // everything in the folder
    std::vector<Entry> shown_;            // what's shown, in order
    Selection sel_;
    std::vector<Drive> drives_;
    std::wstring selectAfterLoad_;
    bool loading_ = false;
    bool loadedOnce_ = false;
    ListError error_ = ListError::None;
    DWORD errorCode_ = 0;
    bool partial_ = false;                // stopped part-way through
    std::uint64_t request_ = 0;
    std::shared_ptr<std::atomic<std::uint64_t>> latest_ = std::make_shared<std::atomic<std::uint64_t>>(0);
    DWORD loadStarted_ = 0;

    // Geometry from the last frame, for the mouse and keyboard.
    cjui::Rect listClip_{};
    float listTop_ = 0;         // y of the first row (moves with scrolling)
    float rowH_ = 30;
    int columns_ = 1;           // icons per row in Icons view
    float tileW_ = 156, tileH_ = 172;
    std::unique_ptr<FolderSizes> folderSizes_;   // item counts and sizes of folders (Details, 0.9)
    std::unique_ptr<Thumbnails> thumbs_;
    std::set<std::uint64_t> seenThumbs_;
    struct Busy {
        bool on = false;
        std::wstring what;
        float frac = -1;
        DWORD started = 0, painted = 0;
    } busy_;
    ops::Progress progressFor(bool move, std::size_t count);
    void endBusy();
    void drawBusy(float W, float H);   // pictures already faded in (0.9)
    float thumbAppear(std::uint64_t serial);   // pictures of image files (Icons view)
    float listX_ = 0;
    std::vector<std::pair<std::wstring, cjui::Rect>> driveRects_;

    // Double-clicks (the window class doesn't ask Windows for them).
    int lastClickRow_ = -1;
    DWORD lastClickTime_ = 0;
    POINT lastClickPt_{};

    // Typing a name to jump to it.
    std::wstring typed_;
    DWORD typedAt_ = 0;

    HWND address_ = nullptr;
    HFONT addressFont_ = nullptr;
    HBRUSH addressBrush_ = nullptr;
    float addressScale_ = 0;
    bool editing_ = false;
    cjui::Rect addressRect_{};
    std::wstring windowTitle_;

    // Renaming in place: a text field over the item's name.
    HWND rename_ = nullptr;
    int renameRow_ = -1;
    std::wstring renameName_, renameIn_;
    cjui::Rect renameRect_{};
    bool renaming_ = false;
    std::set<std::wstring> cutPaths_;          // waiting to be moved by Paste (drawn faded); lower case
    bool quiet_ = false;                       // listing again in the background (the folder changed)
    std::vector<Entry> pending_;
    std::set<std::wstring> selectNames_;       // select these when the listing arrives
    std::uint64_t recycleItems_ = 0, recycleBytes_ = 0;
    int recycleIcon_ = -1;
};

}  // namespace nrf
