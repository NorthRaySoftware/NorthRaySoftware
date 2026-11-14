// A NorthRay Files window: the night-sky sidebar (places and drives), the
// toolbar with the breadcrumb address bar, the folder's contents in Details or
// Icons view, and the status bar. Drawn with cjui like the other NorthRay apps.
//
// Version 0.1 only reads: it lists, sorts, selects and opens. Nothing here
// changes, moves or deletes a file; file operations come in 0.2 as their own
// service, called from the commands below.
#pragma once

#include "settings.hpp"
#include "shell.hpp"
#include "thumbnails.hpp"

#include "nrf/listing.hpp"

#include "cjui/menu.hpp"
#include "cjui/window.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace nrf {

class FilesWindow : public cjui::AppWindow {
public:
    static constexpr const wchar_t* kClassName = L"NorthRayFiles";

    FilesWindow(Settings& settings, std::wstring settingsPath);
    ~FilesWindow() override;
    bool create(HINSTANCE inst, int show, const std::wstring& startPath);
    // Keys for the address field; true if the message was used.
    bool preTranslate(MSG& msg);

protected:
    void drawFrame(float W, float H) override;
    bool onKeyDown(WPARAM vk, bool ctrl, bool shift) override;
    void onTimer(UINT_PTR id) override;
    void onSystemThemeChanged() override { applyTheme(); }
    void onDestroy() override;
    LRESULT onMessage(UINT msg, WPARAM wp, LPARAM lp, bool& handled) override;
    bool onMouseDown(float x, float y) override;
    bool onRightClick(float x, float y) override;
    const char* navPrefix() const override { return "tb:"; }

private:
    // Where the window is: a folder, or This PC (the drives).
    struct Location {
        bool thisPC = false;
        std::wstring path;
        bool operator==(const Location& o) const { return thisPC == o.thisPC && path == o.path; }
    };

    // Navigation
    void go(const Location& to, bool remember = true, const std::wstring& select = L"");
    void goPath(const std::wstring& path, bool remember = true) { go({false, path}, remember); }
    void back();
    void forward();
    void up();
    void refresh();
    void open(std::size_t row);
    void openSelection();
    void showHere();   // the listing arrived or the view settings changed: filter, sort, keep the selection

    // Data from the worker threads
    void receive(ListBatch* b);
    void receive(DriveBatch* b);

    // The address field (typing a path)
    void beginEditingAddress();
    void endEditingAddress(bool go);

    // Commands
    void showMenu();
    void showItemMenu(POINT screen);
    void setView(ViewMode v);
    void sortBy(SortKey k);
    void toggleHidden();
    void toggleProtected();
    void applyTheme();
    void copyPaths();
    void ensureVisible(std::size_t row);
    void typeAhead(wchar_t c);
    std::wstring rowPath(std::size_t row) const;

    // Drawing
    void drawSidebar(const cjui::Rect& r);
    void drawToolbar(const cjui::Rect& r);
    void drawAddress(const cjui::Rect& r);
    void drawContent(const cjui::Rect& r);
    void drawDetails(const cjui::Rect& r);
    void drawIcons(const cjui::Rect& r);
    void drawThisPC(const cjui::Rect& r);
    void drawMessage(const cjui::Rect& r, const std::wstring& title, const std::wstring& text, bool withRetry);
    void drawStatus(const cjui::Rect& r);
    void navButton(const std::string& id, const cjui::Rect& r, int icon, bool enabled, std::function<void()> act);
    void drawIcon(int index, const cjui::Rect& r, float opacity = 1.0f);
    void drawRowBackground(const cjui::Rect& r, std::size_t row, float radius);
    std::wstring dateText(std::uint64_t fileTime) const;

    Settings& settings_;
    std::wstring settingsPath_;
    std::vector<Place> places_;

    Location here_;
    std::vector<Location> backStack_, forwardStack_;
    std::vector<Entry> listed_;           // everything in the folder
    std::vector<Entry> shown_;            // what's shown, in order
    Selection sel_;
    std::vector<Drive> drives_;
    std::wstring selectAfterLoad_;
    bool loading_ = false;
    bool loadedOnce_ = false;
    ListError error_ = ListError::None;
    DWORD errorCode_ = 0;
    bool partial_ = false;                // stopped part-way through
    std::uint64_t request_ = 0;
    std::shared_ptr<std::atomic<std::uint64_t>> latest_ = std::make_shared<std::atomic<std::uint64_t>>(0);
    DWORD loadStarted_ = 0;

    // Geometry from the last frame, for the mouse and keyboard.
    cjui::Rect listClip_{};
    float listTop_ = 0;         // y of the first row (moves with scrolling)
    float rowH_ = 30;
    int columns_ = 1;           // icons per row in Icons view
    float tileW_ = 156, tileH_ = 172;
    std::unique_ptr<Thumbnails> thumbs_;   // pictures of image files (Icons view)
    float listX_ = 0;
    std::vector<std::pair<std::wstring, cjui::Rect>> driveRects_;

    // Double-clicks (the window class doesn't ask Windows for them).
    int lastClickRow_ = -1;
    DWORD lastClickTime_ = 0;
    POINT lastClickPt_{};

    // Typing a name to jump to it.
    std::wstring typed_;
    DWORD typedAt_ = 0;

    HWND address_ = nullptr;
    HFONT addressFont_ = nullptr;
    HBRUSH addressBrush_ = nullptr;
    float addressScale_ = 0;
    bool editing_ = false;
    cjui::Rect addressRect_{};
    std::wstring windowTitle_;
};

}  // namespace nrf
