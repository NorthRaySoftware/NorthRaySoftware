// NorthRay Capture's window: studio mode and transitions (system 5).
//
// Without studio mode the scene picked in Scenes is live, and switching runs the transition.
// With it, Scenes picks what the preview (left) shows, for preparing privately; Transition (or
// Ctrl+Enter) puts it live in the program (right). Editing the scene that's live shows at once, and
// the preview says so.
#include "capture_window.hpp"

#include "log.hpp"

#include "cjui/menu.hpp"

#include <shobjidl.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace ncap {

using cjui::Align;
using cjui::Color;
using cjui::Font;
using cjui::MenuItem;
using cjui::narrow;
using cjui::Rect;
using cjui::widen;
namespace tr = transition;

namespace {
enum : UINT {
    CmdKind0 = 100,        // + Kind
    CmdMs0 = 200,          // + index into kDurations
    CmdEase0 = 300,        // + Easing
    CmdDir0 = 400,         // + Direction
    CmdPattern0 = 500,     // + Pattern
    CmdSoft0 = 600,        // + index into kSoftness
    CmdInvert = 700,
    CmdStingerFile, CmdStingerCut, CmdShaderFile, CmdShaderExample, CmdShaderReload,
};
const int kDurations[] = {150, 300, 500, 750, 1000, 2000};
const float kSoftness[] = {0.0f, 0.05f, 0.12f, 0.25f};
const wchar_t* kKindNames[] = {L"Cut", L"Fade", L"Slide", L"Swipe", L"Luma wipe", L"Stinger", L"Custom shader", L"Aurora sweep"};
constexpr int kKinds = 8;
const wchar_t* kEaseNames[] = {L"Steady", L"Smooth (ease in and out)", L"Quick start, gentle stop"};
const wchar_t* kDirNames[] = {L"To the left", L"To the right", L"Upwards", L"Downwards"};
const wchar_t* kPatternNames[] = {L"Left to right", L"Top to bottom", L"Diagonal", L"Circle from the centre", L"Clock", L"Blinds"};
const wchar_t* kSoftNames[] = {L"Hard edge", L"Slightly soft", L"Soft", L"Very soft"};
bool fileDialog(HWND owner, bool save, const wchar_t* title, const COMDLG_FILTERSPEC* types, UINT n, const wchar_t* ext, const std::wstring& name,
                std::wstring& out) {
    IFileDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                save ? __uuidof(IFileSaveDialog) : __uuidof(IFileOpenDialog), reinterpret_cast<void**>(&dlg))))
        return false;
    dlg->SetFileTypes(n, types);
    dlg->SetTitle(title);
    if (ext) dlg->SetDefaultExtension(ext);
    if (save && !name.empty()) dlg->SetFileName(name.c_str());
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | (save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST));
    bool ok = false;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        PWSTR p = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
            out = p;
            ok = true;
            CoTaskMemFree(p);
        }
        if (item) item->Release();
    }
    dlg->Release();
    return ok;
}

std::wstring shortName(const std::string& path) {
    const std::wstring w = widen(path);
    return w.substr(w.find_last_of(L"\\/") + 1);
}
}  // namespace

void CaptureWindow::applyTransitionFiles(bool reload) {
    const tr::Settings& t = settings_.transition;
    // The stinger: a media source of its own, kept paused at its start.
    if (t.stingerFile != stingerFile_ || reload) {
        stingerFile_ = t.stingerFile;
        stinger_.reset();
        if (!stingerFile_.empty())
            stinger_ = video::createVideoSource(SourceKind::Media,
                                                nlohmann::json{{"files", {stingerFile_}}, {"loop", false}, {"startPaused", true}}.dump());
        syncAudio();
    }
    // The shader's code, read from its file.
    if (t.shaderFile != shaderFile_ || reload) {
        shaderFile_ = t.shaderFile;
        shaderCode_.clear();
        if (!shaderFile_.empty()) {
            std::ifstream f(std::filesystem::path(widen(shaderFile_)), std::ios::binary);
            shaderCode_.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            if (shaderCode_.size() > 256 * 1024) shaderCode_.clear();   // not a transition
            if (shaderCode_.empty()) flash(L"The shader file couldn't be read.", 6);
        }
    }
}

