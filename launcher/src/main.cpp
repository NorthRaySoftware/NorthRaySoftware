// NorthRay Launcher entry point.
//   NorthRayLauncher.exe [--app <id>]   opens straight to an app's page
#include <objbase.h>
#include <windows.h>
#include <shellapi.h>

#include "cjui/packages.hpp"
#include "launcher.hpp"

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int show) {
    cjui::installCrashReport(L"NorthRay Launcher", L"Launcher");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::string openApp;
    bool startInTray = false;   // --tray: started with Windows, so no window yet
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(cmdLine && *cmdLine ? cmdLine : L"x", &argc);
        for (int i = 0; argv && i < argc; ++i) {
            if (i + 1 < argc && lstrcmpW(argv[i], L"--app") == 0) openApp = cjui::narrow(argv[i + 1]);
            if (lstrcmpW(argv[i], L"--tray") == 0) startInTray = true;
        }
        if (argv) LocalFree(argv);
    }

    // One launcher at a time: bring the existing window forward instead.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\NorthRayLauncher");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(L"NorthRayLauncher", nullptr)) {
            if (!startInTray) {
                // It may be hidden in the tray: show it.
                ShowWindow(existing, IsIconic(existing) ? SW_RESTORE : SW_SHOW);
                SetForegroundWindow(existing);
            }
        }
        return 0;
    }
    void* running = cjui::markAppRunning("launcher");

    int code = 0;
    {
        nrl::Launcher launcher;
        if (!launcher.create(inst, show, openApp, startInTray)) {
            MessageBoxW(nullptr, L"NorthRay Launcher couldn't create its window. Your graphics driver may not support Direct2D.",
                        L"NorthRay Launcher", MB_ICONERROR);
            code = 1;
        } else {
            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            code = (int)msg.wParam;
        }
    }
    if (running) CloseHandle(running);
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return code;
}
