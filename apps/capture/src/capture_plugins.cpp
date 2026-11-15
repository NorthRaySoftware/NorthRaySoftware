// NorthRay Capture's window: Settings › Plugins (M9 part 3). Lists the DLLs in the plugins folder with
// their SHA-256 and signature; none runs until switched on here, after a confirmation that shows both.
#include "capture_window.hpp"

#include "audio/plugin_host.hpp"
#include "log.hpp"

#include "cjui/dialog.hpp"

#include <shellapi.h>

namespace ncap {

using cjui::Align;
using cjui::Font;
using cjui::narrow;
using cjui::Rect;
using cjui::widen;

void CaptureWindow::pluginsChanged() {
    processors_.clear();   // chains with plugins are made again (with or without them)
    syncAudio();
    invalidate();
}

float CaptureWindow::drawPluginSettings(float x, float y0, float colW) {
    PluginHost& host = PluginHost::get();
    float y = y0;
    y += gfx_.text(L"Audio-effect plugins add your own filters to the Filters page. They run inside Capture, so only use ones you trust: "
                   L"none runs until you switch it on here, and one that changes is switched off again.",
                   {x, y, colW, 60}, Font::Small, pal_.inkMuted) + 10;
    button("plg:open", {x, y, 190, 32}, L"Open the folder", [this, &host] {
        ShellExecuteW(hwnd(), L"open", host.folder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    });
    button("plg:scan", {x + 200, y, 150, 32}, L"Look again", [this, &host] {
        host.rescan();
        for (auto& m : host.takeMessages()) flash(widen(m), 8);
        pluginsChanged();
    });
    y += 44;
    const auto& es = host.entries();
    if (es.empty()) {
        y += gfx_.text(L"No plugins in the folder. Put a plugin's .dll there, then Look again. (Making one: sdk/northray_audio_plugin.h in the "
                       L"source, with an example.)",
                       {x, y, colW, 40}, Font::Small, pal_.inkMuted) + 8;
        return y - y0;
    }
    for (const PluginEntry& e : es) {
        const float top = y;
        const std::wstring name = e.loaded && !e.info.name.empty() ? widen(e.info.name) + L"  ·  " + widen(e.file) : widen(e.file);
        gfx_.text(name, {x, y, colW - 70, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false, true, true});
        const std::string file = e.file, sha = e.sha256;
        toggle("plg:on:" + e.file, x + colW - 44, y, e.enabled, [this, &host, file, sha, e] {
            if (e.enabled) {
                host.disable(file);
                flash(L"Switched off " + widen(file) + L". Filters using it now pass sound through.", 6);
                pluginsChanged();
                return;
            }
            if (sha.empty()) {
                flash(L"That file can't be read, so it can't be switched on.", 6);
                return;
            }
            const std::wstring msg = L"Switch on “" + widen(file) + L"”?\n\n" + e.signature + L".\n\nSHA-256:\n" + widen(sha.substr(0, 32)) + L"\n" + widen(sha.substr(32)) +
                                     L"\n\nA plugin runs inside Capture with your permissions: it can read your files and crash Capture. Switch it on only if "
                                     L"you trust where it came from. If the file changes, it's switched off again.";
            if (cjui::messageBox(hwnd(), msg, L"Plugins", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) return;
            std::wstring err;
            if (!host.enable(file, sha, err)) flash(L"Not switched on: " + err, 8);
            else {
                const PluginEntry* now = host.entry(file);
                flash(L"Switched on " + (now ? widen(now->info.name) : widen(file)) + L". Add it from a source's Filters page.", 6);
            }
            pluginsChanged();
        });
        y += 26;
        std::wstring status;
        if (e.loaded) status = L"Running: by " + widen(e.info.author) + L", version " + widen(e.info.version) + L".";
        else if (!e.problem.empty()) status = L"Not running: " + widen(e.problem);
        else if (!e.offReason.empty()) status = L"Off: " + widen(e.offReason);
        else status = L"Off.";
        wchar_t kb[48];
        if (e.size < 1024) swprintf(kb, 48, L"%llu bytes", (unsigned long long)e.size);
        else swprintf(kb, 48, L"%.0f KB", e.size / 1024.0);
        y += gfx_.text(status + L"  " + e.signature + L".  " + kb, {x, y, colW - 60, 40}, Font::Small,
                       e.loaded ? pal_.ribbon : (!e.problem.empty() || !e.offReason.empty()) ? cjui::hexColor("#E0566A") : pal_.inkMuted) + 2;
        y += gfx_.text(L"SHA-256 " + widen(e.sha256), {x, y, colW - 60, 20}, Font::Small, pal_.inkFaint, {Align::Left, false, true, true}) + 10;
        gfx_.line(x, y + 0.5f, x + colW, y + 0.5f, pal_.rule.withAlpha(0.5f));
        y += 10;
        (void)top;
    }
    return y - y0;
}

}  // namespace ncap
