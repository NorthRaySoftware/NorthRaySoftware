// NorthRay Capture: what can be heard: input devices (microphones, capture cards' sound), output
// devices (for desktop audio) and the applications playing sound right now (for application audio).
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace ncap::audio {

struct Device {
    std::wstring id;     // the endpoint id: what a source remembers
    std::wstring name;   // "Microphone (Blue Yeti)"
    bool isDefault = false;
};
std::vector<Device> listInputs();
std::vector<Device> listOutputs();
std::wstring defaultDevice(bool input);   // the current default's id, or ""
std::wstring deviceName(const std::wstring& id);

// An application with an audio session on any output device. Several processes of one program
// (Chrome, Discord) show as one, by the top process of the program's tree, which is what process
// loopback capture takes.
struct App {
    std::wstring exe;           // "Spotify.exe": what an application audio source remembers
    std::wstring name;          // "Spotify" (the program's own description where it has one)
    DWORD pid = 0;              // the top process of its tree
    bool playing = false;       // an active session right now
    std::vector<std::wstring> devices;   // the output devices it has sessions on
};
std::vector<App> listAudioApps();
// The top process of a running program's tree, or 0. Prefers a process with an audio session.
DWORD findProgram(const std::wstring& exe);
std::wstring describeExe(const std::wstring& exe);   // "Spotify" from "Spotify.exe" if not running

// Process loopback capture needs Windows 10 build 20348 (in practice: Windows 11).
bool applicationAudioSupported();

}  // namespace ncap::audio
