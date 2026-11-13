#include "radio_app.hpp"

#include "cjui/format.hpp"
#include "cjui/http.hpp"
#include "cjui/packages.hpp"
#include "resource.h"
#include "version.hpp"

#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include "cj/json_safe.hpp"
#include <sstream>
#include <cctype>
#include <cstdio>
#include <thread>
#include "cj/ical.hpp"

namespace cjr {

using nlohmann::json;

static const float kRailWidth = 200;
static const float kTransportHeight = 68;
static const UINT WM_APP_STATION = WM_APP + 1;
static const UINT WM_APP_PLAYER = WM_APP + 2;
static const UINT WM_APP_RECORDINGS = WM_APP + 3;
static const UINT WM_APP_TWITCH = WM_APP + 4;
static const UINT WM_APP_CALENDAR = WM_APP + 5;
static const cj::UtcSeconds kCalendarRefresh = 15 * 60;
static const UINT_PTR kSlowTimer = 1;   // clock, countdowns
static const UINT_PTR kMeterTimer = 2;  // level meter while playing

static const Color kAccent = hexColor("#B4442C");
static const Color kLamp = hexColor("#F0533E");

// ------------------------------------------------------------------ helpers

static std::wstring exeDir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    std::wstring p(buf, n);
    return p.substr(0, p.find_last_of(L"\\/"));
}

static bool readFile(const std::wstring& path, std::string& out) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

static void writeFileAtomic(const std::wstring& path, const std::string& text) {
    const std::wstring tmp = path + L".tmp";
    {
        std::ofstream f(tmp.c_str(), std::ios::binary | std::ios::trunc);
        if (!f) return;
        f << text;
    }
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// ----------------------------------------------------------------- lifecycle

bool RadioApp::create(HINSTANCE inst, int show) {
    load();
    WindowSpec spec;
    spec.instance = inst;
    spec.className = L"CJRadioWindow";
    spec.title = L"CJ Radio";
    spec.iconId = IDI_APP;
    spec.width = 1060;
    spec.height = 720;
    spec.minWidth = 780;
    spec.minHeight = 540;
    spec.show = show;
    if (!createWindow(spec)) return false;
    applyTheme();
    player_.setNotify(hwnd(), WM_APP_PLAYER);
    ShowWindow(hwnd(), show);
    UpdateWindow(hwnd());
    SetTimer(hwnd(), kSlowTimer, 1000, nullptr);
    monitor_.start(station_, hwnd(), WM_APP_STATION);
    fetchRecordings();
    fetchCalendar();
    startTwitch();
    return true;
}

void RadioApp::startTwitch() {
    const std::string channel = station_.embedTwitchPlayer ? cj::station::twitchChannel(station_.twitchUrl) : std::string{};
    if (channel == twitchChannel_ && (twitch_.ready() || !twitchNote_.empty())) return;
    twitch_.destroy();
    twitchChannel_ = channel;
    twitchStatus_ = {};
    twitchPlaying_ = false;
    twitchNote_.clear();
    if (channel.empty()) return;
    twitch_.setSoundOnly(twitchSoundOnly_);
    if (!twitch_.create(hwnd(), exeDir(), stateDir_ + L"\\CJ Radio\\WebView2", channel, hwnd(), WM_APP_TWITCH))
        twitchNote_ = twitch_.error();
}

void RadioApp::onDestroy() {
    KillTimer(hwnd(), kSlowTimer);
    KillTimer(hwnd(), kMeterTimer);
    player_.stop();
    monitor_.stop();
    twitch_.destroy();
    saveVolume();
}

cj::UtcSeconds RadioApp::now() const { return cjui::nowUtc(); }

void RadioApp::load() {
    warnings_.clear();
    dataDir_ = exeDir() + L"\\data";
    // Settings live in %LOCALAPPDATA%\\NorthRay\\CJ Radio. Files from the old
    // CJ Community folder are copied across once (the originals are left alone).
    stateDir_ = cjui::northRayDataDir();
    std::wstring oldDir;
    {
        PWSTR local = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) oldDir = std::wstring(local) + L"\\CJ Community";
        CoTaskMemFree(local);
    }
    CreateDirectoryW(stateDir_.c_str(), nullptr);
    CreateDirectoryW((stateDir_ + L"\\CJ Radio").c_str(), nullptr);
    for (const wchar_t* f : {L"\\CJ Radio\\radio.json", L"\\CJ Radio\\presenter.json"})
        if (!oldDir.empty()) CopyFileW((oldDir + f).c_str(), (stateDir_ + f).c_str(), TRUE);  // TRUE: never overwrite

    std::string text;
    // Theme and demo data: CJ Radio's own setting if it has one, else CJ Philip
    // Community's (or the old launcher's), else the defaults.
    demoData_ = true;
    themeChoice_ = 0;
    if (readFile(stateDir_ + L"\\CJ Philip Community\\settings.json", text) ||
        (!oldDir.empty() && readFile(oldDir + L"\\settings.json", text))) {
        json j = json::parse(text, nullptr, false);
        if (j.is_object()) {
            // Read defensively: CJ Philip Community keeps its theme as 0/1/2, older versions as text.
            if (j.contains("demoData") && j["demoData"].is_boolean()) demoData_ = j["demoData"].get<bool>();
            if (j.contains("theme")) {
                const json& t = j["theme"];
                if (t.is_number_integer()) themeChoice_ = std::clamp(t.get<int>(), 0, 2);
                else if (t.is_string()) themeChoice_ = t == "light" ? 1 : t == "dark" ? 2 : 0;
            }
        }
    }
    if (readFile(stateDir_ + L"\\CJ Radio\\radio.json", text)) {
        json j = json::parse(text, nullptr, false);
        if (j.is_object()) {
            player_.setVolume(cj::valueOr(j, "volume", 0.8f));
            player_.setMuted(cj::valueOr(j, "muted", false));
            twitchSoundOnly_ = cj::valueOr(j, "twitchSoundOnly", false);
        }
    }
    presenter_ = {};
    if (readFile(stateDir_ + L"\\CJ Radio\\presenter.json", text)) {
        json j = json::parse(text, nullptr, false);
        if (j.is_object()) {
            presenter_.enabled = cj::valueOr(j, "enabled", false);
            presenter_.callInDirectorUrl = cj::valueOr(j, "callInDirectorUrl", "");
            if (presenter_.callInDirectorUrl.rfind("https://", 0) != 0) presenter_.callInDirectorUrl.clear();
        }
    }

    station_ = {};
    if (readFile(dataDir_ + L"\\radio\\station.json", text)) station_ = cj::station::parseConfig(text, warnings_);
    else warnings_.push_back("radio/station.json is missing.");

    const cj::UtcSeconds t = now();
    fileSchedule_ = {};
    if (readFile(dataDir_ + L"\\radio\\schedule.json", text)) fileSchedule_ = cj::radio::loadSchedule(text, t);
    if (!station_.calendarUrl.empty()) demoData_ = false;  // a real calendar: never mix in demo programmes
    if (demoData_ && readFile(dataDir_ + L"\\demo\\radio-schedule.demo.json", text)) {
        cj::radio::Schedule demo = cj::radio::loadSchedule(text, t);
        fileSchedule_.programmes.insert(fileSchedule_.programmes.end(), demo.programmes.begin(), demo.programmes.end());
    }
    for (auto& w : fileSchedule_.warnings) warnings_.push_back("schedule: " + w);

    // The calendar copy from last time, so the schedule shows offline too.
    calendarProgrammes_.clear();
    calendarStatus_.clear();
    calendarOk_ = false;
    if (!station_.calendarUrl.empty()) {
        if (readFile(stateDir_ + L"\\CJ Radio\\calendar.ics", text)) {
            applyCalendar(text);
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            if (GetFileAttributesExW((stateDir_ + L"\\CJ Radio\\calendar.ics").c_str(), GetFileExInfoStandard, &fa)) {
                const unsigned long long ft = (static_cast<unsigned long long>(fa.ftLastWriteTime.dwHighDateTime) << 32) |
                                              fa.ftLastWriteTime.dwLowDateTime;
                calendarCheckedUtc_ = static_cast<cj::UtcSeconds>(ft / 10000000ULL) - 11644473600LL;
            }
            calendarStatus_ = L"Showing the copy saved " + cjui::formatDay(calendarCheckedUtc_) + L" at " +
                              cjui::formatTime(calendarCheckedUtc_) + L". Checking for changes...";
        } else {
            calendarStatus_ = L"Loading the schedule...";
        }
    }
    rebuildSchedule();
}

void RadioApp::rebuildSchedule() {
    schedule_ = fileSchedule_;
    schedule_.programmes.insert(schedule_.programmes.end(), calendarProgrammes_.begin(), calendarProgrammes_.end());
    std::sort(schedule_.programmes.begin(), schedule_.programmes.end(),
              [](auto& a, auto& b) { return a.startUtc < b.startUtc; });
}

// Calendar programmes from a year ago to 13 months ahead (the month view
// goes back and forward a year).
void RadioApp::applyCalendar(const std::string& ics) {
    const cj::UtcSeconds t = now();
    auto r = cj::ical::toProgrammes(ics, t - 400 * 86400, t + 400 * 86400, fileSchedule_.series);
    calendarProgrammes_ = r.programmes;
    calendarOk_ = r.valid;
    for (auto& w : r.warnings) warnings_.push_back("calendar: " + w);
    rebuildSchedule();
}

void RadioApp::fetchCalendar() {
    if (station_.calendarUrl.empty() || calendarFetching_) return;
    calendarFetching_ = true;
    calendarNextFetch_ = now() + kCalendarRefresh;
    const std::string url = station_.calendarUrl;
    HWND target = hwnd();
    std::thread([url, target] {
        auto* res = new cjui::HttpResult(cjui::httpGet(url, 15000, 4 * 1024 * 1024));
        if (!PostMessageW(target, WM_APP_CALENDAR, 0, reinterpret_cast<LPARAM>(res))) delete res;
    }).detach();
}

