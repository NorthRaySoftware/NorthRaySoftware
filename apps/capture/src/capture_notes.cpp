// NorthRay Capture's window: the Notes panel and its teleprompter (0.12).
//
// The panel (Layout menu › Notes) has two sides. Write: an ordinary text box (no scroll bar; the wheel scrolls)
// whose text is saved to notes.txt in Capture's folder about a second after you stop typing, and when Capture
// closes. Read: the same text large, scrolling up by itself at the speed chosen (or by the wheel), with a marker
// line a third of the way down to read from. Viewers never see it: it's in the window, not on the canvas.
#include "capture_window.hpp"

#include "log.hpp"

#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace ncap {

using cjui::Align;
using cjui::Font;
using cjui::Rect;
using cjui::widen;

namespace {
constexpr UINT_PTR kNotesSaveTimer = 43;
constexpr UINT_PTR kTeleTimer = 44;
const float kSpeeds[] = {12, 20, 30, 42, 56, 75, 100};   // DIPs a second
constexpr int kSpeedCount = 7;

LRESULT CALLBACK notesEditProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_MOUSEWHEEL) {   // no scroll bar: the wheel scrolls it
        SendMessageW(h, EM_LINESCROLL, 0, -GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA * 3);
        return 0;
    }
    if (m == WM_CHAR && w == 1) {   // Ctrl+A selects everything
        SendMessageW(h, EM_SETSEL, 0, -1);
        return 0;
    }
    if (m == WM_NCDESTROY) RemoveWindowSubclass(h, notesEditProc, 1);
    return DefSubclassProc(h, m, w, l);
}
}  // namespace

std::wstring CaptureWindow::notesPath() const { return stateFolder_ + L"\\notes.txt"; }

void CaptureWindow::createNotes() {
    notesEdit_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, 0, 0, 10, 10, hwnd(), nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
    SetWindowSubclass(notesEdit_, notesEditProc, 1, 0);
    SendMessageW(notesEdit_, EM_SETLIMITTEXT, 400000, 0);
    std::ifstream f(std::filesystem::path(notesPath()), std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) text.erase(0, 3);
    // Windows' text box wants \r\n.
    std::wstring w = widen(text), out;
    out.reserve(w.size() + w.size() / 20);
    for (std::size_t i = 0; i < w.size(); ++i) {
        if (w[i] == L'\n' && (i == 0 || w[i - 1] != L'\r')) out += L'\r';
        out += w[i];
    }
    loadingNotes_ = true;
    SetWindowTextW(notesEdit_, out.c_str());
    loadingNotes_ = false;
    notesText_ = out;
}

void CaptureWindow::notesChanged() {
    if (loadingNotes_) return;
    notesDirty_ = true;
    SetTimer(hwnd(), kNotesSaveTimer, 900, nullptr);
}

void CaptureWindow::saveNotes() {
    KillTimer(hwnd(), kNotesSaveTimer);
    if (!notesDirty_ || !notesEdit_) return;
    notesDirty_ = false;
    const int n = GetWindowTextLengthW(notesEdit_);
    std::wstring w((std::size_t)n + 1, L'\0');
    GetWindowTextW(notesEdit_, w.data(), n + 1);
    w.resize((std::size_t)n);
    notesText_ = w;
    std::wstring plain;   // saved with \n line ends
    for (wchar_t c : w)
        if (c != L'\r') plain += c;
    const std::wstring tmp = notesPath() + L".tmp";
    {
        std::ofstream f(std::filesystem::path(tmp), std::ios::binary | std::ios::trunc);
        f << cjui::narrow(plain);
        if (!f) {
            log::warn("The notes couldn't be saved");
            return;
        }
    }
    if (!MoveFileExW(tmp.c_str(), notesPath().c_str(), MOVEFILE_REPLACE_EXISTING)) log::warn("The notes couldn't be saved");
}

