// CJ Philip Community: the home of the apps associated with CJ Philip.
//
// It lists every app whose metadata names CJ Philip as its associated creator
// (CJ Radio today; others appear by themselves as they're released), and is the
// one place they're installed, updated, opened and uninstalled. NorthRay
// Launcher doesn't list them; it installs CJ Philip Community itself.
//
// Installs use the same catalogue, checks and silent setups as NorthRay
// Launcher (its Model and Jobs, built into this app too).
#pragma once

#include "cj/registry.hpp"
#include "cjui/window.hpp"
#include "jobs.hpp"
#include "model.hpp"

#include <string>
#include <vector>

namespace cjp {

using namespace cjui;

class CommunityApp : public AppWindow {
public:
    bool create(HINSTANCE inst, int show);
    ~CommunityApp();

protected:
    void drawFrame(float W, float H) override;
    bool onKeyDown(WPARAM vk, bool ctrl, bool shift) override;
    void onTimer(UINT_PTR id) override;
    void onSystemThemeChanged() override { applyTheme(); }
    LRESULT onMessage(UINT msg, WPARAM wp, LPARAM lp, bool& handled) override;

private:
    void load();
    void refresh();                // installed / running state
    void checkCatalogue(bool userAsked);
    void applyTheme();
    void saveSettings();
    std::vector<nrl::AppItem*> cjApps();
    void open(const std::string& id);
    void install(const std::string& id);
    void uninstall(const std::string& id);
    void openLauncher();
    void drawCard(const Rect& r, const nrl::AppItem& a);
    float drawCards(const Rect& r);
    void progressBar(const Rect& r, float fraction, const std::string& key);
    std::wstring statusLine(const nrl::AppItem& a) const;

    nrl::Model model_;
    nrl::Jobs jobs_;
    bool checking_ = false;
    std::wstring stateDir_;
    std::wstring launcherExe_;
    std::string downloadPage_;
    std::string developer_ = "NorthRay Software";
    int theme_ = 0;                // 0 as Windows, 1 light, 2 dark
};

}  // namespace cjp
