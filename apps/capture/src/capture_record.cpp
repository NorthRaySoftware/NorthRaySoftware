// NorthRay Capture's window: recording.
#include "capture_window.hpp"

#include "log.hpp"

#include "cjui/dialog.hpp"

#include <mfapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>

namespace ncap {

using cjui::Align;
using cjui::Color;
using cjui::Font;
using cjui::MenuItem;
using cjui::narrow;
using cjui::Rect;
using cjui::widen;

std::wstring CaptureWindow::recordingsFolder() const {
    if (!settings_.recordingsFolder.empty()) return settings_.recordingsFolder;
    std::wstring folder;
    PWSTR videos = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &videos))) folder = std::wstring(videos) + L"\\NorthRay Capture";
    CoTaskMemFree(videos);
    return folder;
}

void CaptureWindow::startRecording() {
    if (recorder_.active()) return;
    output::RecordSettings rs;
    rs.folder = recordingsFolder();
    rs.video.width = scenes_.canvasWidth;
    rs.video.height = scenes_.canvasHeight;
    rs.video.fps = settings_.outputFps;
    rs.video.bitrateKbps = settings_.recordKbps;
    rs.profile = settings_.profile;
    rs.video.constantBitrate = false;
    rs.video.encoder = settings_.recordEncoder;
    rs.audioKbps = settings_.audioKbps;
    rs.tracks.clear();
    // Each track's name says what's on it, for editors that show track names.
    for (int t = 1; t <= 6; ++t) {
        if (!(settings_.recordTracks & (1u << (t - 1)))) continue;
        rs.tracks.push_back(t);
        std::vector<std::string> on;
        for (auto& s : scenes_.sources())
            if (hasSound(s.kind) && (audioBuses(s) & routing::track(t)) && !audioMuted(s)) on.push_back(s.name);
        std::string name = "Track " + std::to_string(t);
        if (!on.empty()) {
            name += ": ";
            for (size_t i = 0; i < on.size() && i < 3; ++i) name += (i ? ", " : "") + on[i];
            if (on.size() > 3) name += ", +" + std::to_string(on.size() - 3);
        }
        rs.trackNames.push_back(name);
    }
    MFStartup(MF_VERSION, MFSTARTUP_LITE);   // the encoders need Media Foundation (balanced when recording ends)
    std::wstring error;
    markers_.clear();
    if (!recorder_.start(engine_, mixer_, rs, error)) {
        MFShutdown();
        log::error("Recording couldn't start: " + narrow(error));
        cjui::messageBox(hwnd(), error, L"Recording couldn’t start", MB_OK | MB_ICONWARNING);
        return;
    }
    if (settings_.sceneChapters) markers_.push_back({0, liveSceneName()});   // the first chapter: the scene it starts on
    const output::RecordStatus st = recorder_.status();
    log::info("Recording started: " + narrow(st.path) + " with " + narrow(st.encoder) + (st.hardware ? " (hardware)" : " (software)") + ", " +
              std::to_string(rs.video.width) + "x" + std::to_string(rs.video.height) + "@" + std::to_string(rs.video.fps) + ", " +
              std::to_string(rs.video.bitrateKbps) + " kbps, " + std::to_string(rs.tracks.size()) + " audio track(s)");
    invalidate();
}

void CaptureWindow::stopRecording() {
    if (!recorder_.active()) return;
    recorder_.stop();
    recordTick();
    invalidate();
}

