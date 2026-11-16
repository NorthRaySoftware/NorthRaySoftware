// NorthRay Capture's settings (%LOCALAPPDATA%\NorthRay\Capture\settings.json). Versioned and read
// defensively like the other NorthRay apps' settings: a missing or wrong-typed value falls back to
// its default, and a file from a newer version is never overwritten with less.
#pragma once

#include <map>
#include "ncap/transition.hpp"

#include <string>
#include <vector>

namespace ncap {

enum class Theme { System = 0, Light = 1, Dark = 2 };

struct Settings {
    static constexpr int kVersion = 1;
    Theme theme = Theme::System;
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
    bool preLiveChecklist = false;           // Start streaming shows the pre-live checklist first
    std::vector<std::string> checklistItems; // your own list on it (core/checklist; empty: the starting suggestions)            // an MP4 copy when a recording ends (the MKV is kept)
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
    transition::Settings transition;  // used when the program changes scene          // the longest wait between reconnection attempts, in seconds    // the monitor device is headphones (no echo into a microphone)

    bool load(const std::wstring& path);
    bool save(const std::wstring& path) const;   // atomic: written beside, then swapped in
    bool readOnly = false;
};

}  // namespace ncap
