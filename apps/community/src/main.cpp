// CJ Philip Community entry point: an optional NorthRay app.
#include <objbase.h>
#include <windows.h>

#include "hub.hpp"
#include "cjui/packages.hpp"

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    // Per-monitor DPI awareness is also declared in the manifest; this covers
    // the case where the manifest is stripped.
    cjui::installCrashReport(L"CJ Philip Community", L"CJ Philip Community");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // One launcher at a time: bring the existing window forward instead.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\CJPhilipCommunity");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(L"CJPhilipCommunityWindow", nullptr)) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        return 0;
    }

    void* running = cjui::markAppRunning("cj-philip-community");
    (void)running;
    cjp::CommunityApp launcher;
    if (!launcher.create(inst, show)) {
        MessageBoxW(nullptr, L"CJ Philip Community couldn't create its window. Your graphics driver may not support Direct2D.",
                    L"CJ Philip Community", MB_ICONERROR);
        return 1;
    }
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return (int)msg.wParam;
}