void RadioApp::applyTheme() {
    const bool dark = themeChoice_ == 2 || (themeChoice_ == 0 && systemPrefersDark());
    applyPalette(makePalette(dark));
}

void RadioApp::navigate(Page p) {
    if (p == Page::Presenter && !presenter_.enabled) return;
    if (page_ != p) {
        page_ = p;
        resetScroll();
        clearFocus();
    }
    invalidate();
}

void RadioApp::saveVolume() {
    json j;
    j["volume"] = player_.volume();
    j["muted"] = player_.muted();
    j["twitchSoundOnly"] = twitchSoundOnly_;
    writeFileAtomic(stateDir_ + L"\\CJ Radio\\radio.json", j.dump(2));
}

namespace {
struct RecordingsResult {
    std::vector<cj::station::Recording> list;
    std::wstring status;
    bool failed = false;       // couldn't reach the source: keep what's shown
};
}  // namespace

// Recordings that ship with the app: MP3 files in the Recordings folder next to
// CJRadio.exe. Name them "2026-11-14 The CJ Philip Show.mp3" for a date and
// title, and optionally add recordings.json there for descriptions.
void RadioApp::scanLocalRecordings() {
    localRecordings_.clear();
    const std::wstring dir = exeDir() + L"\\Recordings";
    json extra;
    std::string text;
    if (readFile(dir + L"\\recordings.json", text)) {
        json j = json::parse(text, nullptr, false);
        if (j.is_object() && j.contains("recordings") && j["recordings"].is_array()) extra = j["recordings"];
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.mp3").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring name = fd.cFileName;
        const std::string file = narrow(name);
        cj::station::Recording r;
        r.id = "local:" + file;
        r.url = narrow(dir + L"\\" + name);
        std::string stem = file.substr(0, file.size() - 4);
        // Date from the name, else the file's own date.
        if (stem.size() >= 10 && cj::parseLocalIso(stem.substr(0, 10) + "T12:00")) {
            r.recordedUtc = *cj::parseUtcIso(stem.substr(0, 10) + "T12:00:00Z");
            stem = stem.substr(10);
            while (!stem.empty() && (stem[0] == ' ' || stem[0] == '_' || stem[0] == '-')) stem.erase(0, 1);
        } else {
            ULARGE_INTEGER t;
            t.LowPart = fd.ftLastWriteTime.dwLowDateTime;
            t.HighPart = fd.ftLastWriteTime.dwHighDateTime;
            r.recordedUtc = (cj::UtcSeconds)(t.QuadPart / 10000000ULL) - 11644473600LL;
        }
        for (char& c : stem) if (c == '_') c = ' ';
        r.title = stem.empty() ? "CJ Radio recording" : stem;
        for (const auto& e : extra) {
            if (!e.is_object() || cj::valueOr(e, "file", "") != file) continue;
            r.title = cj::valueOr(e, "title", r.title);
            r.description = cj::valueOr(e, "description", "");
            r.presenter = cj::valueOr(e, "presenter", "");
            r.category = cj::valueOr(e, "category", "");
            r.durationMinutes = std::clamp(cj::valueOr(e, "durationMinutes", 0), 0, 24 * 60);
        }
        localRecordings_.push_back(std::move(r));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void RadioApp::mergeRecordings() {
    recordings_ = localRecordings_;
    recordings_.insert(recordings_.end(), remoteRecordings_.begin(), remoteRecordings_.end());
    std::stable_sort(recordings_.begin(), recordings_.end(),
                     [](const auto& a, const auto& b) { return a.recordedUtc > b.recordedUtc; });
    if (!recordings_.empty()) recordingsStatus_.clear();
}

void RadioApp::fetchRecordings() {
    scanLocalRecordings();
    remoteRecordings_.clear();
    recordingsStatus_ = L"No recordings have been published yet.";
    mergeRecordings();
    if (!station_.recordingsGithub.empty()) { fetchGithubRecordings(); return; }
    if (station_.recordingsUrl.empty()) return;
    const std::string src = station_.recordingsUrl;
    if (src.rfind("https://", 0) != 0 && src.rfind("http://", 0) != 0) {
        // A file shipped with the app (relative to data\radio).
        std::string text;
        std::vector<std::string> w;
        if (src.find("..") == std::string::npos && readFile(dataDir_ + L"\\radio\\" + widen(src), text))
            remoteRecordings_ = cj::station::parseRecordings(text, w);
        mergeRecordings();
        return;
    }
    if (recordings_.empty()) recordingsStatus_ = L"Loading recordings...";
    HWND target = hwnd();
    std::thread([src, target] {
        auto* res = new RecordingsResult;
        cjui::HttpResult r = cjui::httpGet(src, 10000);
        std::vector<std::string> w;
        if (r.ok()) {
            res->list = cj::station::parseRecordings(r.body, w);
            if (res->list.empty()) res->status = L"No recordings have been published yet.";
        } else {
            res->status = L"Couldn't load the recordings list. " + r.error;
        }
        if (!PostMessageW(target, WM_APP_RECORDINGS, 0, reinterpret_cast<LPARAM>(res))) delete res;
    }).detach();
}

// Recordings kept in a GitHub repository: MP3 files in it (top level, or a
// "Recordings" folder) and MP3s attached to its releases. The last list is
// kept so recordings still show offline.
void RadioApp::fetchGithubRecordings() {
    const std::wstring cache = stateDir_ + L"\\CJ Radio\\recordings-cache.json";
    {
        std::string text;
        std::vector<std::string> w;
        if (readFile(cache, text)) remoteRecordings_ = cj::station::parseRecordings(text, w);
        mergeRecordings();
    }
    if (recordings_.empty()) recordingsStatus_ = L"Loading recordings...";
    const std::string repo = station_.recordingsGithub;
    HWND target = hwnd();
    std::thread([repo, cache, target] {
        auto* res = new RecordingsResult;
        const std::string api = "https://api.github.com/repos/" + repo;
        auto encodePath = [](const std::string& p) {
            std::string o;
            for (unsigned char c : p) {
                if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
                else { char b[4]; std::snprintf(b, sizeof b, "%%%02X", c); o += b; }
            }
            return o;
        };
        cjui::HttpResult top = cjui::httpGet(api + "/contents", 15000);
        if (top.status == 404) {
            res->status = L"The recordings repository couldn't be found, or it's private.";
            res->failed = true;
        } else if (!top.ok()) {
            res->status = L"Couldn't reach GitHub for the recordings. " + top.error;
            res->failed = true;
        } else {
            auto listing = cj::station::parseGithubContents(top.body);
            std::vector<cj::station::Recording> all = listing.recordings;
            std::string indexUrl = listing.indexUrl;
            for (size_t i = 0; i < listing.folders.size() && i < 4; ++i) {
                cjui::HttpResult sub = cjui::httpGet(api + "/contents/" + encodePath(listing.folders[i]), 15000);
                if (!sub.ok()) continue;
                auto more = cj::station::parseGithubContents(sub.body);
                all.insert(all.end(), more.recordings.begin(), more.recordings.end());
                if (indexUrl.empty()) indexUrl = more.indexUrl;
            }
            cjui::HttpResult rel = cjui::httpGet(api + "/releases?per_page=100", 15000);
            if (rel.ok()) {
                auto more = cj::station::parseGithubReleases(rel.body);
                all.insert(all.end(), more.begin(), more.end());
            }
            if (!indexUrl.empty()) {
                cjui::HttpResult idx = cjui::httpGet(indexUrl, 15000);
                if (idx.ok()) cj::station::applyRecordingDetails(all, idx.body);
            }
            std::sort(all.begin(), all.end(), [](auto& a, auto& b) { return a.recordedUtc > b.recordedUtc; });
            res->list = all;
            if (all.empty()) res->status = L"No recordings have been published yet.";
            writeFileAtomic(cache, cj::station::recordingsToJson(all));
        }
        if (!PostMessageW(target, WM_APP_RECORDINGS, 0, reinterpret_cast<LPARAM>(res))) delete res;
    }).detach();
}

// ---------------------------------------------------------------- playback

cj::radio::StationStatus RadioApp::status() const {
    return cj::radio::computeStatus(schedule_, cj::station::combineLive(nowPlaying_, twitchStatus_), now());
}

void RadioApp::listenLive() {
    if (!station_.serverConfigured()) return;
    pausedLive_ = false;
    playingRecordingId_.clear();
    const auto st = status();
    playingTitle_ = st.current ? widen(st.current->title) : L"CJ Radio";
    if (playingTitle_.empty()) playingTitle_ = L"CJ Radio";
    twitch_.pause();
    player_.play(station_.streamUrl, true);
    SetTimer(hwnd(), kMeterTimer, 50, nullptr);
}

void RadioApp::playRecording(const cj::station::Recording& r) {
    pausedLive_ = false;
    playingRecordingId_ = r.id;
    playingTitle_ = widen(r.title) + L", " + cjui::formatDay(r.recordedUtc);
    twitch_.pause();
    player_.play(r.url, false);
    SetTimer(hwnd(), kMeterTimer, 50, nullptr);
}

bool RadioApp::isPaused() const { return pausedLive_ || (player_.paused() && player_.state() != StreamPlayer::State::Stopped); }

void RadioApp::togglePause() {
    const auto ps = player_.state();
    const bool active = ps == StreamPlayer::State::Connecting || ps == StreamPlayer::State::Buffering ||
                        ps == StreamPlayer::State::Playing || ps == StreamPlayer::State::Reconnecting;
    if (pausedLive_) {                       // live: pick up at the live point
        pausedLive_ = false;
        if (status().state == cj::radio::StationState::Live) listenLive();
    } else if (active && player_.live()) {
        player_.stop();
        pausedLive_ = true;
    } else if (active || player_.paused()) {
        player_.setPaused(!player_.paused());
    } else if (twitchPlaying_) {
        twitch_.pause();
    } else if (page_ == Page::Listen && twitch_.ready() && twitchStatus_.online) {
        twitch_.play();
    } else if (status().state == cj::radio::StationState::Live && station_.serverConfigured()) {
        listenLive();
    }
    invalidate();
}

void RadioApp::stopPlayback() {
    pausedLive_ = false;
    player_.stop();
    playingRecordingId_.clear();
    KillTimer(hwnd(), kMeterTimer);
    invalidate();
}

LRESULT RadioApp::onMessage(UINT msg, WPARAM wp, LPARAM lp, bool& handled) {
    handled = true;
    if (msg == WM_APP_STATION) {
        const bool wasLive = cj::station::toLiveSignal(nowPlaying_).onAir;
        nowPlaying_ = monitor_.latest();
        const bool isLive = cj::station::toLiveSignal(nowPlaying_).onAir;
        // The server says the show is over: stop rather than play silence.
        if (wasLive && !isLive && nowPlaying_.reachable && player_.live() &&
            player_.state() != StreamPlayer::State::Stopped) {
            stopPlayback();
            flash(L"The broadcast has ended. Thanks for listening.", 8);
        }
        invalidate();
        return 0;
    }
    if (msg == WM_APP_PLAYER) {
        const auto s = player_.state();
        if (s == StreamPlayer::State::Finished) {
            KillTimer(hwnd(), kMeterTimer);
            flash(L"End of recording.", 5);
        } else if (s == StreamPlayer::State::Error) {
            KillTimer(hwnd(), kMeterTimer);
        }
        invalidate();
        return 0;
    }
    if (msg == WM_APP_TWITCH) {
        using E = TwitchView::Event;
        switch ((E)wp) {
            case E::Ready: break;
            case E::Online:
                if (!twitchStatus_.online) twitchStatus_.sinceUtc = now();
                twitchStatus_.known = true;
                twitchStatus_.online = true;
                break;
            case E::Offline:
                if (twitchStatus_.online && twitchPlaying_) flash(L"The broadcast has ended. Thanks for listening.", 8);
                twitchStatus_.known = true;
                twitchStatus_.online = false;
                twitchStatus_.sinceUtc = 0;
                break;
            case E::Playing:
                twitchPlaying_ = true;
                // Only one thing plays at a time.
                if (player_.state() != StreamPlayer::State::Stopped) {
                    player_.stop();
                    playingRecordingId_.clear();
                    KillTimer(hwnd(), kMeterTimer);
                }
                break;
            case E::Paused: twitchPlaying_ = false; break;
            case E::SoundOnlyOn:
            case E::SoundOnlyOff: soundOnlyNote_.clear(); break;
            case E::SoundOnlyUnavailable:
                soundOnlyNote_ = L"This stream isn't offering Twitch's Audio Only setting right now, so the video is still playing.";
                break;
            case E::Failed:
                twitchPlaying_ = false;
                twitchStatus_ = {};  // unknown again: never guess live
                twitchNote_ = L"The Twitch player stopped working. Press F5 to try again.";
                twitch_.destroy();
                break;
        }
        invalidate();
        return 0;
    }
    if (msg == WM_APP_CALENDAR) {
        std::unique_ptr<cjui::HttpResult> res(reinterpret_cast<cjui::HttpResult*>(lp));
        calendarFetching_ = false;
        const bool looksLikeCalendar = res->ok() && res->body.find("BEGIN:VCALENDAR") != std::string::npos;
        if (looksLikeCalendar) {
            warnings_.erase(std::remove_if(warnings_.begin(), warnings_.end(),
                                           [](const std::string& w) { return w.rfind("calendar: ", 0) == 0; }),
                            warnings_.end());
            applyCalendar(res->body);
            writeFileAtomic(stateDir_ + L"\\CJ Radio\\calendar.ics", res->body);
            calendarCheckedUtc_ = now();
            calendarStatus_.clear();
        } else {
            std::wstring why = res->status == 404 || res->status == 403
                ? L"The calendar isn't public yet, so the schedule can't be read."
                : res->ok() ? L"The calendar link didn't return a calendar."
                            : L"Couldn't reach the calendar. " + res->error;
            if (calendarCheckedUtc_ > 0)
                why += L" Showing the copy saved " + cjui::formatDay(calendarCheckedUtc_) + L" at " + cjui::formatTime(calendarCheckedUtc_) + L".";
            calendarStatus_ = why;
        }
        invalidate();
        return 0;
    }
    if (msg == WM_APP_RECORDINGS) {
        std::unique_ptr<RecordingsResult> res(reinterpret_cast<RecordingsResult*>(lp));
        if (res->failed && !remoteRecordings_.empty()) {
            // Offline or GitHub unreachable: keep the saved list, and say so quietly.
            mergeRecordings();
            flash(L"Showing the saved list of recordings. " + res->status, 6);
            invalidate();
            return 0;
        }
        remoteRecordings_ = res->list;
        recordingsStatus_ = res->status;
        mergeRecordings();
        invalidate();
        return 0;
    }
    handled = false;
    return 0;
}

void RadioApp::onTimer(UINT_PTR) {
    if (!station_.calendarUrl.empty() && calendarNextFetch_ != 0 && now() >= calendarNextFetch_) fetchCalendar();
    invalidate();
}

bool RadioApp::onKeyDown(WPARAM vk, bool ctrl, bool) {
    if (ctrl && vk >= '1' && vk <= '4') { navigate(static_cast<Page>(vk - '1')); return true; }
    const bool playing = player_.state() != StreamPlayer::State::Stopped &&
                         player_.state() != StreamPlayer::State::Finished && player_.state() != StreamPlayer::State::Error;
    if ((ctrl && vk == 'P') || vk == VK_MEDIA_PLAY_PAUSE) {
        (void)playing;
        togglePause();
        return true;
    }
    if (vk == VK_MEDIA_STOP) { stopPlayback(); return true; }
    if (ctrl && vk == 'M') { player_.setMuted(!player_.muted()); saveVolume(); return true; }
    if (ctrl && (vk == VK_UP || vk == VK_DOWN)) {
        player_.setVolume(player_.volume() + (vk == VK_UP ? 0.05f : -0.05f));
        saveVolume();
        return true;
    }
    if (vk == VK_F5) {
        load();
        applyTheme();
        monitor_.start(station_, hwnd(), WM_APP_STATION);
        fetchRecordings();
        fetchCalendar();
        if (!twitchNote_.empty()) { twitchNote_.clear(); twitch_.destroy(); twitchChannel_.clear(); }
        startTwitch();
        flash(L"Refreshed the schedule and station status.");
        return true;
    }
    return false;
}

// ------------------------------------------------------------------- frame

void RadioApp::drawFrame(float W, float H) {
    const float pad = W - kRailWidth > 860 ? 40 : 28;
    const float top = beginScroll({kRailWidth, 0, W - kRailWidth, H - kTransportHeight});
    Rect page{kRailWidth + pad, top + 28, std::min(W - kRailWidth - pad * 2, 920.0f), 0};
    float h = 0;
    playerSlotValid_ = false;
    switch (page_) {
        case Page::Listen: h = pageListen(page); break;
        case Page::Schedule: h = pageSchedule(page); break;
        case Page::Recordings: h = pageRecordings(page); break;
        case Page::Presenter: h = pagePresenter(page); break;
    }
    endScroll(h + 56);
    drawRail({0, 0, kRailWidth, H - kTransportHeight});
    drawTransport({0, H - kTransportHeight, W, kTransportHeight});
    placeTwitch(W, H);
}

void RadioApp::placeTwitch(float W, float H) {
    if (!playerSlotValid_ || page_ != Page::Listen || IsIconic(hwnd())) {
        twitch_.hide();
        return;
    }
    const float s = gfx_.dpiScale();
    auto px = [s](float v) { return (LONG)std::lround(v * s); };
    const RECT bounds{px(playerSlot_.x), px(playerSlot_.y), px(playerSlot_.right()), px(playerSlot_.bottom())};
    const RECT visible{px(kRailWidth), 0, px(W), px(H - kTransportHeight)};
    twitch_.place(bounds, visible);
}

void RadioApp::drawRail(const Rect& r) {
    gfx_.fill(r, pal_.console);
    for (int i = 0; i < (int)(r.w / 6) - 2; ++i) {
        const float tx = r.x + 12 + i * 6;
        gfx_.line(tx, r.y + 6, tx, r.y + (i % 5 == 0 ? 13.0f : 9.0f), pal_.consoleRule, 1);
    }
    const float cjW = gfx_.measureWidth(L"CJ", Font::Wordmark);
    gfx_.text(L"CJ", {r.x + 22, r.y + 24, 60, 30}, Font::Wordmark, pal_.consoleInk, {Align::Left, false});
    gfx_.text(L"Radio", {r.x + 22 + cjW + 5, r.y + 24, 120, 30}, Font::WordmarkLight, pal_.consoleInk, {Align::Left, false});

    const bool live = status().state == cj::radio::StationState::Live;
    if (live) {
        // A slow glow while on air: the target flips each second and eases between.
        const bool up = (GetTickCount64() / 1000) % 2 == 0;
        const float glow = anim("rail#lamp", up ? 1.0f : 0.35f, 2.2f);
        gfx_.circle(r.x + 27, r.y + 66, 9.0f, kLamp.withAlpha(0.18f * glow), true);
        gfx_.circle(r.x + 27, r.y + 66, 4.5f, kLamp.mix(pal_.consoleInk, 0.25f * glow), true);
    } else {
        gfx_.circle(r.x + 27, r.y + 66, 4.0f, pal_.consoleMuted, false, 1.2f);
    }
    gfx_.text(live ? L"ON AIR" : L"OFF AIR", {r.x + 38, r.y + 58, 120, 16}, Font::Label,
              live ? pal_.consoleInk : pal_.consoleMuted, {Align::Left, false});

    struct Item { Page page; const wchar_t* label; };
    std::vector<Item> items = {{Page::Listen, L"Listen"}, {Page::Schedule, L"Schedule"}, {Page::Recordings, L"Recordings"}};
    if (presenter_.enabled) items.push_back({Page::Presenter, L"Presenter"});
    float y = r.y + 96;
    // The selection slides between items.
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].page == page_) {
            const float sy = slideTo("nav#sel", r.y + 96 + 36.0f * i);
            gfx_.fillRounded({r.x + 10, sy, r.w - 20, 34}, 3, pal_.consoleRule);
            gfx_.fill({r.x + 10, sy + 9, 3, 16}, kAccent.mix(kLamp, 0.3f));
        }
    for (const Item& it : items) {
        const std::string id = "nav:" + std::to_string((int)it.page);
        Rect row{r.x + 10, y, r.w - 20, 34};
        addHit(id, row, [this, p = it.page] { navigate(p); });
        const bool sel = page_ == it.page;
        const float hv = hoverAmount(id);
        if (!sel && hv > 0.01f) gfx_.fillRounded(row, 3, pal_.consoleRule.withAlpha(0.5f * hv));
        gfx_.text(it.label, {row.x + 16, row.y, row.w - 20, row.h}, sel ? Font::BodyStrong : Font::Body,
                  sel ? pal_.consoleInk : pal_.consoleMuted, {Align::Left, false, true, true});
        if (focused(id)) focusRing(row);
        y += 36;
    }
    gfx_.text(L"NorthRay Software", {r.x + 22, r.bottom() - 46, r.w - 30, 18}, Font::Small, pal_.consoleMuted,
              {Align::Left, false, true});
    gfx_.text(L"Version " CJR_VERSION_WSTR, {r.x + 22, r.bottom() - 28, r.w - 30, 18}, Font::Small,
              pal_.consoleMuted.withAlpha(0.7f), {Align::Left, false, true});
}

