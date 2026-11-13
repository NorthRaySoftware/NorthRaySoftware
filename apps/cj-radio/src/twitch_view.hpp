#pragma once
// Hosts Twitch's own embedded player inside CJ Radio, using Microsoft Edge
// WebView2. The player is always shown at a usable size with its controls:
// Twitch's embed rules don't allow hiding or covering it, so CJ Radio pauses
// it when the Listen page isn't showing rather than playing it unseen.
//
// The player reports whether the channel is broadcasting; that report is
// passed to the app as one of the messages below.

#include <windows.h>

#include <memory>
#include <string>

namespace cjr {

class TwitchView {
public:
    enum class Event { Ready, Online, Offline, Playing, Paused, Failed, SoundOnlyOn, SoundOnlyOff, SoundOnlyUnavailable };

    TwitchView();
    ~TwitchView();
    TwitchView(const TwitchView&) = delete;
    TwitchView& operator=(const TwitchView&) = delete;

    // Starts WebView2 asynchronously. `notify` receives `message` with the
    // Event in wParam. Returns false (and sets error()) if WebView2 can't be
    // used on this PC at all.
    bool create(HWND parent, const std::wstring& exeDir, const std::wstring& userDataDir,
                const std::string& channel, HWND notify, UINT message);
    void destroy();

    // Position in parent client pixels, clipped to `visible` (the scrolling
    // page area). Pass an empty rect to hide it.
    void place(const RECT& bounds, const RECT& visible);
    void hide();
    void pause();
    void play();
    // Twitch's own "Audio Only" quality: the player stays on screen, but only
    // the sound is streamed. Remembered until changed; applied once playing.
    void setSoundOnly(bool on);

    bool ready() const;
    const std::wstring& error() const { return error_; }

    // Called by the WebView2 callbacks.
    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
    std::wstring error_;
    bool soundOnly_ = false;
};

}  // namespace cjr
