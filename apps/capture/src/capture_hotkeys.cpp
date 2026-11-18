// NorthRay Capture's window: global hotkeys (Settings › Hotkeys; core/hotkeys). Each binding is registered
// with Windows (RegisterHotKey, without auto-repeat), so it works while a game has the focus and a Stream
// Deck's Hotkey action can press it. A combination another program already holds is shown in red.
//
// Assigning keys: the next key pressed in Capture (with its Ctrl, Alt, Shift) is taken; the registered
// hotkeys are released meanwhile so they don't catch it. Esc cancels.
#include "capture_window.hpp"

#include "log.hpp"

#include "ncap/hotkeys.hpp"

#include "cjui/menu.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace ncap {

using cjui::Align;
using cjui::Font;
using cjui::MenuItem;
using cjui::narrow;
using cjui::Rect;
using cjui::widen;
namespace hk = hotkeys;

namespace {
constexpr int kHotkeyBase = 0x5000;
}

void CaptureWindow::unregisterHotkeys() {
    for (int i = 0; i < hkRegistered_; ++i) UnregisterHotKey(hwnd(), kHotkeyBase + i);
    hkRegistered_ = 0;
}

void CaptureWindow::registerHotkeys() {
    unregisterHotkeys();
    hkFailed_.assign(settings_.hotkeys.size(), false);
    for (std::size_t i = 0; i < settings_.hotkeys.size(); ++i) {
        const hk::Combo& c = settings_.hotkeys[i].combo;
        if (!RegisterHotKey(hwnd(), kHotkeyBase + (int)i, c.mods | MOD_NOREPEAT, c.vk)) {
            hkFailed_[i] = true;
            log::warn("Hotkey " + hk::text(c) + " couldn't be registered (another program has it)");
        }
    }
    hkRegistered_ = (int)settings_.hotkeys.size();
}

bool CaptureWindow::hotkeyMessage(WPARAM id) {
    const int i = (int)id - kHotkeyBase;
    if (i < 0 || i >= (int)settings_.hotkeys.size()) return false;
    runHotkey(settings_.hotkeys[(std::size_t)i]);
    return true;
}

void CaptureWindow::runHotkey(const hk::Binding& b) {
    log::info("Hotkey " + hk::text(b.combo) + ": " + hk::describe(b.action, b.target));
    switch (b.action) {
        case hk::Action::Scene: {
            for (auto& sc : scenes_.scenes())
                if (sc.name == b.target) {
                    scenes_.setCurrent(sc.id);   // in studio mode: the preview (Transition sends it live)
                    scenesChanged();
                    return;
                }
            flash(L"Hotkey " + widen(hk::text(b.combo)) + L": there's no scene called " + widen(b.target) + L".", 5);
            return;
        }
        case hk::Action::ToggleStream:
            // Straight away, without the pre-live checklist: a hotkey is often pressed from inside a game.
            if (streamer_.active()) stopStreaming();
            else startStreaming();
            return;
        case hk::Action::ToggleRecord:
            if (recorder_.active()) stopRecording();
            else startRecording();
            return;
        case hk::Action::ToggleMute: {
            for (auto& s : scenes_.sources())
                if (hasSound(s.kind) && s.name == b.target) {
                    const bool muted = !audioMuted(s);
                    setAudioSetting(s.id, "muted", muted);
                    flash(widen(s.name) + (muted ? L" muted." : L" unmuted."), 2);
                    return;
                }
            flash(L"Hotkey " + widen(hk::text(b.combo)) + L": there's no sound source called " + widen(b.target) + L".", 5);
            return;
        }
        case hk::Action::PlaySound: {
            for (auto& s : scenes_.sources()) {
                if (s.kind != SourceKind::Soundboard) continue;
                const auto sounds = soundboardSounds(s);
                for (std::size_t i = 0; i < sounds.size(); ++i)
                    if (sounds[i].name == b.target && boards_.count(s.id)) {
                        boards_[s.id]->play(i);
                        return;
                    }
            }
            flash(L"Hotkey " + widen(hk::text(b.combo)) + L": there's no soundboard sound called " + widen(b.target) + L".", 5);
            return;
        }
        case hk::Action::StopSounds:
            for (auto& [id, p] : boards_) p->stopAll();
            return;
        case hk::Action::Marker: {
            if (!recorder_.active()) {
                flash(L"Markers go in recordings: start recording first.", 4);
                return;
            }
            const double s = recorder_.status().seconds;
            markers_.push_back({(std::int64_t)(s * 1e6), "Marker " + std::to_string(markers_.size() + 1)});
            wchar_t t[64];
            swprintf(t, 64, L"Marker %zu at %d:%02d:%02d", markers_.size(), (int)s / 3600, (int)s / 60 % 60, (int)s % 60);
            flash(t, 3);
            return;
        }
        case hk::Action::Transition:
            if (!settings_.studioMode) {
                flash(L"That hotkey sends the preview live in studio mode, which is off.", 4);
                return;
            }
            takeToProgram();
            return;
        case hk::Action::Brb:
            toggleBrb();
            return;
        case hk::Action::ToggleSource: {
            // In the scene that's live (in studio mode the program, not the preview being set up).
            const Id scene = programScene();
            if (const Scene* sc = scenes_.scene(scene))
                for (auto& it : sc->items)
                    if (const Source* src = scenes_.source(it.source); src && src->name == b.target) {
                        const bool show = !it.visible;
                        const Id itemId = it.id;
                        scenes_.setItemVisible(scene, itemId, show);
                        scenesChanged();
                        flash(widen(b.target) + (show ? L" shown." : L" hidden."), 2);
                        return;
                    }
            flash(L"Hotkey " + widen(hk::text(b.combo)) + L": " + widen(b.target) + L" isn't in the scene that's live.", 4);
            return;
        }
    }
}

