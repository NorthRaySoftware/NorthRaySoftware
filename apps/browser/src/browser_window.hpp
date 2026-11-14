// A NorthRay Browser window: the tab strip (night sky, like the Launcher's
// sidebar), the toolbar with the address bar, the page, and the status bar.
// Drawn with cjui like the other NorthRay apps; the page itself is the
// engine's child window, placed in the content area.
#pragma once

#include "engine.hpp"
#include "nrb/load_errors.hpp"
#include "nrb/omnibox.hpp"
#include "nrb/tab_model.hpp"
#include "settings.hpp"

#include "cjui/window.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nrb {

class BrowserWindow : public cjui::AppWindow {
public:
    static constexpr const wchar_t* kClassName = L"NorthRayBrowser";

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
    const char* navPrefix() const override { return "tb:"; }

private:
    struct TabView {
        int id = 0;
        std::unique_ptr<Page> page;
        std::wstring pendingUrl;          // to open once the engine is ready
        bool showStart = true;            // the NorthRay start page instead of a web page
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
    void drawTab(const nrb::Tab& t, const cjui::Rect& r, bool active);
    void drawToolbar(const cjui::Rect& r);
    void toolButton(const std::string& id, const cjui::Rect& r, int icon, bool enabled, std::function<void()> act,
                    const wchar_t* tip);
    void drawAddressBar(const cjui::Rect& r);
    void drawStartPage(const cjui::Rect& r);
    void drawErrorPage(const cjui::Rect& r, TabView& v);
    void drawEngineProblem(const cjui::Rect& r);
    void drawStatusBar(const cjui::Rect& r);
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
    float dragStartX_ = 0;
    bool dragging_ = false;
    std::vector<std::pair<int, cjui::Rect>> tabRects_;
    std::wstring windowTitle_;
};

}  // namespace nrb
