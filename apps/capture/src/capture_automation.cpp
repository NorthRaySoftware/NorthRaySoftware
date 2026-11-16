// NorthRay Capture's window: automation (Settings › Automation). Rules switch the scene when a program
// opens or closes (core/automation). Every 2 seconds the running programs are listed (a Toolhelp process
// snapshot: file names only, nothing else about them) and the watcher says which rule, if any, fires.
#include "capture_window.hpp"

#include "log.hpp"
#include "video/devices.hpp"

#include "ncap/automation.hpp"

#include "cjui/menu.hpp"

#include <tlhelp32.h>

#include <algorithm>
#include <map>

namespace ncap {

using cjui::Align;
using cjui::Font;
using cjui::MenuItem;
using cjui::narrow;
using cjui::Rect;
using cjui::widen;
namespace au = automation;

namespace {
std::set<std::string> runningPrograms() {
    std::set<std::string> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        const std::string exe = au::normaliseExe(narrow(pe.szExeFile));
        if (!exe.empty()) out.insert(exe);
    }
    CloseHandle(snap);
    return out;
}
}  // namespace

void CaptureWindow::automationTick() {
    if (settings_.automationRules.empty()) return;
    const ULONGLONG now = GetTickCount64();
    if (now - autoLastCheck_ < 1900) return;
    autoLastCheck_ = now;
    const bool live = streamer_.active() || recorder_.active();
    const int i = autoWatch_.update(runningPrograms(), settings_.automationRules, live);
    if (i < 0) return;
    const au::Rule& r = settings_.automationRules[(std::size_t)i];
    const Scene* target = nullptr;
    for (auto& sc : scenes_.scenes())
        if (sc.name == r.scene) target = &sc;
    const std::wstring who = widen(r.label.empty() ? r.exe : r.label);
    const std::wstring what = r.when == au::When::Opens ? L" opened" : L" closed";
    if (!target) {
        log::warn("Automation: " + au::describe(r) + ", but there's no scene called that");
        flash(who + what + L", but there's no scene called " + widen(r.scene) + L" in this scene collection.", 6);
        return;
    }
    const Id current = settings_.studioMode ? programScene_ : scenes_.current();
    if (current == target->id) return;
    log::info("Automation: " + au::describe(r));
    if (settings_.studioMode) programScene_ = target->id;
    else scenes_.setCurrent(target->id);
    scenesChanged();
    flash(who + what + L": switched to " + widen(r.scene) + L".", 4);
}

void CaptureWindow::automationChanged() {
    settings_.save(settingsPath_);
    autoWatch_.reset();   // what's running now is the starting point for the new rules
    autoLastCheck_ = 0;
    invalidate();
}

// Step two of adding a rule: when, and which scene.
void CaptureWindow::addAutomationRuleFor(const std::string& exe, const std::string& label) {
    const auto& scenes = scenes_.scenes();
    if (scenes.empty()) return;
    std::vector<MenuItem> opens, closes;
    for (std::size_t i = 0; i < scenes.size() && i < 400; ++i) {
        opens.push_back(MenuItem::item(widen(scenes[i].name), 1000 + (UINT)i));
        closes.push_back(MenuItem::item(widen(scenes[i].name), 2000 + (UINT)i));
    }
    const std::wstring who = widen(label.empty() ? exe : label);
    std::vector<MenuItem> items = {MenuItem::sub(L"When " + who + L" opens, switch to", opens), MenuItem::sub(L"When " + who + L" closes, switch to", closes)};
    POINT pt;
    GetCursorPos(&pt);
    const UINT cmd = cjui::showMenu(hwnd(), items, pt, pal_);
    if (cmd < 1000 || cmd >= 2000 + scenes.size()) return;
    if (settings_.automationRules.size() >= au::kMaxRules) {
        flash(L"That's the most rules there can be (20).", 4);
        return;
    }
    au::Rule r;
    r.exe = exe;
    r.label = label;
    r.when = cmd < 2000 ? au::When::Opens : au::When::Closes;
    r.scene = scenes[(cmd < 2000 ? cmd - 1000 : cmd - 2000)].name;
    settings_.automationRules.push_back(r);
    log::info("Automation rule added: " + au::describe(r));
    automationChanged();
}

