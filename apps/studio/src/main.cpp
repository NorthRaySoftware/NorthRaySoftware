// NorthRay Writing Studio entry point. Runs on its own; the launcher is optional.
#include <objbase.h>
#include <windows.h>

#include "cjui/packages.hpp"
#include "studio/import.hpp"
#include "studio_app.hpp"
#include "version.hpp"

#include <shellapi.h>
#include <shlobj.h>

#include <cstdio>
#include <ctime>
#include <exception>
#include <string>

namespace {

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring exeFolder() {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    std::wstring d(buf, n);
    return d.substr(0, d.find_last_of(L"\\/"));
}

// If the Studio ever crashes, note where, so the cause can be found and fixed.
// The note goes to %LOCALAPPDATA%\NorthRay\Writing Studio\crash.log.
std::wstring g_crashLog;
void writeCrash(const char* what, const void* address) {
    if (g_crashLog.empty()) return;
    FILE* f = _wfopen(g_crashLog.c_str(), L"a");
    if (!f) return;
    char when[64];
    std::time_t t = std::time(nullptr);
    std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S UTC", std::gmtime(&t));
    std::fprintf(f, "%s  NorthRay Writing Studio %s  %s", when, CJ_VERSION_STR, what);
    HMODULE mod = nullptr;
    if (address && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                      static_cast<LPCWSTR>(address), &mod) && mod) {
        wchar_t name[MAX_PATH];
        GetModuleFileNameW(mod, name, MAX_PATH);
        std::wstring n = name;
        n = n.substr(n.find_last_of(L"\\/") + 1);
        std::fprintf(f, "  at %s+0x%llx", utf8(n).c_str(),
                     (unsigned long long)(reinterpret_cast<const char*>(address) - reinterpret_cast<const char*>(mod)));
    }
    std::fprintf(f, "\n");
    std::fclose(f);
}

LONG WINAPI onCrash(EXCEPTION_POINTERS* e) {
    char what[64];
    std::snprintf(what, sizeof what, "crash 0x%08lx", e && e->ExceptionRecord ? e->ExceptionRecord->ExceptionCode : 0ul);
    writeCrash(what, e && e->ExceptionRecord ? e->ExceptionRecord->ExceptionAddress : nullptr);
    return EXCEPTION_CONTINUE_SEARCH;
}

void onTerminate() {
    std::string what = "uncaught error";
    if (auto ex = std::current_exception()) {
        try { std::rethrow_exception(ex); }
        catch (const std::exception& e) { what += ": " + std::string(e.what()); }
        catch (...) {}
    }
    writeCrash(what.c_str(), nullptr);
    std::abort();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    // Import helper: reads one file into a plan folder and exits. Running the
    // readers in their own process means a file that crashes one can't take
    // the Studio (and any unsaved work) down with it.
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv && argc >= 4 && std::wstring(argv[1]) == L"--import-worker") {
            SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);  // no crash dialogs from the helper
            studio::import::setPdfiumFolder(utf8(exeFolder()));
            const auto plan = studio::import::readFileSafely(utf8(argv[1 + 1]));
            std::string err;
            const bool saved = studio::import::savePlan(plan, utf8(argv[3]), err);
            LocalFree(argv);
            return saved ? 0 : 3;
        }
        if (argv) LocalFree(argv);
    }

    {
        PWSTR local = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
            g_crashLog = std::wstring(local) + L"\\NorthRay\\Writing Studio";
            SHCreateDirectoryExW(nullptr, g_crashLog.c_str(), nullptr);
            g_crashLog += L"\\crash.log";
        }
        CoTaskMemFree(local);
        SetUnhandledExceptionFilter(onCrash);
        std::set_terminate(onTerminate);
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // One window per library: a second launch brings the first forward.
    if (cjui::isAppRunning("writing-studio")) {
        if (HWND existing = FindWindowW(L"NorthRayWritingStudio", nullptr)) {
            if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        return 0;
    }
    void* running = cjui::markAppRunning("writing-studio");

    int code = 0;
    {
        nrs::StudioApp app;
        if (!app.create(inst, show)) {
            MessageBoxW(nullptr, L"NorthRay Writing Studio couldn't create its window. Your graphics driver may not support Direct2D.",
                        L"NorthRay Writing Studio", MB_ICONERROR);
            code = 1;
        } else {
            app.handleCommandLine();
            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                if (app.preTranslate(msg)) continue;
                if (TranslateAcceleratorW(app.hwnd(), app.accelerators(), &msg)) continue;
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
