# Microsoft Edge WebView2 SDK (subset)

From the official NuGet package Microsoft.Web.WebView2 1.0.961.33 (BSD-3-Clause, see LICENSE.txt):

- `WebView2Loader.dll` (x64): Microsoft-signed, shipped next to CJRadio.exe.
- `WebView2.idl`: the interface definitions.
- `WebView2.h`: generated from `WebView2.idl` with mingw-w64's `widl`
  (`x86_64-w64-mingw32-widl -h -I<mingw-w64-headers/include> WebView2.idl`), so MinGW and MSVC builds share it.

CJ Radio uses WebView2 only to show Twitch's own embedded player. The WebView2 Runtime itself comes with Windows 10 and 11.
