// A NorthRay Browser window: the tab strip (night sky, like the Launcher's
// sidebar), the toolbar with the address bar, the page, and the status bar.
// Drawn with cjui like the other NorthRay apps; the page itself is the
// engine's child window, placed in the content area.
#pragma once

#include "engine.hpp"
#include "nrb/bookmarks.hpp"
#include "nrb/blocker.hpp"
#include "nrb/downloads.hpp"
#include "nrb/history.hpp"
#include "nrb/references.hpp"
#include "nrb/weather.hpp"
#include "nrb/load_errors.hpp"
#include "nrb/omnibox.hpp"
#include "nrb/tab_model.hpp"
#include "settings.hpp"

#include "cjui/menu.hpp"
#include "cjui/window.hpp"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nrb {

class BrowserWindow : public cjui::AppWindow {
public:
    static constexpr const wchar_t* kClassName = L"NorthRayBrowser";
    static constexpr float kBookmarksBarH = 34;

    BrowserWindow(Engine& engine, Settings& settings, std::wstring settingsPath);
    ~BrowserWindow() override;

    bool create(HINSTANCE inst, int show);
    // Called once the engine has started (or failed to).
    void engineStarted(bool ok, const std::wstring& problem);
    // Addresses from the command line, or from a second launch.
    void openFromCommandLine(const std::vector<std::wstring>& args);
    // Development only (--no-engine): the interface without an engine.
    void setEngineOffForDevelopment() { engineOffForDevelopment_ = true; invalidate(); }
    // Keys for the address bar; true if the message was used.
    bool preTranslate(MSG& msg);

protected:
    void drawFrame(float W, float H) override;
    bool onKeyDown(WPARAM vk, bool ctrl, bool shift) override;
    void onTimer(UINT_PTR id) override;
    void onSystemThemeChanged() override;
    void onDestroy() override;
    LRESULT onMessage(UINT msg, WPARAM wp, LPARAM lp, bool& handled) override;
    bool onMouseDown(float x, float y) override;
    void onMouseMove(float x, float y, bool leftDown) override;
    bool onMouseUp(float x, float y) override;
    bool onRightClick(float x, float y) override;
    bool onWheel(float x, float y, float deltaLines) override;
    const char* navPrefix() const override { return "tb:"; }

private:
    struct TabView {
        int id = 0;
        std::unique_ptr<Page> page;
        std::wstring pendingUrl;          // to open once the engine is ready
        bool showStart = true;            // the NorthRay start page instead of a web page
        bool bookmarksPage = false;       // with showStart: the bookmarks page instead of the start page
        bool historyPage = false;         // with showStart: the history page
        bool downloadsPage = false;       // with showStart: the downloads page
        bool referencesPage = false;      // with showStart: the reference boards
        int blocked = 0;                  // ads and trackers blocked on the page showing
        std::int64_t visitId = 0;         // the history visit of the page showing (for its title)
        std::string visitUrl;
        std::optional<LoadErrorText> error;
        std::wstring errorUrl;
        std::wstring lastTyped;           // for "Search instead"
        bool crashed = false;
        std::wstring status;              // link under the pointer
        std::wstring typed;               // address bar text being edited in this tab
        bool editing = false;
        std::vector<unsigned char> favicon;
        int favW = 0, favH = 0, favGen = 0;
    };

    // Tabs
    int newTab(const std::wstring& url, bool activate, bool afterActive, int openerId = 0);
    void closeTab(int id);
    void closeOtherTabs(int id);
    void activateTab(int id);
    void activateNeighbour(int delta);
    void reopenClosedTab();
    void duplicateTab(int id);
    TabView* view(int id);
    TabView* activeView();
    Page* ensurePage(TabView& v, bool forNewWindow = false);
    PageEvents eventsFor(int id);
    void syncTabFromPage(int id);

    // Navigation
    void submitAddress();
    void submitText(const std::wstring& text);
    void navigateTab(TabView& v, const std::wstring& url);
    void goHome();
    void stopOrReload();
    void focusAddressBar();
    void focusPage();

    // Commands
    bool shortcut(UINT vk, bool ctrl, bool shift, bool alt);
    void showMenu();
    void showTabMenu(int id, POINT screen);
    void showAbout();
    void setFullscreen(bool on, bool fromPage);
    void applyTheme();

