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
#include <string>

namespace nrl {

using namespace cjui;

enum class Page { Welcome, Library, App, Updates, Settings };

class Launcher : public AppWindow {
public:
    bool create(HINSTANCE inst, int show, const std::string& openApp);

protected:
    void drawFrame(float W, float H) override;
    bool onKeyDown(WPARAM vk, bool ctrl, bool shift) override;
    void onTimer(UINT_PTR id) override;
    void onSystemThemeChanged() override { applyTheme(); }
    LRESULT onMessage(UINT msg, WPARAM wp, LPARAM lp, bool& handled) override;
    void onDestroy() override;

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

    void drawNav(const Rect& r);
    void drawStatusBar(const Rect& r);
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
};

}  // namespace nrl
