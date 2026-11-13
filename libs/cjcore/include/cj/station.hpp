// CJ Radio's connection to its broadcast server.
//
// The station runs on AzuraCast (Icecast underneath). The host keeps using
// OBS + SE.Live for Twitch/YouTube and sends an audio feed to AzuraCast at the
// same time. AzuraCast is the only source of truth for "live" in our apps.
#pragma once

#include "cj/radio.hpp"

#include <string>
#include <vector>

namespace cj::station {

enum class CallInService { Discord, Other };

struct CallInConfig {
    bool enabled = false;       // the host switches this on for a show
    std::string guestUrl;       // a Discord invite to the server with the call-in Stage (or another https link)
    std::string notice;         // shown to callers before they join
    int minimumAge = 16;
};

struct Config {
    std::string name = "CJ Radio";
    std::string nowPlayingUrl;  // AzuraCast: https://<host>/api/nowplaying/<station>
    std::string streamUrl;      // https://<host>/listen/<station>/radio.mp3
    std::string recordingsUrl;  // JSON index of published recordings (local path or https)
    std::string recordingsGithub;  // "owner/repo": recordings are MP3s in that GitHub repository and its releases
    std::string twitchUrl;      // optional "also live on" links
    std::string youtubeUrl;
    // The schedule's calendar: a public .ics link (e.g. Google Calendar's
    // "Public address in iCal format"), and a link listeners use to add it.
    std::string calendarUrl;
    std::string calendarSubscribeUrl;
    bool embedTwitchPlayer = true;  // show the Twitch player on the Listen page
    int pollSeconds = 15;
    CallInConfig callIn;
    bool demo = false;

    bool serverConfigured() const { return !nowPlayingUrl.empty() && !streamUrl.empty(); }
};

struct NowPlaying {
    bool reachable = false;     // got a valid response
    bool stationOnline = false; // AzuraCast's is_online
    bool presenterLive = false; // AzuraCast's live.is_live
    std::string presenter;      // live.streamer_name
    std::string trackText;      // what AzuraCast says is playing (title or show name)
    int listeners = -1;
    UtcSeconds liveSinceUtc = 0;
    std::string error;          // why it isn't reachable, for the presenter screen
};

struct Recording {
    std::string id;
    std::string file;           // file name, when the recording came from a folder or repository
    std::string title;
    std::string description;
    std::string presenter;
    std::string category;
    UtcSeconds recordedUtc = 0;
    int durationMinutes = 0;
    std::string url;
    std::string transcriptUrl;
};

Config parseConfig(const std::string& jsonText, std::vector<std::string>& warnings);

// ---- Recordings kept on GitHub -------------------------------------------
// "owner/repo", or a github.com address -> "owner/repo"; "" if it isn't one.
std::string githubRepo(const std::string& text);
// A recording named after its file: "2026-11-14 The CJ Philip Show.mp3" ->
// dated 14 November 2026, titled "The CJ Philip Show". `fallbackUtc` is used
// when the name has no date.
Recording recordingFromFileName(const std::string& fileName, const std::string& url, UtcSeconds fallbackUtc);
// GitHub's "contents" listing of a folder: MP3 files become recordings.
// Also reports the recordings.json index (if any) and sub-folders worth
// looking in ("Recordings", "Episodes").
struct GithubListing {
    std::vector<Recording> recordings;
    std::string indexUrl;
    std::vector<std::string> folders;  // paths
};
GithubListing parseGithubContents(const std::string& jsonText);
// GitHub's releases list: each MP3 attached to a (non-draft) release.
std::vector<Recording> parseGithubReleases(const std::string& jsonText);
// Extra details from a recordings.json index ({"recordings":[{"file": "...mp3", "title", "description",
// "presenter", "category", "durationMinutes", "date"}]}), matched by file name.
void applyRecordingDetails(std::vector<Recording>& recs, const std::string& indexJson);
std::string recordingsToJson(const std::vector<Recording>& recs);  // the recordings index format (for caching)

// Accepts AzuraCast's single-station response or the all-stations array
// (first station used).
NowPlaying parseAzuraCastNowPlaying(const std::string& jsonText);

// Live only when the presenter is connected AND the station reports itself
// online. Either alone isn't enough (see AzuraCast issue #8363).
radio::LiveSignal toLiveSignal(const NowPlaying& np);

// Discord invite links (discord.gg/..., discord.com/invite/...) are Discord;
// anything else is treated as a plain web link.
CallInService callInService(const std::string& url);

// The channel login in a Twitch channel address ("https://www.twitch.tv/cjphilip"
// -> "cjphilip"), lower-cased. Empty if the address isn't a Twitch channel.
std::string twitchChannel(const std::string& url);

// What the embedded Twitch player last reported. Twitch's own player is the
// source: it only says "online" when the channel really is broadcasting.
struct TwitchStatus {
    bool known = false;       // the player has reported at least once
    bool online = false;
    UtcSeconds sinceUtc = 0;  // when this app first saw it go online
};

// Live if the audio server says so, or if Twitch's player says the channel is
// broadcasting. Server details (listeners, presenter) win when both are live.
radio::LiveSignal combineLive(const NowPlaying& np, const TwitchStatus& twitch);

std::vector<Recording> parseRecordings(const std::string& jsonText, std::vector<std::string>& warnings);

}  // namespace cj::station
