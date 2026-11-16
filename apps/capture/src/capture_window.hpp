// NorthRay Capture's window (system 1): the night-sky top bar, the preview, the docked panels
// (Scenes, Sources, Audio mixer, Controls) in a resizable, rearrangeable row, the status bar, and
// the settings page. Drawn with cjui like the other NorthRay apps. Never on the media path: the
// capture, mixing and encoding threads (from Milestone 2) run whether or not this window draws.
#pragma once

#include "settings.hpp"
#include "system.hpp"

#include "ncap/design.hpp"
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
#include "output/twitch_chat.hpp"
#include "output/mp4_copy.hpp"
#include "audio/soundboard_player.hpp"
#include "ncap/wizard.hpp"

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
    void exportScene(const Id& id);
    void importScene();
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
    // Sources made in Capture (capture_made.cpp): adding them, their own settings, and the exact
    // placement and opacity of any item.
    void addImage();
    void addText();
    void addColour();
    void addSlideshow();
    void addRegion();
    void addMedia();
    void addMusic();
    void madeMenuItems(const Source& src, std::vector<cjui::MenuItem>& menu);
    bool madeCommand(UINT cmd, const Id& source);
    void itemExtraItems(const SceneItem& item, std::vector<cjui::MenuItem>& menu);
    bool itemExtraCommand(UINT cmd, const Id& item);
    void changeSourceSetting(const Id& source, const char* key, const nlohmann::json& value);
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
    void secondOutputMenu(POINT screen);
    // The overlay designer (capture_designer.cpp).
    void addDesign();
    void openDesigner(const Id& source);
    void drawDesigner(const cjui::Rect& area);
    void drawDesignerProperties(const cjui::Rect& r);
    void designChanged(bool keepUndo = true);
    void designUndo();
    void exportDesign();
    void exportDesignGif();
    bool designerMouseDown(float x, float y);
    bool designerMouseMove(float x, float y, bool leftDown);
    bool designerMouseUp();
    bool designerKey(WPARAM vk, bool ctrl, bool shift);
    void loadProfile();
    void loadBrand();
    void saveBrand(const design::Brand& b);
    void editBrand();
    void pickFont(const std::string& now, bool offerBrand, std::function<void(const std::string&)> done);
    void editProfile();
    bool pickImageFile(std::wstring& path);
    // Settings › Plugins (capture_plugins.cpp).
    float drawPluginSettings(float x, float y, float colW);
    void pluginsChanged();
    // The Camera Border maker (capture_border.cpp), drawn in the dock under the live preview.
    void openBorder(const Id& item);
    void setItemBorder(const Id& item, const border::Style& b);
    void saveBorderPng(const Id& item);
    void drawBorderEditor(const cjui::Rect& area);
    float drawRoutes(float x, float y, float w);
    // Audio filters (capture_filters.cpp): a page per sound source, like Settings.
    void openFilters(const Id& source);
    void drawFilters(const cjui::Rect& area);
    std::shared_ptr<dsp::Processor> filtersFor(const Source& s);   // the settings page's routing view; returns its height

    // Recording (capture_record.cpp)
    void startRecording();
    void stopRecording();
    void recordTick();                          // finished files, problems
    void chooseRecordingsFolder();
    void encoderMenu(POINT screen);
    std::wstring recordingsFolder() const;
    float drawRecordingSettings(float x, float y, float w);

    // Streaming (capture_stream.cpp)
    void startStreaming();                      // every destination that's switched on
    void stopStreaming();
    bool startDestination(const std::string& id, bool quiet);
    void stopDestination(const std::string& id);
    void streamTick();
    float drawStreamControls(float x, float y, float w, float bottom);   // the button and the destinations; returns the new y
    std::wstring streamStatusText(bool& live) const;
    float drawDestinationSettings(int index, float x, float y, float w);
    float drawStreamingSettings(float x, float y, float w);

    // Studio mode and transitions (capture_studio.cpp)
    Id programScene();                          // the scene that's live (fixed up if it was deleted)
    void setStudioMode(bool on);
    void takeToProgram();                       // studio mode: the preview's scene goes live, with the transition
    void transitionMenu(POINT screen);
    float drawStudioControls(float x, float y, float w, float bottom);
    std::wstring transitionLabel() const;
    void applyTransitionFiles(bool reload = false);   // the stinger's video and the shader's code, from their files
    video::Items buildItems(const Id& scene, int depth) const;
    video::Fit editFit() const;                 // where the edited canvas sits in the preview area

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
    Id enteredScene_;
    std::map<Id, ULONGLONG> designEntered_;   // when each design's scene last went live
    std::set<std::string> timersFired_;       // countdowns that already switched scene (source, element, start)
    output::Mp4Copy mp4Copy_;                 // the MP4 copy after a recording
    std::vector<mp4::Chapter> markers_;       // this recording's markers (chapters in the MP4)
    // Profiles and scene collections (capture_profiles.cpp).
    std::wstring profilesFolder() const;
    std::vector<std::string> profileNames() const;
    std::vector<std::string> collectionNames() const;
    bool busyForSwitch(const wchar_t* what);
    void applyProfile();
    void switchProfile(const std::string& name);
    void profileMenu(POINT pt);
    void switchCollection(const std::string& name, bool copyCurrent);
    void collectionMenu(POINT pt);
    // The setup wizard (capture_wizard.cpp).
    void openWizard();
    void openChecklist();
    void drawChecklist(const cjui::Rect& area);
    bool checklistOpen_ = false;
    std::vector<bool> checkTicks_;   // your own list, ticked this time
    void drawWizard(const cjui::Rect& area);
    void wizardTick();
    int wizardTwitch() const;
    void startBandwidthTest();
    bool wizardOpen_ = false, wizTesting_ = false;
    int wizStep_ = 0, wizGoal_ = 2, wizUpload_ = 0;
    wizard::TestResult wizTest_;
    std::wstring wizTestError_;
    unsigned long long wizTestStart_ = 0;
    // The soundboard (capture_soundboard.cpp).
    std::map<Id, std::unique_ptr<audio::SoundboardPlayer>> boards_;
    Id soundBoard_;                                            // the board the panel shows
    std::vector<std::pair<std::size_t, cjui::Rect>> soundRows_;   // its buttons (right-click)
    std::vector<audio::SoundboardSound> soundboardSounds(const Source& s) const;
    void setSoundboardSounds(const Id& source, const std::vector<audio::SoundboardSound>& sounds);
    void addSoundboard();
    void drawSoundboardPanel(const cjui::Rect& body);
    void soundboardMenu(const Id& source, std::size_t i);
    // Green screen (capture_key.cpp).
    std::vector<cjui::MenuItem> keyMenuItems(const SceneItem& item);
    bool keyCommand(UINT cmd, const Id& item);
    bool keyPickClick(float x, float y);
    void keyPickTick();
    Id keyPickItem_, keyPickScene_;
    keying::Key keyPickWas_;
    bool keyPickWaiting_ = false;
    int keyPickTicks_ = 0;
    void chatTick();                          // joins Twitch chat while a design shows it
    std::wstring chatStatusText() const;
    void drawChatPanel(const cjui::Rect& body);   // the Chat panel: your Twitch chat, read-only
    output::TwitchChat chat_;
    void timerTick();                         // countdowns reaching zero switch scenes                         // the program scene whose designs last played their entrances
    Id designFor_;                            // the design source the designer has open
    design::Document designDoc_;
    std::string designSel_, designRendered_, dBefore_;
    std::vector<std::string> designUndo_;
    std::vector<std::uint8_t> designPixels_;
    int designPixW_ = 0, designPixH_ = 0;
    cjui::Rect designCanvas_;
    float designScale_ = 1;
    int ddrag_ = 0;                           // 1 moving, 2 resizing
    float dStartX_ = 0, dStartY_ = 0;
    design::Box dStartBox_;
    std::vector<float> dGuidesX_, dGuidesY_;
    std::vector<std::pair<std::string, std::string>> profile_;   // {name}, {twitch}... for designs
    Id borderFor_;                            // the item whose border is being made
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
        float duckDb = 0;                     // auto-duck: how far it's turned down now
        double heardAt = -1;                  // when it last had sound (the pre-live checklist's mic check)
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
    Id filtersFor_;                          // the source whose filters page is open
    std::map<Id, std::pair<std::string, std::shared_ptr<dsp::Processor>>> processors_;   // by source: its chain and the running filters
    Id programScene_;                       // studio mode: what's live (otherwise the current scene)
    std::shared_ptr<video::VideoSource> stinger_;   // a stinger transition's video, waiting at its start
    std::string stingerFile_, shaderCode_, shaderFile_;
    bool mfForStream_ = false;
    std::vector<std::wstring> startErrors_;  // collected while starting several destinations              // MFStartup done for streaming
    std::map<std::string, int> destStates_;  // last seen state per destination, to report changes

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
