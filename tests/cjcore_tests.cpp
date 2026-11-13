// Unit tests for cjcore. Deliberately dependency-free: run the binary, it
// prints each check and exits non-zero on any failure.
#include "cj/radio.hpp"
#include "cj/registry.hpp"
#include "cj/station.hpp"
#include "cj/skillswap.hpp"
#include <fstream>
#include <sstream>
#include "cj/time.hpp"
#include "cj/ical.hpp"

#include <cstdio>
#include <string>

static int g_failed = 0, g_passed = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (cond) { ++g_passed; }                                                   \
        else { ++g_failed; std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

using namespace cj;
using namespace cj::radio;

static UtcSeconds utc(const char* iso) { return *parseUtcIso(iso); }

static void testTime() {
    std::puts("time");
    CHECK(weekday(2026, 11, 14) == 6);   // Saturday
    CHECK(weekday(1970, 1, 1) == 4);     // Thursday
    // 2026: BST from 29 March 01:00 UTC to 25 October 01:00 UTC.
    CHECK(!isBritishSummerTime(utc("2026-03-29T00:59:59Z")));
    CHECK(isBritishSummerTime(utc("2026-03-29T01:00:00Z")));
    CHECK(isBritishSummerTime(utc("2026-10-25T00:59:59Z")));
    CHECK(!isBritishSummerTime(utc("2026-10-25T01:00:00Z")));
    // Summer Saturday 19:00 UK = 18:00 UTC; winter = 19:00 UTC.
    CHECK(*ukToUtc(*parseLocalIso("2026-07-11T19:00")) == utc("2026-07-11T18:00:00Z"));
    CHECK(*ukToUtc(*parseLocalIso("2026-11-14T19:00")) == utc("2026-11-14T19:00:00Z"));
    // Spring-forward gap doesn't exist.
    CHECK(!ukToUtc(*parseLocalIso("2026-03-29T01:30")).has_value());
    // Fall-back ambiguity resolves to the earlier (BST) instant.
    CHECK(*ukToUtc(*parseLocalIso("2026-10-25T01:30")) == utc("2026-10-25T00:30:00Z"));
    // Round trip and validation.
    CHECK(formatUtcIso(utc("2027-02-28T23:05:00Z")) == "2027-02-28T23:05:00Z");
    CHECK(!parseLocalIso("2026-11-31T10:00").has_value());
    CHECK(!parseLocalIso("2026-11-14 19:00").has_value());
    CHECK(!parseLocalIso("2026-11-14T19:00junk").has_value());
}

static Schedule sched(const char* json, UtcSeconds now) { return loadSchedule(json, now); }

static void testRadioStatus() {
    std::puts("radio status");
    const UtcSeconds thursday = utc("2026-11-12T20:00:00Z");
    const UtcSeconds saturdayEvening = utc("2026-11-14T19:30:00Z");

    // An empty schedule on a Saturday evening: off air, nothing next, no error.
    {
        Schedule s = sched(R"({"programmes": []})", saturdayEvening);
        StationStatus st = computeStatus(s, LiveSignal{}, saturdayEvening);
        CHECK(st.state == StationState::OffAir);
        CHECK(!st.next.has_value());
        CHECK(s.warnings.empty());
    }
    // A series alone publishes nothing.
    {
        Schedule s = sched(R"({"series":[{"id":"cjps","title":"The CJ Philip Show","defaultUkTime":"19:00"}],
                               "programmes": []})", thursday);
        CHECK(computeStatus(s, LiveSignal{}, thursday).state == StationState::OffAir);
        CHECK(publicProgrammes(s, thursday, thursday + 30 * 86400).empty());
        // ...but the host sees draft suggestions for the coming Saturdays.
        auto drafts = draftSaturdaySuggestions(s, s.series[0], thursday, thursday + 15 * 86400);
        CHECK(drafts.size() == 2);
        CHECK(drafts[0].status == Status::Draft);
        CHECK(drafts[0].startUtc == utc("2026-11-14T19:00:00Z"));
    }
    const char* oneShow = R"({"programmes": [
        {"id":"p1","title":"The CJ Philip Show","kind":"regular-saturday","status":"confirmed",
         "startUk":"2026-11-14T19:00","durationMinutes":120},
        {"id":"p0","title":"Draft idea","kind":"additional","status":"draft","startUk":"2026-11-13T12:00"},
        {"id":"px","title":"Between the Pages","kind":"additional","status":"cancelled",
         "startUk":"2026-11-18T20:00","durationMinutes":60}
    ]})";
    // Confirmed show two days away: upcoming, and the draft isn't "next".
    {
        Schedule s = sched(oneShow, thursday);
        StationStatus st = computeStatus(s, LiveSignal{}, thursday);
        CHECK(st.state == StationState::Upcoming);
        CHECK(st.next && st.next->id == "p1");
        CHECK(st.recentChanges.size() == 1);
        CHECK(std::string(stateLabel(st.state)) == "Off air");
        auto pub = publicProgrammes(s, thursday, thursday + 30 * 86400);
        CHECK(pub.size() == 2);   // confirmed + cancelled; never the draft
    }
    // Twenty minutes before: starting soon.
    {
        Schedule s = sched(oneShow, thursday);
        CHECK(computeStatus(s, LiveSignal{}, utc("2026-11-14T18:40:00Z")).state == StationState::StartingSoon);
    }
    // Start time has passed but no live signal: NOT live. Reported as overdue.
    {
        Schedule s = sched(oneShow, thursday);
        StationStatus st = computeStatus(s, LiveSignal{}, saturdayEvening);
        CHECK(st.state != StationState::Live);
        CHECK(st.overdue && st.overdue->id == "p1");
    }
    // Calendar says nothing, but the infrastructure reports a spontaneous broadcast.
    {
        Schedule s = sched(R"({"programmes": []})", thursday);
        LiveSignal sig;
        sig.connected = true;
        sig.onAir = true;
        sig.title = "Quick chat";
        sig.startedUtc = thursday - 600;
        StationStatus st = computeStatus(s, sig, thursday);
        CHECK(st.state == StationState::Live);
        CHECK(st.current && st.current->kind == Kind::Spontaneous && st.current->title == "Quick chat");
    }
    // A signal that is connected but not on air never produces Live.
    {
        Schedule s = sched(oneShow, thursday);
        LiveSignal sig;
        sig.connected = true;
        CHECK(computeStatus(s, sig, saturdayEvening).state != StationState::Live);
    }
    // Validation warnings.
    {
        Schedule s = sched(R"({"programmes": [
            {"id":"a","kind":"regular-saturday","status":"confirmed","startUk":"2026-11-15T19:00"},
            {"id":"b","status":"confirmed","startUk":"2026-03-29T01:30"},
            {"id":"c","status":"confirmed"}]})", thursday);
        CHECK(s.programmes.size() == 1);
        CHECK(s.warnings.size() == 3);
    }
    // Relative demo dates resolve to the next Saturday in UK time.
    {
        Schedule s = sched(R"({"demo":true,"programmes":[{"id":"d","status":"confirmed","kind":"regular-saturday",
            "relative":{"weekday":6,"weeksAhead":1,"ukTime":"19:00"}}]})", thursday);
        CHECK(s.programmes.size() == 1 && s.programmes[0].demo);
        CHECK(s.programmes[0].startUtc == utc("2026-11-21T19:00:00Z"));
    }
}