void RadioApp::drawTransport(const Rect& r) {
    gfx_.fill(r, pal_.console.mix(hexColor("#000000"), 0.25f));
    gfx_.line(r.x, r.y + 0.5f, r.right(), r.y + 0.5f, pal_.consoleRule);
    const auto ps = player_.state();
    const bool active = ps == StreamPlayer::State::Connecting || ps == StreamPlayer::State::Buffering ||
                        ps == StreamPlayer::State::Playing || ps == StreamPlayer::State::Reconnecting;
    const bool canListenLive = status().state == cj::radio::StationState::Live && station_.serverConfigured();

    // Play / pause, and stop
    const float cx = r.x + 40, cy = r.y + r.h / 2;
    const Rect btn{cx - 20, cy - 20, 40, 40};
    const bool paused = isPaused();
    const bool twitchCan = page_ == Page::Listen && twitch_.ready() && (twitchPlaying_ || twitchStatus_.online);
    const bool showPause = (active && !player_.paused()) || (!active && !paused && twitchPlaying_);
    if (active || paused || canListenLive || twitchCan) {
        const std::string id = "transport:play";
        addHit(id, btn, [this] { togglePause(); });
        const float hv = hoverAmount(id);
        // The button swells a touch on hover; its icon morphs between play and pause.
        const float rad = 20 + 1.5f * hv;
        gfx_.circle(cx, cy, rad, pal_.consoleInk.mix(pal_.console, 0.1f - 0.1f * hv), true);
        const float m = anim("transport#icon", showPause ? 1.0f : 0.0f, 22.0f);
        if (m > 0.5f) {
            const float a = (m - 0.5f) * 2;
            gfx_.fill({cx - 7, cy - 8, 5, 16}, pal_.console.withAlpha(a));
            gfx_.fill({cx + 2, cy - 8, 5, 16}, pal_.console.withAlpha(a));
        } else {
            const float a = (0.5f - m) * 2;
            gfx_.fillTriangle(cx - 5, cy - 8, cx - 5, cy + 8, cx + 9, cy, pal_.console.withAlpha(a));
        }
        if (focused(id)) focusRing(btn);
    } else {
        gfx_.circle(cx, cy, 19, pal_.consoleRule, false, 1.5f);
        gfx_.fillTriangle(cx - 5, cy - 8, cx - 5, cy + 8, cx + 9, cy, pal_.consoleRule);
    }
    // Stop: only while something of ours is playing or paused.
    const float stopA = anim("transport#stop", (active || paused) ? 1.0f : 0.0f, 14.0f);
    if (stopA > 0.01f) {
        const Rect sb{cx + 26, cy - 13, 26, 26};
        const std::string sid = "transport:stop";
        if (active || paused) addHit(sid, sb, [this] { stopPlayback(); });
        const float hv = hoverAmount(sid);
        gfx_.circle(sb.x + 13, cy, 13, pal_.consoleRule.mix(pal_.consoleMuted, 0.4f * hv).withAlpha(stopA), true);
        gfx_.fill({sb.x + 8.5f, cy - 4.5f, 9, 9}, pal_.consoleInk.withAlpha(stopA));
        if (focused(sid)) focusRing(sb);
    }

    // What's playing
    const float tx = r.x + 76 + 34 * anim("transport#stop", (active || paused) ? 1.0f : 0.0f, 14.0f);
    const float rightW = 330;
    const float tw = std::max(80.0f, r.w - 76 - rightW - 20);
    std::wstring title, line2;
    Color line2Color = pal_.consoleMuted;
    switch (ps) {
        case StreamPlayer::State::Stopped:
            if (pausedLive_) { title = playingTitle_; line2 = L"Paused  ·  play to rejoin the live show"; break; }
            if (twitchPlaying_) { title = L"CJ Radio on Twitch"; line2 = L"Playing in the Twitch player"; break; }
            title = canListenLive ? L"CJ Radio is live" : L"Not playing";
            line2 = canListenLive ? L"Press play to listen" : L"CJ Radio is off air";
            break;
        case StreamPlayer::State::Connecting: title = playingTitle_; line2 = L"Connecting..."; break;
        case StreamPlayer::State::Buffering: title = playingTitle_; line2 = L"Buffering..."; break;
        case StreamPlayer::State::Reconnecting:
            title = playingTitle_;
            line2 = L"Connection lost. Reconnecting...";
            line2Color = pal_.consoleInk;
            break;
        case StreamPlayer::State::Playing: {
            title = playingTitle_;
            if (player_.live()) {
                line2 = L"Live";
                if (nowPlaying_.listeners >= 0)
                    line2 += L"  ·  " + std::to_wstring(nowPlaying_.listeners) +
                             (nowPlaying_.listeners == 1 ? L" listener" : L" listeners");
            } else {
                line2 = std::wstring(player_.paused() ? L"Paused" : L"Catch-up") + L"  ·  " +
                        cjui::formatElapsed(0, (cj::UtcSeconds)player_.secondsPlayed());
            }
            break;
        }
        case StreamPlayer::State::Finished: title = playingTitle_; line2 = L"Finished"; break;
        case StreamPlayer::State::Error:
            title = L"Couldn't play";
            line2 = player_.error();
            line2Color = pal_.consoleInk;
            break;
    }
    if (ps == StreamPlayer::State::Playing && player_.live()) {
        gfx_.circle(tx + 4, r.y + 22, 3.5f, kLamp, true);
        gfx_.text(title, {tx + 14, r.y + 13, tw - 14, 20}, Font::BodyStrong, pal_.consoleInk, {Align::Left, false, true});
    } else {
        gfx_.text(title, {tx, r.y + 13, tw, 20}, Font::BodyStrong, pal_.consoleInk, {Align::Left, false, true});
    }
    gfx_.text(line2, {tx, r.y + 35, tw, 18}, Font::Small, line2Color, {Align::Left, false, true});

    // Level meter, mute, volume
    const float mx = r.right() - rightW;
    const float lvl = active ? player_.level() : 0.0f;
    const int segs = 12;
    const float db = lvl > 0.0001f ? 20.0f * std::log10(lvl) : -60.0f;   // -60..0 dB
    const int lit = (int)std::round(std::clamp((db + 48.0f) / 48.0f, 0.0f, 1.0f) * segs);
    for (int i = 0; i < segs; ++i) {
        const Color on = i >= segs - 2 ? kLamp : i >= segs - 4 ? hexColor("#D9B65A") : hexColor("#9BC08A");
        gfx_.fill({mx + i * 7.0f, cy - 6, 5, 12}, i < lit ? on : pal_.consoleRule);
    }
    // Mute
    const float sx = mx + segs * 7.0f + 18;
    const Rect mute{sx, cy - 14, 28, 28};
    const std::string muteId = "transport:mute";
    addHit(muteId, mute, [this] { player_.setMuted(!player_.muted()); saveVolume(); });
    const Color ic = pal_.consoleMuted.mix(pal_.consoleInk, hoverAmount(muteId));
    gfx_.fill({sx + 6, cy - 4, 5, 8}, ic);
    gfx_.fillTriangle(sx + 9, cy - 4, sx + 16, cy - 10, sx + 16, cy, ic);
    gfx_.fillTriangle(sx + 9, cy + 4, sx + 16, cy + 10, sx + 16, cy, ic);
    if (player_.muted()) gfx_.line(sx + 4, cy + 10, sx + 24, cy - 10, kLamp, 2);
    else gfx_.arc(sx + 15, cy, 7, -0.9f, 0.9f, ic, 1.5f);
    if (focused(muteId)) focusRing(mute);
    // Volume
    const Rect vol{sx + 38, cy - 10, 110, 20};
    // Draw the slider in console colours.
    const Palette saved = pal_;
    pal_.rule = pal_.consoleRule;
    pal_.ink = pal_.consoleInk;
    pal_.paper = pal_.console;
    slider("transport:volume", vol, player_.volume(), [this](float v) { player_.setVolume(v); player_.setMuted(false); saveVolume(); });
    pal_ = saved;
    const std::wstring pct = player_.muted() ? L"Muted" : std::to_wstring((int)std::round(player_.volume() * 100)) + L"%";
    gfx_.text(pct, {vol.right() + 10, cy - 9, 50, 18}, Font::Small, pal_.consoleMuted, {Align::Left, false});

    if (!flashText().empty()) {
        gfx_.text(flashText(), {tx + tw * 0.45f, r.y + 35, tw * 0.55f, 18}, Font::SmallStrong, flashColor(pal_.consoleInk),
                  {Align::Right, false, true});
    }
}