Id CaptureWindow::programScene() {
    if (!settings_.studioMode) {
        programScene_ = scenes_.current();
    } else if (!scenes_.scene(programScene_)) {
        programScene_ = scenes_.current();   // the live scene was deleted: the preview's goes live
    }
    return programScene_;
}

std::wstring CaptureWindow::transitionLabel() const {
    const tr::Settings& t = settings_.transition;
    std::wstring s = kKindNames[(int)t.kind];
    if (t.kind == tr::Kind::Stinger) {
        wchar_t b[48];
        swprintf(b, 48, L" \u00B7 cut at %.2g s", t.cutMs / 1000.0);
        return s + b;
    }
    if (t.kind != tr::Kind::Cut) {
        wchar_t b[32];
        swprintf(b, 32, L" · %.2g s", t.durationMs / 1000.0);
        s += b;
    }
    return s;
}

void CaptureWindow::setStudioMode(bool on) {
    if (settings_.studioMode == on) return;
    // Entering: the preview starts on the scene that's live. Leaving: the live scene stays live and
    // becomes the one shown and edited.
    if (on) programScene_ = scenes_.current();
    else if (scenes_.scene(programScene_)) scenes_.setCurrent(programScene_);
    settings_.studioMode = on;
    settings_.save(settingsPath_);
    log::info(on ? "Studio mode on" : "Studio mode off");
    scenesChanged();
}

void CaptureWindow::takeToProgram() {
    if (!settings_.studioMode) return;
    const Id next = scenes_.current();
    if (next == programScene_) {
        flash(L"That scene is already live. Pick another in Scenes to prepare it.", 4);
        return;
    }
    programScene_ = next;
    if (const Scene* sc = scenes_.scene(next)) log::info("Live: " + sc->name + " (" + narrow(transitionLabel()) + ")");
    invalidate();
}