static void testRegistry() {
    std::puts("registry");
    CHECK(isValidAppId("cj-radio"));
    CHECK(!isValidAppId("CJ Radio"));
    CHECK(!isValidAppId("a--b"));
    CHECK(!isValidAppId("-a"));
    std::string err;
    auto a = parseAppEntry(R"({"schema":1,"id":"skillswap","name":"SkillSwap","status":"in-development",
        "credits":{"associatedCreator":"CJ Philip"},"launch":{"executable":"SkillSwap.exe"}})", err);
    CHECK(a && a->name == "SkillSwap" && a->status == AppStatus::InDevelopment && a->developer.empty());
    CHECK(!parseAppEntry(R"({"id":"x","name":"X","launch":{"executable":"..\\..\\evil.exe"}})", err));
    CHECK(!parseAppEntry(R"({"id":"x","name":"X","launch":{"executable":"C:\\Windows\\evil.exe"}})", err));
    CHECK(!parseAppEntry(R"({"id":"x","name":"X","schema":2})", err));
    CHECK(!parseAppEntry("not json", err));
    LoadResult r;
    auto p = parseProfile(R"({"displayName":"Someone","localDemo":false})", r);
    CHECK(p && p->localDemo);   // files can't masquerade as online accounts
}

static void testStation() {
    std::puts("station");
    using namespace cj::station;
    std::vector<std::string> w;
    Config c = parseConfig(R"({"nowPlayingUrl":"https://radio.example/api/nowplaying/cj","streamUrl":"ftp://bad",
        "callIn":{"enabled":true,"guestUrl":"http://insecure"}})", w);
    CHECK(!c.serverConfigured());          // bad stream URL dropped
    CHECK(!c.callIn.enabled);               // non-https call-in link disables call-ins
    CHECK(w.size() == 2);
    w.clear();
    Config ok = parseConfig(R"({"nowPlayingUrl":"https://r.example/api/nowplaying/cj","streamUrl":"https://r.example/listen/cj/radio.mp3"})", w);
    CHECK(ok.serverConfigured() && w.empty());

    // Presenter connected and station online: live.
    const char* live = R"({"is_online":true,"listeners":{"total":12,"unique":10,"current":11},
        "live":{"is_live":true,"streamer_name":"CJ Philip","broadcast_start":1794517200},
        "now_playing":{"song":{"text":"The CJ Philip Show","title":"The CJ Philip Show"}}})";
    NowPlaying np = parseAzuraCastNowPlaying(live);
    CHECK(np.reachable && np.presenterLive && np.listeners == 11 && np.presenter == "CJ Philip");
    cj::radio::LiveSignal sig = toLiveSignal(np);
    CHECK(sig.connected && sig.onAir && sig.listeners == 11 && sig.title == "The CJ Philip Show");

    // AutoDJ playing between shows: online but nobody live -> not on air.
    NowPlaying autodj = parseAzuraCastNowPlaying(R"({"is_online":true,"live":{"is_live":false,"streamer_name":""},
        "listeners":{"current":3}})");
    CHECK(autodj.reachable && !toLiveSignal(autodj).onAir);

    // is_live true but station offline (the AzuraCast #8363 mismatch): not on air.
    NowPlaying mismatch = parseAzuraCastNowPlaying(R"({"is_online":false,"live":{"is_live":true}})");
    CHECK(!toLiveSignal(mismatch).onAir);

    // Missing is_online counts as offline; array responses use the first station.
    CHECK(!toLiveSignal(parseAzuraCastNowPlaying(R"([{"live":{"is_live":true}}])")).onAir);
    CHECK(toLiveSignal(parseAzuraCastNowPlaying(std::string("[") + live + "]")).onAir);

    // Garbage and HTML error pages never produce live.
    CHECK(!parseAzuraCastNowPlaying("<html>502</html>").reachable);
    CHECK(!parseAzuraCastNowPlaying("{}").reachable);
    CHECK(!toLiveSignal(parseAzuraCastNowPlaying("{}")).connected);

    // A live signal feeds straight into the station status rules.
    Schedule s = loadSchedule(R"({"programmes":[]})", 0);
    CHECK(computeStatus(s, sig, 1794520000).state == StationState::Live);
    CHECK(computeStatus(s, toLiveSignal(autodj), 1794520000).state == StationState::OffAir);

    // Call-in services.
    CHECK(callInService("https://discord.gg/abc123") == CallInService::Discord);
    CHECK(callInService("https://discord.com/invite/abc123") == CallInService::Discord);
    CHECK(callInService("https://discord.gg/") == CallInService::Other);
    CHECK(callInService("https://discord.gg.evil.example/abc") == CallInService::Other);
    CHECK(callInService("https://vdo.ninja/?room=x") == CallInService::Other);

    // Twitch channel addresses.
    CHECK(twitchChannel("https://www.twitch.tv/cjphilip") == "cjphilip");
    CHECK(twitchChannel("https://twitch.tv/CJPhilip/") == "cjphilip");
    CHECK(twitchChannel("https://www.twitch.tv/cjphilip?sr=a") == "cjphilip");
    CHECK(twitchChannel("https://www.twitch.tv/directory").empty());
    CHECK(twitchChannel("https://www.youtube.com/@CJPReviewsAndWriting").empty());
    CHECK(twitchChannel("https://www.twitch.tv/bad<name").empty());
    CHECK(twitchChannel("http://www.twitch.tv/cjphilip").empty());

    // Twitch's player can make the station live; nothing else can.
    TwitchStatus tw;
    CHECK(!combineLive(autodj, tw).onAir);                 // player hasn't reported yet
    tw.known = true;
    CHECK(!combineLive(NowPlaying{}, tw).onAir);           // offline on Twitch, no server
    tw.online = true;
    tw.sinceUtc = 1794519000;
    CHECK(combineLive(NowPlaying{}, tw).onAir);
    CHECK(combineLive(NowPlaying{}, tw).listeners == -1);
    CHECK(combineLive(NowPlaying{}, tw).startedUtc == 1794519000);
    CHECK(combineLive(np, tw).listeners == sig.listeners); // server details win
    CHECK(computeStatus(s, combineLive(NowPlaying{}, tw), 1794520000).state == StationState::Live);

    // Recordings index.
    auto recs = parseRecordings(R"({"recordings":[
        {"id":"a","title":"Show A","recordedUtc":"2026-11-07T19:00:00Z","url":"https://cdn.example/a.mp3","durationMinutes":120},
        {"id":"b","title":"Show B","recordedUtc":"2026-11-14T19:00:00Z","url":"https://cdn.example/b.mp3"},
        {"id":"c","title":"No link","recordedUtc":"2026-11-14T19:00:00Z","url":"file:///c.mp3"}]})", w);
    CHECK(recs.size() == 2 && recs[0].id == "b");
    CHECK(w.size() == 1);
}

