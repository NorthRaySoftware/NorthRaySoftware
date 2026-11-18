// NorthRay Launcher: a general-purpose app manager for NorthRay Software.
// Find apps, install, update, open and uninstall them. Community features
// live in the separate, optional CJ Philip Community app.
#pragma once

#include "cjui/format.hpp"
#include "cjui/window.hpp"
#include "jobs.hpp"
#include "model.hpp"
#include "version.hpp"

#include <functional>
#include <map>
#include <string>

namespace nrl {

using namespace cjui;

enum class Page { Welcome, Library, App, Updates, Settings };

class Launcher : public AppWindow {
public:
    bool create(HINSTANCE inst, int show, const std::string& openApp, bool startInTray);

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

private:
    void applyTheme();
    void navigate(Page p, const std::string& appId = {});
    void checkCatalogue(bool userAsked);
    void install(const std::string& appId);
    void updateAll();
    void uninstall(const std::string& appId);
    void open(const std::string& appId);
    void chooseInstallRoot();
    void autoUpdate();

    // The notification area (tray): the launcher keeps running there so updates still install.
    void addTrayIcon();
    void removeTrayIcon();
    void trayMenu();
    void showFromTray();
    void hideToTray();
    void quit();
    void notifyTray(const std::wstring& title, const std::wstring& text);
    void setStartWithWindows(bool on);
    void checkbox(const std::string& id, const Rect& r, const std::wstring& label, bool on, std::function<void()> toggle, bool night);

    void drawNav(const Rect& r);
    void drawAurora(const Rect& r);
    void drawIcon(const AppItem& a, const Rect& r);
    float drawHeader(const Rect& r, const std::wstring& title, const std::wstring& subtitle);
    void actionArea(const AppItem& a, const Rect& r, const std::string& prefix, bool compact);
    void progressBar(const Rect& r, float fraction, const std::string& key = {});
    std::wstring statusLine(const AppItem& a) const;

    void drawLightbox(float W, float H);
    void nightButton(const std::string& id, const Rect& r, const std::wstring& label, std::function<void()> act, bool primary = true);
    void drawWelcome(float W, float H);
    void finishWelcome();
    float pageLibrary(const Rect& r);
    float drawToday(const Rect& r);   // the home page's Today strip (0 if there's nothing to show)
    float appCard(const AppItem& a, const Rect& r);
    float pageApp(const Rect& r);
    float pageUpdates(const Rect& r);
    float pageSettings(const Rect& r);

    Model model_;
    Jobs jobs_;
    Page page_ = Page::Library;
    std::string appId_;
    int filter_ = 0;           // 0 all, 1 installed, 2 not installed
    bool checking_ = false;
    std::wstring markPng_;     // NorthRay mark for the sidebar
    int lightbox_ = -1;        // the screenshot shown large, or -1
    int welcomeStep_ = 0;      // the first-run tour: 0 welcome, 1 install folder, 2 updates and tray
    bool trayAdded_ = false;
    bool quitting_ = false;
    bool toldAboutTray_ = false;
    Today today_;              // read when the window is shown or activated
    std::map<std::string, unsigned long long> diskUse_;   // installed apps' folder sizes, worked out once per visit
    UINT taskbarCreated_ = 0;  // Explorer restarted: the tray icon has to be added again
    // Dragging app cards into another order (0.97).
    std::vector<std::pair<std::string, Rect>> cardRects_;
    std::string dragApp_;
    float dragX_ = 0, dragY_ = 0;
    bool dragging_ = false;
    std::wstring lastAppUpdated(const AppItem& a) const;   // "Updated 18 Nov" from the app's files
    // The tray icon shows progress and which apps have updates (0.97).
    void updateTrayState();
    std::wstring trayTip_;
    int trayProgress_ = -1;    // the percentage drawn on the icon, or -1 for the plain icon
};

}  // namespace nrl
