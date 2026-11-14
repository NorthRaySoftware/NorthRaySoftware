#include "engine.hpp"

#include "WebView2MinGW.h"

#include <wincodec.h>

#include <utility>

namespace nrb {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string o((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), o.data(), n, nullptr, nullptr);
    return o;
}

namespace {

std::wstring describeStartFailure(HRESULT hr) {
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
        return L"The Microsoft Edge WebView2 Runtime, which NorthRay Browser uses to show web pages, isn't installed. "
               L"It comes with Windows 11; on Windows 10 it can be installed from Microsoft.";
    if (hr == HRESULT_FROM_WIN32(ERROR_INVALID_STATE))
        return L"The browser's data folder is being used by another program. Close other copies of NorthRay Browser and try again.";
    if (hr == E_ACCESSDENIED || hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED))
        return L"Windows didn't allow the browser to use its data folder (%LOCALAPPDATA%\\NorthRay\\Browser).";
    wchar_t code[32];
    swprintf(code, 32, L"0x%08lX", (unsigned long)hr);
    return std::wstring(L"The browser engine couldn't start (") + code + L").";
}

// The oldest runtime NorthRay Browser accepts. Features newer than this are
// asked for with QueryInterface and simply missing on an older runtime.
// (WebView2 1.0.1210 / Edge 101 added the controller options used for
// profiles and private windows.)
const wchar_t kMinimumRuntime[] = L"101.0.1210.39";

wchar_t* dupCoTask(const std::wstring& s) {
    auto* p = static_cast<wchar_t*>(CoTaskMemAlloc((s.size() + 1) * sizeof(wchar_t)));
    if (p) memcpy(p, s.c_str(), (s.size() + 1) * sizeof(wchar_t));
    return p;
}

// The environment's options: no extra command-line switches (nothing that
// weakens the sandbox or security), no single sign-on with the Windows
// account, overlay scroll bars to match NorthRay's no-scroll-bar look.
class EnvironmentOptions final : public ICoreWebView2EnvironmentOptions, public ICoreWebView2EnvironmentOptions8 {
public:
    explicit EnvironmentOptions(std::wstring language) : language_(std::move(language)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ICoreWebView2EnvironmentOptions))
            *out = static_cast<ICoreWebView2EnvironmentOptions*>(this);
        else if (IsEqualIID(riid, IID_ICoreWebView2EnvironmentOptions8))
            *out = static_cast<ICoreWebView2EnvironmentOptions8*>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return (ULONG)r;
    }
    // ICoreWebView2EnvironmentOptions
    HRESULT STDMETHODCALLTYPE get_AdditionalBrowserArguments(LPWSTR* v) override { *v = dupCoTask(L""); return S_OK; }
    HRESULT STDMETHODCALLTYPE put_AdditionalBrowserArguments(LPCWSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_Language(LPWSTR* v) override { *v = dupCoTask(language_); return S_OK; }
    HRESULT STDMETHODCALLTYPE put_Language(LPCWSTR v) override { language_ = v ? v : L""; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_TargetCompatibleBrowserVersion(LPWSTR* v) override { *v = dupCoTask(kMinimumRuntime); return S_OK; }
    HRESULT STDMETHODCALLTYPE put_TargetCompatibleBrowserVersion(LPCWSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_AllowSingleSignOnUsingOSPrimaryAccount(BOOL* v) override { *v = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE put_AllowSingleSignOnUsingOSPrimaryAccount(BOOL) override { return E_NOTIMPL; }
    // ICoreWebView2EnvironmentOptions8
    HRESULT STDMETHODCALLTYPE get_ScrollBarStyle(COREWEBVIEW2_SCROLLBAR_STYLE* v) override {
        *v = COREWEBVIEW2_SCROLLBAR_STYLE_FLUENT_OVERLAY;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_ScrollBarStyle(COREWEBVIEW2_SCROLLBAR_STYLE) override { return E_NOTIMPL; }

private:
    std::wstring language_;
    LONG refs_ = 1;
};

// Decodes a PNG stream (a site's icon) to straight RGBA.
bool decodePng(IStream* stream, int& w, int& h, std::vector<unsigned char>& rgba) {
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                                reinterpret_cast<void**>(wic.put()))))
        return false;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, dec.put()))) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, frame.put()))) return false;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(conv.put()))) return false;
    if (FAILED(conv->Initialize(frame.get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)))
        return false;
    UINT uw = 0, uh = 0;
    conv->GetSize(&uw, &uh);
    if (uw == 0 || uh == 0 || uw > 512 || uh > 512) return false;
    rgba.resize((size_t)uw * uh * 4);
    if (FAILED(conv->CopyPixels(nullptr, uw * 4, (UINT)rgba.size(), rgba.data()))) return false;
    w = (int)uw;
    h = (int)uh;
    return true;
}