static std::string slurp(const char* p) { std::ifstream f(p); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }

static void testSkillSwap() {
    std::puts("skillswap");
    using namespace cj::skillswap;
    Catalogue c = loadCatalogue(slurp(CJ_SOURCE_DIR "/shared/metadata/skillswap/categories.json"),
                                slurp(CJ_SOURCE_DIR "/shared/metadata/demo/skillswap.demo.json"));
    CHECK(c.warnings.empty());
    CHECK(c.categories.size() == 7 && c.creators.size() == 3 && c.offerings.size() == 6);
    CHECK(c.offerings[0].demo);
    Filter free;
    free.pricing = (int)Pricing::Free;
    CHECK(filterOfferings(c, free).size() == 2);
    Filter drawing;
    drawing.category = "drawing";
    auto d = filterOfferings(c, drawing);
    CHECK(d.size() == 3);
    // Free offerings keep their place; price never reorders the list.
    CHECK(d[0]->pricing == Pricing::Free);
    CHECK(priceLabel(c.offerings[2]) == "\xC2\xA3" "12");
    CHECK(priceLabel(c.offerings[1]) == "Pay what you can (suggested \xC2\xA3" "5)");
    Catalogue bad = loadCatalogue("{}", R"({"creators":[{"id":"a","name":"A"}],"offerings":[
        {"id":"x","creator":"nobody"},{"id":"y","creator":"a","pricing":"fixed"}]})");
    CHECK(bad.offerings.size() == 1 && bad.offerings[0].pricing == Pricing::Free && bad.warnings.size() == 2);
}