float RadioApp::drawHeader(const Rect& r, const std::wstring& title, const std::wstring& subtitle) {
    gfx_.text(title, {r.x, r.y, r.w - 120, 36}, Font::Title, pal_.ink, {Align::Left, false, true});
    float h = 38;
    if (!subtitle.empty()) h += gfx_.text(subtitle, {r.x, r.y + h, std::min(r.w, 640.0f), 200}, Font::Body, pal_.inkMuted) + 4;
    if (demoData_) {
        float w = gfx_.measureWidth(L"DEMO DATA", Font::Label) + 10;
        tag(r.right() - w, r.y + 10, L"DEMO DATA", pal_.demo);
    }
    return h + 18;
}

// ------------------------------------------------------------ programme row

float RadioApp::programmeRow(const Rect& r, const cj::radio::Programme& p, bool showDate) {
    using cj::radio::Status;
    const float timeW = showDate ? 116 : 72;
    if (showDate) {
        gfx_.text(cjui::formatRelative(p.startUtc, now()), {r.x, r.y, timeW - 8, 18}, Font::Small, pal_.inkMuted,
                  {Align::Left, false, true});
        gfx_.text(cjui::formatTime(p.startUtc), {r.x, r.y + 18, timeW - 8, 22}, Font::Heading, pal_.ink, {Align::Left, false});
    } else {
        gfx_.text(cjui::formatTime(p.startUtc), {r.x, r.y, timeW - 8, 22}, Font::Heading, pal_.ink, {Align::Left, false});
    }
    const float x = r.x + timeW, w = r.w - timeW;
    float tagsW = 0, tw = 0;
    float tx = r.right();
    if (p.demo) { tw = gfx_.measureWidth(L"DEMO", Font::Label) + 10; tx -= tw; tag(tx, r.y + 1, L"DEMO", pal_.demo); tagsW += tw + 6; tx -= 6; }
    if (p.status != Status::Confirmed) {
        const std::wstring s = p.status == Status::Cancelled ? L"CANCELLED" : L"POSTPONED";
        tw = gfx_.measureWidth(s, Font::Label) + 10;
        tx -= tw;
        tag(tx, r.y + 1, s, pal_.ribbon);
        tagsW += tw + 6;
    }
    TextOpts o;
    o.strike = p.status != Status::Confirmed;
    o.wrap = false;
    o.ellipsis = true;
    float h = gfx_.text(widen(p.title), {x, r.y, w - tagsW - 8, 22}, Font::BodyStrong,
                        p.status == Status::Confirmed ? pal_.ink : pal_.inkMuted, o);
    h = std::max(h, 20.0f) + 2;
    std::wstring meta = widen(cj::radio::kindLabel(p.kind));
    if (!p.presenter.empty()) meta += L"  ·  " + widen(p.presenter);
    if (p.durationMinutes > 0 && p.status == Status::Confirmed) meta += L"  ·  " + cjui::formatDuration(p.durationMinutes);
    const std::wstring hint = cjui::ukHint(p.startUtc);
    if (!hint.empty()) meta += hint;
    h += gfx_.text(meta, {x, r.y + h, w, 18}, Font::Small, pal_.inkMuted, {Align::Left, false, true}) + 2;
    const std::string& body = p.status == Status::Confirmed ? p.description : (p.note.empty() ? p.description : p.note);
    if (!body.empty()) h += gfx_.text(widen(body), {x, r.y + h + 2, std::min(w, 620.0f), 200}, Font::Small, pal_.inkMuted) + 2;
    return std::max(h, showDate ? 44.0f : 24.0f);
}