COREWEBVIEW2_COLOR toColor(COLORREF c) { return {255, GetRValue(c), GetGValue(c), GetBValue(c)}; }

bool keyDown(int vk) { return (GetKeyState(vk) & 0x8000) != 0; }

}  // namespace

// ---------------------------------------------------------------- Engine

Engine::Engine() = default;

Engine::~Engine() {
    env_.reset();
    if (loader_) FreeLibrary(loader_);
}

void Engine::start(const EngineOptions& options, std::function<void(bool, const std::wstring&)> done) {
    loader_ = LoadLibraryExW(options.loaderPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!loader_) {
        done(false, L"WebView2Loader.dll is missing from NorthRay Browser's folder. Reinstall NorthRay Browser.");
        return;
    }
    using CreateEnvFn = HRESULT(STDAPICALLTYPE*)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*,
                                                ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
    auto createEnv = reinterpret_cast<CreateEnvFn>(
        reinterpret_cast<void*>(GetProcAddress(loader_, "CreateCoreWebView2EnvironmentWithOptions")));
    if (!createEnv) {
        done(false, L"WebView2Loader.dll in NorthRay Browser's folder is damaged. Reinstall NorthRay Browser.");
        return;
    }
    auto opts = ComPtr<ICoreWebView2EnvironmentOptions>::adopt(new EnvironmentOptions(options.language));
    auto handler = makeHandler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, HRESULT, ICoreWebView2Environment*>(
        [this, done](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(hr) || !env) {
                done(false, describeStartFailure(FAILED(hr) ? hr : E_FAIL));
                return S_OK;
            }
            env_ = ComPtr<ICoreWebView2Environment>::share(env);
            LPWSTR v = nullptr;
            if (SUCCEEDED(env->get_BrowserVersionString(&v))) version_ = takeString(v);
            done(true, L"");
            return S_OK;
        });
    const HRESULT hr = createEnv(nullptr, options.userDataFolder.c_str(), opts.get(), handler.get());
    if (FAILED(hr)) done(false, describeStartFailure(hr));
}

void Engine::shutdown(std::function<void()> exited) {
    if (!env_) { if (exited) exited(); return; }
    auto env5 = env_.as<ICoreWebView2Environment5>();
    if (!env5) { env_.reset(); if (exited) exited(); return; }
    auto fired = std::make_shared<bool>(false);
    auto handler = makeHandler<ICoreWebView2BrowserProcessExitedEventHandler, ICoreWebView2Environment*,
                               ICoreWebView2BrowserProcessExitedEventArgs*>(
        [exited, fired](ICoreWebView2Environment*, ICoreWebView2BrowserProcessExitedEventArgs*) -> HRESULT {
            if (!*fired) { *fired = true; if (exited) exited(); }
            return S_OK;
        });
    EventRegistrationToken token{};
    env5->add_BrowserProcessExited(handler.get(), &token);
    // The environment is released when the app quits (Engine's destructor).
}

// ---------------------------------------------------------------- Page

Page::Page(Engine& engine, HWND parent, Options options, PageEvents events)
    : engine_(engine), parent_(parent), options_(std::move(options)), events_(std::move(events)) {}

Page::~Page() {
    close();
    *alive_ = false;
}