bool CaptureWindow::notesTimer(UINT_PTR id) {
    if (id == kNotesSaveTimer) {
        saveNotes();
        return true;
    }
    if (id != kTeleTimer) return false;
    const ULONGLONG now = GetTickCount64();
    const float dt = std::min(0.2f, (float)(now - teleLast_) / 1000.0f);
    teleLast_ = now;
    if (telePlaying_ && layout_.shows(Layout::Notes) && !settingsOpen_) {
        teleScroll_ += kSpeeds[std::clamp(settings_.notesSpeed, 0, kSpeedCount - 1)] * dt;
        if (teleScroll_ >= teleMax_) {   // the end: stop there
            teleScroll_ = teleMax_;
            telePlaying_ = false;
            KillTimer(hwnd(), kTeleTimer);
        }
        invalidate();
    }
    return true;
}

void CaptureWindow::setTelePlaying(bool on) {
    telePlaying_ = on;
    teleLast_ = GetTickCount64();
    if (on) {
        if (teleScroll_ >= teleMax_ - 1) teleScroll_ = 0;   // at the end: from the top again
        SetTimer(hwnd(), kTeleTimer, 16, nullptr);
    } else {
        KillTimer(hwnd(), kTeleTimer);
    }
    invalidate();
}

bool CaptureWindow::notesWheel(float x, float y, float lines) {
    if (!notesRead_ || !notesBody_.contains(x, y)) return false;
    teleScroll_ = std::clamp(teleScroll_ + lines * 30, 0.0f, teleMax_);
    invalidate();
    return true;
}