// ----------------------------------------------------------------- on air

float RadioApp::onAirPanel(const Rect& r) {
    using cj::radio::StationState;
    const auto st = status();
    const bool live = st.state == StationState::Live;
    const float pad = 26;
    const float x = r.x + pad, w = r.w - pad * 2;

    // Measure first, then draw the panel behind the content.
    struct Line { float h; std::function<void(float)> draw; };
    std::vector<Line> lines;
    const TextOpts single{Align::Left, false, true};

    lines.push_back({28, [=, this](float y) {
        gfx_.circle(x + 6, y + 9, live ? 6.0f : 5.0f, live ? kLamp : pal_.consoleMuted, live, 1.5f);
        gfx_.text(live ? L"ON AIR" : st.state == StationState::StartingSoon ? L"STARTING SOON" : L"OFF AIR",
                  {x + 20, y + 1, 200, 18}, Font::Label, live ? pal_.consoleInk : pal_.consoleMuted, {Align::Left, false});
    }});

    if (live) {
        const auto sig = cj::station::combineLive(nowPlaying_, twitchStatus_);
        const std::wstring title = st.current && !st.current->title.empty() ? widen(st.current->title) : L"Live on CJ Radio";
        lines.push_back({44, [=, this](float y) { gfx_.text(title, {x, y, w, 40}, Font::Title, pal_.consoleInk, single); }});
        std::wstring meta;
        if (!nowPlaying_.presenter.empty()) meta = L"With " + widen(nowPlaying_.presenter);
        if (sig.startedUtc > 0) meta += std::wstring(meta.empty() ? L"" : L"  ·  ") + L"On for " + cjui::formatElapsed(sig.startedUtc, now());
        if (sig.listeners >= 0)
            meta += (meta.empty() ? L"" : L"  ·  ") + std::to_wstring(sig.listeners) + (sig.listeners == 1 ? L" listener" : L" listeners");
        if (!meta.empty()) lines.push_back({26, [=, this](float y) { gfx_.text(meta, {x, y, w, 20}, Font::Body, pal_.consoleMuted, single); }});
        if (!station_.serverConfigured()) {
            lines.push_back({40, [=, this](float y) {
                gfx_.text(twitchPlaying_ ? L"Playing in the Twitch player below." : L"Press play on the Twitch player below to listen.",
                          {x, y + 12, w, 20}, Font::Body, pal_.consoleInk, single);
            }});
        } else
        lines.push_back({62, [=, this](float y) {
            const auto ps = player_.state();
            const bool playingLive = player_.live() && (ps == StreamPlayer::State::Playing || ps == StreamPlayer::State::Connecting ||
                                                        ps == StreamPlayer::State::Buffering || ps == StreamPlayer::State::Reconnecting);
            const Rect b{x, y + 14, 150, 38};
            const std::string id = "onair:listen";
            addHit(id, b, [this, playingLive] { if (playingLive) togglePause(); else listenLive(); });
            const bool hov = hovered(id);
            gfx_.fillRounded(b, 3, playingLive ? (hov ? pal_.consoleRule : pal_.console) : (hov ? kLamp : kAccent));
            if (playingLive) gfx_.strokeRounded(b, 3, pal_.consoleMuted);
            gfx_.text(playingLive ? L"Pause" : pausedLive_ ? L"Rejoin live" : L"Listen live", b, Font::BodyStrong, pal_.consoleInk,
                      {Align::Center, false, true, true});
            if (focused(id)) focusRing(b);
            // Also live elsewhere: links only, they open in the browser.
            float lx = b.right() + 22;
            auto link = [&](const std::string& url, const wchar_t* label, const char* lid) {
                if (url.empty()) return;
                const float lw = gfx_.measureWidth(label, Font::Small);
                const Rect lr{lx, y + 24, lw, 18};
                addHit(lid, lr, [url] { cjui::openInBrowser(url); });
                gfx_.text(label, {lr.x, lr.y, lw + 2, 18}, Font::Small, pal_.consoleMuted.mix(pal_.consoleInk, hoverAmount(lid)), {Align::Left, false});
                gfx_.line(lr.x, lr.bottom() + 1, lr.right(), lr.bottom() + 1, pal_.consoleMuted.withAlpha(0.5f));
                if (focused(lid)) focusRing(lr);
                lx += lw + 18;
            };
            link(station_.twitchUrl, L"Watch on Twitch", "onair:twitch");
            link(station_.youtubeUrl, L"Watch on YouTube", "onair:youtube");
        }});
    } else {
        lines.push_back({44, [=, this](float y) {
            gfx_.text(st.state == StationState::StartingSoon ? L"Starting soon" : L"Currently off air", {x, y, w, 40},
                      Font::Title, pal_.consoleInk, single);
        }});
        if (st.overdue) {
            const std::wstring t = widen(st.overdue->title) + L" was due to start at " + cjui::formatTime(st.overdue->startUtc) +
                                   L", but it isn't live yet. It'll appear here as soon as it starts.";
            const float h = gfx_.measureHeight(t, w, Font::Body);
            lines.push_back({h + 10, [=, this](float y) { gfx_.text(t, {x, y, w, h + 4}, Font::Body, pal_.consoleInk); }});
        }
        if (st.next) {
            const auto p = *st.next;
            lines.push_back({30, [=, this](float y) {
                gfx_.text(L"NEXT CONFIRMED PROGRAMME", {x, y + 8, w, 16}, Font::Label, pal_.consoleMuted, {Align::Left, false});
            }});
            lines.push_back({26, [=, this](float y) {
                float tw = 0;
                if (p.demo) {
                    tw = gfx_.measureWidth(L"DEMO", Font::Label) + 10;
                    const Rect tr{x + w - tw, y + 2, tw, 18};
                    gfx_.strokeRounded(tr, 2, hexColor("#D9B65A").withAlpha(0.8f));
                    gfx_.text(L"DEMO", tr, Font::Label, hexColor("#D9B65A"), {Align::Center, false, false, true});
                }
                gfx_.text(widen(p.title), {x, y, w - tw - 10, 24}, Font::Heading, pal_.consoleInk, single);
            }});
            lines.push_back({24, [=, this](float y) {
                gfx_.text(cjui::formatWhen(p.startUtc) + cjui::ukHint(p.startUtc) + L"  ·  " +
                              cjui::formatCountdown(p.startUtc, now()),
                          {x, y, w, 20}, Font::Body, pal_.consoleInk, single);
            }});
        } else {
            const std::wstring t = L"No programme has been confirmed yet. Regular shows go out on Saturdays, and each one is listed in the schedule once its time is announced.";
            const float h = gfx_.measureHeight(t, std::min(w, 560.0f), Font::Body);
            lines.push_back({h + 8, [=, this](float y) { gfx_.text(t, {x, y, std::min(w, 560.0f), h + 4}, Font::Body, pal_.consoleInk); }});
        }
        if (!station_.twitchUrl.empty() || !station_.youtubeUrl.empty()) {
            lines.push_back({34, [=, this](float y) {
                float lx = x;
                gfx_.text(L"Follow CJ on", {lx, y + 10, 100, 18}, Font::Small, pal_.consoleMuted, {Align::Left, false});
                lx += gfx_.measureWidth(L"Follow CJ on", Font::Small) + 10;
                auto link = [&](const std::string& url, const wchar_t* label, const char* lid) {
                    if (url.empty()) return;
                    const float lw = gfx_.measureWidth(label, Font::Small);
                    const Rect lr{lx, y + 10, lw, 18};
                    addHit(lid, lr, [url] { cjui::openInBrowser(url); });
                    gfx_.text(label, {lr.x, lr.y, lw + 2, 18}, Font::Small, pal_.consoleMuted.mix(pal_.consoleInk, hoverAmount(lid)), {Align::Left, false});
                    gfx_.line(lr.x, lr.bottom() + 1, lr.right(), lr.bottom() + 1, pal_.consoleMuted.withAlpha(0.5f));
                    if (focused(lid)) focusRing(lr);
                    lx += lw + 16;
                };
                link(station_.twitchUrl, L"Twitch", "offair:twitch");
                link(station_.youtubeUrl, L"YouTube", "offair:youtube");
            }});
        }
        std::wstring note;
        if (!station_.serverConfigured() && twitchChannel_.empty())
            note = L"CJ Radio's broadcast server hasn't been set up yet, so live programmes can't be played here.";
        else if (!nowPlaying_.reachable && monitor_.hasResult()) note = L"Can't reach the CJ Radio server right now, so live status is unknown.";
        if (!note.empty()) {
            const float h = gfx_.measureHeight(note, w, Font::Small);
            lines.push_back({h + 22, [=, this](float y) {
                gfx_.line(x, y + 8.5f, x + w, y + 8.5f, pal_.consoleRule);
                gfx_.text(note, {x, y + 14, w, h + 4}, Font::Small, pal_.consoleMuted);
            }});
        }
    }

    float total = pad + 4;
    for (auto& l : lines) total += l.h;
    total += pad - 4;
    gfx_.fillRounded({r.x, r.y, r.w, total}, 4, pal_.console);
    if (pal_.dark) gfx_.strokeRounded({r.x, r.y, r.w, total}, 4, pal_.consoleRule);
    for (int i = 0; i < (int)(r.w / 6) - 3; ++i) {
        const float tx = r.x + 12 + i * 6;
        gfx_.line(tx, r.y + 5, tx, r.y + (i % 5 == 0 ? 12.0f : 8.0f), pal_.consoleRule, 1);
    }
    float y = r.y + pad + 4;
    for (auto& l : lines) { l.draw(y); y += l.h; }
    return total;
}

