// Shared window base for every CJ Community app.
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

struct Palette {
    Color paper, surface, sunken, ink, inkMuted, inkFaint, rule, ribbon, focus, live, demo;
    Color console, consoleInk, consoleMuted, consoleRule;  // CJ Radio's dark console, same in both themes
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

struct WindowSpec {
    HINSTANCE instance = nullptr;
    const wchar_t* className = L"CJCommunityWindow";
    const wchar_t* title = L"CJ Community";
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
    // Page fade-in progress, 0..1, eased (fast start, gentle landing).
    float pageProgress();
    // A soft shadow under a raised card; `amount` 0..1 (e.g. hoverAmount).
    void softShadow(const Rect& r, float radius, float amount);
    // Status messages fade in and out: draw flashText() in flashColor(c).
    Color flashColor(Color c);

    // Controls
    bool hovered(const std::string& id) const { return hover_ == id; }
    bool focused(const std::string& id) const { return focus_ == id && keyboardFocus_; }
    bool pressed(const std::string& id) const { return pressed_ == id && hover_ == id; }
    void addHit(const std::string& id, const Rect& r, std::function<void()> action, bool focusable = true);
    void focusRing(const Rect& r);
    bool button(const std::string& id, const Rect& r, const std::wstring& label, std::function<void()> action,
                bool primary = false);
    void disabledButton(const Rect& r, const std::wstring& label);
    bool linkText(const std::string& id, float x, float y, const std::wstring& label, std::function<void()> action,
                  Font f = Font::Small);
    void segmented(const std::string& id, const Rect& r, const std::vector<std::wstring>& options, int selected,
                   std::function<void(int)> onSelect);
    void tag(float x, float y, const std::wstring& label, Color c, float* outWidth = nullptr);
    float sectionLabel(float x, float y, float w, const std::wstring& label);
    // Horizontal slider, value 0..1. Mouse drag, or focus + Left/Right.
    void slider(const std::string& id, const Rect& r, float value, std::function<void(float)> onChange);

    // A short message, e.g. for a status bar. Clears itself after `seconds`.
    void flash(const std::wstring& message, int seconds = 4);
    const std::wstring& flashText() const { return flashVisible_ ? flash_ : empty_; }

    void clearFocus() { focus_.clear(); keyboardFocus_ = false; }

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
    std::vector<Hit> hits_, lastHits_;
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
    float scrollTarget_ = 0;
    double lastScrollTime_ = -10;
    void readAnimationSetting();
    double clockSeconds() const;
};

}  // namespace cjui
