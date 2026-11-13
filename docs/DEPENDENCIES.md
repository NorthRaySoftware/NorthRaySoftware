# Dependencies

## Launcher (Windows)

| Dependency | Version | Licence | Why |
|---|---|---|---|
| Windows 10 1607+ / Windows 11 | — | — | Per-monitor DPI v2 APIs |
| Direct2D, DirectWrite, DWM, Shell | system | — | Drawing, text, dark title bar, known folders |
| nlohmann/json | 3.11.3 (vendored single header) | MIT | Reading metadata and settings |
| miniaudio | 0.11.25 (vendored single header) | Public domain or MIT No Attribution | Audio output and MP3 decoding in CJ Radio |
| Microsoft Edge WebView2 SDK | 1.0.961.33 (loader DLL + header generated from its IDL) | BSD 3-Clause | Shows Twitch's embedded player in CJ Radio. Needs the WebView2 Runtime, included with Windows 10 and 11 |
| WinHTTP, BCrypt | system | — | HTTPS requests and streams; SHA-256 for package checks |

Build tools: CMake ≥ 3.21 and either MSVC (Visual Studio 2022) or MinGW-w64 GCC 13. MinGW builds link the runtime statically, so the .exe needs no extra DLLs.

## Website

| Dependency | Version | Licence |
|---|---|---|
| Node.js | ≥ 22 | — |
| Astro | 7.4.2 (pinned) | MIT |

## Development-only

- Wine and Xvfb were used to run the Windows build and take screenshots during testing on Linux. They're not needed on Windows.
