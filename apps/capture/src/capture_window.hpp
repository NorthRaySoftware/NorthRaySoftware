// NorthRay Capture's window (system 1): the night-sky top bar, the preview, the docked panels
// (Scenes, Sources, Audio mixer, Controls) in a resizable, rearrangeable row, the status bar, and
// the settings page. Drawn with cjui like the other NorthRay apps. Never on the media path: the
// capture, mixing and encoding threads (from Milestone 2) run whether or not this window draws.
#pragma once

#include "settings.hpp"
#include "system.hpp"

#include "ncap/diag.hpp"
#include "ncap/placement.hpp"
#include "ncap/routing.hpp"
#include "ncap/scenes.hpp"

#include "audio/capture.hpp"
#include "audio/devices.hpp"
#include "audio/mixer.hpp"
#include "video/devices.hpp"
#include "video/engine.hpp"
#include "output/recorder.hpp"
#include "output/streamer.hpp"

#include "cjui/menu.hpp"
#include "cjui/window.hpp"

#include "nlohmann/json.hpp"

#include <functional>
#include <map>
#include <memory>
#include <set>
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

    // Sources and the preview (capture_sources.cpp)
    void startEngine();
    void syncSources();                       // capture objects follow the scene collection
    void pushSnapshot();                      // what the video thread draws
    void placePreview();                      // the preview window over the preview area
    ItemSize sizeOf(const Id& source) const;  // the source's picture size (0x0 before the first)
    std::vector<ItemSize> sizesOf(const Scene& s) const;
    const SceneItem* selectedItem() const;
    const Source* selectedSource() const;
    Vec2 toCanvas(float x, float y) const;    // window DIPs to canvas pixels
    void addCapture(SourceKind kind, const std::string& name, const std::string& settings);
    void addExisting(const Id& source);
    void itemMenu(const Id& item, POINT screen);
    void removeItem(const Id& item);
    void renameSource(const Id& source);
    void setTransform(const Id& item, const Transform& t, bool save = true);
    void transformCommand(UINT cmd);
    void checkPendingFits();
    bool previewMouseDown(float x, float y);
    bool previewMouseMove(float x, float y, bool leftDown);
    bool sourceKey(WPARAM vk, bool ctrl, bool shift);
    std::wstring sourceStatus(const Id& source, bool* problem = nullptr) const;

    // Sound (capture_audio.cpp)
    struct AudioChoices {
        std::vector<audio::Device> inputs, outputs;
        std::vector<audio::App> apps;
        bool appsSupported = false;
    };
    AudioChoices audioChoices() const;
    std::vector<cjui::MenuItem> audioAddItems(const AudioChoices& c) const;   // submenus for the "+" menus
    bool audioAddCommand(UINT cmd, const AudioChoices& c);
    void addAudioMenu(POINT screen);
    void startAudio();
    void syncAudio();                           // captures and the mixer follow the collection
    void audioItemMenu(const Id& source, POINT screen);
    void removeAudio(const Id& source);
    void saveTestClip(const Id& source);
    void meterTick();
    float audioGain(const Source& s) const;
    bool audioMuted(const Source& s) const;
    void setAudioSetting(const Id& source, const char* key, const nlohmann::json& value);
    void drawMeter(const cjui::Rect& r, audio::Meter& left, audio::Meter& right, bool muted);
    std::uint32_t audioBuses(const Source& s) const;
    routing::AudioSource routingOf(const Source& s) const;
    void analyseRouting();                      // the leak and echo checks, against what's playing now
    void saveMixClip(int bus);
    void applyMonitor();
    void monitorMenu(POINT screen);
    float drawRoutes(float x, float y, float w);   // the settings page's routing view; returns its height

    // Recording (capture_record.cpp)
    void startRecording();
    void stopRecording();
    void recordTick();                          // finished files, problems
    void chooseRecordingsFolder();
    void encoderMenu(POINT screen);
    std::wstring recordingsFolder() const;
    float drawRecordingSettings(float x, float y, float w);

    // Streaming (capture_stream.cpp)
    void startStreaming();
    void stopStreaming();
    void streamTick();
    float drawStreamingSettings(float x, float y, float w);

    // Layout
    void layoutMenu(POINT screen);
    void panelMenu(int panel, POINT screen);
    void saveLayout();
    void applyTheme();
    void openSettings(bool on);
    void showAbout();
    void copyDiagnostics();

    // Asking for a name: a small card over the window.
    void askText(const std::wstring& title, const std::wstring& initial, std::function<void(const std::wstring&)> done, bool secret = false);
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

    // Capture and the preview
    video::Engine engine_;
    std::wstring engineError_;
    struct Live { std::shared_ptr<video::VideoSource> capture; std::string settings; };
    std::map<Id, Live> live_;                 // by source id
    Id selected_, hoverItem_;                 // items in the current scene
    enum class Focus { Scenes, Sources, Preview } focus_ = Focus::Scenes;
    enum class PreviewDrag { None, Move, Resize, Crop } pdrag_ = PreviewDrag::None;
    Transform dragStartT_{};
    Vec2 dragStartCanvas_{};
    Handle dragHandle_ = Handle::None;
    ItemSize dragSize_{};
    std::vector<float> guidesX_, guidesY_;
    std::set<Id> pendingFit_;                 // new items: placed once their size is known
    cjui::Rect previewInner_{};               // where the preview window sits (DIPs)
    bool previewLive_ = false;                // the preview window is showing
    std::vector<std::pair<Id, cjui::Rect>> sourceRows_;   // item id, row
    Id lastClickItem_;
    DWORD lastClickItemTime_ = 0;

    // Sound
    audio::Mixer mixer_;
    struct AudioLive {
        std::shared_ptr<audio::Capture> capture;
        std::string key;                      // the device or program it was started with
        audio::Meter left, right;
    };
    std::map<Id, AudioLive> audio_;           // by source id
    audio::Meter mixLeft_, mixRight_, monLeft_, monRight_;
    std::vector<routing::Warning> warnings_;
    std::map<Id, std::vector<std::string>> carried_;   // desktop sources: the apps they carry now
    std::vector<std::pair<Id, cjui::Rect>> audioRows_;
    bool metersRunning_ = false;
    float mixerScroll_ = 0, mixerContentH_ = 0;
    cjui::Rect mixerBody_{};

    // Recording
    output::Recorder recorder_;
    output::Streamer streamer_;

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
