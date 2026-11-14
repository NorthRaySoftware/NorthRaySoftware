// NorthRay Capture: what can be captured right now: monitors, application windows and cameras
// (for the Sources panel's "+" menu, and for finding a source's monitor, window or camera again).
#pragma once

#include "ncap/placement.hpp"

#include <windows.h>

#include <string>
#include <vector>

namespace ncap::video {

struct MonitorInfo {
    std::wstring device;     // "\\.\DISPLAY1": what a display source remembers
    std::wstring name;       // "DELL U2415" (from the monitor), or "" when Windows doesn't say
    RECT rect{};             // on the desktop, in physical pixels
    bool primary = false;
    HMONITOR handle = nullptr;
    int number = 0;          // 1, 2, ... in Windows' own order
    std::wstring label() const;   // "Display 1: DELL U2415 (1920×1200, main)"
};
std::vector<MonitorInfo> listMonitors();

struct WindowInfo {
    HWND hwnd = nullptr;
    WindowIdentity id;       // title, class, exe (UTF-8)
    DWORD pid = 0;
};
// Top-level application windows someone could want to capture: visible, not minimised away
// entirely, not cloaked (other virtual desktops, suspended Store apps), not tool windows, not
// NorthRay Capture's own. Sorted by program, then title.
std::vector<WindowInfo> listWindows();
// The window that best matches a remembered identity, or null.
HWND findWindow(const WindowIdentity& wanted);
WindowIdentity identityOf(HWND hwnd);

struct CameraInfo {
    std::wstring name;       // "Logitech BRIO", "Cam Link 4K"
    std::wstring link;       // the device's symbolic link: what a camera source remembers
};
std::vector<CameraInfo> listCameras();   // needs COM; starts and stops Media Foundation itself

}  // namespace ncap::video
