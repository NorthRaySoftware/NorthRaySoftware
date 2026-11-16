// NorthRay Capture: video sources (system 2). Each source keeps its newest picture in its own
// texture on the graphics card; the compositor draws whatever is there, so a slow or stalled source
// never holds up the output.
//
//   Display  DXGI Desktop Duplication (fastest, the default), else Windows.Graphics.Capture, else
//            GDI (a compatibility fallback that copies through the CPU).
//   Window   Windows.Graphics.Capture (works when the window is covered), else GDI PrintWindow.
//            Remembers the program, window class and title, and finds the window again when the
//            program restarts.
//   Camera   Media Foundation source reader, converted to RGB on the CPU for now (a GPU path is
//            part of Milestone 12's optimisation work).
//
// Threads: update() runs on the video thread only and is where all drawing happens. Cameras and the
// GDI methods read on their own thread and hand over a CPU picture. Settings changes come from the
// UI thread through setSettings() and are applied by the next update().
#pragma once

#include "video/gpu.hpp"

#include "ncap/audio.hpp"
#include "ncap/scenes.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace ncap::video {

class VideoSource {
public:
    virtual ~VideoSource() = default;

    // Video thread. Takes in the newest picture, if there's one.
    virtual void update(Gpu& gpu) = 0;
    ID3D11ShaderResourceView* view() const { return hasPicture_ ? out_.srv.Get() : nullptr; }   // video thread
    ID3D11Texture2D* texture() const { return hasPicture_ ? out_.tex.Get() : nullptr; }        // video thread
    // Pictures with see-through parts (images, text): drawn with their alpha. Captures are opaque.
    virtual bool hasAlpha() const { return false; }
    // Media files: their sound, for the mixer (48 kHz stereo float), and playback commands
    // ("pause", "play", "restart", "next"). Other sources have neither.
    virtual std::shared_ptr<audio::Ring> sound() { return nullptr; }
    virtual void command(const std::string&) {}
    virtual double length() const { return 0; }   // media: the current file's length in seconds (0 unknown)

    // Any thread.
    int width() const { return w_.load(); }
    int height() const { return h_.load(); }
    std::wstring status() const;     // "Desktop Duplication · 1920×1080", "Waiting for Minecraft"...
    bool problem() const { return problem_.load(); }   // the status is a problem, not progress
    void setSettings(const std::string& json);

protected:
    void setStatus(const std::wstring& s, bool problem = false);
    // The settings to apply, once, when they've changed (video thread).
    bool takeSettings(std::string& json);
    // Threads that make CPU pictures skip the work when nobody has asked for one lately (the
    // source isn't in the shown scene, or nothing is composing).
    void wanted() { lastWanted_ = GetTickCount64(); }
    bool recentlyWanted() const { return GetTickCount64() - lastWanted_.load() < 500; }
    Target out_;
    bool hasPicture_ = false;
    std::atomic<ULONGLONG> lastWanted_{0};
    std::atomic<int> w_{0}, h_{0};

private:
    mutable std::mutex m_;
    std::wstring status_ = L"Starting…";
    std::atomic<bool> problem_{false};
    std::string pending_;
    bool changed_ = false;
};

// Null for kinds that aren't pictures captured from the system (images, text and so on arrive in
// Milestone 8).
std::shared_ptr<VideoSource> createVideoSource(SourceKind kind, const std::string& settings);
// The sources made in Capture itself (made_sources.cpp): images, text, colours, slideshows, regions.
std::shared_ptr<VideoSource> createMadeSource(SourceKind kind, const std::string& settings);
// The Stream mix's frequency bands (0..1) and loudness for the aurora pulse, set by the window as it
// analyses the sound and read by the sources on the video thread.
void setSoundBands(const std::vector<float>& bands, float level);

}  // namespace ncap::video