void CaptureWindow::recordTick() {
    {   // An MP4 copy that has finished.
        bool ok = false;
        std::wstring why;
        const std::wstring done = mp4Copy_.takeFinished(&ok, &why);
        if (!done.empty()) {
            if (ok) flash(L"MP4 copy made: " + done + (markers_.empty() ? L"" : L" (" + std::to_wstring(markers_.size()) + L" chapters)"), 8);
            else flash(L"The MP4 copy couldn't be made: " + why + L". The MKV recording is fine.", 10);
        }
    }
    // A recording that stopped by itself (the disk filled, a write failed) is wound up here.
    if (recorder_.active() && !recorder_.status().active) recorder_.stop();
    bool ok = false;
    std::wstring problem;
    const std::wstring file = recorder_.takeFinished(&ok, &problem);
    if (file.empty()) return;
    MFShutdown();
    if (ok) {
        const output::RecordStatus st = recorder_.status();
        log::info("Recording saved: " + narrow(file) + " (" + std::to_string((int)st.seconds) + " s, " + std::to_string(st.bytes / (1024 * 1024)) +
                  " MB, " + std::to_string(st.droppedFrames) + " late, " + std::to_string(st.encoderDropped) + " dropped by the encoder)");
        flash(L"Saved the recording: " + file + (problem.empty() ? L"" : L". " + problem) + (settings_.recordMp4 ? L" Making an MP4 copy\u2026" : L""), 10);
        if (settings_.recordMp4) mp4Copy_.start(file, markers_);
    } else {
        log::error("Recording failed: " + narrow(problem));
        cjui::messageBox(hwnd(), problem.empty() ? std::wstring(L"The recording couldn’t be saved.") : problem, L"Recording", MB_OK | MB_ICONWARNING);
    }
}

void CaptureWindow::chooseRecordingsFolder() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Where recordings go");
    IShellItem* start = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(recordingsFolder().c_str(), nullptr, IID_PPV_ARGS(&start)))) {
        dlg->SetFolder(start);
        start->Release();
    }
    if (SUCCEEDED(dlg->Show(hwnd()))) {
        IShellItem* item = nullptr;
        PWSTR path = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            settings_.recordingsFolder = path;
            settings_.save(settingsPath_);
            CoTaskMemFree(path);
        }
        if (item) item->Release();
    }
    dlg->Release();
    invalidate();
}

void CaptureWindow::encoderMenu(POINT screen) {
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    const auto list = encode::listH264Encoders();
    MFShutdown();
    std::vector<MenuItem> items = {MenuItem::item(L"Automatic: the best one found", 1, {}, true, settings_.recordEncoder.empty())};
    if (!list.empty()) items.push_back(MenuItem::sep());
    for (size_t i = 0; i < list.size() && i < 50; ++i)
        items.push_back(MenuItem::item(list[i].name, 10 + (UINT)i, list[i].hardware ? L"hardware" : L"software", true, list[i].name == settings_.recordEncoder));
    if (list.empty()) items.push_back(MenuItem::item(L"No H.264 encoders found on this PC", 2, {}, false));
    const UINT cmd = cjui::showMenu(hwnd(), items, screen, pal_);
    if (cmd == 1) settings_.recordEncoder.clear();
    else if (cmd >= 10 && cmd < 10 + list.size()) settings_.recordEncoder = list[cmd - 10].name;
    else return;
    settings_.save(settingsPath_);
    invalidate();
}

