// NorthRay Browser: start-up, one window per user session, the message loop.
#include "browser_window.hpp"
#include "engine.hpp"
#include "settings.hpp"

#include "cjui/packages.hpp"
#include "nrb/backup.hpp"

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

namespace {

std::wstring exeFolder() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring s(buf, n);
    const size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? s : s.substr(0, slash);
}

std::wstring stateFolder() {
    std::wstring dir;
    PWSTR local = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) dir = std::wstring(local) + L"\\NorthRay\\Browser";
    CoTaskMemFree(local);
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir;
}

// Development only: "--no-engine" leaves the engine off, to check the
// interface where WebView2 can't run (screenshots under Wine). Shown in the
// status bar whenever it's on.
bool hasSwitch(const wchar_t* name) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool found = false;
    for (int i = 1; argv && i < argc; ++i)
        if (std::wstring(argv[i]) == name) found = true;
    if (argv) LocalFree(argv);
    return found;
}

std::vector<std::wstring> commandLineAddresses() {
    std::vector<std::wstring> out;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        const std::wstring a = argv[i];
        if (!a.empty() && a[0] != L'-') out.push_back(a);   // switches are ignored, never passed to the engine
    }
    if (argv) LocalFree(argv);
    return out;
}

// A second launch hands its addresses to the running window and exits.
bool forwardToRunningWindow(const std::vector<std::wstring>& args) {
    HWND existing = FindWindowW(nrb::BrowserWindow::kClassName, nullptr);
    if (!existing) return false;
    std::wstring all;
    for (const std::wstring& a : args) all += a + L"\n";
    COPYDATASTRUCT cd{0x4E524231, (DWORD)(all.size() * sizeof(wchar_t)), all.empty() ? nullptr : all.data()};
    DWORD_PTR result = 0;
    AllowSetForegroundWindow(ASFW_ANY);
    return SendMessageTimeoutW(existing, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cd), SMTO_ABORTIFHUNG, 5000, &result) != 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    cjui::installCrashReport(L"NorthRay Browser", L"Browser");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;

    const std::vector<std::wstring> args = commandLineAddresses();
    if (hasSwitch(L"--after-restore")) {
        // Restarted to finish a restore: wait for the old window to close (it saves as it goes).
        for (int i = 0; i < 100 && FindWindowW(nrb::BrowserWindow::kClassName, nullptr); ++i) Sleep(100);
    }
    if (cjui::isAppRunning("browser") && !hasSwitch(L"--after-restore") && forwardToRunningWindow(args)) {
        CoUninitialize();
        return 0;
    }
    void* running = cjui::markAppRunning("browser");

    const std::wstring state = stateFolder();
    // A restore that's waiting is finished before anything is opened.
    std::wstring restoreNotice;
    if (nrb::backup::pending(std::filesystem::path(state))) {
        std::time_t now = std::time(nullptr);
        std::tm t{};
        localtime_s(&t, &now);
        char stamp[32];
        std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H%M", &t);
        std::string error;
        std::filesystem::path kept;
        if (nrb::backup::applyPending(std::filesystem::path(state), error, &kept, stamp))
            restoreNotice = L"Restored from the backup." + std::wstring(kept.empty() ? L"" : L" What it replaced is in \u201C" + kept.filename().wstring() + L"\u201D.");
        else
            MessageBoxW(nullptr, std::wstring(error.begin(), error.end()).c_str(), L"NorthRay Browser", MB_ICONWARNING);
    }
    const std::wstring settingsPath = state + L"\\settings.json";
    nrb::Settings settings;
    settings.load(settingsPath);

    int code = 0;
    {
        nrb::Engine engine;
        nrb::BrowserWindow window(engine, settings, settingsPath);
        if (!window.create(inst, show)) {
            MessageBoxW(nullptr, L"NorthRay Browser couldn't create its window. Your graphics driver may not support Direct2D.",
                        L"NorthRay Browser", MB_ICONERROR);
            code = 1;
        } else {
            if (hasSwitch(L"--no-engine")) window.setEngineOffForDevelopment();
            if (!restoreNotice.empty()) window.notice(restoreNotice, 12);
            nrb::EngineOptions eo;
            // Browsing data (cache, cookies, site storage) lives apart from
            // anything the person makes, so clearing it can't touch their work.
            eo.userDataFolder = state + L"\\Engine";
            eo.loaderPath = exeFolder() + L"\\WebView2Loader.dll";
            if (!hasSwitch(L"--no-engine")) engine.start(eo, [&window, args](bool ok, const std::wstring& problem) {
                window.engineStarted(ok, problem);
                if (ok && !args.empty()) window.openFromCommandLine(args);
            });
            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                if (window.preTranslate(msg)) continue;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            code = (int)msg.wParam;
        }
    }
    if (running) CloseHandle(running);
    CoUninitialize();
    return code;
}
