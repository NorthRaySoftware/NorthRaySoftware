# Microsoft Edge WebView2 SDK 1.0.4314.35 (subset)

From the official NuGet package [Microsoft.Web.WebView2 1.0.4314.35](https://www.nuget.org/packages/Microsoft.Web.WebView2/1.0.4314.35)
(BSD-3-Clause, see `LICENSE.txt` and `NOTICE.txt`). Used by NorthRay Browser. The Writing Studio and CJ Radio still use
the older subset in `../webview2` (1.0.961.33); they can move to this one when they next need a newer WebView2 feature.

| File | From the package | Notes |
|---|---|---|
| `WebView2.h` | `build/native/include/WebView2.h` | Microsoft's MIDL-generated header. One change: `#include "EventToken.h"` → `#include <eventtoken.h>` (mingw-w64's spelling; Windows' file system ignores case, Linux's doesn't). |
| `WebView2MinGW.h` | generated | `__CRT_UUID_DECL` for each of the header's 297 interfaces, so MinGW's `__uuidof()` works. Made by `tools/webview2-uuids.py`; include it instead of `WebView2.h`. Harmless under MSVC. |
| `WebView2Loader.dll` | `build/native/x64/WebView2Loader.dll` | x64, Authenticode-signed by Microsoft Corporation (Microsoft Code Signing PCA 2024). SHA-256 `d3fce90bdeecb08596e377dea6a316354dd8949e32ff09b89d51be00c14e21b6`. Loaded at run time; ships next to the app. |

Why not `widl`, as `../webview2` did: this IDL declares some derived interfaces before their bases (MIDL reorders
them, widl doesn't), so a widl-generated header doesn't compile. Microsoft's own header does, with MinGW and MSVC.

To update: download the new `.nupkg` from nuget.org, copy the three files above, redo the one-line include change,
run `tools/webview2-uuids.py`, update the version and hash here and in `apps/browser/docs/ARCHITECTURE.md`.

The WebView2 Runtime itself (the engine, `msedgewebview2.exe`) isn't shipped: it's part of Windows 11 and is
kept up to date by Microsoft (the Evergreen runtime).
