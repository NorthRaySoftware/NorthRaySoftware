// CJ Radio entry point. Runs on its own: closing CJ Community doesn't stop it.
#include <objbase.h>
#include <windows.h>

#include "cjui/packages.hpp"
#include "radio_app.hpp"

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    cjui::installCrashReport(L"CJ Radio", L"CJ Radio");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // One CJ Radio at a time: a second launch brings the first window forward.
    if (cjui::isAppRunning("cj-radio")) {
        if (HWND existing = FindWindowW(L"CJRadioWindow", nullptr)) {
            if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        return 0;
    }
    void* running = cjui::markAppRunning("cj-radio");

    int code = 0;
    {
        cjr::RadioApp app;
        if (!app.create(inst, show)) {
            MessageBoxW(nullptr, L"CJ Radio couldn't create its window. Your graphics driver may not support Direct2D.",
                        L"CJ Radio", MB_ICONERROR);
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
    return code;
}
