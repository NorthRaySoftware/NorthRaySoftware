// The browser engine: Microsoft Edge WebView2 (Chromium), kept up to date by
// Windows. This is the only part of NorthRay Browser that knows about
// WebView2; the window and the creative tools talk to Engine and Page.
//
// Everything here runs on the UI thread (WebView2 calls back on the thread
// that created it, through the window's message loop).
#pragma once

#include "com.hpp"

#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

struct ICoreWebView2;
struct ICoreWebView2Controller;
struct ICoreWebView2Environment;
struct ICoreWebView2NewWindowRequestedEventArgs;

namespace nrb {

class Page;

struct EngineOptions {
    std::wstring userDataFolder;  // the engine's cache, cookies and site data (disposable browsing data)
    std::wstring loaderPath;      // WebView2Loader.dll next to the executable
    std::wstring language;        // "en-GB"; empty for Windows' setting
};

class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Starts the engine. `done(ok, problem)` is called once; `problem` is a
    // plain sentence for the person when it fails (no runtime, damaged loader).
    void start(const EngineOptions& options, std::function<void(bool ok, const std::wstring& problem)> done);
    bool ready() const { return env_.get() != nullptr; }
    ICoreWebView2Environment* environment() const { return env_.get(); }
    // "142.0.3595.53": the installed WebView2 Runtime.
    const std::wstring& version() const { return version_; }
    // Releases the engine; its processes exit once every page has closed.
    // `exited` is called when the engine's main process has gone (or at once
    // if it wasn't running), so the app can quit without leaving processes.
    void shutdown(std::function<void()> exited);

private:
    HMODULE loader_ = nullptr;
    ComPtr<ICoreWebView2Environment> env_;
    std::wstring version_;
};

// What a page tells its tab. All optional.
struct PageEvents {
    std::function<void()> ready;                       // the page can be used
    std::function<void(bool ok, const std::wstring& problem)> createFailed;
    std::function<void()> changed;                     // address, title, history or loading state
    std::function<void(const std::wstring& uri)> navigationStarting;  // main frame (an error page should go)
    std::function<void(int failure, const std::wstring& uri)> loadFailed;  // nrb::LoadFailure value
    std::function<void(const std::wstring& text)> statusText;  // a link under the pointer, or ""
    std::function<void(int w, int h, std::vector<unsigned char> rgba)> favicon;  // RGBA, empty when none
    std::function<void(bool on)> fullscreen;           // the page wants (or leaves) full screen
    std::function<void()> closeRequested;              // window.close()
    std::function<void(bool crashed)> processFailed;   // the page's process ended or hung
    std::function<void(ICoreWebView2NewWindowRequestedEventArgs* args)> newWindow;  // window.open, target=_blank
    // A key pressed while the page has the keyboard. Return true if the app
    // used it (it then doesn't reach the page).
    std::function<bool(UINT vk, bool ctrl, bool shift, bool alt)> key;
    std::function<void()> gotFocus;
};

class Page {
public:
    struct Options {
        std::wstring profile = L"Default";
        bool inPrivate = false;        // a private window: nothing kept after it closes
        COLORREF background = RGB(255, 255, 255);  // shown before the page paints
    };

    Page(Engine& engine, HWND parent, Options options, PageEvents events);
    ~Page();
    Page(const Page&) = delete;
    Page& operator=(const Page&) = delete;

    // Creates the page's view (asynchronously; PageEvents::ready follows).
    void create();
    // Creates the view for a page another page opened (window.open or a link
    // with target=_blank), so the two stay connected (window.opener). Holds a
    // deferral on `args` until the view exists.
    void createForNewWindow(ICoreWebView2NewWindowRequestedEventArgs* args);
    bool ready() const { return web_.get() != nullptr; }

    void navigate(const std::wstring& url);
    void back();
    void forward();
    void reload(bool bypassCache = false);
    void stop();
    void print();
    void openDevTools();

    void setBounds(const RECT& r);   // parent client pixels
    void show(bool visible);
    void focus();
    void parentMoved();              // the window moved (keeps menus and pop-ups in place)
    void setBackground(COLORREF c);

    const std::wstring& url() const { return url_; }
    const std::wstring& title() const { return title_; }
    bool loading() const { return loading_; }
    bool canGoBack() const { return canBack_; }
    bool canGoForward() const { return canForward_; }

    // Closes the view and its renderer. The Page can then be destroyed.
    void close();

private:
    void attach(ComPtr<ICoreWebView2Controller> controller);
    void wireEvents();
    void refreshState();
    void requestFavicon();

    Engine& engine_;
    HWND parent_;
    Options options_;
    PageEvents events_;
    ComPtr<ICoreWebView2Controller> controller_;
    ComPtr<ICoreWebView2> web_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);  // callbacks check this after the Page is gone
    std::wstring url_, title_, pendingUrl_;
    int certFailure_ = 0;
    std::wstring navigatingTo_;   // the main-frame address being opened   // why the current navigation's certificate was refused
    bool loading_ = false, canBack_ = false, canForward_ = false, visible_ = true, closed_ = false;
    RECT bounds_{};
};

}  // namespace nrb
