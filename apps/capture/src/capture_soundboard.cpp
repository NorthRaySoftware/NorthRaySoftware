// NorthRay Capture's window: the soundboard (the Soundboard panel, Layout menu). A Soundboard source in
// the scene collection holds its buttons ({"sounds": [{"name", "starter" | "file"}]}); its sound is a
// mixer channel like any other. The first time, it comes with the starter pack (core/soundboard).
#include "capture_window.hpp"

#include "log.hpp"

#include "cjui/menu.hpp"

#include <shobjidl.h>

#include <algorithm>
#include <filesystem>

namespace ncap {

using cjui::Align;
using cjui::Font;
using cjui::MenuItem;
using cjui::narrow;
using cjui::Rect;
using cjui::widen;
using json = nlohmann::json;

namespace {
constexpr std::size_t kMaxSounds = 48;

bool pickSound(HWND owner, std::wstring& out) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    dlg->SetTitle(L"Choose a sound");
    const COMDLG_FILTERSPEC types[] = {{L"Sounds", L"*.mp3;*.wav;*.m4a;*.aac;*.flac;*.wma;*.ogg"}, {L"All files", L"*.*"}};
    dlg->SetFileTypes(2, types);
    bool ok = false;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        PWSTR path = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            out = path;
            ok = true;
            CoTaskMemFree(path);
        }
        if (item) item->Release();
    }
    dlg->Release();
    return ok;
}
}  // namespace

std::vector<audio::SoundboardSound> CaptureWindow::soundboardSounds(const Source& s) const {
    std::vector<audio::SoundboardSound> out;
    json j = json::parse(s.settings, nullptr, false);
    if (!j.is_object()) j = json::object();
    if (!j.contains("sounds") || !j["sounds"].is_array()) return out;
    for (auto& x : j["sounds"]) {
        if (!x.is_object() || out.size() >= kMaxSounds) continue;
        audio::SoundboardSound o;
        if (x.contains("name") && x["name"].is_string()) o.name = x["name"].get<std::string>().substr(0, 40);
        if (x.contains("starter") && x["starter"].is_string()) o.starter = x["starter"].get<std::string>();
        if (x.contains("file") && x["file"].is_string()) o.file = x["file"].get<std::string>();
        if (o.starter.empty() && o.file.empty()) continue;
        if (o.name.empty()) o.name = "Sound";
        out.push_back(o);
    }
    return out;
}

void CaptureWindow::setSoundboardSounds(const Id& source, const std::vector<audio::SoundboardSound>& sounds) {
    json arr = json::array();
    for (auto& s : sounds) {
        json o{{"name", s.name}};
        if (!s.starter.empty()) o["starter"] = s.starter;
        else o["file"] = s.file;
        arr.push_back(o);
    }
    setAudioSetting(source, "sounds", arr);   // saves, and the mixer picks it up
}

void CaptureWindow::addSoundboard() {
    json arr = json::array();
    for (auto& st : soundboard::starterPack()) arr.push_back({{"name", st.name}, {"starter", st.id}});
    const Id id = scenes_.addSource(SourceKind::Soundboard, "Soundboard", json{{"sounds", arr}, {"volume", 0.7}}.dump());
    scenesChanged();
    syncAudio();
    log::info("Soundboard added with the starter pack");
    flash(L"Soundboard added with 12 starter sounds, made by NorthRay and free to use on stream. Its volume is in the mixer.", 7);
    (void)id;
}

void CaptureWindow::soundboardMenu(const Id& source, std::size_t i) {
    const Source* s = scenes_.source(source);
    if (!s) return;
    auto sounds = soundboardSounds(*s);
    if (i >= sounds.size()) return;
    std::vector<MenuItem> items = {MenuItem::item(L"Rename…", 1), MenuItem::item(L"Another sound file…", 2),
                                   MenuItem::item(L"Move earlier", 3, {}, i > 0), MenuItem::item(L"Move later", 4, {}, i + 1 < sounds.size()),
                                   MenuItem::sep(), MenuItem::item(L"Remove", 5)};
    POINT pt;
    GetCursorPos(&pt);
    const UINT cmd = cjui::showMenu(hwnd(), items, pt, pal_);
    if (cmd == 1) {
        askText(L"The button's name", widen(sounds[i].name), [this, source, i](const std::wstring& v) {
            const Source* s2 = scenes_.source(source);
            if (!s2 || v.empty()) return;
            auto list = soundboardSounds(*s2);
            if (i >= list.size()) return;
            list[i].name = narrow(v).substr(0, 40);
            setSoundboardSounds(source, list);
        });
    } else if (cmd == 2) {
        std::wstring path;
        if (!pickSound(hwnd(), path)) return;
        sounds[i].file = narrow(path);
        sounds[i].starter.clear();
        setSoundboardSounds(source, sounds);
    } else if (cmd == 3 || cmd == 4) {
        std::swap(sounds[i], sounds[cmd == 3 ? i - 1 : i + 1]);
        setSoundboardSounds(source, sounds);
    } else if (cmd == 5) {
        sounds.erase(sounds.begin() + (long)i);
        setSoundboardSounds(source, sounds);
    }
}