float CaptureWindow::drawRecordingSettings(float x, float y0, float colW) {
    float y = y0;
    const bool busy = recorder_.active();
    auto row = [&](const std::wstring& label, const std::wstring& help, float controlW) {
        gfx_.text(label, {x, y, colW - controlW - 20, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
        float h = 24;
        if (!help.empty()) h += gfx_.text(help, {x, y + 24, colW - controlW - 20, 80}, Font::Small, pal_.inkMuted);
        return h;
    };
    {
        const float h = row(L"Where recordings go", recordingsFolder(), 300);
        button("rec:folder", {x + colW - 300, y - 2, 140, 32}, L"Choose…", [this] { chooseRecordingsFolder(); });
        button("rec:open", {x + colW - 150, y - 2, 150, 32}, L"Open the folder", [this] {
            const std::wstring f = recordingsFolder();
            SHCreateDirectoryExW(nullptr, f.c_str(), nullptr);
            ShellExecuteW(hwnd(), L"open", f.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        });
        y += std::max(h, 34.0f) + 14;
    }
    {
        const float h = row(L"Video encoder", L"Hardware encoders (NVIDIA, AMD, Intel) leave the processor free for the game.", 300);
        const std::wstring cur = settings_.recordEncoder.empty() ? L"Automatic" : settings_.recordEncoder;
        button("rec:encoder", {x + colW - 300, y - 2, 300, 32}, cur, [this, busy] {
            if (busy) {
                flash(L"Stop recording to change the encoder.", 4);
                return;
            }
            POINT pt;
            GetCursorPos(&pt);
            encoderMenu(pt);
        });
        y += std::max(h, 34.0f) + 14;
    }
    {
        const float h = row(L"Video quality", L"The average bitrate. Higher is sharper and makes bigger files: 20 Mbit/s is about 9 GB an hour.", 330);
        const int v[] = {6000, 12000, 20000, 40000};
        int sel = -1;
        for (int i = 0; i < 4; ++i)
            if (settings_.recordKbps == v[i]) sel = i;
        segmented("rec:kbps", {x + colW - 330, y - 2, 330, 32}, {L"6 Mbit/s", L"12", L"20", L"40"}, sel, [this, busy](int i) {
            if (busy) return;
            const int vv[] = {6000, 12000, 20000, 40000};
            settings_.recordKbps = vv[i];
            settings_.save(settingsPath_);
        });
        y += std::max(h, 34.0f) + 14;
    }
    {
        const float h = row(L"Audio tracks in the file", L"Each is a separate track an editor can use on its own. Choose what goes on each in the Audio mixer.", 230);
        float cx = x + colW - 230;
        for (int t = 1; t <= 6; ++t) {
            const Rect chip{cx, y - 2, 34, 32};
            const bool on = (settings_.recordTracks & (1u << (t - 1))) != 0;
            const std::string id = "rec:track" + std::to_string(t);
            addHit(id, chip, [this, t, busy] {
                if (busy) return;
                const unsigned next = settings_.recordTracks ^ (1u << (t - 1));
                if (!next) {
                    flash(L"A recording needs at least one audio track.", 4);
                    return;
                }
                settings_.recordTracks = next;
                settings_.save(settingsPath_);
            });
            const float hv = hoverAmount(id);
            if (on) gfx_.fillRounded(chip, 7, pal_.ribbon.withAlpha(pal_.dark ? 0.30f : 0.22f));
            gfx_.strokeRounded(chip, 7, on ? pal_.ribbon : pal_.rule.mix(pal_.ink, 0.3f * hv), 1);
            gfx_.text(std::to_wstring(t), chip, Font::Body, on ? pal_.ink : pal_.inkFaint, {Align::Center, false, false, true});
            if (focused(id)) focusRing(chip);
            cx += 39;
        }
        y += std::max(h, 34.0f) + 14;
    }
    {
        const float h = row(L"MP4 copy", L"When a recording ends, an MP4 beside it for editors and uploading (copied, not re-encoded: no loss, and quick). "
                                         L"The MKV is kept: it survives crashes. Markers become chapters.", 60);
        toggle("rec:mp4", x + colW - 52, y + 4, settings_.recordMp4, [this] {
            settings_.recordMp4 = !settings_.recordMp4;
            settings_.save(settingsPath_);
        });
        y += std::max(h, 34.0f) + 14;
    }
    {
        const float h = row(L"Audio quality", L"AAC, per track.", 300);
        const int v[] = {96, 128, 160, 192};
        int sel = 2;
        for (int i = 0; i < 4; ++i)
            if (settings_.audioKbps == v[i]) sel = i;
        segmented("rec:akbps", {x + colW - 300, y - 2, 300, 32}, {L"96", L"128", L"160", L"192 kbit/s"}, sel, [this, busy](int i) {
            if (busy) return;
            const int vv[] = {96, 128, 160, 192};
            settings_.audioKbps = vv[i];
            settings_.save(settingsPath_);
        });
        y += std::max(h, 34.0f) + 14;
    }
    y += gfx_.text(L"Recordings are MKV files, which keep everything written so far if the PC crashes. Most editors and players open them; "
                   L"switch on MP4 copy above for the ones that don't.",
                   {x, y, colW, 60}, Font::Small, pal_.inkMuted) + 8;
    if (busy) y += gfx_.text(L"These can't change while recording.", {x, y, colW, 20}, Font::Small, cjui::hexColor("#E0566A")) + 6;
    return y - y0;
}

}  // namespace ncap
