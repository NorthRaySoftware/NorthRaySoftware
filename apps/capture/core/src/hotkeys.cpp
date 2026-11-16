#include "ncap/hotkeys.hpp"

#include <nlohmann/json.hpp>

#include <set>

namespace ncap::hotkeys {

using nlohmann::json;

namespace {
const char* const kKeys[kActions] = {"scene", "toggle-stream", "toggle-record", "toggle-mute", "play-sound", "stop-sounds", "marker", "transition"};

// Windows virtual-key codes used here (kept portable: no windows.h).
enum : unsigned {
    kBack = 0x08, kTab = 0x09, kReturn = 0x0D, kShift = 0x10, kCtrl = 0x11, kMenu = 0x12, kPause = 0x13, kCaps = 0x14, kEscape = 0x1B,
    kSpace = 0x20, kPgUp = 0x21, kPgDn = 0x22, kEnd = 0x23, kHome = 0x24, kLeft = 0x25, kUp = 0x26, kRight = 0x27, kDown = 0x28,
    kPrint = 0x2C, kInsert = 0x2D, kDelete = 0x2E, kLWin = 0x5B, kRWin = 0x5C, kNum0 = 0x60, kNum9 = 0x69, kMultiply = 0x6A,
    kAdd = 0x6B, kSubtract = 0x6D, kDecimal = 0x6E, kDivide = 0x6F, kF1 = 0x70, kF24 = 0x87, kScroll = 0x91, kLShift = 0xA0,
    kRMenu = 0xA5, kMute = 0xAD, kVolDown = 0xAE, kVolUp = 0xAF, kNext = 0xB0, kPrev = 0xB1, kStop = 0xB2, kPlay = 0xB3,
    kOem1 = 0xBA, kOemPlus = 0xBB, kOemComma = 0xBC, kOemMinus = 0xBD, kOemPeriod = 0xBE, kOem2 = 0xBF, kOem3 = 0xC0,
    kOem4 = 0xDB, kOem5 = 0xDC, kOem6 = 0xDD, kOem7 = 0xDE,
};

// Keys that produce text when typed: they need Ctrl, Alt or Win.
bool typingKey(unsigned vk) {
    return (vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z') || vk == kSpace || vk == kReturn || vk == kBack || vk == kTab ||
           (vk >= kOem1 && vk <= kOem3) || (vk >= kOem4 && vk <= kOem7) || (vk >= kLeft && vk <= kDown) || vk == kHome || vk == kEnd ||
           vk == kPgUp || vk == kPgDn || vk == kInsert || vk == kDelete || vk == kEscape;
}
}  // namespace

const char* actionKey(Action a) {
    const int i = (int)a;
    return i >= 0 && i < kActions ? kKeys[i] : "scene";
}

bool actionFromKey(const std::string& k, Action& out) {
    for (int i = 0; i < kActions; ++i)
        if (k == kKeys[i]) {
            out = (Action)i;
            return true;
        }
    return false;
}

bool needsTarget(Action a) { return a == Action::Scene || a == Action::ToggleMute || a == Action::PlaySound; }

std::string describe(Action a, const std::string& t) {
    switch (a) {
        case Action::Scene: return "Switch to " + t;
        case Action::ToggleStream: return "Start or stop streaming";
        case Action::ToggleRecord: return "Start or stop recording";
        case Action::ToggleMute: return "Mute or unmute " + t;
        case Action::PlaySound: return "Play " + t;
        case Action::StopSounds: return "Stop all sounds";
        case Action::Marker: return "Add a marker to the recording";
        case Action::Transition: return "Studio mode: send the preview live";
    }
    return {};
}

bool isModifierKey(unsigned vk) {
    return vk == kShift || vk == kCtrl || vk == kMenu || vk == kLWin || vk == kRWin || (vk >= kLShift && vk <= kRMenu);
}

std::string keyName(unsigned vk) {
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::string(1, (char)vk);
    if (vk >= kF1 && vk <= kF24) return "F" + std::to_string(vk - kF1 + 1);
    if (vk >= kNum0 && vk <= kNum9) return "Num " + std::to_string(vk - kNum0);
    switch (vk) {
        case kSpace: return "Space";
        case kReturn: return "Enter";
        case kBack: return "Backspace";
        case kTab: return "Tab";
        case kEscape: return "Esc";
        case kPgUp: return "Page Up";
        case kPgDn: return "Page Down";
        case kEnd: return "End";
        case kHome: return "Home";
        case kLeft: return "Left";
        case kUp: return "Up";
        case kRight: return "Right";
        case kDown: return "Down";
        case kInsert: return "Insert";
        case kDelete: return "Delete";
        case kPause: return "Pause";
        case kScroll: return "Scroll Lock";
        case kMultiply: return "Num *";
        case kAdd: return "Num +";
        case kSubtract: return "Num -";
        case kDecimal: return "Num .";
        case kDivide: return "Num /";
        case kMute: return "Mute";
        case kVolDown: return "Volume Down";
        case kVolUp: return "Volume Up";
        case kNext: return "Next Track";
        case kPrev: return "Previous Track";
        case kStop: return "Stop";
        case kPlay: return "Play/Pause";
        case kOem1: return ";";
        case kOemPlus: return "=";
        case kOemComma: return ",";
        case kOemMinus: return "-";
        case kOemPeriod: return ".";
        case kOem2: return "/";
        case kOem3: return "`";
        case kOem4: return "[";
        case kOem5: return "\\";
        case kOem6: return "]";
        case kOem7: return "'";
        default: return {};   // Print Screen, Caps Lock and anything unknown
    }
}

std::string text(const Combo& c) {
    std::string s;
    if (c.mods & Ctrl) s += "Ctrl+";
    if (c.mods & Alt) s += "Alt+";
    if (c.mods & Shift) s += "Shift+";
    if (c.mods & Win) s += "Win+";
    const std::string k = keyName(c.vk);
    return s + (k.empty() ? "?" : k);
}

bool allowed(const Combo& c, std::string& why) {
    if (c.mods & ~(Alt | Ctrl | Shift | Win)) {
        why = "That combination can't be used.";
        return false;
    }
    if (isModifierKey(c.vk) || keyName(c.vk).empty()) {
        why = c.vk == kPrint ? "Print Screen belongs to Windows." : isModifierKey(c.vk) ? "Add a key to go with it." : "That key can't be a hotkey.";
        return false;
    }
    if (typingKey(c.vk) && !(c.mods & (Ctrl | Alt | Win))) {
        why = "Letters, numbers and typing keys need Ctrl, Alt or Win with them, so they don't stop you typing in other programs.";
        return false;
    }
    if (c.mods & Win) {
        // Windows keeps most Win+key combinations for itself.
        why = "Windows keeps Win key combinations for itself; use Ctrl, Alt or Shift.";
        return false;
    }
    if (c.mods == (Ctrl | Alt) && c.vk == kDelete) {
        why = "Ctrl+Alt+Delete belongs to Windows.";
        return false;
    }
    if (c.mods == Alt && (c.vk == kTab || c.vk == 0x73 /* F4 */ || c.vk == kEscape)) {
        why = "Alt+Tab, Alt+F4 and Alt+Esc belong to Windows.";
        return false;
    }
    return true;
}

std::string toJson(const std::vector<Binding>& bs) {
    json a = json::array();
    for (auto& b : bs) a.push_back({{"mods", b.combo.mods}, {"vk", b.combo.vk}, {"action", actionKey(b.action)}, {"target", b.target}});
    return a.dump();
}

std::vector<Binding> fromJson(const std::string& text) {
    std::vector<Binding> out;
    std::set<Combo> seen;
    const json a = json::parse(text, nullptr, false);
    if (!a.is_array()) return out;
    for (auto& j : a) {
        if (!j.is_object() || out.size() >= kMaxBindings) continue;
        if (!j.contains("mods") || !j["mods"].is_number_unsigned() || !j.contains("vk") || !j["vk"].is_number_unsigned()) continue;
        Binding b;
        b.combo = {j["mods"].get<unsigned>(), j["vk"].get<unsigned>()};
        std::string why;
        if (!allowed(b.combo, why) || seen.count(b.combo)) continue;
        if (!j.contains("action") || !j["action"].is_string() || !actionFromKey(j["action"].get<std::string>(), b.action)) continue;
        if (j.contains("target") && j["target"].is_string()) b.target = j["target"].get<std::string>().substr(0, 200);
        if (!needsTarget(b.action)) b.target.clear();
        else if (b.target.empty()) continue;
        seen.insert(b.combo);
        out.push_back(b);
    }
    return out;
}

}  // namespace ncap::hotkeys
