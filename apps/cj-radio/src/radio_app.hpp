// CJ Radio: listen live, see the schedule, catch up on recordings, and (on
// the presenter's own PC) check the broadcast before and during a show.
#pragma once

#include "cj/radio.hpp"
#include "cj/station.hpp"
#include "cjui/station_monitor.hpp"
#include "cjui/window.hpp"
#include "player.hpp"
#include "twitch_view.hpp"

#include <string>
#include <vector>

namespace cjr {

using namespace cjui;

enum class Page { Listen, Schedule, Recordings, Presenter };

struct PresenterSettings {
    bool enabled = false;          // presenter mode is switched on for this PC
    std::string callInDirectorUrl; // the host's VDO.Ninja director link (kept on this PC only)
};

class RadioApp : public AppWindow {
public:
    bool create(HINSTANCE inst, int show);

protected:
    void drawFrame(float W, float H) override;
    bool onKeyDown(WPARAM vk, bool ctrl, bool shift) override;
    void onTimer(UINT_PTR id) override;
    void onSystemThemeChanged() override { applyTheme(); }
    LRESULT onMessage(UINT msg, WPARAM wp, LPARAM lp, bool& handled) override;
    void onDestroy() override;
    const char* navPrefix() const override { return "nav:"; }

private:
    void load();
    void applyTheme();
    void navigate(Page p);
    void fetchRecordings();
    void fetchGithubRecordings();
    void fetchCalendar();
    void applyCalendar(const std::string& ics);
    void rebuildSchedule();
    void scanLocalRecordings();
    void mergeRecordings();
    void startTwitch();
    void placeTwitch(float W, float H);
    void saveVolume();

    void listenLive();
    void playRecording(const cj::station::Recording& r);
    void stopPlayback();
    void togglePause();          // the transport button: pause or resume whatever is playing
    bool isPaused() const;

    cj::radio::StationStatus status() const;
    cj::UtcSeconds now() const;

    // Drawing
    void drawRail(const Rect& r);
    void drawTransport(const Rect& r);
    float drawHeader(const Rect& r, const std::wstring& title, const std::wstring& subtitle);
    float onAirPanel(const Rect& r);
    float programmeRow(const Rect& r, const cj::radio::Programme& p, bool showDate);
    float pageListen(const Rect& r);
    float pageSchedule(const Rect& r);
    float pageRecordings(const Rect& r);
    float pagePresenter(const Rect& r);
    float scheduleMonth(const Rect& r);

    // Data
    std::wstring dataDir_, stateDir_;
    cj::station::Config station_;
    cj::radio::Schedule schedule_;          // what's shown: schedule.json + calendar
    cj::radio::Schedule fileSchedule_;      // schedule.json (and demo data)
    std::vector<cj::radio::Programme> calendarProgrammes_;
    std::wstring calendarStatus_;           // shown under the Schedule heading
    bool calendarOk_ = false;
    cj::UtcSeconds calendarCheckedUtc_ = 0; // last successful fetch
    cj::UtcSeconds calendarNextFetch_ = 0;
    bool calendarFetching_ = false;
    std::vector<cj::station::Recording> recordings_;        // shown: local + published
    std::vector<cj::station::Recording> localRecordings_;   // MP3s in the app's Recordings folder
    std::vector<cj::station::Recording> remoteRecordings_;  // from station.json's recordingsUrl
    std::wstring recordingsStatus_;      // why the list is empty, when it is
    std::vector<std::string> warnings_;
    PresenterSettings presenter_;
    bool demoData_ = true;
    int themeChoice_ = 0;                // 0 system, 1 light, 2 dark (shared with the launcher)

    cjui::StationMonitor monitor_;
    cj::station::NowPlaying nowPlaying_;
    StreamPlayer player_;
    std::wstring playingTitle_;          // what the transport bar shows
    std::string playingRecordingId_;
    bool pausedLive_ = false;            // the live stream was paused (resumes at the live point)

    // Twitch's own player, shown on the Listen page.
    TwitchView twitch_;
    std::string twitchChannel_;          // empty when not embedding
    cj::station::TwitchStatus twitchStatus_;
    bool twitchPlaying_ = false;
    bool twitchSoundOnly_ = false;       // the listener's choice, saved in radio.json
    std::wstring soundOnlyNote_;         // shown when Twitch has no audio-only version
    std::wstring twitchNote_;            // why the player isn't available, if it isn't
    bool playerSlotValid_ = false;       // set while drawing the Listen page
    Rect playerSlot_{};

    Page page_ = Page::Listen;
    bool monthView_ = false;
    int monthOffset_ = 0;                // months from the current one
    bool callerAgeConfirmed_ = false;
};

}  // namespace cjr