// ------------------------------------------------------------------ Listen

float RadioApp::pageListen(const Rect& r) {
    float y = r.y;
    if (demoData_) {
        float w = gfx_.measureWidth(L"DEMO DATA", Font::Label) + 10;
        tag(r.right() - w, y - 18, L"DEMO DATA", pal_.demo);
    }
    y += onAirPanel({r.x, y, r.w, 0}) + 20;

    // Twitch's own player, at full size with its own controls.
    if (!twitchChannel_.empty()) {
        // Sound only keeps Twitch's player on screen at its smallest supported size.
        const float pw = twitchSoundOnly_ ? std::min(r.w, 534.0f) : r.w;
        const float ph = twitchSoundOnly_ ? 300.0f : std::clamp(pw * 9.0f / 16.0f, 300.0f, 480.0f);
        const Rect slot{r.x, y, pw, ph};
        if (!twitchNote_.empty()) {
            gfx_.fillRounded(slot, 4, pal_.sunken);
            const float th = gfx_.measureHeight(twitchNote_, std::min(pw - 60, 520.0f), Font::Body);
            gfx_.text(twitchNote_, {slot.x + (pw - std::min(pw - 60, 520.0f)) / 2, slot.y + ph / 2 - th / 2 - 14,
                                    std::min(pw - 60, 520.0f), th + 4},
                      Font::Body, pal_.ink, {Align::Center});
            const std::string url = station_.twitchUrl;
            const float lw = gfx_.measureWidth(L"Watch on Twitch instead", Font::Small);
            linkText("listen:twitch-fallback", slot.x + pw / 2 - lw / 2, slot.y + ph / 2 + th / 2, L"Watch on Twitch instead",
                     [url] { cjui::openInBrowser(url); });
        } else {
            gfx_.fill(slot, hexColor("#111111"));
            gfx_.text(L"Loading the Twitch player...", slot, Font::Body, hexColor("#AAAAAA"), {Align::Center, false, false, true});
            playerSlot_ = slot;
            playerSlotValid_ = true;
        }
        y += ph + 10;
        if (twitchNote_.empty()) {
        segmented("listen:sound", {r.x, y, 260, 30}, {L"Video and sound", L"Sound only"}, twitchSoundOnly_ ? 1 : 0, [this](int i) {
            twitchSoundOnly_ = i == 1;
            soundOnlyNote_.clear();
            twitch_.setSoundOnly(twitchSoundOnly_);
            saveVolume();
            invalidate();
        });
        y += 38;
        const std::wstring caption = !soundOnlyNote_.empty() ? soundOnlyNote_
            : twitchSoundOnly_ ? L"Sound only uses Twitch's Audio Only setting, which saves data. The player's own controls start, stop and adjust the sound."
                               : L"Player provided by Twitch. Its own controls start, stop and adjust the sound.";
        y += gfx_.text(caption, {r.x, y, std::min(r.w, 640.0f), 60}, Font::Small, soundOnlyNote_.empty() ? pal_.inkMuted : pal_.ink) + 22;
        } else {
            y += 18;
        }
    } else {
        y += 8;
    }

    // Call-ins: only while live, only when the host has opened them.
    const auto st = status();
    if (st.state == cj::radio::StationState::Live && station_.callIn.enabled) {
        y += sectionLabel(r.x, y, r.w, L"Call in");
        const bool discord = cj::station::callInService(station_.callIn.guestUrl) == cj::station::CallInService::Discord;
        const std::wstring notice = station_.callIn.notice.empty()
            ? L"If CJ puts you on air, you'll be heard live and in the recording of this show, including on Twitch and YouTube."
            : widen(station_.callIn.notice);
        y += gfx_.text(notice, {r.x, y, std::min(r.w, 640.0f), 120}, Font::Body, pal_.ink) + 8;
        y += gfx_.text(discord
                           ? L"Calls happen on CJ's Discord, which needs a Discord account. Join the call-in Stage, press Request to Speak, and wait until CJ brings you up. While you're on air, pause this player: the broadcast runs a few seconds behind, and you'll hear CJ directly in Discord."
                           : L"You'll wait in a queue until you're let on. While you're on air, pause this player: the broadcast runs a few seconds behind, and you'll hear CJ directly through the call.",
                       {r.x, y, std::min(r.w, 640.0f), 120}, Font::Small, pal_.inkMuted) + 12;
        const std::string cid = "callin:age";
        const std::wstring ageText = L"I'm " + std::to_wstring(station_.callIn.minimumAge) + L" or over and I agree to be broadcast and recorded";
        const float aw = gfx_.measureWidth(ageText, Font::Body) + 30;
        const Rect box{r.x, y, aw, 24};
        addHit(cid, box, [this] { callerAgeConfirmed_ = !callerAgeConfirmed_; });
        gfx_.strokeRounded({r.x, y + 3, 18, 18}, 3, pal_.inkMuted.mix(pal_.ink, hoverAmount(cid)), 1.5f);
        if (callerAgeConfirmed_) {
            gfx_.line(r.x + 4, y + 12, r.x + 8, y + 17, pal_.ink, 2);
            gfx_.line(r.x + 8, y + 17, r.x + 15, y + 7, pal_.ink, 2);
        }
        gfx_.text(ageText, {r.x + 28, y + 2, aw, 20}, Font::Body, pal_.ink, {Align::Left, false});
        if (focused(cid)) focusRing(box);
        y += 38;
        if (callerAgeConfirmed_) {
            button("callin:request", {r.x, y, 180, 34}, discord ? L"Call in on Discord" : L"Request to call", [this, discord] {
                if (cjui::openInBrowser(station_.callIn.guestUrl))
                    flash(discord ? L"Opened CJ's Discord. Join the Stage and press Request to Speak." : L"Opened the call-in queue in your browser.", 8);
            }, true);
        } else {
            disabledButton({r.x, y, 180, 34}, discord ? L"Call in on Discord" : L"Request to call");
        }
        y += 58;
    }

    // Coming up
    y += sectionLabel(r.x, y, r.w, L"Coming up");
    const auto upcoming = cj::radio::publicProgrammes(schedule_, now(), now() + 60 * 86400);
    int n = 0;
    for (const auto& p : upcoming) {
        if (n++ == 3) break;
        y += programmeRow({r.x, y, r.w, 0}, p, true) + 14;
        gfx_.line(r.x, y - 7.5f, r.right(), y - 7.5f, pal_.rule);
    }
    if (upcoming.empty()) {
        y += gfx_.text(L"Nothing is scheduled yet.", {r.x, y, r.w, 22}, Font::Body, pal_.inkMuted) + 10;
    }
    linkText("listen:schedule", r.x, y + 2, L"Full schedule", [this] { navigate(Page::Schedule); });
    y += 40;

    // Latest recording
    y += sectionLabel(r.x, y, r.w, L"Catch up");
    if (!recordings_.empty()) {
        const auto& rec = recordings_.front();
        gfx_.text(widen(rec.title), {r.x, y, r.w - 140, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
        gfx_.text(cjui::formatDay(rec.recordedUtc) + (rec.durationMinutes ? L"  ·  " + cjui::formatDuration(rec.durationMinutes) : L""),
                  {r.x, y + 22, r.w - 140, 18}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
        const bool cur = playingRecordingId_ == rec.id && player_.state() != StreamPlayer::State::Stopped &&
                         player_.state() != StreamPlayer::State::Finished;
        button("listen:latest", {r.right() - 110, y + 4, 110, 32}, cur ? (player_.paused() ? L"Resume" : L"Pause") : L"Play",
               [this, rec, cur] { if (cur) togglePause(); else playRecording(rec); });
        y += 52;
        linkText("listen:recordings", r.x, y, L"All recordings", [this] { navigate(Page::Recordings); });
        y += 30;
    } else {
        y += gfx_.text(recordingsStatus_, {r.x, y, r.w, 40}, Font::Body, pal_.inkMuted) + 10;
    }
    return y - r.y;
}

// ---------------------------------------------------------------- Schedule

float RadioApp::pageSchedule(const Rect& r) {
    float y = r.y + drawHeader(r, L"Schedule",
                               L"Regular programmes go out on Saturdays, with occasional extra shows on other days. Only confirmed programmes are listed, in your local time.");
    segmented("sched:view", {r.x, y, 220, 30}, {L"Upcoming", L"Month"}, monthView_ ? 1 : 0, [this](int i) {
        monthView_ = i == 1;
        invalidate();
    });
    if (!station_.calendarSubscribeUrl.empty()) {
        const std::string sub = station_.calendarSubscribeUrl;
        const float lw = gfx_.measureWidth(L"Add to my calendar", Font::Body);
        linkText("sched:subscribe", r.right() - lw, y + 6, L"Add to my calendar", [sub] { cjui::openInBrowser(sub); });
    }
    y += 40;
    if (!calendarStatus_.empty())
        y += gfx_.text(calendarStatus_, {r.x, y, std::min(r.w, 680.0f), 60}, Font::Small, pal_.inkMuted) + 12;
    else if (calendarCheckedUtc_ > 0)
        y += gfx_.text(L"Up to date with the CJ Radio calendar (checked " + cjui::formatTime(calendarCheckedUtc_) + L").",
                       {r.x, y, std::min(r.w, 680.0f), 40}, Font::Small, pal_.inkFaint) + 12;
    y += 8;
    if (monthView_) return y + scheduleMonth({r.x, y, r.w, 0}) - r.y;

    const auto list = cj::radio::publicProgrammes(schedule_, now() - 3 * 3600, now() + 120 * 86400);
    int lastDay = -1;
    for (const auto& p : list) {
        if (p.endUtc() < now()) continue;
        const int day = cjui::localDayKey(p.startUtc);
        if (day != lastDay) {
            if (lastDay != -1) y += 12;
            const std::wstring d = cjui::formatDay(p.startUtc);
            gfx_.text(d, {r.x, y, r.w, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false});
            const std::wstring rel = cjui::formatRelative(p.startUtc, now());
            if (rel == L"Today" || rel == L"Tomorrow")
                gfx_.text(rel, {r.x + gfx_.measureWidth(d, Font::BodyStrong) + 10, y + 1, 100, 20}, Font::Small, pal_.inkMuted,
                          {Align::Left, false});
            y += 26;
            gfx_.line(r.x, y + 0.5f, r.right(), y + 0.5f, pal_.rule);
            y += 12;
            lastDay = day;
        }
        y += programmeRow({r.x, y, r.w, 0}, p, false) + 16;
    }
    if (lastDay == -1) {
        y += gfx_.text(L"Nothing is scheduled at the moment. Saturday programmes and any extra shows will appear here once they're confirmed.",
                       {r.x, y, std::min(r.w, 620.0f), 80}, Font::Body, pal_.inkMuted);
    }
    return y - r.y;
}

float RadioApp::scheduleMonth(const Rect& r) {
    // Work out the month to show in local time.
    const cj::UtcSeconds t = now();
    const int key = cjui::localDayKey(t);
    int year = key / 10000, month = (key / 100) % 100;
    month += monthOffset_;
    while (month > 12) { month -= 12; ++year; }
    while (month < 1) { month += 12; --year; }
    static const wchar_t* kMonths[] = {L"January", L"February", L"March", L"April", L"May", L"June", L"July",
                                       L"August", L"September", L"October", L"November", L"December"};

    float y = r.y;
    button("month:prev", {r.x, y, 34, 30}, L"‹", [this] { monthOffset_--; invalidate(); });
    gfx_.text(std::wstring(kMonths[month - 1]) + L" " + std::to_wstring(year), {r.x + 46, y, 240, 30}, Font::Heading,
              pal_.ink, {Align::Left, false, false, true});
    button("month:next", {r.x + 250, y, 34, 30}, L"›", [this] { monthOffset_++; invalidate(); });
    if (monthOffset_ != 0) linkText("month:today", r.x + 300, y + 6, L"This month", [this] { monthOffset_ = 0; invalidate(); });
    y += 44;

    static const wchar_t* kHead[] = {L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat", L"Sun"};
    const float cw = r.w / 7;
    for (int i = 0; i < 7; ++i)
        gfx_.text(kHead[i], {r.x + i * cw + 8, y, cw - 8, 18}, i == 5 ? Font::SmallStrong : Font::Small,
                  i == 5 ? pal_.ink : pal_.inkMuted, {Align::Left, false});
    y += 24;

    const int firstWd = (cj::weekday(year, month, 1) + 6) % 7;  // Monday = 0
    const int days = (int)(cj::daysFromCivil(month == 12 ? year + 1 : year, month == 12 ? 1 : month + 1, 1) -
                           cj::daysFromCivil(year, month, 1));
    const int rows = (firstWd + days + 6) / 7;
    const float ch = 96;
    const auto list = cj::radio::publicProgrammes(schedule_, t - 400 * 86400, t + 400 * 86400);
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < 7; ++col) {
            const int d = row * 7 + col - firstWd + 1;
            const Rect c{r.x + col * cw, y + row * ch, cw, ch};
            if (col == 5) gfx_.fill(c, pal_.sunken.withAlpha(0.6f));  // Saturdays, the usual broadcast day
            gfx_.stroke(c, pal_.rule, 1);
            if (d < 1 || d > days) continue;
            const int dayKey = year * 10000 + month * 100 + d;
            const bool today = dayKey == key;
            if (today) gfx_.strokeRounded(c.inset(1), 2, pal_.ribbon, 2);
            gfx_.text(std::to_wstring(d), {c.x + 8, c.y + 6, 40, 18}, today ? Font::SmallStrong : Font::Small,
                      today ? pal_.ribbon : pal_.inkMuted, {Align::Left, false});
            float py = c.y + 28;
            for (const auto& p : list) {
                if (cjui::localDayKey(p.startUtc) != dayKey) continue;
                if (py > c.bottom() - 18) break;
                TextOpts o;
                o.wrap = false;
                o.ellipsis = true;
                o.strike = p.status != cj::radio::Status::Confirmed;
                gfx_.text(cjui::formatTime(p.startUtc) + L" " + widen(p.title), {c.x + 8, py, cw - 14, 18}, Font::Small,
                          p.status == cj::radio::Status::Confirmed ? pal_.ink : pal_.inkMuted, o);
                py += 20;
            }
        }
    }
    y += rows * ch + 12;
    y += gfx_.text(L"Shaded columns are Saturdays. A Saturday with nothing listed has no confirmed programme.",
                   {r.x, y, r.w, 40}, Font::Small, pal_.inkMuted);
    return y - r.y;
}

// -------------------------------------------------------------- Recordings

float RadioApp::pageRecordings(const Rect& r) {
    float y = r.y + drawHeader(r, L"Recordings",
                               L"Catch up on past programmes. Recordings are published after each show, once the host has checked them.");
    if (recordings_.empty()) {
        y += gfx_.text(recordingsStatus_, {r.x, y, std::min(r.w, 620.0f), 60}, Font::Body, pal_.inkMuted);
        return y - r.y;
    }
    for (const auto& rec : recordings_) {
        const float bw = 100;
        gfx_.text(cjui::formatDay(rec.recordedUtc), {r.x, y, r.w - bw - 20, 18}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
        float h = 20;
        h += gfx_.text(widen(rec.title), {r.x, y + h, r.w - bw - 20, 24}, Font::Heading, pal_.ink, {Align::Left, false, true}) + 2;
        std::wstring meta;
        if (!rec.presenter.empty()) meta = widen(rec.presenter);
        if (rec.durationMinutes) meta += (meta.empty() ? L"" : L"  ·  ") + cjui::formatDuration(rec.durationMinutes);
        if (!rec.category.empty()) meta += (meta.empty() ? L"" : L"  ·  ") + widen(rec.category);
        if (!meta.empty()) h += gfx_.text(meta, {r.x, y + h, r.w - bw - 20, 18}, Font::Small, pal_.inkMuted, {Align::Left, false, true}) + 4;
        if (!rec.description.empty())
            h += gfx_.text(widen(rec.description), {r.x, y + h, std::min(r.w - bw - 20, 620.0f), 200}, Font::Body, pal_.ink) + 4;
        if (!rec.transcriptUrl.empty()) {
            const std::string url = rec.transcriptUrl;
            linkText("rec:transcript:" + rec.id, r.x, y + h + 2, L"Transcript", [url] { cjui::openInBrowser(url); });
            h += 26;
        }
        const bool thisOne = playingRecordingId_ == rec.id && player_.state() != StreamPlayer::State::Stopped &&
                             player_.state() != StreamPlayer::State::Finished;
        button("rec:play:" + rec.id, {r.right() - bw, y + 18, bw, 32}, thisOne ? (player_.paused() ? L"Resume" : L"Pause") : L"Play",
               [this, rec, thisOne] { if (thisOne) togglePause(); else playRecording(rec); }, !thisOne);
        y += std::max(h, 60.0f) + 16;
        gfx_.line(r.x, y - 8.5f, r.right(), y - 8.5f, pal_.rule);
    }
    return y - r.y;
}

// --------------------------------------------------------------- Presenter

float RadioApp::pagePresenter(const Rect& r) {
    float y = r.y + drawHeader(r, L"Presenter",
                               L"Keep using OBS and SE.Live as you do now. This page shows whether your audio is reaching CJ Radio, which is what listeners in the app and on the website hear.");

    const bool configured = station_.serverConfigured();
    const bool reachable = nowPlaying_.reachable;
    const bool online = reachable && nowPlaying_.stationOnline;
    const bool feed = reachable && nowPlaying_.presenterLive;
    const bool live = feed && online;

    // Big state
    const Rect badge{r.x, y, r.w, 64};
    gfx_.fillRounded(badge, 4, live ? kLamp.mix(hexColor("#000000"), 0.15f) : pal_.console);
    gfx_.circle(badge.x + 30, badge.y + 32, 8, live ? hexColor("#FFFFFF") : pal_.consoleMuted, live, 1.5f);
    gfx_.text(live ? L"LIVE ON CJ RADIO" : L"NOT LIVE ON CJ RADIO", {badge.x + 52, badge.y, badge.w - 60, badge.h}, Font::Heading,
              live ? hexColor("#FFFFFF") : pal_.consoleInk, {Align::Left, false, true, true});
    y += 64 + 24;

    // Checklist from real server data.
    y += sectionLabel(r.x, y, r.w, L"Station check");
    auto check = [&](bool ok, bool unknown, const std::wstring& label, const std::wstring& detail) {
        const Color c = unknown ? pal_.inkFaint : ok ? hexColor("#3F7D4E") : pal_.live;
        if (unknown) gfx_.circle(r.x + 9, y + 10, 6, c, false, 1.5f);
        else if (ok) {
            gfx_.circle(r.x + 9, y + 10, 8, c, true);
            gfx_.line(r.x + 5, y + 10, r.x + 8, y + 13, hexColor("#FFFFFF"), 2);
            gfx_.line(r.x + 8, y + 13, r.x + 13, y + 7, hexColor("#FFFFFF"), 2);
        } else {
            gfx_.circle(r.x + 9, y + 10, 8, c, true);
            gfx_.line(r.x + 6, y + 7, r.x + 12, y + 13, hexColor("#FFFFFF"), 2);
            gfx_.line(r.x + 12, y + 7, r.x + 6, y + 13, hexColor("#FFFFFF"), 2);
        }
        gfx_.text(label, {r.x + 30, y, 260, 20}, Font::BodyStrong, pal_.ink, {Align::Left, false});
        const float h = gfx_.text(detail, {r.x + 300, y, r.w - 300, 60}, Font::Body, pal_.inkMuted);
        y += std::max(24.0f, h) + 12;
    };
    check(configured, false, L"Server set up", configured ? L"Connected to " + widen(station_.nowPlayingUrl.substr(0, station_.nowPlayingUrl.find('/', 8)))
                                                       : L"No server address in radio/station.json yet.");
    check(reachable, !configured, L"Server reachable", reachable ? L"Answering" : configured ? widen(nowPlaying_.error.empty() ? "Checking..." : nowPlaying_.error) : L"");
    check(online, !reachable, L"Station online", online ? L"AzuraCast reports the station running" : reachable ? L"AzuraCast reports the station offline" : L"");
    check(feed, !reachable, L"Your audio feed", feed ? L"Connected" + (nowPlaying_.presenter.empty() ? std::wstring() : L" as " + widen(nowPlaying_.presenter))
                                                      : reachable ? L"Not connected. Start the CJ Radio output in OBS." : L"");
    if (reachable && nowPlaying_.listeners >= 0)
        check(true, false, L"Listeners", std::to_wstring(nowPlaying_.listeners) + L" on CJ Radio (Twitch and YouTube viewers are counted separately)");
    linkText("pres:refresh", r.x, y, L"Check again", [this] { monitor_.pollNow(); flash(L"Checking the station..."); });
    y += 40;

    y += sectionLabel(r.x, y, r.w, L"Going live");
    static const wchar_t* steps[] = {
        L"In OBS, start your stream with SE.Live as usual for Twitch and YouTube.",
        L"Start the CJ Radio feed: in OBS press Start Recording (it's set to send audio to the CJ Radio server), or start BUTT if you use that instead.",
        L"Wait for \"Your audio feed: Connected\" above and the LIVE ON CJ RADIO bar. Listeners can't hear you on CJ Radio until then.",
        L"To finish, stop the CJ Radio feed first, then end your stream. CJ Radio switches to Off Air on its own.",
    };
    int i = 1;
    for (const wchar_t* s : steps) {
        gfx_.text(std::to_wstring(i++) + L".", {r.x, y, 24, 20}, Font::BodyStrong, pal_.ink, {Align::Left, false});
        y += gfx_.text(s, {r.x + 26, y, std::min(r.w - 26, 640.0f), 80}, Font::Body, pal_.ink) + 10;
    }
    y += 16;

    y += sectionLabel(r.x, y, r.w, L"Call-ins");
    if (cj::station::callInService(station_.callIn.guestUrl) == cj::station::CallInService::Discord) {
        static const wchar_t* discordSteps[] = {
            L"In Discord, make a Stage channel for call-ins. Listeners join as audience and press Request to Speak.",
            L"Invite a caller up when you're ready for them. Mute them, or use Move to Audience to take them off air.",
            L"In OBS, add an Application Audio Capture source for Discord, so callers go out on the stream and to CJ Radio.",
            L"Wear headphones, so callers don't hear themselves back through your speakers.",
            L"Set callIn.enabled to true in station.json for shows that take calls. Listeners only see Call in on Discord while you're live.",
        };
        int n = 1;
        for (const wchar_t* s : discordSteps) {
            gfx_.text(std::to_wstring(n++) + L".", {r.x, y, 24, 20}, Font::BodyStrong, pal_.ink, {Align::Left, false});
            y += gfx_.text(s, {r.x + 26, y, std::min(r.w - 26, 640.0f), 80}, Font::Body, pal_.ink) + 8;
        }
        y += 4;
        button("pres:discord", {r.x, y, 200, 34}, L"Open your Discord", [this] { cjui::openInBrowser(station_.callIn.guestUrl); });
        y += 48;
    } else if (!presenter_.callInDirectorUrl.empty()) {
        button("pres:director", {r.x, y, 230, 34}, L"Open call-in control room", [this] {
            cjui::openInBrowser(presenter_.callInDirectorUrl);
        }, true);
        y += 48;
        y += gfx_.text(L"Callers wait in the queue until you approve them. Mute or remove anyone from the control room straight away. Listeners only see the Request to call button while call-ins are switched on in station.json.",
                       {r.x, y, std::min(r.w, 640.0f), 120}, Font::Small, pal_.inkMuted) + 8;
    } else {
        y += gfx_.text(L"Call-ins aren't set up yet. Put your Discord server's invite link in station.json as callIn.guestUrl (for example https://discord.gg/...), and this page will show how to run calls from a Discord Stage.",
                       {r.x, y, std::min(r.w, 640.0f), 120}, Font::Body, pal_.ink) + 10;
    }
    y += 10;
    y += gfx_.text(L"Presenter mode is switched on for this PC only, by its presenter.json file. It's for CJ Philip's own PC.",
                   {r.x, y, std::min(r.w, 640.0f), 80}, Font::Small, pal_.inkFaint);
    return y - r.y;
}

}  // namespace cjr
