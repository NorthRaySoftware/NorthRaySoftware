// NorthRay Capture's window (system 1): the night-sky top bar, the preview, the docked panels
// (Scenes, Sources, Audio mixer, Controls) in a resizable, rearrangeable row, the status bar, and
// the settings page. Drawn with cjui like the other NorthRay apps. Never on the media path: the
// capture, mixing and encoding threads (from Milestone 2) run whether or not this window draws.
#pragma once

#include "settings.hpp"
#include "system.hpp"

#include "ncap/diag.hpp"
#include "ncap/scenes.hpp"

#include "cjui/window.hpp"

#include <functional>
#include <string>
#include <vector>

namespace ncap {

class CaptureWindow : public cjui::AppWindow {
public:
    static constexpr const wchar_t* kClassName = L"NorthRayCapture";
    CaptureWindow(Settings& settings, std::wstring settingsPath, std::wstring stateFolder);
    ~CaptureWindow() override;
    bool create(HINSTANCE inst, int show);
    bool preTranslate(MSG& msg);

protected:
    void drawFrame(float W, float H) override;
    bool onKeyDown(WPARAM vk, bool ctrl, bool shift) override;
    void onTimer(UINT_PTR id) override;
    void onSystemThemeChanged() override { applyTheme(); }
    void onDestroy() override;
    LRESULT onMessage(UINT msg, WPARAM wp, LPARAM lp, bool& handled) override;
    bool onMouseDown(float x, float y) override;
    void onMouseMove(float x, float y, bool leftDown) override;
    bool onMouseUp(float x, float y) override;
    bool onRightClick(float x, float y) override;
    bool onWheel(float x, float y, float deltaLines) override;

private:
    // Drawing
    void drawTopBar(const cjui::Rect& r);
    void drawPreview(const cjui::Rect& r);
    void drawPanel(int panel, const cjui::Rect& r);
    void drawScenes(const cjui::Rect& body);
    void drawSources(const cjui::Rect& body);
    void drawMixer(const cjui::Rect& body);
    void drawControls(const cjui::Rect& body);
    void drawStatusBar(const cjui::Rect& r);
    void drawSettings(const cjui::Rect& r);
    void drawAsk(float W, float H);
    float panelHeader(const cjui::Rect& r, int panel, const std::wstring& title);
    void note(const cjui::Rect& r, const std::wstring& title, const std::wstring& text);
    void toggle(const std::string& id, float x, float y, bool on, std::function<void()> act);

    // Scenes
    void loadScenes();
    void saveScenes();
    void scenesChanged();
    void newScene();
    void renameScene(const Id& id);
    void deleteScene(const Id& id);
    void sceneMenu(const Id& id, POINT screen);
    void addSourceMenu(POINT screen);

    // Layout
    void layoutMenu(POINT screen);
    void panelMenu(int panel, POINT screen);
    void saveLayout();
    void applyTheme();
    void openSettings(bool on);
    void showAbout();
    void copyDiagnostics();

    // Asking for a name: a small card over the window.
    void askText(const std::wstring& title, const std::wstring& initial, std::function<void(const std::wstring&)> done);
    void endAsk(bool commit);

    Settings& settings_;
    std::wstring settingsPath_, stateFolder_, scenesPath_;
    SceneCollection scenes_;
    Layout layout_;
    std::uint64_t savedChanges_ = 0;

    // Geometry from the last frame.
    cjui::Rect previewRect_{}, dockRect_{};
    cjui::Rect hSplit_{};                                  // between the preview and the panels
    std::vector<std::pair<int, cjui::Rect>> panelRects_;   // shown panels, left to right
    std::vector<cjui::Rect> vSplits_;                      // between panels
    std::vector<std::pair<Id, cjui::Rect>> sceneRows_;
    enum class Drag { None, Horizontal, Vertical, Panel } drag_ = Drag::None;
    int dragIndex_ = -1;
    float dragStart_ = 0, dragStartValue_ = 0;
    int dragPanel_ = -1;
    float dragPanelX_ = 0;
    Id lastClickScene_;
    DWORD lastClickTime_ = 0;

    // Status
    sys::UsageSampler sampler_;
    sys::Usage usage_;
    std::vector<sys::Encoder> encoders_;
    bool encodersKnown_ = false;
    bool settingsOpen_ = false;

    // The ask card
    HWND ask_ = nullptr;
    HFONT askFont_ = nullptr;
    HBRUSH askBrush_ = nullptr;
    float askScale_ = 0;
    bool asking_ = false;
    std::wstring askTitle_;
    std::function<void(const std::wstring&)> askDone_;
    cjui::Rect askCard_{};
};

}  // namespace ncap