void CaptureWindow::transitionMenu(POINT screen) {
    tr::Settings& t = settings_.transition;
    std::vector<MenuItem> kinds, ms, ease, dir, pat, soft;
    for (int i = 0; i < kKinds; ++i) kinds.push_back(MenuItem::item(kKindNames[i], CmdKind0 + (UINT)i, {}, true, (int)t.kind == i));
    for (int i = 0; i < 6; ++i) {
        wchar_t b[32];
        swprintf(b, 32, kDurations[i] == 1000 ? L"1 second" : L"%.2g seconds", kDurations[i] / 1000.0);
        ms.push_back(MenuItem::item(b, CmdMs0 + (UINT)i, {}, true, t.durationMs == kDurations[i]));
    }
    for (int i = 0; i < 3; ++i) ease.push_back(MenuItem::item(kEaseNames[i], CmdEase0 + (UINT)i, {}, true, (int)t.easing == i));
    for (int i = 0; i < 4; ++i) dir.push_back(MenuItem::item(kDirNames[i], CmdDir0 + (UINT)i, {}, true, (int)t.direction == i));
    for (int i = 0; i < 6; ++i) pat.push_back(MenuItem::item(kPatternNames[i], CmdPattern0 + (UINT)i, {}, true, (int)t.pattern == i));
    for (int i = 0; i < 4; ++i) soft.push_back(MenuItem::item(kSoftNames[i], CmdSoft0 + (UINT)i, {}, true, std::abs(t.softness - kSoftness[i]) < 0.01f));
    std::vector<MenuItem> items = kinds;
    items.push_back(MenuItem::sep());
    const bool timed = t.kind != tr::Kind::Cut && t.kind != tr::Kind::Stinger;
    items.push_back(MenuItem::sub(L"Duration", ms));
    items.back().enabled = timed;
    items.push_back(MenuItem::sub(L"Easing", ease));
    items.back().enabled = timed;
    if (t.kind == tr::Kind::Slide || t.kind == tr::Kind::Swipe || t.kind == tr::Kind::Aurora) items.push_back(MenuItem::sub(L"Direction", dir));
    if (t.kind == tr::Kind::Aurora) items.push_back(MenuItem::item(L"In your brand kit's main and second colours", 1, {}, false));
    if (t.kind == tr::Kind::LumaWipe) {
        items.push_back(MenuItem::sub(L"Pattern", pat));
        items.push_back(MenuItem::sub(L"Edge", soft));
        items.push_back(MenuItem::item(L"Run it the other way", CmdInvert, {}, true, t.invert));
    }
    if (t.kind == tr::Kind::Stinger) {
        items.push_back(MenuItem::item(L"Choose the video\u2026", CmdStingerFile, t.stingerFile.empty() ? L"none yet" : shortName(t.stingerFile)));
        wchar_t cut[32];
        swprintf(cut, 32, L"%.2g s", t.cutMs / 1000.0);
        items.push_back(MenuItem::item(L"When the scene switches\u2026", CmdStingerCut, cut));
    }
    if (t.kind == tr::Kind::Shader) {
        items.push_back(MenuItem::item(L"Choose the shader file\u2026", CmdShaderFile, t.shaderFile.empty() ? L"none yet" : shortName(t.shaderFile)));
        items.push_back(MenuItem::item(L"Make an example file to start from\u2026", CmdShaderExample));
        items.push_back(MenuItem::item(L"Read the file again", CmdShaderReload, L"after editing it", !t.shaderFile.empty()));
        const std::string err = engine_.stats().shaderError;
        if (!err.empty()) items.push_back(MenuItem::item(L"It doesn't compile, so a fade is used: " + widen(err.substr(0, 90)), 1, {}, false));
    }
    const UINT cmd = cjui::showMenu(hwnd(), items, screen, pal_);
    if (!cmd) return;
    const COMDLG_FILTERSPEC videos[] = {{L"Videos", L"*.mp4;*.mov;*.mkv;*.avi;*.wmv;*.webm;*.m4v"}, {L"All files", L"*.*"}};
    const COMDLG_FILTERSPEC shaders[] = {{L"HLSL shaders", L"*.hlsl;*.fx;*.txt"}, {L"All files", L"*.*"}};
    std::wstring path;
    if (cmd >= CmdKind0 && cmd < CmdKind0 + kKinds) t.kind = (tr::Kind)(cmd - CmdKind0);
    else if (cmd == CmdStingerFile) {
        if (!fileDialog(hwnd(), false, L"Choose the stinger video", videos, 2, nullptr, {}, path)) return;
        t.stingerFile = narrow(path);
    } else if (cmd == CmdStingerCut) {
        wchar_t now[32];
        swprintf(now, 32, L"%.2g", t.cutMs / 1000.0);
        askText(L"Seconds into the video when the scene switches (when it covers the screen)", now, [this](const std::wstring& v) {
            const double secs = _wtof(v.c_str());
            if (secs < 0 || secs > 60) return;
            settings_.transition.cutMs = (int)(secs * 1000 + 0.5);
            settings_.save(settingsPath_);
            invalidate();
        });
        return;
    } else if (cmd == CmdShaderFile) {
        if (!fileDialog(hwnd(), false, L"Choose the shader file", shaders, 2, nullptr, {}, path)) return;
        t.shaderFile = narrow(path);
    } else if (cmd == CmdShaderExample) {
        if (!fileDialog(hwnd(), true, L"Save an example transition shader", shaders, 2, L"hlsl", L"circle transition.hlsl", path)) return;
        std::ofstream f(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
        f << tr::exampleShader();
        if (!f.good()) {
            flash(L"The example couldn't be written there.", 6);
            return;
        }
        t.shaderFile = narrow(path);
        flash(L"Saved an example and chose it. Edit it in any text editor, then use Read the file again.", 8);
    } else if (cmd == CmdShaderReload) {
        applyTransitionFiles(true);
        invalidate();
        return;
    }
    else if (cmd >= CmdMs0 && cmd < CmdMs0 + 6) t.durationMs = kDurations[cmd - CmdMs0];
    else if (cmd >= CmdEase0 && cmd < CmdEase0 + 3) t.easing = (tr::Easing)(cmd - CmdEase0);
    else if (cmd >= CmdDir0 && cmd < CmdDir0 + 4) t.direction = (tr::Direction)(cmd - CmdDir0);
    else if (cmd >= CmdPattern0 && cmd < CmdPattern0 + 6) t.pattern = (tr::Pattern)(cmd - CmdPattern0);
    else if (cmd >= CmdSoft0 && cmd < CmdSoft0 + 4) t.softness = kSoftness[cmd - CmdSoft0];
    else if (cmd == CmdInvert) t.invert = !t.invert;
    else if (cmd != CmdStingerFile && cmd != CmdShaderFile && cmd != CmdShaderExample) return;
    settings_.save(settingsPath_);
    applyTransitionFiles();
    log::info("Transition: " + std::string(tr::key(t.kind)) + ", " + std::to_string(t.durationMs) + " ms, " + tr::key(t.easing));
    invalidate();
}

float CaptureWindow::drawStudioControls(float x, float y, float w, float bottom) {
    const bool studio = settings_.studioMode;
    const Color red = cjui::hexColor("#E0566A");
    // Studio mode on/off, with the transition in use beside it.
    if (y + 34 <= bottom) {
        const Rect r{x, y, w, 34};
        addHit("ctl:studio", r, [this, studio] { setStudioMode(!studio); });
        const float hv = hoverAmount("ctl:studio");
        gfx_.fillRounded(r, 8, studio ? pal_.ribbon.withAlpha(0.18f + 0.08f * hv) : pal_.ink.withAlpha((pal_.dark ? 0.07f : 0.05f) + 0.05f * hv));
        gfx_.strokeRounded(r, 8, studio ? pal_.ribbon : pal_.rule.mix(pal_.ink, 0.3f * hv), 1);
        gfx_.text(studio ? L"Studio mode: on" : L"Studio mode", {r.x + 12, r.y, r.w - 24, r.h}, Font::Body, pal_.ink, {Align::Left, false, true, true});
        if (focused("ctl:studio")) focusRing(r);
        y += 40;
    }
    // Studio mode: put the preview live.
    if (studio && y + 40 <= bottom) {
        const Rect r{x, y, w, 40};
        const bool same = scenes_.current() == programScene_;
        const bool busy = engine_.stats().transitioning;
        addHit("ctl:take", r, [this] { takeToProgram(); });
        const float hv = hoverAmount("ctl:take");
        gfx_.fillRounded(r, 8, same ? pal_.ink.withAlpha(0.04f) : red.withAlpha(0.20f + 0.12f * hv));
        gfx_.strokeRounded(r, 8, same ? pal_.rule : red, 1);
        const Scene* next = scenes_.scene(scenes_.current());
        const std::wstring label = busy ? L"Transitioning…" : same ? L"Preview is live" : L"Transition: " + (next ? widen(next->name) : L"") + L" goes live";
        gfx_.text(label, {r.x + 12, r.y, r.w - 24, r.h}, Font::BodyStrong, same ? pal_.inkFaint : pal_.ink, {Align::Left, false, true, true});
        if (w > 260) gfx_.text(L"Ctrl+Enter", {r.x, r.y, r.w - 12, r.h}, Font::Small, pal_.inkFaint, {Align::Right, false, false, true});
        if (focused("ctl:take")) focusRing(r);
        y += 46;
    }
    // Which transition.
    if (y + 28 <= bottom) {
        const Rect r{x, y, w, 28};
        addHit("ctl:trans", r, [this] {
            POINT pt;
            GetCursorPos(&pt);
            transitionMenu(pt);
        });
        const float hv = hoverAmount("ctl:trans");
        if (hv > 0) gfx_.fillRounded(r, 6, pal_.ink.withAlpha(0.05f * hv));
        gfx_.text(L"Transition", {r.x + 8, r.y, 100, r.h}, Font::Body, pal_.inkMuted, {Align::Left, false, true, true});
        gfx_.text(transitionLabel() + L"  ›", {r.x, r.y, r.w - 8, r.h}, Font::Body, pal_.ink, {Align::Right, false, true, true});
        if (focused("ctl:trans")) focusRing(r);
        y += 34;
    }
    return y;
}

}  // namespace ncap