    // Drawing
    void drawTabStrip(const cjui::Rect& r);
    // The tab strip is also the title bar: Windows' caption is removed, and
    // minimise, maximise and close are drawn at its right end.
    void drawCaptionButtons(const cjui::Rect& strip);
    LRESULT hitTestFrame(HWND h, LPARAM lp);
    float frameBorder() const;          // the sizing border inside the top edge, in DIPs
    bool maximized() const;
    cjui::Rect maxButton_{};             // where maximise is, for Windows 11's snap layouts
    bool maxHover_ = false, maxPressed_ = false, trackingNc_ = false;
    bool activeWindow_ = true;
    void drawTab(const nrb::Tab& t, const cjui::Rect& r, bool active);
    void drawToolbar(const cjui::Rect& r);
    void toolButton(const std::string& id, const cjui::Rect& r, int icon, bool enabled, std::function<void()> act,
                    const wchar_t* tip);
    void drawAddressBar(const cjui::Rect& r);
    void drawStartPage(const cjui::Rect& r);
    void drawErrorPage(const cjui::Rect& r, TabView& v);
    void drawEngineProblem(const cjui::Rect& r);

    void placePages(const cjui::Rect& content);
    void updateAddressText();
    void updateTitle();
    void layoutAddressFont();
    bool anyLoading() const;

    Engine& engine_;
    Settings& settings_;
    std::wstring settingsPath_;
    TabModel model_;
    std::vector<TabView> views_;
    bool engineOk_ = false, engineFailed_ = false;
    std::wstring engineProblem_;
    bool runtimeMissing_ = false;
    bool engineOffForDevelopment_ = false;
    std::vector<std::wstring> queued_;   // addresses waiting for the window

    HWND edit_ = nullptr;
    // Messages float over the page in a small window of their own (the page is a
    // separate window that nothing can be drawn on). There's no status bar.
    HWND note_ = nullptr;
    std::wstring noteText_;
    void updateNote();
    HFONT editFont_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    float editScale_ = 0;
    RECT editRect_{};
    bool editFocused_ = false;
    bool settingText_ = false;

    bool fullscreen_ = false, pageFullscreen_ = false;
    WINDOWPLACEMENT placement_{sizeof(WINDOWPLACEMENT)};
    LONG_PTR savedStyle_ = 0;

    bool closing_ = false;
    int dragTab_ = 0;
    float dragStartX_ = 0, dragStartY_ = 0;
    bool dragging_ = false;
    std::vector<std::pair<int, cjui::Rect>> tabRects_;
    std::wstring windowTitle_;

    // ---- Bookmarks (browser_bookmarks.cpp)
    void openBookmarkStore();
    void starClicked();
    bool currentBookmarkable(std::string* url = nullptr);
    void toggleBookmarksBar();
    void drawStar(float cx, float cy, float r, cjui::Color c, bool filled);
    void drawBookmarksBar(const cjui::Rect& r);
    void drawBookmarkIcon(const Bookmark& b, const cjui::Rect& r, bool onNight = false);
    void openBookmark(const Bookmark& b, bool newTab);
    void showBookmarkMenu(std::int64_t id, POINT screen);
    void showFolderMenu(std::int64_t folder, POINT screen);
    std::vector<cjui::MenuItem> folderItems(std::int64_t folder, std::vector<std::int64_t>& ids, int depth);
    std::vector<cjui::MenuItem> bookmarksSubmenu(std::vector<std::int64_t>& ids);
    void openFromMenuId(UINT cmd, const std::vector<std::int64_t>& ids, bool newTab);
    void showBookmarksPage(std::int64_t folder = 0, std::int64_t rename = 0);
    void drawBookmarksPage(const cjui::Rect& r);
    void folderTree(std::int64_t folder, int depth, float x, float& y, float w);
    void startRename(std::int64_t id);
    void finishRename(bool commit);
    void deleteBookmark(std::int64_t id);
    void moveBookmarkBy(std::int64_t id, int delta);
    void importBookmarks();
    void exportBookmarks();
    bool bookmarkRightClick(float x, float y);
    bool bookmarkMiddleClick(float x, float y);
    HWND makeEdit(const wchar_t* cue);
    void placeEdit(HWND e, const cjui::Rect& r, bool show);
    void positionExtraEdits();
    // Passwords and forms
    void setFormSaving(bool passwords, bool forms);
    void clearSavedPasswords();
    void explainPasswords();

