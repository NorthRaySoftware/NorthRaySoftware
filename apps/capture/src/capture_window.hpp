// NorthRay Capture's window (system 1): the night-sky top bar, the preview, the docked panels
// (Scenes, Sources, Audio mixer, Controls) in a resizable, rearrangeable row, the status bar, and
// the settings page. Drawn with cjui like the other NorthRay apps. Never on the media path: the
// capture, mixing and encoding threads (from Milestone 2) run whether or not this window draws.
#pragma once

#include "settings.hpp"
#include "system.hpp"

#include "cj/medialib.hpp"
#include "ncap/aurora_sky.hpp"
#include "ncap/visualiser.hpp"
#include "ncap/scene_templates.hpp"
#include "ncap/design.hpp"
#include "ncap/diag.hpp"
#include "ncap/live.hpp"
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
#include <mutex>
#include <thread>
#include <set>
#include <string>
#include <vector>

namespace ncap {

// The transition's stinger file when it's the brand kit's (Settings.transition.stingerFile).
inline constexpr const char* kBrandStinger = "brand:stinger";


class CaptureWindow : public cjui::AppWindow {
public:
    static constexpr const wchar_t* kClassName = L"NorthRayCapture";
    CaptureWindow(Settings& settings, std::wstring settingsPath, std::wstring stateFolder);
    ~CaptureWindow() override;
    bool create(HINSTANCE inst, int show);
    // NorthRay Launcher's Jump back in (0.58): show this scene (and this collection first, if given). From
    // `--scene` on the command line, or from a second launch passed on by WM_COPYDATA. While recording or
    // streaming it stays on its scene and says so.
    void openOn(const std::string& collection, const std::string& scene);
    static constexpr ULONG_PTR kOpenOnMessage = 0x4E524353;   // "NRCS"
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
    void drawPreviewArea(const cjui::Rect& r);
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
    // Undo and redo for the scenes (0.10.32): a snapshot of the collection each time a change settles (when
    // it's saved), without which scene is current; Ctrl+Z / Ctrl+Y step through them.
    std::vector<std::string> undo_, redo_;
    std::map<Id, std::wstring> thumbKeys_;
    bool starting_ = false;
    bool compact_ = false;
    std::string lastPage_;
    // Test my setup (capture_setuptest.cpp)
    bool setupTestOpen_ = false;
    std::unique_ptr<audio::SoundboardPlayer> testSound_;   // its chime, on the monitor only
    double testPlayedAt_ = -1;
    void openSetupTest();
    void drawSetupTest(const cjui::Rect& area);
    void playTestSound();            // the fade-ins: the page shown last frame, and the panels
    std::set<int> lastPanels_;           // compact layout this frame (Settings › Compact layout)          // the first paint, before the scenes have loaded
    void showWhatsNew(bool asked);
    std::wstring keepUpAdvice(std::wstring& why);   // the Sources panel's pictures: the key each is drawn under
    std::string undoBase_;
    static std::string undoForm(const std::string& collectionJson);
    void noteUndo();
    void undoScenes(bool redo);
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
    std::wstring withHotkey(const std::wstring& text, hotkeys::Action a, const std::string& target = {}) const;
    void drawDesigner(const cjui::Rect& area);
    void drawDesignerProperties(const cjui::Rect& r);
    void drawDesignerPropertiesBody(const cjui::Rect& r);   // sets dPropEnd_ to where it finished
    // A picture element's file and its size in pixels, for cropping (0.16): false for a picture that's a placeholder
    // ({cover}) or can't be read.
    bool cropPicture(const ncap::design::Element& e, std::wstring& path, float& iw, float& ih);
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
    bool dockWrapped_ = false;   // the panels in rows on a narrow dock (suite 0.176)
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
    void automationTick();
    void addAuroraSky();
    // The media library (capture_library.cpp).
    void openLibrary();
    void loadLibrary(bool force);
    bool saveLibrary();
    void libraryAddFiles();
    void libraryUse(const std::string& id);
    void librarySaveDesign();
    void libraryItemMenu(const std::string& id);
    void drawLibrary(const cjui::Rect& area);
    bool libraryRightClick(float x, float y);
    void libraryExportPack();
    void libraryImportPack();
    std::wstring libraryThumb(const cj::medialib::Item& it);
    std::set<std::string> libThumbFailed_;
    cj::medialib::Library library_;
    long long libStamp_ = -1;
    bool libBroken_ = false, libraryOpen_ = false;
    int libKind_ = -1;
    std::string libFolder_, libTag_, libQuery_;
    // The Library's sizes and lengths (0.10.31). The lengths are filled by worker threads, so they're shared.
    std::map<std::wstring, std::uint64_t> libSizes_;
    struct LenLock { std::mutex m; };
    std::shared_ptr<std::map<std::wstring, double>> libLengths_ = std::make_shared<std::map<std::wstring, double>>();
    std::shared_ptr<LenLock> libLenMutex_ = std::make_shared<LenLock>();
    std::uint64_t librarySize(const std::wstring& path);
    double libraryLength(const std::wstring& path);
    static std::wstring sizeWords(std::uint64_t bytes);
    std::vector<std::pair<std::string, cjui::Rect>> libRows_;
    void addAuroraPulse(bool wave = false);
    void visualiserTick();
    std::shared_ptr<audio::Tap> visTap_;
    std::unique_ptr<visualiser::Analyser> visAnalyser_;
    void writingTick();
    std::vector<std::pair<std::string, std::string>> writingValues_;   // from Writing Studio's summary
    ULONGLONG writingChecked_ = 0;
    long long writingStamp_ = -1;
    // Writing Studio (0.13): what was seen last, for celebrations, scenes by workspace, chapter markers and words this stream.
    bool writingSeen_ = false, writingLive_ = false;
    std::string writingDay_, writingWorkspace_, writingChapterId_;
    long long writingWords_ = 0, writingSprintAt_ = 0, streamWordsBase_ = -1, streamWordsLast_ = 0;
    void celebrateWriting(const std::string& text);
    float drawWritingSettings(float x, float y0, float colW);
    void switchToSceneNamed(const std::string& name, const std::wstring& why);
    void newSceneMenu();
    void sceneFromTemplate(const scene_templates::SceneTemplate& t);
    void addBrandVideo(bool intro);
    void brandVideos();
    void brandSky(aurora_sky::Settings& a) const;
    void brandSkies();
    // The stream's theme (0.21; capture_themes.cpp).
    design::Theme currentTheme() const;
    bool worldAccentFor(const std::string& id) const;
    void loadOwnThemes();
    void saveOwnThemes();
    void pushThemes();
    void themeSky(aurora_sky::Settings& a) const;
    bool rethemeCollection(const design::Theme& to, const design::Theme* previous);
    void setStreamTheme(const std::string& id, const design::Theme* previous = nullptr);
    void catchUpTheme();
    void openThemes();
    void drawThemes(const cjui::Rect& area);
    void editOwnTheme(const std::string& id, std::function<void(design::Theme&)> change);
    design::Element themedElement(design::Element e) const;
    float drawDesignTheme(const cjui::Rect& r, float y);
    std::vector<design::Theme> ownThemes_;
    bool themesOpen_ = false;
    std::string themeSel_;
    struct ThemePic {
        int w = 0, h = 0;
        std::vector<std::uint8_t> bgra;
    };
    std::map<std::string, std::pair<std::string, ThemePic>> themeThumbs_;
    void registerHotkeys();
    void unregisterHotkeys();
    bool hotkeyMessage(WPARAM id);
    bool hotkeyCapture(const MSG& msg);
    void runHotkey(const hotkeys::Binding& b);
    void listenForHotkey(const hotkeys::Binding& b, int editIndex);
    void addHotkey();
    float drawHotkeySettings(float x, float y0, float colW);
    int hkRegistered_ = 0;
    std::vector<bool> hkFailed_;
    bool hkListening_ = false;
    int hkEditIndex_ = -1;
    hotkeys::Binding hkPending_;
    void automationChanged();
    void addAutomationRule();
    void addAutomationRuleFor(const std::string& exe, const std::string& label);
    float drawAutomationSettings(float x, float y0, float colW);
    automation::Watcher autoWatch_;
    ULONGLONG autoLastCheck_ = 0;
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
    // The Notes panel and teleprompter (capture_notes.cpp).
    void createNotes();
    void drawNotesPanel(const cjui::Rect& body);
    // The Writing panel (0.20): the Writing card's parts switched off and on live, for every Writing card.
    void drawWritingPanel(const cjui::Rect& body);
    void toggleWritingPart(const std::string& key);
    void notesChanged();
    void saveNotes();
    bool notesTimer(UINT_PTR id);
    bool notesWheel(float x, float y, float lines);
    void setTelePlaying(bool on);
    std::wstring notesPath() const;
    HWND notesEdit_ = nullptr;
    HFONT notesFont_ = nullptr;
    float notesFontScale_ = 0;
    bool notesDrawn_ = false, notesRead_ = false, notesDirty_ = false, loadingNotes_ = false, telePlaying_ = false;
    std::wstring notesText_;
    cjui::Rect notesBody_;
    float teleScroll_ = 0, teleMax_ = 0;
    ULONGLONG teleLast_ = 0;
    void soundboardMenu(const Id& source, std::size_t i);
    // Green screen (capture_key.cpp).
    std::vector<cjui::MenuItem> keyMenuItems(const SceneItem& item);
    std::vector<cjui::MenuItem> pointerMenuItems(const SceneItem& item);
    bool pointerCommand(UINT cmd, const Id& item);
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
    // The stream's extras (capture_live.cpp): the plan (go-live time, length, next stream), going live by itself,
    // recording alongside, chapters at scene changes, muting by scene, Be right back, and the stream report.
    void liveTick();                          // each second
    void liveSceneCheck();                    // the live scene may have changed: chapters, the report, mutes
    void liveStateCheck();                    // streaming may have started or stopped
    void applyStreamPlan();                   // the plan to the design timers
    void toggleBrb();
    bool brbOn() const { return !brbFrom_.empty(); }
    bool mixMuted(const Source& s) const { return audioMuted(s) || autoMuted_.count(s.id) > 0; }
    std::string liveSceneName();
    float drawLiveSettings(float x, float y0, float colW);   // Settings › Streaming › Your stream
    float drawBrbButton(float x, float y, float w);
    void openReport(int index);               // 0 = the newest saved
    void openDestView();                      // what each destination gets (capture_destview.cpp)
    void drawDestView(const cjui::Rect& area);
    bool destViewOpen_ = false;
    void drawReport(const cjui::Rect& area);
    std::vector<std::wstring> reportFiles() const;   // newest first
    std::wstring reportsFolder() const;
    live::Collector report_;
    live::Report shownReport_;
    int shownReportIndex_ = 0;
    bool reportOpen_ = false;
    Id liveSceneSeen_, brbFrom_;
    std::set<Id> autoMuted_;
    bool wasStreaming_ = false, autoRecording_ = false;
    ULONGLONG streamStartTick_ = 0;
    void timerTick();                         // countdowns reaching zero switch scenes                         // the program scene whose designs last played their entrances
    Id designFor_;                            // the design source the designer has open
    design::Document designDoc_;
    std::string designSel_, designRendered_, dBefore_;
    std::vector<std::string> designUndo_;
    std::vector<std::uint8_t> designPixels_;
    int designPixW_ = 0, designPixH_ = 0;
    cjui::Rect designCanvas_;
    float designScale_ = 1;
    int ddrag_ = 0;                           // 1 moving, 2 resizing, 3 a selection box, 4 a guide from a ruler
    std::vector<std::string> designMulti_;    // 0.14: everything selected (designSel_ is the one whose properties show)
    std::map<std::string, design::Box> dStartBoxes_;
    cjui::Rect dMarquee_{}, designCentre_{};
    int dGuideAxis_ = 0, dGuideIndex_ = -1;   // the guide being dragged: 0 across (x), 1 down (y)
    // The gradient bar (0.15): where it is (the stops' handles under it included), whose fill it shows (an element's
    // id), the chosen stop, and a drag in progress (moved; pulled off the bar to remove).
    cjui::Rect dGradBar_{};
    std::string dGradFor_, dStopOwner_;   // dStopOwner_: whose stop dStopSel_ is
    int dStopSel_ = -1;
    bool dStopMoved_ = false, dStopOff_ = false;
    float dStopDownX_ = 0;
    std::wstring dGradKey_;
    // Cropping a picture (0.16): the element being cropped (dragging it moves the picture inside it; the wheel zooms).
    std::string dCropFor_;
    float dCropLastX_ = 0, dCropLastY_ = 0;
    // The properties panel scrolls with the wheel when it's taller than the window (0.15; no scroll bar: faded edges).
    cjui::Rect dPropRect_{};
    float dPropScroll_ = 0, dPropContentH_ = 0, dPropEnd_ = 0;
    std::vector<std::string> designSelection() const;
    void designSelect(std::vector<std::string> ids, const std::string& primary);
    void designGroupSelected(bool group);
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
    audio::Meter busLeft_[8], busRight_[8];   // every bus, for what each destination gets
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
