// Shared window base for every NorthRay app.
//
// An immediate-mode UI: each frame the app draws itself and registers "hits"
// (interactive regions) as it goes. Mouse and keyboard input are resolved
// against the hits from the previous frame, so a control's drawing and
// behaviour live in one place. The base handles DPI, theme, scrolling,
// keyboard focus (Tab, Enter, Space, Escape) and the window procedure.
#pragma once

#include "cjui/gfx.hpp"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace cjui {

// Slow ambient motion (twinkling stars): drawing code reads the frame's clock
// and asks for another frame. `ambientTime()` is seconds, or < 0 when the
// window shows no motion (Windows' animations are off); `wantAmbientFrame()`
// keeps the window redrawing gently (about 12 times a second) while visible.
double ambientTime();
void wantAmbientFrame();

struct Palette {
    Color paper, surface, sunken, ink, inkMuted, inkFaint, rule, ribbon, focus, live, demo;
    Color console, consoleInk, consoleMuted, consoleRule;  // a dark console look, the same in both themes
    bool dark = false;
};

// Mirrors shared/metadata/brand/tokens.json.
Palette makePalette(bool dark);
bool systemPrefersDark();

struct Hit {
    std::string id;
    Rect rect;
    Rect clip;
    std::function<void()> action;
    bool focusable = true;
};

// Interface zoom (suite 0.172): each NorthRay window can be drawn larger or smaller than Windows' display scaling
// says, 50% to 200%, saved for that app on this PC. Menus and message boxes opened over a window follow its zoom.
float interfaceZoomOf(HWND window);
const std::vector<float>& zoomSteps();   // 50%, 67%, 75%, 80%, 90%, 100%, 110%, 125%, 150%, 175%, 200%

struct WindowSpec {
    HINSTANCE instance = nullptr;
    const wchar_t* className = L"NorthRayWindow";
    const wchar_t* title = L"NorthRay";
    int iconId = 0;
    int width = 1180, height = 760;        // DIPs
    int minWidth = 860, minHeight = 560;   // DIPs
    int show = SW_SHOWNORMAL;
    // Window style. Apps that host child windows they don't draw themselves
    // (a browser engine's page) add WS_CLIPCHILDREN so painting skips them.
    DWORD style = WS_OVERLAPPEDWINDOW;
    // Post WM_QUIT when the window is destroyed. Apps whose message loop
    // belongs to someone else (CEF's) turn this off and quit it themselves.
    bool quitOnDestroy = true;
    // No Windows title bar: the app draws minimise, maximise and close itself
    // (drawCaptionButtons) in its own top area, and the empty part of that
    // area (the top `captionHeight` DIPs) moves the window.
    bool customFrame = false;
    float captionHeight = 40;
};

class AppWindow {
public:
    virtual ~AppWindow() = default;
    bool createWindow(const WindowSpec& spec);
    HWND hwnd() const { return hwnd_; }

protected:
    // Draw the whole window. W and H are in DIPs.
    virtual void drawFrame(float W, float H) = 0;
    // Return true if the key was handled.
    virtual bool onKeyDown(WPARAM vk, bool ctrl, bool shift) { (void)vk; (void)ctrl; (void)shift; return false; }
    virtual void onTimer(UINT_PTR id) { (void)id; }
    virtual void onSystemThemeChanged() {}
    virtual void onDestroy() {}
    // Messages the base doesn't handle (e.g. WM_APP notifications from worker threads).
    virtual LRESULT onMessage(UINT msg, WPARAM wp, LPARAM lp, bool& handled) {
        (void)msg; (void)wp; (void)lp; handled = false; return 0;
    }
    // Raw mouse hooks (DIP coordinates), for drag-and-drop, context menus and
    // extra scrolling areas. Return true to stop the base's own handling.
    virtual bool onMouseDown(float x, float y) { (void)x; (void)y; return false; }
    virtual void onMouseMove(float x, float y, bool leftDown) { (void)x; (void)y; (void)leftDown; }
    virtual bool onMouseUp(float x, float y) { (void)x; (void)y; return false; }
    virtual bool onRightClick(float x, float y) { (void)x; (void)y; return false; }
    virtual bool onWheel(float x, float y, float deltaLines) { (void)x; (void)y; (void)deltaLines; return false; }
    const std::string& hoverId() const { return hover_; }
    float mouseX() const { return mouseX_; }
    float mouseY() const { return mouseY_; }
    // The id under a point, from the last frame's hits ("" if none).
    std::string hitAt(float x, float y) const;
    void cancelPress() { pressed_.clear(); }
    // Clip for hits added outside beginScroll/endScroll (e.g. a second scrolling panel).
    void setHitClip(const Rect& r) { hitClip_ = r; hitClipSet_ = true; }
    void clearHitClip() { hitClipSet_ = false; }

