// NorthRay Capture's settings (%LOCALAPPDATA%\NorthRay\Capture\settings.json). Versioned and read
// defensively like the other NorthRay apps' settings: a missing or wrong-typed value falls back to
// its default, and a file from a newer version is never overwritten with less.
#pragma once

#include <map>
#include <string>

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
    std::wstring recordingsFolder;    // "" = the Videos folder
    bool welcomed = false;
    std::wstring monitorDevice = L"default";   // where the monitor mix plays: "default", an endpoint id, or "off"
    bool monitorHeadphones = true;
    std::wstring recordEncoder;       // "" = the best available
    int recordKbps = 20000;           // video bitrate for recordings
    unsigned recordTracks = 0x0F;     // bit n-1 = recording track n (1-4 by default)
    int audioKbps = 160;
    std::string streamService = "twitch";   // twitch, youtube, kick, custom
    std::string streamUrl = "rtmps://ingest.global-contribute.live-video.net/app";
    int streamKbps = 6000;            // video bitrate for streaming (CBR)    // the monitor device is headphones (no echo into a microphone)

    bool load(const std::wstring& path);
    bool save(const std::wstring& path) const;   // atomic: written beside, then swapped in
    bool readOnly = false;
};

}  // namespace ncap