static void testIcal() {
    std::puts("ical");
    using cj::ical::toProgrammes;
    std::vector<Series> series(1);
    series[0].id = "cj-philip-show"; series[0].title = "The CJ Philip Show"; series[0].presenter = "CJ Philip"; series[0].category = "Talk";
    // Google-style feed: a weekly Saturday show at 19:00 UK, one week moved,
    // one week removed, plus a one-off, a tentative, an all-day and a private event.
    const std::string ics =
        "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nX-WR-CALNAME:CJ Radio\r\n"
        "BEGIN:VTIMEZONE\r\nTZID:Europe/London\r\nBEGIN:STANDARD\r\nDTSTART:19701025T020000\r\nEND:STANDARD\r\nEND:VTIMEZONE\r\n"
        "BEGIN:VEVENT\r\nUID:show1@google.com\r\nDTSTART;TZID=Europe/London:20261003T190000\r\nDTEND;TZID=Europe/London:20261003T210000\r\n"
        "RRULE:FREQ=WEEKLY;BYDAY=SA\r\nEXDATE;TZID=Europe/London:20261031T190000\r\nSUMMARY:The CJ Philip Show\r\n"
        "DESCRIPTION:Books\\, art and chat.\\nWith callers.\r\n"
        "BEGIN:VALARM\r\nTRIGGER:-PT10M\r\nSUMMARY:alarm\r\nEND:VALARM\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:show1@google.com\r\nRECURRENCE-ID;TZID=Europe/London:20261107T190000\r\n"
        "DTSTART;TZID=Europe/London:20261108T180000\r\nDTEND;TZID=Europe/London:20261108T200000\r\nSUMMARY:The CJ Philip Show (Sunday spe\r\n cial)\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:extra@google.com\r\nDTSTART:20261118T200000Z\r\nDURATION:PT90M\r\nSUMMARY:Writing Q&A\r\n"
        "DESCRIPTION:Guest: A Visiting Author<br>Questions welcome\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:maybe@google.com\r\nDTSTART:20261119T200000Z\r\nSTATUS:TENTATIVE\r\nSUMMARY:Maybe\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:off@google.com\r\nDTSTART:20261121T190000Z\r\nSTATUS:CANCELLED\r\nSUMMARY:Off\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:allday@google.com\r\nDTSTART;VALUE=DATE:20261120\r\nSUMMARY:Holiday\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:p@google.com\r\nDTSTART:20261120T100000Z\r\nCLASS:PRIVATE\r\nSUMMARY:Dentist\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:ny@google.com\r\nDTSTART;TZID=America/New_York:20261120T100000\r\nSUMMARY:Elsewhere\r\nEND:VEVENT\r\n"
        "END:VCALENDAR\r\n";
    auto r = toProgrammes(ics, utc("2026-10-01T00:00:00Z"), utc("2026-11-30T00:00:00Z"), series);
    CHECK(r.valid);
    CHECK(r.calendarName == "CJ Radio");
    // Saturdays 3,10,17,24 Oct, 14,21,28 Nov (31 Oct excluded, 7 Nov moved) = 7, + override, extra, tentative, cancelled = 11
    CHECK(r.programmes.size() == 11);
    int saturdays = 0;
    for (auto& p : r.programmes) if (p.title == "The CJ Philip Show") ++saturdays;
    CHECK(saturdays == 7);
    // 3 Oct is BST: 19:00 UK = 18:00Z. 14 Nov is GMT: 19:00Z.
    CHECK(r.programmes[0].startUtc == utc("2026-10-03T18:00:00Z"));
    CHECK(r.programmes[0].durationMinutes == 120);
    CHECK(r.programmes[0].kind == Kind::RegularSaturday);
    CHECK(r.programmes[0].presenter == "CJ Philip");
    CHECK(r.programmes[0].status == Status::Confirmed);
    CHECK(r.programmes[0].description == "Books, art and chat.\nWith callers.");
    CHECK(r.programmes[0].authoredUkTime == "2026-10-03T19:00");
    bool sawOct31 = false, sawNov7 = false, sawSunday = false, sawNov14 = false;
    for (auto& p : r.programmes) {
        if (p.startUtc == utc("2026-10-31T19:00:00Z")) sawOct31 = true;
        if (p.startUtc == utc("2026-11-07T19:00:00Z")) sawNov7 = true;
        if (p.startUtc == utc("2026-11-08T18:00:00Z")) { sawSunday = p.title == "The CJ Philip Show (Sunday special)" && p.kind == Kind::Additional; }
        if (p.startUtc == utc("2026-11-14T19:00:00Z")) sawNov14 = true;
        if (p.title == "Writing Q&A") { CHECK(p.presenter == "CJ Philip"); CHECK(p.durationMinutes == 90); CHECK(p.description == "Guest: A Visiting Author\nQuestions welcome"); }
        if (p.title == "Maybe") CHECK(p.status == Status::Draft);
        if (p.title == "Off") CHECK(p.status == Status::Cancelled);
        CHECK(p.title != "Holiday" && p.title != "Dentist" && p.title != "Elsewhere");
    }
    CHECK(!sawOct31 && !sawNov7 && sawSunday && sawNov14);
    CHECK(r.warnings.size() == 2);   // all-day + unknown time zone
    // Window and listener view.
    Schedule s; s.programmes = r.programmes;
    auto pub = publicProgrammes(s, utc("2026-11-15T00:00:00Z"), utc("2026-11-30T00:00:00Z"));
    CHECK(pub.size() == 4);  // Q&A 18 Nov, Off (cancelled) and the show on 21 Nov, show 28 Nov
    // COUNT, UNTIL, INTERVAL, monthly last-Saturday.
    const std::string rules =
        "BEGIN:VCALENDAR\nBEGIN:VEVENT\nUID:a\nDTSTART:20261107T190000\nRRULE:FREQ=WEEKLY;COUNT=3\nSUMMARY:A\nEND:VEVENT\n"
        "BEGIN:VEVENT\nUID:b\nDTSTART:20261107T120000Z\nRRULE:FREQ=WEEKLY;INTERVAL=2;UNTIL=20261205T120000Z\nSUMMARY:B\nEND:VEVENT\n"
        "BEGIN:VEVENT\nUID:c\nDTSTART:20261031T190000\nRRULE:FREQ=MONTHLY;BYDAY=-1SA\nSUMMARY:C\nEND:VEVENT\n"
        "BEGIN:VEVENT\nUID:d\nDTSTART:20261101T090000\nRRULE:FREQ=DAILY;BYHOUR=9\nSUMMARY:D\nEND:VEVENT\nEND:VCALENDAR\n";
    auto r2 = toProgrammes(rules, utc("2026-11-01T00:00:00Z"), utc("2027-01-31T00:00:00Z"));
    int a = 0, b = 0, c = 0, d = 0;
    for (auto& p : r2.programmes) { a += p.title == "A"; b += p.title == "B"; d += p.title == "D";
        if (p.title == "C") { ++c; auto uk = utcToUk(p.startUtc); CHECK(weekday(uk.year, uk.month, uk.day) == 6); CHECK(uk.day >= 25); } }
    CHECK(a == 3);
    CHECK(b == 3);   // 7 Nov, 21 Nov, 5 Dec
    CHECK(c == 3);   // 28 Nov, 26 Dec, 30 Jan (31 Oct is before the window)
    CHECK(d == 1);   // unsupported rule: first date only, with a warning
    CHECK(r2.warnings.size() == 1);
    CHECK(!toProgrammes("<html>Not found</html>", 0, 1).valid);
    CHECK(cj::ical::normaliseUrl("webcal://example.com/a.ics") == "https://example.com/a.ics");
    CHECK(cj::ical::normaliseUrl("http://example.com/a.ics").empty());
    CHECK(cj::ical::normaliseUrl(" https://calendar.google.com/x/basic.ics ") == "https://calendar.google.com/x/basic.ics");
}