    std::unique_ptr<BookmarkStore> bookmarks_;
    std::vector<std::pair<std::int64_t, cjui::Rect>> barRects_, tileRects_, rowRects_;
    std::int64_t pageFolder_ = BookmarkStore::kBar;   // the folder the bookmarks page shows
    std::wstring pageQuery_;
    HWND pageSearch_ = nullptr, renameEdit_ = nullptr, startSearch_ = nullptr;
    std::int64_t renaming_ = 0;
    cjui::Rect renameRect_{}, pageSearchRect_{}, startSearchRect_{};
    bool renameVisible_ = false, pageSearchVisible_ = false, startSearchVisible_ = false;
    bool clearPasswordsWhenReady_ = false;

    // ---- History (browser_history.cpp)
    void openHistoryStore();
    void recordVisit(int id, const std::wstring& uri);
    void showHistoryPage();
    void drawHistoryPage(const cjui::Rect& r);
    void clearHistoryMenu(POINT screen);
    void setPageSearchCue(const wchar_t* cue);
    std::wstring pageSearchCue_;
    std::unique_ptr<HistoryStore> history_;

    // ---- Downloads (browser_downloads.cpp)
    void openDownloadStore();
    void downloadStarted(int tabId, std::shared_ptr<DownloadHandle> h);
    void downloadChanged(std::int64_t id);
    void showDownloadsPage();
    void drawDownloadsPage(const cjui::Rect& r);
    void drawDownloadsButton(const cjui::Rect& r);
    void openDownloaded(std::int64_t id, bool folder);
    bool tabHasDownloads(int tabId) const;
    void releaseParkedPages();
    std::unique_ptr<DownloadStore> downloads_;
    struct LiveDownload { std::shared_ptr<DownloadHandle> handle; int tab = 0; };
    std::map<std::int64_t, LiveDownload> live_;
    std::vector<std::pair<int, std::unique_ptr<Page>>> parked_;   // closed tabs' pages, kept until their downloads finish
    std::vector<std::int64_t> finishedDownloads_;   // let go of on the next timer (not inside their own callback)
    bool downloadsSeen_ = false;                    // the toolbar button shows once there's been a download

    // ---- Reference boards and the colour picker (browser_references.cpp)
    struct PictureJob;
    void openReferenceStore();
    std::wstring referencesFolder() const;
    void savePicture(const std::wstring& picture, const std::wstring& page, const std::wstring& title);
    void pictureSaved(PictureJob* job);
    void showReferencesPage();
    void drawReferencesPage(const cjui::Rect& r);
    void addMainColours(std::int64_t picture);
    void startColourPick();
    void drawColourPick(const cjui::Rect& r);
    void finishColourPick(bool take);
    std::unique_ptr<ReferenceStore> refs_;
    std::int64_t refBoard_ = 0;           // the board the page shows
    std::int64_t renamingBoard_ = 0;
    struct ColourPick {
        bool on = false;
        int w = 0, h = 0, gen = 0;
        std::vector<unsigned char> rgba;
        std::wstring page;
        float mx = -1, my = -1;
        cjui::Rect shown{};
    } pick_;

    // ---- Blocking ads and trackers
    bool blockedOn(const std::wstring& pageUrl) const;
    void blockerMenu(POINT screen);

    // ---- Tabs down the side (settings_.verticalTabs)
    void drawSideTabs(const cjui::Rect& r);
    void setVerticalTabs(bool on);
    float sideW() const;
    float sideScroll_ = 0, sideContentH_ = 0;
    cjui::Rect sideRect_{};
    bool drawingSide_ = false;   // drawTab: a row in the side list

    // ---- Start page extras (browser_start.cpp): background, clock, weather, notes, default browser
    struct Fetch;
    void fetchText(const std::wstring& url, std::function<void(bool ok, const std::string& body)> done);
    void fetched(Fetch* f);
    void refreshWeather(bool force);
    void startWeatherSetup();
    void finishWeatherSetup(bool commit);
    void customiseMenu(POINT screen);
    void chooseStartBackground();
    std::wstring userFolder() const;
    void loadNotes();
    void saveNotes();
    bool isDefaultBrowser() const;
    void makeDefaultBrowser();
    HWND notesEdit_ = nullptr;
    HBRUSH notesBrush_ = nullptr;
    cjui::Rect notesRect_{};
    bool notesVisible_ = false, notesActive_ = false;
    Weather weather_;
    std::int64_t weatherAt_ = 0;     // when it was fetched (Unix seconds)
    bool weatherBusy_ = false, weatherSetup_ = false;

    // ---- Start page (browser_start.cpp)
    void drawStartTiles(const cjui::Rect& area, float& y);
};

}  // namespace nrb
