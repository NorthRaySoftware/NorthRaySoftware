// NorthRay Files: start-up and the message loop. Each launch opens its own
// window (as File Explorer does); a folder or file path on the command line
// opens there.
#include "files_window.hpp"
#include "settings.hpp"

#include "cjui/packages.hpp"

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <string>

namespace {

std::wstring stateFolder() {
    std::wstring dir;
    PWSTR local = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) dir = std::wstring(local) + L"\\NorthRay\\Files";
    CoTaskMemFree(local);
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir;
}

std::wstring commandLinePath() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring out;
    for (int i = 1; argv && i < argc; ++i) {
        const std::wstring a = argv[i];
        if (!a.empty() && a[0] != L'-') { out = a; break; }
    }
    if (argv) LocalFree(argv);
    return out;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    cjui::installCrashReport(L"NorthRay Files", L"Files");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(OleInitialize(nullptr))) return 1;   // COM, and OLE for drag and drop with other programs
    // No "insert a disk" or "drive not ready" dialogs when a drive is empty: the window says so instead.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);

    const std::wstring settingsPath = stateFolder() + L"\\settings.json";
    nrf::Settings settings;
    settings.load(settingsPath);

    int code = 0;
    {
        nrf::FilesWindow window(settings, settingsPath);
        if (!window.create(inst, show, commandLinePath())) {
            MessageBoxW(nullptr, L"NorthRay Files couldn't create its window. Your graphics driver may not support Direct2D.",
                        L"NorthRay Files", MB_ICONERROR);
            code = 1;
        } else {
            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                if (window.preTranslate(msg)) continue;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            code = (int)msg.wParam;
        }
    }
    OleUninitialize();
    return code;
}
