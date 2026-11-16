// NorthRay Capture: global hotkeys (Settings › Hotkeys). Each binding is a key combination and what it
// does; Windows delivers them even while a game has the focus (RegisterHotKey), and a Stream Deck's own
// Hotkey action can press them. Portable and unit-tested; the window registers and acts on them.
//
// Letters, digits and other typing keys need Ctrl, Alt or Win with them, so a hotkey never swallows
// ordinary typing in other programs. F1-F24, the numeric keypad, media and other non-typing keys may be
// used alone or with Shift.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ncap::hotkeys {

enum class Action {
    Scene,            // switch to the scene named `target`
    ToggleStream,     // start or stop streaming
    ToggleRecord,     // start or stop recording
    ToggleMute,       // mute or unmute the audio source named `target`
    PlaySound,        // play the soundboard sound named `target`
    StopSounds,       // stop every soundboard sound
    Marker,           // add a marker to the recording (a chapter in its MP4 copy)
    Transition,       // studio mode: send the preview scene to the program
};
constexpr int kActions = 8;
const char* actionKey(Action a);          // "scene", "toggle-stream"...
bool actionFromKey(const std::string& k, Action& out);
bool needsTarget(Action a);               // Scene, ToggleMute, PlaySound
std::string describe(Action a, const std::string& target);   // "Switch to Gameplay", "Start or stop streaming"

// Modifier bits, as Windows' RegisterHotKey wants them.
enum : unsigned { Alt = 0x1, Ctrl = 0x2, Shift = 0x4, Win = 0x8 };

struct Combo {
    unsigned mods = 0;
    unsigned vk = 0;   // a Windows virtual-key code
    bool operator==(const Combo&) const = default;
    bool operator<(const Combo& o) const { return mods != o.mods ? mods < o.mods : vk < o.vk; }
};
std::string keyName(unsigned vk);         // "F5", "A", "Num 7", "Page Up"; "" for keys that can't be used
std::string text(const Combo& c);         // "Ctrl+Shift+F5"
bool isModifierKey(unsigned vk);          // Shift, Ctrl, Alt, Win themselves
// Whether a combination can be a hotkey; if not, `why` says so.
bool allowed(const Combo& c, std::string& why);

struct Binding {
    Combo combo;
    Action action = Action::Scene;
    std::string target;
    bool operator==(const Binding&) const = default;
};
constexpr std::size_t kMaxBindings = 60;

std::string toJson(const std::vector<Binding>& b);           // a JSON array
std::vector<Binding> fromJson(const std::string& json);      // bad, unusable or repeated combinations skipped

}  // namespace ncap::hotkeys