// The key pressed while assigning one (from preTranslate). True if it was taken.
bool CaptureWindow::hotkeyCapture(const MSG& msg) {
    if (!hkListening_ || (msg.message != WM_KEYDOWN && msg.message != WM_SYSKEYDOWN)) return false;
    const unsigned vk = (unsigned)msg.wParam;
    if (vk == VK_ESCAPE) {
        hkListening_ = false;
        registerHotkeys();
        flash(L"No change.", 2);
        invalidate();
        return true;
    }
    if (hk::isModifierKey(vk)) return true;   // wait for the key that goes with them
    hk::Combo c;
    c.vk = vk;
    if (GetKeyState(VK_CONTROL) & 0x8000) c.mods |= hk::Ctrl;
    if (GetKeyState(VK_MENU) & 0x8000) c.mods |= hk::Alt;
    if (GetKeyState(VK_SHIFT) & 0x8000) c.mods |= hk::Shift;
    if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000)) c.mods |= hk::Win;
    std::string why;
    if (!hk::allowed(c, why)) {
        flash(widen(hk::text(c)) + L": " + widen(why) + L" Press other keys, or Esc.", 6);
        return true;
    }
    for (std::size_t i = 0; i < settings_.hotkeys.size(); ++i)
        if ((int)i != hkEditIndex_ && settings_.hotkeys[i].combo == c) {
            flash(widen(hk::text(c)) + L" already does " + widen(hk::describe(settings_.hotkeys[i].action, settings_.hotkeys[i].target)) +
                      L". Press other keys, or Esc.",
                  6);
            return true;
        }
    hk::Binding b = hkPending_;
    b.combo = c;
    if (hkEditIndex_ >= 0 && hkEditIndex_ < (int)settings_.hotkeys.size()) settings_.hotkeys[(std::size_t)hkEditIndex_] = b;
    else settings_.hotkeys.push_back(b);
    hkListening_ = false;
    settings_.save(settingsPath_);
    registerHotkeys();
    const std::size_t at = hkEditIndex_ >= 0 ? (std::size_t)hkEditIndex_ : settings_.hotkeys.size() - 1;
    if (at < hkFailed_.size() && hkFailed_[at])
        flash(L"Another program already uses " + widen(hk::text(c)) + L", so it won't reach Capture. Choose other keys with Keys….", 8);
    else flash(widen(hk::text(c)) + L": " + widen(hk::describe(b.action, b.target)) + L".", 4);
    log::info("Hotkey set: " + hk::text(c) + " = " + hk::describe(b.action, b.target));
    invalidate();
    return true;
}

void CaptureWindow::listenForHotkey(const hk::Binding& b, int editIndex) {
    hkPending_ = b;
    hkEditIndex_ = editIndex;
    hkListening_ = true;
    unregisterHotkeys();   // so they don't catch the keys being assigned
    invalidate();
}

void CaptureWindow::addHotkey() {
    if (settings_.hotkeys.size() >= hk::kMaxBindings) {
        flash(L"That's the most hotkeys there can be (60).", 4);
        return;
    }
    std::vector<hk::Binding> choices;
    auto add = [&](hk::Action a, const std::string& t = {}) {
        choices.push_back({{}, a, t});
        return 100 + (UINT)choices.size() - 1;
    };
    std::vector<MenuItem> scenes, mutes, sounds, shows;
    for (auto& sc : scenes_.scenes())
        if (scenes.size() < 200) scenes.push_back(MenuItem::item(widen(sc.name), add(hk::Action::Scene, sc.name)));
    for (auto& s : scenes_.sources()) {
        if (shows.size() < 100) shows.push_back(MenuItem::item(widen(s.name), add(hk::Action::ToggleSource, s.name)));
        if (hasSound(s.kind) && mutes.size() < 100) mutes.push_back(MenuItem::item(widen(s.name), add(hk::Action::ToggleMute, s.name)));
        if (s.kind == SourceKind::Soundboard)
            for (auto& snd : soundboardSounds(s))
                if (sounds.size() < 100) sounds.push_back(MenuItem::item(widen(snd.name), add(hk::Action::PlaySound, snd.name)));
    }
    std::vector<MenuItem> items;
    if (!scenes.empty()) items.push_back(MenuItem::sub(L"Switch to a scene", scenes));
    items.push_back(MenuItem::item(L"Start or stop streaming", add(hk::Action::ToggleStream)));
    items.push_back(MenuItem::item(L"Start or stop recording", add(hk::Action::ToggleRecord)));
    items.push_back(MenuItem::item(L"Add a marker to the recording", add(hk::Action::Marker)));
    items.push_back(MenuItem::item(L"Be right back (and back again)", add(hk::Action::Brb)));
    items.push_back(MenuItem::sep());
    if (!shows.empty()) items.push_back(MenuItem::sub(L"Show or hide a source", shows));
    if (!mutes.empty()) items.push_back(MenuItem::sub(L"Mute or unmute", mutes));
    if (!sounds.empty()) items.push_back(MenuItem::sub(L"Play a sound", sounds));
    items.push_back(MenuItem::item(L"Stop all sounds", add(hk::Action::StopSounds)));
    items.push_back(MenuItem::sep());
    items.push_back(MenuItem::item(L"Studio mode: send the preview live", add(hk::Action::Transition)));
    POINT pt;
    GetCursorPos(&pt);
    const UINT cmd = cjui::showMenu(hwnd(), items, pt, pal_);
    if (cmd < 100 || cmd >= 100 + choices.size()) return;
    listenForHotkey(choices[cmd - 100], -1);
}

