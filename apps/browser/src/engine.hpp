// The browser engine: Microsoft Edge WebView2 (Chromium), kept up to date by
// Windows. This is the only part of NorthRay Browser that knows about
// WebView2; the window and the creative tools talk to Engine and Page.
//
// Everything here runs on the UI thread (WebView2 calls back on the thread
// that created it, through the window's message loop).
#pragma once

#include "com.hpp"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct ICoreWebView2;
struct ICoreWebView2Controller;
struct ICoreWebView2Environment;
struct ICoreWebView2NewWindowRequestedEventArgs;
struct ICoreWebView2DownloadOperation;

namespace nrb {

class Page;

// A download the engine is doing, for the Downloads page. Lives as long as someone holds it; the
// download itself belongs to the page that started it.
class DownloadHandle {
public:
    enum State { Running, Paused, Done, Cancelled, Failed };
    struct Info {
        std::wstring url, path, mime;
        std::int64_t received = 0, total = -1;
        State state = Running;
        std::wstring reason;            // why it failed, in words
        std::int64_t estimatedEnd = 0;  // Unix seconds, 0 = unknown
        bool canResume = false;
    };
    explicit DownloadHandle(ICoreWebView2DownloadOperation* op);
    ~DownloadHandle();
    Info info() const;
    void cancel();
    void pause();
    void resume();
    // Called when the bytes, time left or state change.
    void onChanged(std::function<void()> fn) { *changed_ = std::move(fn); }

private:
    ComPtr<ICoreWebView2DownloadOperation> op_;
    std::shared_ptr<std::function<void()>> changed_ = std::make_shared<std::function<void()>>();
};

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
    std::function<void(const std::wstring& uri)> loaded;  // a main-frame page finished opening (for the history)
    std::function<void(const std::wstring& fileName)> downloadStarted;  // a link became a download
    // The download itself (shown on the Downloads page instead of the engine's own list).
    std::function<void(std::shared_ptr<DownloadHandle> download)> download;
    std::function<void(int count)> blocked;            // ads and trackers blocked on this page so far
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
        // The engine's own password manager and form filling (Chromium's, as in
        // Microsoft Edge): it offers to save a password after a sign-in, stores
        // it encrypted with a key protected by the person's Windows account, and
        // fills it in next time. Off for private windows regardless.
        bool savePasswords = true;
        bool fillForms = true;
        // Ads and trackers: request filters (the engine only asks about these), and whether a request
        // from the page (its own address) is refused.
        std::vector<std::wstring> blockFilters;
        std::function<bool(const std::wstring& request, const std::wstring& page)> block;
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
    // Turns saving passwords and filling forms on or off for the page's profile.
    void setFormSaving(bool passwords, bool forms);
    // Deletes every password (and, with `forms`, form entry) the profile has saved.
    void clearSavedPasswords(bool forms, std::function<void(bool ok)> done);

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
    int certFailure_ = 0;         // why the current navigation's certificate was refused
    std::uint64_t navId_ = 0;        // the latest main-frame navigation
    bool downloading_ = false;       // it turned into a download
    int blocked_ = 0;                // requests refused on the page showing
    std::wstring navigatingTo_;   // the main-frame address being opened
    bool loading_ = false, canBack_ = false, canForward_ = false, visible_ = true, closed_ = false;
    RECT bounds_{};
};

}  // namespace nrb