static void testGithubRecordings() {
    std::puts("github recordings");
    using namespace cj::station;
    CHECK(githubRepo("calebjphilip08-sudo/The-CJ-Philip-Show") == "calebjphilip08-sudo/The-CJ-Philip-Show");
    CHECK(githubRepo("https://github.com/calebjphilip08-sudo/The-CJ-Philip-Show/") == "calebjphilip08-sudo/The-CJ-Philip-Show");
    CHECK(githubRepo("https://github.com/a/b.git") == "a/b");
    CHECK(githubRepo("a/b/c").empty() && githubRepo("nope").empty() && githubRepo("a/b?x=1").empty() && githubRepo("../x").empty());
    auto r = recordingFromFileName("2026-11-14 The_CJ_Philip_Show - Episode 3.mp3", "https://x/y.mp3", 5);
    CHECK(r.title == "The CJ Philip Show - Episode 3" && r.recordedUtc == utc("2026-11-14T12:00:00Z") && r.file.size() > 0);
    CHECK(recordingFromFileName("Episode.mp3", "u", 5).recordedUtc == 5);
    CHECK(recordingFromFileName("2026-11-14.mp3", "u", 5).title == "The CJ Philip Show");
    const std::string contents = R"([
      {"name":"README.md","path":"README.md","type":"file","download_url":"https://raw.githubusercontent.com/o/r/main/README.md"},
      {"name":"2026-11-07 Pilot.mp3","path":"2026-11-07 Pilot.mp3","type":"file","download_url":"https://raw.githubusercontent.com/o/r/main/2026-11-07%20Pilot.mp3"},
      {"name":"recordings.json","path":"recordings.json","type":"file","download_url":"https://raw.githubusercontent.com/o/r/main/recordings.json"},
      {"name":"Recordings","path":"Recordings","type":"dir","download_url":null},
      {"name":"bad.mp3","path":"bad.mp3","type":"file","download_url":"http://insecure/bad.mp3"}])";
    auto g = parseGithubContents(contents);
    CHECK(g.recordings.size() == 1 && g.recordings[0].title == "Pilot");
    CHECK(g.indexUrl == "https://raw.githubusercontent.com/o/r/main/recordings.json");
    CHECK(g.folders.size() == 1 && g.folders[0] == "Recordings");
    CHECK(parseGithubContents("{\"message\":\"Not Found\"}").recordings.empty());
    const std::string releases = R"([
      {"name":"Episode 4: Winter","tag_name":"ep4","draft":false,"published_at":"2026-11-21T22:10:00Z","body":"Callers and a reading.",
       "assets":[{"name":"show.mp3","browser_download_url":"https://github.com/o/r/releases/download/ep4/show.mp3"}]},
      {"name":"Two parts","tag_name":"ep5","draft":false,"published_at":"2026-11-28T22:00:00Z","body":"",
       "assets":[{"name":"2026-11-28 Part 1.mp3","browser_download_url":"https://github.com/o/r/releases/download/ep5/p1.mp3"},
                 {"name":"2026-11-28 Part 2.mp3","browser_download_url":"https://github.com/o/r/releases/download/ep5/p2.mp3"},
                 {"name":"notes.pdf","browser_download_url":"https://github.com/o/r/releases/download/ep5/notes.pdf"}]},
      {"name":"Draft","tag_name":"d","draft":true,"assets":[{"name":"x.mp3","browser_download_url":"https://x/x.mp3"}]}])";
    auto rel = parseGithubReleases(releases);
    CHECK(rel.size() == 3);
    CHECK(rel.size() == 3 && rel[0].title == "Episode 4: Winter" && rel[0].recordedUtc == utc("2026-11-21T22:10:00Z") &&
          rel[0].description == "Callers and a reading.");
    CHECK(rel.size() == 3 && rel[1].title == "Part 1" && rel[2].title == "Part 2");
    std::vector<Recording> list = g.recordings;
    applyRecordingDetails(list, R"({"recordings":[{"file":"2026-11-07 Pilot.mp3","title":"The Pilot","description":"First show.","durationMinutes":95}]})");
    CHECK(list[0].title == "The Pilot" && list[0].description == "First show." && list[0].durationMinutes == 95);
    std::vector<std::string> w;
    auto back = parseRecordings(recordingsToJson(list), w);
    CHECK(back.size() == 1 && back[0].title == "The Pilot" && back[0].file == "2026-11-07 Pilot.mp3" && w.empty());
    std::vector<std::string> cw;
    auto cfg = parseConfig(R"({"recordingsGithub":"https://github.com/calebjphilip08-sudo/The-CJ-Philip-Show"})", cw);
    CHECK(cfg.recordingsGithub == "calebjphilip08-sudo/The-CJ-Philip-Show" && cw.empty());
}

int main() {
    testGithubRecordings();
    testIcal();
    testSkillSwap();
    testStation();
    testTime();
    testRadioStatus();
    testRegistry();
    std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