void Page::create() {
    ICoreWebView2Environment* env = engine_.environment();
    if (!env) { if (events_.createFailed) events_.createFailed(false, L"The browser engine isn't running."); return; }
    std::weak_ptr<bool> alive = alive_;
    auto handler = makeHandler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, HRESULT, ICoreWebView2Controller*>(
        [this, alive](HRESULT hr, ICoreWebView2Controller* c) -> HRESULT {
            auto a = alive.lock();
            if (!a || !*a) { if (c) c->Close(); return S_OK; }
            if (FAILED(hr) || !c) {
                if (events_.createFailed) events_.createFailed(false, L"The page couldn't be created.");
                return S_OK;
            }
            attach(ComPtr<ICoreWebView2Controller>::share(c));
            return S_OK;
        });
    auto env10 = ComPtr<ICoreWebView2Environment>::share(env).as<ICoreWebView2Environment10>();
    ComPtr<ICoreWebView2ControllerOptions> opts;
    if (env10 && SUCCEEDED(env10->CreateCoreWebView2ControllerOptions(opts.put())) && opts) {
        opts->put_ProfileName(options_.profile.c_str());
        opts->put_IsInPrivateModeEnabled(options_.inPrivate ? TRUE : FALSE);
        env10->CreateCoreWebView2ControllerWithOptions(parent_, opts.get(), handler.get());
    } else if (options_.inPrivate) {
        // Never fall back to an ordinary (remembering) page for a private window.
        if (events_.createFailed)
            events_.createFailed(false, L"Private windows need a newer Microsoft Edge WebView2 Runtime. Update Windows and try again.");
    } else {
        env->CreateCoreWebView2Controller(parent_, handler.get());
    }
}

void Page::createForNewWindow(ICoreWebView2NewWindowRequestedEventArgs* args) {
    ComPtr<ICoreWebView2Deferral> deferral;
    args->GetDeferral(deferral.put());
    auto heldArgs = ComPtr<ICoreWebView2NewWindowRequestedEventArgs>::share(args);
    args->put_Handled(TRUE);
    std::weak_ptr<bool> alive = alive_;
    auto previousReady = events_.ready;
    events_.ready = [this, alive, heldArgs, deferral, previousReady]() mutable {
        if (heldArgs) {
            heldArgs->put_NewWindow(web_.get());
            if (deferral) deferral->Complete();
            heldArgs.reset();
            deferral.reset();
        }
        if (previousReady) previousReady();
    };
    auto previousFailed = events_.createFailed;
    events_.createFailed = [heldArgs, deferral, previousFailed](bool ok, const std::wstring& why) mutable {
        if (deferral) deferral->Complete();   // the opener gets no window; nothing else breaks
        heldArgs.reset();
        deferral.reset();
        if (previousFailed) previousFailed(ok, why);
    };
    create();
}

void Page::attach(ComPtr<ICoreWebView2Controller> controller) {
    controller_ = std::move(controller);
    if (FAILED(controller_->get_CoreWebView2(web_.put())) || !web_) {
        if (events_.createFailed) events_.createFailed(false, L"The page couldn't be created.");
        return;
    }
    if (auto c2 = controller_.as<ICoreWebView2Controller2>()) c2->put_DefaultBackgroundColor(toColor(options_.background));

    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(web_->get_Settings(settings.put())) && settings) {
        // Web pages get no channel into the app: no host objects, no web
        // messages. (Internal NorthRay pages will get a separate, origin-checked
        // channel when they exist.)
        settings->put_AreHostObjectsAllowed(FALSE);
        settings->put_IsWebMessageEnabled(FALSE);
        settings->put_IsScriptEnabled(TRUE);
        settings->put_AreDefaultScriptDialogsEnabled(TRUE);
        settings->put_AreDefaultContextMenusEnabled(TRUE);
        settings->put_AreDevToolsEnabled(TRUE);
        settings->put_IsZoomControlEnabled(TRUE);
        settings->put_IsStatusBarEnabled(FALSE);        // NorthRay's own status bar shows link targets
        settings->put_IsBuiltInErrorPageEnabled(FALSE); // NorthRay draws its own error pages
        if (auto s3 = settings.as<ICoreWebView2Settings3>()) s3->put_AreBrowserAcceleratorKeysEnabled(TRUE);
        // Microsoft Defender SmartScreen checks pages and downloads against
        // Microsoft's list of dangerous sites (see docs: it sends addresses to Microsoft).
        if (auto s8 = settings.as<ICoreWebView2Settings8>()) s8->put_IsReputationCheckingRequired(TRUE);
    }
    // No saved passwords or form data until NorthRay's credential storage is designed.
    if (auto w13 = web_.as<ICoreWebView2_13>()) {
        ComPtr<ICoreWebView2Profile> profile;
        if (SUCCEEDED(w13->get_Profile(profile.put())) && profile) {
            if (auto p6 = profile.as<ICoreWebView2Profile6>()) {
                p6->put_IsPasswordAutosaveEnabled(FALSE);
                p6->put_IsGeneralAutofillEnabled(FALSE);
            }
        }
    }
    wireEvents();
    if (bounds_.right > bounds_.left) controller_->put_Bounds(bounds_);
    controller_->put_IsVisible(visible_ ? TRUE : FALSE);
    if (events_.ready) events_.ready();
    if (!pendingUrl_.empty()) {
        const std::wstring u = std::move(pendingUrl_);
        pendingUrl_.clear();
        navigate(u);
    }
}