float CaptureWindow::drawHotkeySettings(float x, float y0, float colW) {
    float y = y0;
    y += gfx_.text(L"Keys that work anywhere, even while a game has the focus. A Stream Deck's Hotkey action can press them too. Starting a "
                   L"stream from a hotkey skips the pre-live checklist.",
                   {x, y, colW, 80}, Font::Small, pal_.inkMuted) + 12;
    if (hkFailed_.size() != settings_.hotkeys.size()) hkFailed_.resize(settings_.hotkeys.size(), false);
    for (std::size_t i = 0; i < settings_.hotkeys.size(); ++i) {
        const hk::Binding& b = settings_.hotkeys[i];
        const bool editing = hkListening_ && hkEditIndex_ == (int)i;
        const Rect cap{x, y, 170, 28};
        gfx_.fillRounded(cap, 6, pal_.ink.withAlpha(0.07f));
        gfx_.strokeRounded(cap, 6, hkFailed_[i] ? pal_.live : pal_.rule, 1);
        gfx_.text(editing ? L"Press the keys…" : widen(hk::text(b.combo)), cap, Font::BodyStrong, editing ? pal_.ribbon : pal_.ink,
                  {Align::Center, false, true, true});
        gfx_.text(widen(hk::describe(b.action, b.target)), {x + 184, y, colW - 184 - 150, 28}, Font::Body, pal_.ink, {Align::Left, false, true, true});
        button("hk:keys:" + std::to_string(i), {x + colW - 140, y - 1, 90, 30}, L"Keys…", [this, i] {
            if (i < settings_.hotkeys.size()) listenForHotkey(settings_.hotkeys[i], (int)i);
        });
        button("hk:del:" + std::to_string(i), {x + colW - 40, y - 1, 40, 30}, L"×", [this, i] {
            if (i >= settings_.hotkeys.size()) return;
            log::info("Hotkey removed: " + hk::text(settings_.hotkeys[i].combo));
            settings_.hotkeys.erase(settings_.hotkeys.begin() + (long)i);
            hkListening_ = false;
            settings_.save(settingsPath_);
            registerHotkeys();
            invalidate();
        });
        y += 36;
        if (hkFailed_[i]) {
            y += gfx_.text(L"Another program already uses " + widen(hk::text(b.combo)) + L", so it doesn't reach Capture. Choose other keys.",
                           {x + 184, y - 6, colW - 184, 20}, Font::Small, pal_.live, {Align::Left, false, true}) + 4;
        }
    }
    if (hkListening_ && hkEditIndex_ < 0) {
        const Rect cap{x, y, 170, 28};
        gfx_.fillRounded(cap, 6, pal_.ribbon.withAlpha(0.15f));
        gfx_.strokeRounded(cap, 6, pal_.ribbon, 1);
        gfx_.text(L"Press the keys…", cap, Font::BodyStrong, pal_.ribbon, {Align::Center, false, true, true});
        gfx_.text(widen(hk::describe(hkPending_.action, hkPending_.target)) + L" (Esc to cancel)", {x + 184, y, colW - 184, 28}, Font::Body, pal_.inkMuted,
                  {Align::Left, false, true, true});
        y += 36;
    }
    if (settings_.hotkeys.empty() && !hkListening_) y += gfx_.text(L"No hotkeys yet.", {x, y, colW, 22}, Font::Body, pal_.inkFaint, {Align::Left, false, true}) + 10;
    button("hk:add", {x, y + 2, 200, 32}, L"Add a hotkey…", [this] { addHotkey(); });
    y += 44;
    return y - y0;
}

// A tooltip's text with the hotkey you've set for that action, if any ("… (Ctrl+F9)").
std::wstring CaptureWindow::withHotkey(const std::wstring& text, hotkeys::Action a, const std::string& target) const {
    for (const auto& b : settings_.hotkeys)
        if (b.action == a && (target.empty() || b.target == target)) return text + L"  (" + widen(hotkeys::text(b.combo)) + L")";
    return text;
}

}  // namespace ncap
