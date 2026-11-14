// NorthRay Capture: the video thread (system 5). At the output frame rate it takes each shown
// source's newest picture, composes the current scene onto the canvas, and (at the preview's own,
// lower rate) shows the canvas in the preview with the selection drawn over it.
//
// The UI never waits for this thread: it hands over a snapshot of what to draw (the scene's items,
// the selection) and reads back statistics. The preview is a child window with its own swap chain,
// which lets clicks through to the main window, so cjui handles all the mouse work.
#pragma once

#include "video/gpu.hpp"
#include "video/sources.hpp"

#include "ncap/placement.hpp"
#include "ncap/transition.hpp"

#include <dxgi1_2.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace ncap::video {

struct DrawItem;
using Items = std::vector<DrawItem>;

struct DrawItem {
    Id id;
    std::shared_ptr<VideoSource> source;   // null: nothing to draw yet (kinds not built yet)
    Transform transform;
    bool visible = true;
    // A scene shown inside this one: its id and items (composed at canvas size, then placed like a source).
    Id nested;
    std::shared_ptr<const Items> nestedItems;
};

struct Snapshot {
    int canvasW = 1920, canvasH = 1080;
    // Program: what's live (recorded, streamed). Changing `programScene` runs `transition` from the
    // scene that was live to this one.
    Id programScene;
    Items items;                            // bottom first
    transition::Settings transition;
    // Studio mode: the preview (left) shows `previewItems`, the scene being prepared; the program is
    // on the right. Otherwise the one view shows the program, and that's what's edited.
    bool studio = false;
    Items previewItems;
    Id selected, hovered;
    bool selectedLocked = false;
    std::vector<float> guidesX, guidesY;    // snapping guides to show, canvas pixels
    float uiScale = 1;                      // the window's DPI scale, for line and handle sizes
    float background[3] = {0.02f, 0.03f, 0.06f};
};

// Where the canvas sits inside a preview area of w×h (the same in DIPs or pixels): letterboxed,
// centred, with a margin.
struct Fit { float x = 0, y = 0, scale = 1; };
Fit fitCanvas(float canvasW, float canvasH, float areaW, float areaH, float margin);
// The views in a preview area: the one that's edited (and its selection drawn), and in studio mode
// the program beside it on the right. Relative to the area's top-left.
struct Views {
    Fit edit, program;
    float half = 0, gap = 0;   // studio: each view's width, and the space between them
};
Views layoutViews(float canvasW, float canvasH, float areaW, float areaH, bool studio);

struct EngineStats {
    double fps = 0;          // frames composed in the last second
    double renderMs = 0;     // the video thread's time per frame (average over the last second)
    double gpuMs = -1;       // the graphics card's time per frame for composition and preview; -1 unknown
    int late = 0;            // frames that started late since start-up
    std::wstring adapter;
    std::wstring error;      // the engine stopped (e.g. the graphics driver reset)
    bool transitioning = false;
    Id program;              // the scene the program shows (after any transition)
};

class Engine {
public:
    ~Engine() { stop(); }
    // Creates the device, the preview window inside `parent`, and starts the video thread.
    bool start(HWND parent, int outputFps, std::wstring& error);
    void stop();
    bool running() const { return thread_.joinable(); }

    void setSnapshot(Snapshot s);
    // The preview's place in the parent window's client area, in pixels; hidden when !show.
    void setPreview(RECT pixels, bool show, int fps);
    void setOutputFps(int fps) { outputFps_ = std::max(1, fps); }
    // Something takes the canvas (a recording or a stream, from Milestone 5): compose even when
    // the preview is hidden.
    void setOutputsActive(bool on) { outputsActive_ = on; }
    EngineStats stats() const;
    // Called on the video thread with each composed canvas while set (a recording or a stream);
    // `qpc` is when the frame was composed. Composition runs whenever this is set.
    using FrameSink = std::function<void(ID3D11Texture2D* canvas, LONGLONG qpc)>;
    // One sink per output, by name ("record", "stream"); an empty sink removes it. When this
    // returns after removing one, the video thread is no longer calling it.
    void setFrameSink(const std::string& name, FrameSink sink);
    Gpu& gpu() { return gpu_; }   // for the UI thread's device-independent questions only

private:
    void run();
    void frame();
    void compose(const Snapshot& s);
    void composeItems(const Items& items, Target& into, int w, int h, int depth);
    void updateSources(const Items& items, std::set<VideoSource*>& done, int depth);
    void drawFit(Target& t, const Fit& f);
    void present(const Snapshot& s);
    bool ensureSwapChain(int w, int h);

    Gpu gpu_;
    HWND preview_ = nullptr;
    std::thread thread_;
    std::atomic<bool> quit_{false};
    std::atomic<int> outputFps_{60}, previewFps_{30};
    std::atomic<bool> previewShown_{false}, outputsActive_{false};

    mutable std::mutex m_;
    Snapshot snap_;
    std::map<std::string, FrameSink> sinks_;
    std::mutex sinkCallM_;   // held while sinks are called, so removing one waits for the call to end
    EngineStats stats_;

    // Video thread only.
    Target canvas_;                 // the program: what outputs get
    Target from_, to_, studio_;     // a transition's two scenes; studio mode's preview
    std::map<Id, Target> nested_;   // scenes shown inside others, composed once a frame
    std::set<Id> nestedDone_;
    // The running transition (video thread).
    Id lastProgram_;
    Items lastItems_, fromItems_;
    bool transitioning_ = false;
    LONGLONG transStart_ = 0;
    transition::Settings trans_;
    ComPtr<ID3D11ShaderResourceView> mask_;
    transition::Pattern maskPattern_ = transition::Pattern::Horizontal;
    int maskW_ = 0, maskH_ = 0;
    ComPtr<IDXGISwapChain1> swap_;
    ComPtr<ID3D11RenderTargetView> backbuffer_;
    int swapW_ = 0, swapH_ = 0;
    LONGLONG lastPreview_ = 0;
    // GPU timing: a few queries in flight, read back when ready.
    struct Timing { ComPtr<ID3D11Query> disjoint, begin, end; bool pending = false; };
    Timing timing_[4];
    int timingIndex_ = 0;
};

}  // namespace ncap::video