void Page::wireEvents() {
    std::weak_ptr<bool> alive = alive_;
    EventRegistrationToken t{};
    auto live = [alive] { auto a = alive.lock(); return a && *a; };

    auto starting = makeHandler<ICoreWebView2NavigationStartingEventHandler, ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*>(
        [this, live](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* a) -> HRESULT {
            if (!live()) return S_OK;
            LPWSTR uri = nullptr;
            a->get_Uri(&uri);
            const std::wstring u = takeString(uri);
            loading_ = true;
            certFailure_ = 0;
            navigatingTo_ = u;
            if (events_.navigationStarting) events_.navigationStarting(u);
            refreshState();
            return S_OK;
        });
    web_->add_NavigationStarting(starting.get(), &t);

    auto completed = makeHandler<ICoreWebView2NavigationCompletedEventHandler, ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*>(
        [this, live](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* a) -> HRESULT {
            if (!live()) return S_OK;
            loading_ = false;
            BOOL ok = TRUE;
            a->get_IsSuccess(&ok);
            if (!ok) {
                COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                a->get_WebErrorStatus(&status);
                // A refused certificate reaches here as "aborted"; report the real reason.
                const int failure = certFailure_ ? certFailure_ : (int)status;
                certFailure_ = 0;
                // The address that failed: what was being opened (a first navigation
                // never commits, so the view's source is still about:blank).
                std::wstring failed = navigatingTo_;
                if (failed.empty()) {
                    LPWSTR src = nullptr;
                    web_->get_Source(&src);
                    failed = takeString(src);
                }
                if (events_.loadFailed) events_.loadFailed(failure, failed);
            }
            refreshState();
            return S_OK;
        });
    web_->add_NavigationCompleted(completed.get(), &t);

    auto source = makeHandler<ICoreWebView2SourceChangedEventHandler, ICoreWebView2*, ICoreWebView2SourceChangedEventArgs*>(
        [this, live](ICoreWebView2*, ICoreWebView2SourceChangedEventArgs*) -> HRESULT {
            if (live()) refreshState();
            return S_OK;
        });
    web_->add_SourceChanged(source.get(), &t);

    auto history = makeHandler<ICoreWebView2HistoryChangedEventHandler, ICoreWebView2*, IUnknown*>(
        [this, live](ICoreWebView2*, IUnknown*) -> HRESULT {
            if (live()) refreshState();
            return S_OK;
        });
    web_->add_HistoryChanged(history.get(), &t);

    auto title = makeHandler<ICoreWebView2DocumentTitleChangedEventHandler, ICoreWebView2*, IUnknown*>(
        [this, live](ICoreWebView2*, IUnknown*) -> HRESULT {
            if (live()) refreshState();
            return S_OK;
        });
    web_->add_DocumentTitleChanged(title.get(), &t);

    auto newWin = makeHandler<ICoreWebView2NewWindowRequestedEventHandler, ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs*>(
        [this, live](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* a) -> HRESULT {
            if (live() && events_.newWindow) events_.newWindow(a);
            return S_OK;
        });
    web_->add_NewWindowRequested(newWin.get(), &t);

    auto closeReq = makeHandler<ICoreWebView2WindowCloseRequestedEventHandler, ICoreWebView2*, IUnknown*>(
        [this, live](ICoreWebView2*, IUnknown*) -> HRESULT {
            if (live() && events_.closeRequested) events_.closeRequested();
            return S_OK;
        });
    web_->add_WindowCloseRequested(closeReq.get(), &t);

    auto full = makeHandler<ICoreWebView2ContainsFullScreenElementChangedEventHandler, ICoreWebView2*, IUnknown*>(
        [this, live](ICoreWebView2* w, IUnknown*) -> HRESULT {
            if (!live()) return S_OK;
            BOOL on = FALSE;
            w->get_ContainsFullScreenElement(&on);
            if (events_.fullscreen) events_.fullscreen(on != FALSE);
            return S_OK;
        });
    web_->add_ContainsFullScreenElementChanged(full.get(), &t);

    auto failed = makeHandler<ICoreWebView2ProcessFailedEventHandler, ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*>(
        [this, live](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* a) -> HRESULT {
            if (!live()) return S_OK;
            COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED;
            a->get_ProcessFailedKind(&kind);
            // GPU and helper processes are restarted by the engine itself.
            if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED ||
                kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED) {
                loading_ = false;
                if (events_.processFailed) events_.processFailed(true);
            } else if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE) {
                if (events_.processFailed) events_.processFailed(false);
            }
            return S_OK;
        });
    web_->add_ProcessFailed(failed.get(), &t);

    if (auto w14 = web_.as<ICoreWebView2_14>()) {
        // A bad certificate is always refused (no "continue anyway" in this
        // version); remember why, so the error page can say so plainly.
        auto cert = makeHandler<ICoreWebView2ServerCertificateErrorDetectedEventHandler, ICoreWebView2*,
                                ICoreWebView2ServerCertificateErrorDetectedEventArgs*>(
            [this, live](ICoreWebView2*, ICoreWebView2ServerCertificateErrorDetectedEventArgs* a) -> HRESULT {
                a->put_Action(COREWEBVIEW2_SERVER_CERTIFICATE_ERROR_ACTION_CANCEL);
                if (!live()) return S_OK;
                COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_IS_INVALID;
                a->get_ErrorStatus(&status);
                certFailure_ = (int)status;
                return S_OK;
            });
        w14->add_ServerCertificateErrorDetected(cert.get(), &t);
    }

    if (auto w12 = web_.as<ICoreWebView2_12>()) {
        auto status = makeHandler<ICoreWebView2StatusBarTextChangedEventHandler, ICoreWebView2*, IUnknown*>(
            [this, live, w12](ICoreWebView2*, IUnknown*) -> HRESULT {
                if (!live()) return S_OK;
                LPWSTR s = nullptr;
                w12->get_StatusBarText(&s);
                if (events_.statusText) events_.statusText(takeString(s));
                return S_OK;
            });
        w12->add_StatusBarTextChanged(status.get(), &t);
    }

    if (auto w15 = web_.as<ICoreWebView2_15>()) {
        auto fav = makeHandler<ICoreWebView2FaviconChangedEventHandler, ICoreWebView2*, IUnknown*>(
            [this, live](ICoreWebView2*, IUnknown*) -> HRESULT {
                if (live()) requestFavicon();
                return S_OK;
            });
        w15->add_FaviconChanged(fav.get(), &t);
    }

    auto keys = makeHandler<ICoreWebView2AcceleratorKeyPressedEventHandler, ICoreWebView2Controller*,
                            ICoreWebView2AcceleratorKeyPressedEventArgs*>(
        [this, live](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* a) -> HRESULT {
            if (!live() || !events_.key) return S_OK;
            COREWEBVIEW2_KEY_EVENT_KIND kind = COREWEBVIEW2_KEY_EVENT_KIND_KEY_UP;
            a->get_KeyEventKind(&kind);
            if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN && kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN) return S_OK;
            UINT vk = 0;
            a->get_VirtualKey(&vk);
            if (events_.key(vk, keyDown(VK_CONTROL), keyDown(VK_SHIFT), keyDown(VK_MENU))) a->put_Handled(TRUE);
            return S_OK;
        });
    controller_->add_AcceleratorKeyPressed(keys.get(), &t);

    auto focus = makeHandler<ICoreWebView2FocusChangedEventHandler, ICoreWebView2Controller*, IUnknown*>(
        [this, live](ICoreWebView2Controller*, IUnknown*) -> HRESULT {
            if (live() && events_.gotFocus) events_.gotFocus();
            return S_OK;
        });
    controller_->add_GotFocus(focus.get(), &t);
}

