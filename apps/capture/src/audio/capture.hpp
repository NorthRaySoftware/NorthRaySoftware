// NorthRay Capture: sound sources (system 3). Each one reads on its own thread, event-driven
// WASAPI in shared mode, and hands 48 kHz stereo float frames to the mixer through a lock-free ring.
//
//   Input        a microphone or other input device (or whichever is Windows' default).
//   Desktop      everything an output device plays (loopback), or the default output.
//   Application  one program's sound through process loopback (Windows 11): its whole process tree,
//                found again by the program's name whenever it starts. Other programs aren't heard.
//
// Windows converts each device's own format to 48 kHz stereo float (AUTOCONVERTPCM). Gaps where a
// loopback has nothing to play are filled with silence by the mixer.
#pragma once

#include <windows.h>

#include "ncap/audio.hpp"
#include "ncap/scenes.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace ncap::audio {

class Capture {
public:
    explicit Capture(SourceKind kind);
    ~Capture();
    void setSettings(const std::string& json);   // (re)starts with the new device or program

    Ring& ring() { return ring_; }
    std::wstring status() const;
    bool problem() const { return problem_.load(); }
    bool live() const { return live_.load(); }   // reading from a device or program right now

private:
    void run();
    void setStatus(const std::wstring& s, bool problem = false);
    HRESULT readFrom(const std::wstring& deviceId, bool loopback, DWORD processId);

    SourceKind kind_;
    Ring ring_{kRate / 2};   // 0.5 s
    std::thread thread_;
    std::atomic<bool> quit_{false}, restart_{false}, live_{false}, problem_{false};
    mutable std::mutex m_;
    std::string settings_;
    std::wstring status_ = L"Starting…";
};

}  // namespace ncap::audio
