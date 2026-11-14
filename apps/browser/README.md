# NorthRay Browser

A desktop web browser for Windows with built-in creative tools (to come), in the NorthRay Software design language.
Version 0.1.0 (Stage 1: technical foundation). Architecture, decisions and test results: `docs/ARCHITECTURE.md`.

Web pages are shown by Microsoft Edge WebView2 (Chromium), which comes with Windows 11 and is kept up to date by
Windows. NorthRay Browser itself is the window, tabs, address bar and everything around the page.

## Build

It's part of the monorepo and builds with the other apps.

From Linux (MinGW-w64, as `tools/build-windows.sh` does):

```
cmake -S . -B build/windows -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
ninja -C build/windows NorthRayBrowser browsercore_tests
```

On Windows (Visual Studio 2022, "Desktop development with C++"):

```
cmake -S . -B build/vs -G "Visual Studio 17 2022" -A x64
cmake --build build/vs --config Release --target NorthRayBrowser browsercore_tests
```

The portable core and its tests also build natively anywhere: `cmake -S . -B build/native && cmake --build build/native --target browsercore_tests`.

## Run

`NorthRayBrowser.exe` needs `WebView2Loader.dll` beside it (the build copies it) and the WebView2 Runtime (part of
Windows 11). Settings and browsing data: `%LOCALAPPDATA%\NorthRay\Browser`. Addresses on the command line open in tabs;
a second launch hands them to the running window.

Development switch: `--no-engine` starts without the engine, to check the interface where WebView2 can't run (e.g.
under Wine). The status bar says when it's on.

## Keyboard

Ctrl+L / Alt+D / F6 address bar · Ctrl+T new tab · Ctrl+W close tab · Ctrl+Shift+T reopen · Ctrl+Tab / Ctrl+PgDn next
tab · Ctrl+1–9 tab by position · Alt+Left / Alt+Right back / forward · F5 / Ctrl+R reload (Ctrl+F5 bypasses the cache) ·
Alt+Home start page · F11 full screen · Ctrl+P print · F12 developer tools · Esc stop loading. Middle-click a tab to close it.

## Licences

WebView2 SDK (BSD-3-Clause) and nlohmann/json (MIT): `licenses/`. The engine's own licences: `edge://credits`.