void Page::refreshState() {
    if (!web_) return;
    LPWSTR s = nullptr;
    if (SUCCEEDED(web_->get_Source(&s))) url_ = takeString(s);
    s = nullptr;
    if (SUCCEEDED(web_->get_DocumentTitle(&s))) title_ = takeString(s);
    BOOL b = FALSE;
    web_->get_CanGoBack(&b);
    canBack_ = b != FALSE;
    b = FALSE;
    web_->get_CanGoForward(&b);
    canForward_ = b != FALSE;
    if (events_.changed) events_.changed();
}

void Page::requestFavicon() {
    auto w15 = web_.as<ICoreWebView2_15>();
    if (!w15) return;
    std::weak_ptr<bool> alive = alive_;
    auto done = makeHandler<ICoreWebView2GetFaviconCompletedHandler, HRESULT, IStream*>(
        [this, alive](HRESULT hr, IStream* stream) -> HRESULT {
            auto a = alive.lock();
            if (!a || !*a || !events_.favicon) return S_OK;
            int w = 0, h = 0;
            std::vector<unsigned char> rgba;
            if (FAILED(hr) || !stream || !decodePng(stream, w, h, rgba)) rgba.clear();
            events_.favicon(w, h, std::move(rgba));
            return S_OK;
        });
    w15->GetFavicon(COREWEBVIEW2_FAVICON_IMAGE_FORMAT_PNG, done.get());
}