    // Interface zoom (suite 0.172). Ctrl and + / - / 0, and Ctrl with the wheel, change it ("nearest wins": over
    // something with its own zoom, that zooms instead), as do Ctrl+Alt and + / - / 0 everywhere.
    float interfaceZoom() const { return zoom_; }
    void setInterfaceZoom(float zoom, bool announce = true);
    void zoomStep(int direction);   // +1 larger, -1 smaller, 0 back to 100%
    // A place under the pointer with a Ctrl+wheel zoom of its own (a timeline, typeset pages): the wheel is the app's.
    virtual bool ownZoomAt(float x, float y) { (void)x; (void)y; return false; }
    // Ctrl and + / - / 0 belong to the app (the Browser zooms the web page with them); Ctrl+Alt still zooms the interface.
    virtual bool ctrlZoomKeysAreApps() const { return false; }
    // The zoom changed: native controls' fonts and child windows follow (the window is drawn again anyway).
    virtual void onInterfaceZoomChanged() {}
    // Settings' "Interface size": presets (Small, Default, Large, Extra large), a slider from 50% to 200% and a line
    // saying which keys change it. Returns the height it took.
    float zoomControl(const std::string& id, float x, float y, float w);

    // Hits whose id starts with this prefix come first in Tab order.
    virtual const char* navPrefix() const { return "nav:"; }

    void invalidate();
    void applyPalette(const Palette& p);

    // Scrollable content area: call beginScroll, draw content starting at the
    // returned y, then endScroll with the content's total height.
    float beginScroll(const Rect& clip);
    void endScroll(float contentHeight);
    // Back to the top, with the page fade-in (call when the page changes).
    void resetScroll();
    void scrollBy(float dy);
    // Where the scrolling area is scrolled to, for a window with several (NorthRay Files' panes and tabs).
    struct ScrollState { float scroll = 0, target = 0, content = 0, viewport = 0; };
    ScrollState scrollState() const { return {scroll_, scrollTarget_, contentHeight_, viewportHeight_}; }
    void setScrollState(const ScrollState& s) { scroll_ = s.scroll; scrollTarget_ = s.target; contentHeight_ = s.content; viewportHeight_ = s.viewport; }

    // Animation. Values ease towards their target over a few frames; the base
    // keeps repainting until everything has settled. With Windows' "Animation
    // effects" turned off, values jump straight to their targets.
    // A key seen for the first time starts at its target (nothing animates in
    // on first draw).
    float anim(const std::string& key, float target, float speed = 16.0f);
    float hoverAmount(const std::string& id) { return anim(id + "#hover", hovered(id) ? 1.0f : 0.0f, 18.0f); }
    // For positions in DIPs (selection highlights that slide).
    float slideTo(const std::string& key, float target) { return anim(key, target, 20.0f); }
    void setAnim(const std::string& key, float value);  // jump without animating
    bool animationsEnabled() const { return animationsOn_; }
    // Ambient motion (twinkling stars) on or off for this window; on by default. Apps that must
    // stay light (Capture) turn it off so an idle window doesn't redraw.
    void setAmbientAllowed(bool on) { ambientAllowed_ = on; }
    // Page fade-in progress, 0..1, eased (fast start, gentle landing).
    float pageProgress();
    // A soft shadow under a raised card; `amount` 0..1 (e.g. hoverAmount).
    void softShadow(const Rect& r, float radius, float amount);
    // Status messages fade in and out: draw flashText() in flashColor(c).
    Color flashColor(Color c);

    // Controls
    bool hovered(const std::string& id) const { return hover_ == id; }
    bool focused(const std::string& id) const { return focus_ == id && keyboardFocus_; }
    bool hasKeyboardFocus() const { return keyboardFocus_ && !focus_.empty(); }
    bool pressed(const std::string& id) const { return pressed_ == id && hover_ == id; }
    void addHit(const std::string& id, const Rect& r, std::function<void()> action, bool focusable = true);
    // A tooltip for the hit area `id` (registered each frame, after or before its addHit): shown in a small
    // bubble by the pointer once it has rested there for half a second, and gone when it moves off or clicks.
    void tooltip(const std::string& id, std::wstring text);
    void focusRing(const Rect& r);
    bool button(const std::string& id, const Rect& r, const std::wstring& label, std::function<void()> action,
                bool primary = false);
    void disabledButton(const Rect& r, const std::wstring& label);
    void calmFace(const std::string& id, const Rect& r, const std::wstring& label, Font f, bool enabled);
    // The width a calm button needs for `label` in the current label style (0.87): words with their icon, or the icon alone.
    float buttonWidth(const std::wstring& label, Font f = Font::Body);
    bool linkText(const std::string& id, float x, float y, const std::wstring& label, std::function<void()> action,
                  Font f = Font::Small);
    // A small button laid out by its own width (0.88): its icon and words, the icon alone (words as a tooltip) or the words
    // alone, by the label style. Returns how far the next one goes (its frame's width and a gap).
    float linkButton(const std::string& id, float x, float y, const std::wstring& label, std::function<void()> action,
                     Font f = Font::Small);
    void segmented(const std::string& id, const Rect& r, const std::vector<std::wstring>& options, int selected,
                   std::function<void(int)> onSelect);
    void tag(float x, float y, const std::wstring& label, Color c, float* outWidth = nullptr);
    float sectionLabel(float x, float y, float w, const std::wstring& label);
    // Horizontal slider, value 0..1. Mouse drag, or focus + Left/Right.
    void slider(const std::string& id, const Rect& r, float value, std::function<void(float)> onChange);

