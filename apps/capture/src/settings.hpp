// NorthRay Capture's settings (%LOCALAPPDATA%\NorthRay\Capture\settings.json). Versioned and read
// defensively like the other NorthRay apps' settings: a missing or wrong-typed value falls back to
// its default, and a file from a newer version is never overwritten with less.
#pragma once

#include "ncap/automation.hpp"
#include "ncap/hotkeys.hpp"
#include "ncap/transition.hpp"

#include <map>
#include <string>
#include <vector>

namespace ncap {

enum class Theme { System = 0, Light = 1, Dark = 2 };

struct Settings {
    static constexpr int kVersion = 1;
    Theme theme = Theme::System;
    std::string libraryFolder;
    std::string seenVersion;          // the version whose What's new has been shown
    bool existed = false;
    int compactLayout = 0;            // 0 automatic (laptop-sized windows), 1 always, 2 never             // (not saved) the settings file was there when Capture started: not a new install        // the Library's folder when it was last open ("" the top)
    bool lowResource = false;         // preview, meters and animations kept to a minimum; output untouched
    int previewFps = 30;              // 0 = preview off
    int outputFps = 60;
    std::string layout;               // the workspace (ncap::Layout JSON)
    std::map<std::string, std::string> savedLayouts;   // name -> layout JSON
    std::string sceneCollection = "Default";
    std::string profile = "Default";  // this profile's name (the others are in profiles\<name>.json)
    std::wstring recordingsFolder;    // "" = the Videos folder
    bool welcomed = false;
    std::wstring monitorDevice = L"default";   // where the monitor mix plays: "default", an endpoint id, or "off"
    bool monitorHeadphones = true;
    std::wstring secondDevice = L"off";        // a second output (to a streaming PC, say): an endpoint id, "default" or "off"
    int secondBus = 0;                         // what it plays: 0 Stream, 1-6 Track 1-6, 7 Monitor
    std::wstring recordEncoder;       // "" = the best available
    int recordKbps = 20000;           // video bitrate for recordings
    unsigned recordTracks = 0x0F;     // bit n-1 = recording track n (1-4 by default)
    bool recordMp4 = true;
    bool dndReminder = true;           // remind about Windows' Do not disturb when going live
    int notesSpeed = 2;                // the teleprompter's speed (0-6)
    float notesSize = 32;              // its text size
    double timelapse = 0;              // seconds between frames for a time-lapse recording (0: off)
    bool preLiveChecklist = false;           // Start streaming shows the pre-live checklist first
    // Settings › Writing Studio (0.13): what Capture does with Writing Studio's summary.
    struct WritingLink {
        bool spoilerSafe = false;   // leave the story's details (viewpoint, place, story day, cast) out of every design
        bool worldColour = true;    // brand-linked designs take the book's world colour while it's written
        bool celebrate = true;      // the Celebration design pops up for the daily goal and finished sprints
        bool markers = true;        // a recording marker named after each chapter moved to
        bool scenes = false;        // switch scenes by Writing Studio's workspace
        std::map<std::string, std::string> sceneFor;   // workspace ("writing", "planning", "worldbuilding", "other") -> scene name
        std::vector<std::string> hiddenParts;          // Writing card parts switched off live (the Writing panel, hotkeys) (0.20)
    } writing;
    // The stream's theme (0.21): one switch for every design and sky that follows it.
    std::string streamTheme = "northray";
    std::string ownThemesJson;                       // your own themes (design::themesToJson)
    std::map<std::string, bool> themeWorldAccent;    // theme id -> the book's world colour as its main colour while writing
    std::map<std::string, std::string> themeApplied; // scene collection -> the theme its designs are in (caught up when it loads)
    std::vector<std::string> checklistItems; // your own list on it (core/checklist; empty: the starting suggestions)
    std::vector<automation::Rule> automationRules;   // Settings › Automation (core/automation)
    std::vector<hotkeys::Binding> hotkeys;           // Settings › Hotkeys (core/hotkeys)            // an MP4 copy when a recording ends (the MKV is kept)
    int audioKbps = 160;
    // Where streams go (several at once). Each one's key is in Credential Manager as "stream/<id>".
    struct StreamDestination {
        std::string id = "main";
        std::string service = "twitch";   // twitch, youtube, kick, custom
        std::string url = "rtmps://ingest.global-contribute.live-video.net/app";
        int kbps = 6000;                  // video bitrate (CBR); destinations at the same bitrate share one encode
        bool enabled = true;              // started by "Start streaming"
        int bus = 0;                      // the sound it carries: 0 the Stream mix, 1-6 Track 1-6
    };
    std::vector<StreamDestination> destinations{StreamDestination{}};
    int streamRetryMax = 60;
    bool studioMode = false;          // Preview and Program side by side
    // Your stream (0.11).
    bool recordWhileStreaming = true;  // a recording runs whenever you stream, without pressing Record
    bool sceneChapters = true;         // a chapter (marker) in the recording at each scene change
    std::string brbScene;              // Be right back: the scene it switches to ("" = none chosen yet)
    std::string goLiveAt;              // "2026-11-22T19:00", local time ("" = none): timers count to it
    bool goLiveStarts = false;         // at that time, Start streaming happens by itself
    std::string goLiveDone;            // the go-live time already acted on (so reopening Capture doesn't start it again)
    std::string goLiveScene;           // and this scene goes live ("" = stay on the current one)
    int plannedMinutes = 0;            // how long the stream is meant to be (0 = not set), for "time left" timers
    std::string nextStreamAt;          // the next stream after this one, for end screens ("" = not set)
    transition::Settings transition;  // used when the program changes scene          // the longest wait between reconnection attempts, in seconds    // the monitor device is headphones (no echo into a microphone)

    bool load(const std::wstring& path);
    bool save(const std::wstring& path) const;   // atomic: written beside, then swapped in
    bool readOnly = false;
};

}  // namespace ncap