// Step one: which program (one with a window open now, Writing Studio, or a file name typed in).
void CaptureWindow::addAutomationRule() {
    std::map<std::string, std::string> programs;   // exe -> a window title, for the hint
    for (auto& w : video::listWindows()) {
        const std::string exe = au::normaliseExe(w.id.exe);
        if (!exe.empty() && exe != "northraycapture.exe" && !programs.count(exe)) programs[exe] = w.id.title;
    }
    std::vector<std::string> exes;
    std::vector<MenuItem> items;
    for (auto& [exe, title] : programs) {
        if (items.size() >= 40) break;
        exes.push_back(exe);
        items.push_back(MenuItem::item(widen(exe), 100 + (UINT)exes.size() - 1, widen(title.substr(0, 60))));
    }
    if (!items.empty()) items.push_back(MenuItem::sep());
    items.push_back(MenuItem::item(L"NorthRay Writing Studio", 2, L"WritingStudio.exe"));
    items.push_back(MenuItem::item(L"Another program…", 3, L"type its file name, like krita.exe"));
    POINT pt;
    GetCursorPos(&pt);
    const UINT cmd = cjui::showMenu(hwnd(), items, pt, pal_);
    if (cmd >= 100 && cmd < 100 + exes.size()) {
        const std::string exe = exes[cmd - 100];
        std::string label = exe.substr(0, exe.size() - 4);
        if (!label.empty()) label[0] = (char)std::toupper((unsigned char)label[0]);
        addAutomationRuleFor(exe, label);
    } else if (cmd == 2) {
        addAutomationRuleFor("writingstudio.exe", "Writing Studio");
    } else if (cmd == 3) {
        askText(L"The program's file name (as in Task Manager › Details), like krita.exe", L"", [this](const std::wstring& v) {
            const std::string exe = au::normaliseExe(narrow(v));
            if (exe.empty()) {
                flash(L"Type a file name like krita.exe.", 4);
                return;
            }
            std::string label = exe.substr(0, exe.size() - 4);
            if (!label.empty()) label[0] = (char)std::toupper((unsigned char)label[0]);
            addAutomationRuleFor(exe, label);
        });
    }
}

float CaptureWindow::drawAutomationSettings(float x, float y0, float colW) {
    float y = y0;
    y += gfx_.text(L"Switch scenes when a program opens or closes: your drawing app brings up the Art scene, closing the game goes back to "
                   L"Just chatting. Programs already open when Capture starts don't count as opening. If two rules happen at once, the "
                   L"lower one wins.",
                   {x, y, colW, 80}, Font::Small, pal_.inkMuted) + 12;
    auto& rules = settings_.automationRules;
    for (std::size_t i = 0; i < rules.size(); ++i) {
        const au::Rule& r = rules[i];
        bool sceneThere = false;
        for (auto& sc : scenes_.scenes()) sceneThere |= sc.name == r.scene;
        gfx_.text(widen(au::describe(r)), {x, y, colW - 150, 22}, Font::Body, r.on ? pal_.ink : pal_.inkFaint, {Align::Left, false, true, true});
        const float h = sceneThere ? 0.0f
                                   : gfx_.text(L"There's no scene called " + widen(r.scene) + L" in this scene collection.", {x, y + 22, colW - 150, 20},
                                               Font::Small, pal_.live, {Align::Left, false, true});
        toggle("auto:on:" + std::to_string(i), x + colW - 140, y + 1, r.on, [this, i] {
            if (i >= settings_.automationRules.size()) return;
            settings_.automationRules[i].on = !settings_.automationRules[i].on;
            automationChanged();
        });
        button("auto:more:" + std::to_string(i), {x + colW - 80, y - 3, 80, 30}, L"More…", [this, i] {
            if (i >= settings_.automationRules.size()) return;
            const au::Rule cur = settings_.automationRules[i];
            std::vector<MenuItem> items = {
                MenuItem::item(L"Only while live or recording", 1, {}, true, cur.onlyLive),
                MenuItem::item(L"Move up", 2, {}, i > 0),
                MenuItem::item(L"Move down", 3, {}, i + 1 < settings_.automationRules.size()),
                MenuItem::sep(),
                MenuItem::item(L"Remove", 4),
            };
            POINT pt;
            GetCursorPos(&pt);
            const UINT cmd = cjui::showMenu(hwnd(), items, pt, pal_);
            auto& v = settings_.automationRules;
            if (i >= v.size() || !(v[i] == cur)) return;
            if (cmd == 1) v[i].onlyLive = !v[i].onlyLive;
            else if (cmd == 2 && i > 0) std::swap(v[i], v[i - 1]);
            else if (cmd == 3 && i + 1 < v.size()) std::swap(v[i], v[i + 1]);
            else if (cmd == 4) {
                log::info("Automation rule removed: " + au::describe(cur));
                v.erase(v.begin() + (long)i);
            } else return;
            automationChanged();
        });
        y += 34 + (h > 0 ? h + 4 : 0);
    }
    if (rules.empty()) y += gfx_.text(L"No rules yet.", {x, y, colW, 22}, Font::Body, pal_.inkFaint, {Align::Left, false, true}) + 10;
    button("auto:add", {x, y + 2, 200, 32}, L"Add a rule…", [this] { addAutomationRule(); });
    y += 44;
    return y - y0;
}

}  // namespace ncap