    // A short message, e.g. for a status bar. Clears itself after `seconds`.
    void flash(const std::wstring& message, int seconds = 4);
    // The same with an Undo button in it (the calm controls), for changes made at once instead of asked about first.
    void flashUndo(const std::wstring& message, std::function<void()> undo, int seconds = 8);
    // The calm, consistent controls (UI pass, 2026-11; Writing Studio first): every button, small button and link-button
    // drawn alike (one surface, one outline, one radius, the same type), with an icon from its words where there is one;
    // quieter notes in the corner, with Undo when given; livelier hover and page motion.
    void setCalmControls(bool on) { calm_ = on; }
    bool calmControls() const { return calm_; }
    const std::wstring& flashText() const { return flashVisible_ ? flash_ : empty_; }
    // The flash message as a toast: a small rounded note near the bottom of `area`
    // that rises in and fades out (the apps have no status bar).
    void drawToast(const Rect& area);

    void clearFocus() { focus_.clear(); keyboardFocus_ = false; }

    // Custom frame (WindowSpec::customFrame): minimise, maximise/restore and
    // close at the top right of `strip`. `onNight` for a dark background.
    // Maximise is reported to Windows as HTMAXBUTTON, so Windows 11 shows its
    // snap layouts over it. Returns the buttons' total width.
    float drawCaptionButtons(const Rect& strip, bool onNight);
    static constexpr float kCaptionButtonW = 46;
    void setCaptionHeight(float h) { captionHeight_ = h; }
    bool windowActive() const { return windowActive_; }
    bool maximized() const;

    Gfx gfx_;
    Palette pal_;

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void paint();
    void moveFocus(int dir);
    void activateFocused();
    void updateHover();

    HWND hwnd_ = nullptr;
    WindowSpec spec_;
    // Custom frame
    LRESULT frameMessage(UINT msg, WPARAM wp, LPARAM lp, bool& handled);
    float frameBorder() const;
    float captionHeight_ = 40;
    float zoom_ = 1.0f;
    UINT displayDpi() const;
    Rect maxButton_{};
    bool maxHover_ = false, maxPressed_ = false, trackingNc_ = false, windowActive_ = true;
    std::vector<Hit> hits_, lastHits_;
    std::vector<std::pair<std::string, std::wstring>> tips_;
    ULONGLONG hoverSince_ = 0;
    bool tipHidden_ = false;   // clicked: no tip until the pointer moves to something else
    void drawTooltip(float W, float H);
    std::string hover_, pressed_, focus_;
    bool keyboardFocus_ = false;
    float mouseX_ = -1, mouseY_ = -1;
    bool trackingMouse_ = false;

    bool inScroll_ = false;
    Rect hitClip_;
    bool hitClipSet_ = false;
    Rect contentClip_;
    float scroll_ = 0, contentHeight_ = 0, viewportHeight_ = 0;

    struct SliderState { std::string id; Rect rect; float value; std::function<void(float)> onChange; };
    std::vector<SliderState> sliders_, lastSliders_;
    std::string dragging_;

    std::wstring flash_, empty_;
    std::function<void()> flashUndo_;
    bool calm_ = false;
    bool flashActive_ = false;
    bool flashVisible_ = false;      // still fading out after it expired
    double pageStart_ = -10;

    struct AnimState { float value = 0; unsigned frame = 0; };
    std::unordered_map<std::string, AnimState> anims_;
    unsigned frame_ = 0;
    float frameDt_ = 1.0f / 60;
    long long lastFrameCounter_ = 0;
    bool animating_ = false;
    bool animationsOn_ = true;
    bool ambient_ = false;
    bool ambientAllowed_ = true;
    float scrollTarget_ = 0;
    double lastScrollTime_ = -10;
    void readAnimationSetting();
    double clockSeconds() const;
};

}  // namespace cjui