void CaptureWindow::drawNotesPanel(const Rect& body) {
    const float pad = 10;
    // The two sides.
    const Rect seg{body.x + pad, body.y + 6, std::min(200.0f, body.w - pad * 2), 30};
    segmented("notes:mode", seg, {L"Write", L"Read"}, notesRead_ ? 1 : 0, [this](int i) {
        if ((i == 1) == notesRead_) return;
        saveNotes();
        if (i == 1) {
            const int n = GetWindowTextLengthW(notesEdit_);
            std::wstring w((std::size_t)n + 1, L'\0');
            GetWindowTextW(notesEdit_, w.data(), n + 1);
            w.resize((std::size_t)n);
            notesText_ = w;
        } else {
            setTelePlaying(false);
        }
        notesRead_ = i == 1;
        invalidate();
    });
    const Rect box{body.x + pad, seg.bottom() + 8, body.w - pad * 2, body.bottom() - seg.bottom() - 8 - pad};
    notesBody_ = box;
    if (!notesRead_) {
        gfx_.fillRounded(box, 8, pal_.sunken);
        // The text box sits inside, unless something is drawn over the panels.
        if (!asking_ && box.w > 40 && box.h > 30) {
            const float s = gfx_.dpiScale();
            if (notesFontScale_ != s || !notesFont_) {
                notesFontScale_ = s;
                if (notesFont_) DeleteObject(notesFont_);
                notesFont_ = CreateFontW(-(int)std::lround(14 * s), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
                SendMessageW(notesEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(notesFont_), TRUE);
            }
            SetWindowPos(notesEdit_, HWND_TOP, (int)std::lround((box.x + 10) * s), (int)std::lround((box.y + 8) * s),
                         (int)std::lround((box.w - 20) * s), (int)std::lround((box.h - 16) * s), SWP_NOACTIVATE | SWP_SHOWWINDOW);
            notesDrawn_ = true;
        }
        return;
    }

    // Read: the teleprompter. Its controls flow after the switch, wrapping onto more rows in a narrow panel.
    const float size = std::clamp(settings_.notesSize, 18.0f, 72.0f);
    float cx = seg.right() + 8, cy = seg.y;
    auto place = [&](float w) {
        if (cx + w > body.right() - pad) {
            cx = body.x + pad;
            cy += 36;
        }
        const Rect r{cx, cy, w, 30};
        cx += w + 5;
        return r;
    };
    auto small = [&](const std::string& id, const std::wstring& label, float w, const std::wstring& tip, std::function<void()> act) {
        const Rect r = place(w);
        button(id, r, label, std::move(act));
        tooltip(id, tip);
        return r;
    };
    {
        // Drawn icons (not symbol characters, which some fonts lack).
        const Rect r = small("notes:play", L"", 40, L"Play or pause: the notes scroll up at the speed chosen. The wheel scrolls them by hand.",
                             [this] { setTelePlaying(!telePlaying_); });
        const float mx = r.x + r.w / 2, my = r.y + r.h / 2;
        if (telePlaying_) {
            gfx_.fill({mx - 5, my - 6, 3.5f, 12}, pal_.ink);
            gfx_.fill({mx + 1.5f, my - 6, 3.5f, 12}, pal_.ink);
        } else {
            gfx_.fillTriangle(mx - 4, my - 7, mx - 4, my + 7, mx + 7, my, pal_.ink);
        }
    }
    small("notes:slower", L"\u2212", 32, L"Slower", [this] {
        settings_.notesSpeed = std::max(0, settings_.notesSpeed - 1);
        settings_.save(settingsPath_);
    });
    {
        const Rect r = place(24);
        gfx_.text(std::to_wstring(settings_.notesSpeed + 1), r, Font::BodyStrong, pal_.ink, {Align::Center, false, false, true});
    }
    small("notes:faster", L"+", 32, L"Faster", [this] {
        settings_.notesSpeed = std::min(kSpeedCount - 1, settings_.notesSpeed + 1);
        settings_.save(settingsPath_);
    });
    small("notes:smaller", L"A\u2212", 40, L"Smaller text", [this] {
        settings_.notesSize = std::max(18.0f, settings_.notesSize - 4);
        settings_.save(settingsPath_);
    });
    small("notes:bigger", L"A+", 40, L"Bigger text", [this] {
        settings_.notesSize = std::min(72.0f, settings_.notesSize + 4);
        settings_.save(settingsPath_);
    });
    {
        const Rect r = small("notes:top", L"", 36, L"Back to the top", [this] {
            teleScroll_ = 0;
            invalidate();
        });
        const float mx = r.x + r.w / 2, my = r.y + r.h / 2;
        gfx_.line(mx - 6, my - 7, mx + 6, my - 7, pal_.ink, 1.6f);
        gfx_.line(mx, my - 4, mx, my + 7, pal_.ink, 1.6f);
        gfx_.line(mx - 4.5f, my, mx, my - 4.5f, pal_.ink, 1.6f);
        gfx_.line(mx + 4.5f, my, mx, my - 4.5f, pal_.ink, 1.6f);
    }
    const Rect read{body.x + pad, cy + 38, body.w - pad * 2, body.bottom() - cy - 38 - pad};
    if (read.h < 60) return;
    notesBody_ = read;
    gfx_.fillRounded(read, 8, cjui::hexColor("#05070D"));
    gfx_.pushClip(read);
    const std::wstring text = notesText_.empty() ? std::wstring(L"Nothing to read yet: write your notes on the Write side.") : notesText_;
    std::wstring plain;
    for (wchar_t c : text)
        if (c != L'\r') plain += c;
    const Rect col{read.x + 18, 0, read.w - 36, 0};
    const float textH = gfx_.measureSized(plain, col.w, size, true);
    const float marker = read.y + read.h / 3;
    // Scrolling starts with the first line on the marker and ends with the last line there.
    teleMax_ = std::max(0.0f, textH - size * 1.3f);
    teleScroll_ = std::clamp(teleScroll_, 0.0f, teleMax_);
    gfx_.textSized(plain, {col.x, marker - teleScroll_, col.w, textH + 10}, size, true, cjui::hexColor("#F4F1E8"));
    // Fade at the top and bottom, and the marker.
    const cjui::Color bg = cjui::hexColor("#05070D");
    gfx_.fillGradient({read.x, read.y, read.w, read.h * 0.18f}, 0, bg, bg.withAlpha(0));
    gfx_.fillGradient({read.x, read.bottom() - read.h * 0.18f, read.w, read.h * 0.18f}, 0, bg.withAlpha(0), bg);
    gfx_.fill({read.x, marker - 4, 4, size * 1.2f + 8}, pal_.ribbon);
    gfx_.popClip();
}

}  // namespace ncap