void Page::navigate(const std::wstring& url) {
    if (!web_) { pendingUrl_ = url; return; }
    web_->Navigate(url.c_str());
}

void Page::back() { if (web_) web_->GoBack(); }
void Page::forward() { if (web_) web_->GoForward(); }

void Page::reload(bool bypassCache) {
    if (!web_) return;
    if (!bypassCache) { web_->Reload(); return; }
    auto done = makeHandler<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler, HRESULT, LPCWSTR>(
        [](HRESULT, LPCWSTR) -> HRESULT { return S_OK; });
    web_->CallDevToolsProtocolMethod(L"Page.reload", L"{\"ignoreCache\":true}", done.get());
}

void Page::stop() { if (web_) web_->Stop(); }

void Page::print() {
    if (auto w16 = web_.as<ICoreWebView2_16>()) w16->ShowPrintUI(COREWEBVIEW2_PRINT_DIALOG_KIND_BROWSER);
}

void Page::openDevTools() { if (web_) web_->OpenDevToolsWindow(); }

void Page::setBounds(const RECT& r) {
    if (memcmp(&r, &bounds_, sizeof r) == 0) return;
    bounds_ = r;
    if (controller_) controller_->put_Bounds(r);
}

void Page::show(bool visible) {
    if (visible == visible_) return;
    visible_ = visible;
    if (controller_) controller_->put_IsVisible(visible ? TRUE : FALSE);
}

void Page::focus() {
    if (controller_) controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
}

void Page::parentMoved() { if (controller_) controller_->NotifyParentWindowPositionChanged(); }

void Page::setBackground(COLORREF c) {
    options_.background = c;
    if (auto c2 = controller_.as<ICoreWebView2Controller2>()) c2->put_DefaultBackgroundColor(toColor(c));
}

void Page::close() {
    if (closed_) return;
    closed_ = true;
    if (controller_) controller_->Close();
    web_.reset();
    controller_.reset();
}

}  // namespace nrb
