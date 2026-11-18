// NorthRay Capture: start-up and the message loop. One window per user session: a second launch
// brings the running one forward.
#include "capture_window.hpp"
#include "log.hpp"
#include "settings.hpp"
#include "system.hpp"
#include "version.hpp"

#include "cjui/packages.hpp"

#include <objbase.h>
#include <shlobj.h>

#include <string>

namespace {

std::wstring stateFolder() {
    std::wstring dir;
    PWSTR local = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) dir = std::wstring(local) + L"\\NorthRay\\Capture";
    CoTaskMemFree(local);
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    cjui::installCrashReport(L"NorthRay Capture", L"Capture");
    cjui::setCrashReopenNote(L"Your scenes are saved as you change them, so they'll open as they were.");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return 1;

    // Only one Capture at a time: two would fight over devices and encoders.
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\NorthRayCapture.Running");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND w = FindWindowW(ncap::CaptureWindow::kClassName, nullptr)) {
            if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
            SetForegroundWindow(w);
        }
        CloseHandle(single);
        CoUninitialize();
        return 0;
    }

    const std::wstring state = stateFolder();
    ncap::log::start(state + L"\\logs");
    ncap::log::info("NorthRay Capture " CJ_VERSION_STR " starting on " + narrow(ncap::sys::windowsVersion()) + ", " +
                    narrow(ncap::sys::cpuName()) + ", " + narrow(ncap::sys::gpuNames()));

    const std::wstring settingsPath = state + L"\\settings.json";
    ncap::Settings settings;
    if (!settings.load(settingsPath)) ncap::log::info("No settings yet (first start): using the defaults");
    if (settings.readOnly) ncap::log::warn("settings.json is from a newer version: it's read but won't be overwritten");

    int code = 0;
    {
        ncap::CaptureWindow window(settings, settingsPath, state);
        if (!window.create(inst, show)) {
            ncap::log::error("The window couldn't be created (Direct2D unavailable?)");
            MessageBoxW(nullptr, L"NorthRay Capture couldn't create its window. Your graphics driver may not support Direct2D.",
                        L"NorthRay Capture", MB_ICONERROR);
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
    ncap::log::info("NorthRay Capture closed normally");
    if (single) CloseHandle(single);
    CoUninitialize();
    return code;
}
