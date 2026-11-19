// NorthRay Capture's window: what each stream destination receives (0.12).
//
// A page (from Controls' "What each destination gets" and Settings › Streaming) with a card per destination: where it
// goes (the server's host only, never the key), the picture (every destination gets the program at the canvas size
// and frame rate; destinations at the same bitrate share one encode), the sound it carries with that mix's live meter
// and what's on it, and while live, how it's doing. It shows the settings and the live figures; it isn't a separate
// video preview per destination, since the picture is the same for all of them.
#include "capture_window.hpp"

#include <algorithm>
#include <cmath>

namespace ncap {

using cjui::Align;
using cjui::Color;
using cjui::Font;
using cjui::Rect;
using cjui::widen;

std::wstring streamDestName(const Settings& s, std::size_t i);   // capture_stream.cpp

namespace {
std::wstring hostOf(const std::string& url) {
    std::string h = url;
    const auto scheme = h.find("://");
    if (scheme != std::string::npos) h = h.substr(scheme + 3);
    const auto slash = h.find('/');
    if (slash != std::string::npos) h = h.substr(0, slash);
    const auto at = h.rfind('@');   // never show anything before an @ (a user name or password in the address)
    if (at != std::string::npos) h = h.substr(at + 1);
    return widen(h);
}
}  // namespace

void CaptureWindow::openDestView() {
    openSettings(true);
    destViewOpen_ = true;
    invalidate();
}

void CaptureWindow::drawDestView(const Rect& area) {
    gfx_.fill(area, pal_.paper);
    const float colW = std::min(820.0f, area.w - 48);
    const float x = area.x + (area.w - colW) / 2;
    const float top = beginScroll(area);
    float y = top + 24;
    gfx_.text(L"What each destination gets", {x, y, colW - 130, 34}, Font::Title, pal_.ink, {Align::Left, false});
    button("dv:close", {x + colW - 110, y + 2, 110, 34}, L"Done", [this] { openSettings(false); });
    y += 46;
    wchar_t pic[96];
    swprintf(pic, 96, L"%d × %d at %d fps", scenes_.canvasWidth, scenes_.canvasHeight, settings_.outputFps);
    y += gfx_.text(L"Every destination gets the same picture: the program (what's live), " + std::wstring(pic) +
                       L", H.264. What differs is the bitrate and the sound each one carries.",
                   {x, y, colW, 60}, Font::Body, pal_.inkMuted) +
         18;
    const Color live = cjui::hexColor("#3FCF8E"), wait = cjui::hexColor("#E8C547"), bad = cjui::hexColor("#E0566A");
    double totalKbps = 0;
    for (std::size_t i = 0; i < settings_.destinations.size(); ++i) {
        const auto& d = settings_.destinations[i];
        const bool known = streamer_.known(d.id);
        const output::DestinationStatus st = known ? streamer_.status(d.id) : output::DestinationStatus{};
        int sharing = 0;
        for (auto& o : settings_.destinations)
            if (&o != &d && o.enabled && o.kbps == d.kbps) ++sharing;
        if (d.enabled) totalKbps += d.kbps + settings_.audioKbps;
        const Rect card{x, y, colW, 168};
        gfx_.fillRounded(card, 10, pal_.ink.withAlpha(pal_.dark ? 0.05f : 0.035f));
        gfx_.strokeRounded(card, 10, pal_.rule, 1);
        // Name, where it goes, its state.
        Color dot = pal_.inkFaint;
        bool coloured = true;
        coloured = false;
        std::wstring state = d.enabled ? L"Starts with Start streaming" : L"Switched off (start it from its row in Controls)";
        if (known) switch (st.state) {
            case output::DestinationStatus::Live: {
                dot = live;
                coloured = true;
                wchar_t t[160];
                swprintf(t, 160, L"Live · %.1f Mbit/s going out · %llu frames dropped · %d reconnection%ls", st.kbps / 1000.0,
                         (unsigned long long)st.droppedFrames, st.reconnects, st.reconnects == 1 ? L"" : L"s");
                state = t;
                break;
            }
            case output::DestinationStatus::Reconnecting: dot = wait, coloured = true, state = L"Reconnecting"; break;
            case output::DestinationStatus::Connecting:
            case output::DestinationStatus::Waiting: dot = wait, coloured = true, state = L"Connecting"; break;
            case output::DestinationStatus::Failed: dot = bad, coloured = true, state = L"Stopped: " + st.message; break;
            default: break;
        }
        gfx_.circle(card.x + 20, card.y + 24, 5, dot, true);
        gfx_.text(streamDestName(settings_, i), {card.x + 34, card.y + 12, colW * 0.45f, 24}, Font::Heading, pal_.ink, {Align::Left, false, true, true});
        gfx_.text(hostOf(d.url), {card.x + colW * 0.45f, card.y + 14, colW * 0.55f - 18, 22}, Font::Small, pal_.inkMuted, {Align::Right, false, true, true});
        gfx_.text(state, {card.x + 34, card.y + 38, colW - 52, 20}, Font::Small, coloured ? dot : pal_.inkMuted, {Align::Left, false, true, true});
        // Picture and sound, side by side.
        const float half = (colW - 52) / 2;
        const float py = card.y + 70;
        gfx_.text(L"PICTURE", {card.x + 18, py, half, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
        wchar_t vid[160];
        swprintf(vid, 160, L"%ls · %.1f Mbit/s constant", pic, d.kbps / 1000.0);
        gfx_.text(vid, {card.x + 18, py + 20, half, 20}, Font::Body, pal_.ink, {Align::Left, false, true, true});
        gfx_.text(sharing ? L"Shares one encode with " + std::to_wstring(sharing) + (sharing == 1 ? L" other destination" : L" others")
                          : std::wstring(L"Its own encode"),
                  {card.x + 18, py + 42, half, 20}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
        const float sx = card.x + 34 + half;
        gfx_.text(L"SOUND", {sx, py, half, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
        const int bus = std::clamp(d.bus, 0, 6);
        std::wstring mix = bus == 0 ? L"The Stream mix" : L"Track " + std::to_wstring(bus);
        wchar_t aac[48];
        swprintf(aac, 48, L" · AAC %d kbit/s", settings_.audioKbps);
        gfx_.text(mix + aac, {sx, py + 20, half, 20}, Font::Body, pal_.ink, {Align::Left, false, true, true});
        drawMeter({sx, py + 46, half, 8}, busLeft_[bus], busRight_[bus], false);
        std::vector<std::string> on;
        for (auto& s : scenes_.sources())
            if (hasSound(s.kind) && (audioBuses(s) & audio::busBit(bus)) && !audioMuted(s)) on.push_back(s.name);
        std::wstring who = on.empty() ? L"Nothing is sent to this mix" : L"";
        for (std::size_t k = 0; k < on.size() && k < 4; ++k) who += (k ? L", " : L"") + widen(on[k]);
        if (on.size() > 4) who += L", +" + std::to_wstring(on.size() - 4);
        gfx_.text(who, {sx, py + 60, half, 20}, Font::Small, on.empty() ? wait : pal_.inkMuted, {Align::Left, false, true, true});
        y += card.h + 14;
    }
    wchar_t sum[160];
    swprintf(sum, 160, L"Together the switched-on destinations need about %.1f Mbit/s of upload, including a little for the protocol.", totalKbps * 1.05 / 1000.0);
    y += gfx_.text(sum, {x, y + 4, colW, 40}, Font::Small, pal_.inkMuted) + 28;
    endScroll(y - top);
}

}  // namespace ncap