void CaptureWindow::drawSoundboardPanel(const Rect& b) {
    const Source* board = nullptr;
    for (auto& s : scenes_.sources())
        if (s.kind == SourceKind::Soundboard) {
            board = &s;
            break;
        }
    const float pad = 12;
    if (!board) {
        note(b, L"No soundboard yet",
             L"Buttons that play short sounds into your stream: a chime for a new follower, applause, a drum roll. It starts with 12 sounds made "
             L"by NorthRay (free to use on stream) and takes your own files too.");
        button("sb:add", {b.x + pad, b.bottom() - 44, b.w - 2 * pad, 32}, L"Add the soundboard", [this] { addSoundboard(); }, true);
        return;
    }
    const Id id = board->id;
    soundBoard_ = id;
    soundRows_.clear();
    const auto sounds = soundboardSounds(*board);
    audio::SoundboardPlayer* p = boards_.count(id) ? boards_[id].get() : nullptr;
    const std::size_t playing = p ? p->playing() : 0;
    // The buttons, in columns as wide as fit.
    const int cols = std::max(1, (int)((b.w - pad) / 96));
    const float bw = (b.w - pad * 2 - 8 * (cols - 1)) / cols, bh = 32;
    float y = b.y + 10;
    const float bottomBar = b.bottom() - 44 - 22;
    std::size_t shown = 0;
    for (std::size_t i = 0; i <= sounds.size(); ++i) {
        const int c = (int)(i % (size_t)cols);
        if (c == 0 && i > 0) y += bh + 8;
        if (y + bh > bottomBar) break;
        const Rect r{b.x + pad + c * (bw + 8), y, bw, bh};
        if (i == sounds.size()) {
            if (sounds.size() >= kMaxSounds) break;
            button("sb:new", r, L"+ Sound…", [this, id] {
                std::wstring path;
                if (!pickSound(hwnd(), path)) return;
                const Source* s2 = scenes_.source(id);
                if (!s2) return;
                auto list = soundboardSounds(*s2);
                std::wstring stem = std::filesystem::path(path).stem().wstring();
                list.push_back({narrow(stem).substr(0, 40), "", narrow(path)});
                setSoundboardSounds(id, list);
            });
            break;
        }
        ++shown;
        const std::string hid = "sb:" + std::to_string(i);
        const std::wstring problem = p ? p->problem(i) : L"";
        button(hid, r, widen(sounds[i].name), [this, id, i, problem] {
            if (!problem.empty()) {
                flash(L"That sound can't play: " + problem + L". Right-click it to choose another file.", 6);
                return;
            }
            if (boards_.count(id)) boards_[id]->play(i);
        });
        soundRows_.push_back({i, r});
        if (!problem.empty()) gfx_.fillRounded({r.x + 6, r.y + 6, 6, 6}, 3, pal_.live);
    }
    if (shown < sounds.size())
        gfx_.text(std::to_wstring(sounds.size() - shown) + L" more: make the panel bigger to see them", {b.x + pad, bottomBar + 2, b.w - 2 * pad, 18},
                  Font::Small, pal_.inkFaint, {Align::Center, false, true, true});
    const Rect stop{b.x + pad, b.bottom() - 40, b.w - 2 * pad, 30};
    button("sb:stop", stop, playing ? L"Stop all (" + std::to_wstring(playing) + L" playing)" : L"Stop all", [this, id] {
        if (boards_.count(id)) boards_[id]->stopAll();
    });
}

}  // namespace ncap
