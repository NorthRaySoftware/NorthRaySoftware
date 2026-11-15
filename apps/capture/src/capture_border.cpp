// NorthRay Capture's window: the Camera Border maker. Any item in a scene (a camera above all) can have
// rounded corners, a ring of colour and a glow or shadow; it's edited in the dock below the live preview,
// so the change shows on the real picture as it's made. "Save as PNG" writes the same border, with the
// inside clear, as an overlay for other programs.
#include "capture_window.hpp"

#include "log.hpp"

#include "ncap/border.hpp"

#include "cjui/menu.hpp"

#include <shlobj.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <ctime>

namespace ncap {

using cjui::Align;
using cjui::Color;
using cjui::Font;
using cjui::MenuItem;
using cjui::narrow;
using cjui::Rect;
using cjui::widen;
using sourceutil::Rgba;
using video::ComPtr;

namespace {

struct Named {
    const wchar_t* name;
    const char* hex;
};
const Named kColours[] = {{L"White", "#FFFFFF"},     {L"Black", "#000000"},  {L"NorthRay teal", "#3FCF8E"}, {L"Sky blue", "#4DA3FF"},
                          {L"Cyan", "#00E5FF"},      {L"Purple", "#7A5CFF"}, {L"Pink", "#FF3DD6"},          {L"Yellow", "#FFD23F"},
                          {L"Gold", "#D9A441"},      {L"Red", "#E0566A"},    {L"Dark blue", "#1E2A44"}};

Color toColor(const Rgba& c) { return {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f}; }

// The item's size on the canvas (what the border goes round).
ItemSize shownSize(const Transform& t, ItemSize src) {
    const float w = t.width > 0 ? t.width : src.w - t.cropLeft - t.cropRight;
    const float h = t.height > 0 ? t.height : src.h - t.cropTop - t.cropBottom;
    return {std::max(0.0f, w), std::max(0.0f, h)};
}

bool writePng(const std::wstring& path, const std::vector<std::uint8_t>& rgba, int w, int h) {
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(f->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) || FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(enc->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) || FAILED(frame->SetSize((UINT)w, (UINT)h)))
        return false;
    // WIC wants BGRA; straight (not premultiplied) alpha, as PNG keeps it.
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&fmt)) || fmt != GUID_WICPixelFormat32bppBGRA) return false;
    std::vector<std::uint8_t> bgra(rgba);
    for (std::size_t i = 0; i + 3 < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
    return SUCCEEDED(frame->WritePixels((UINT)h, (UINT)w * 4, (UINT)bgra.size(), bgra.data())) && SUCCEEDED(frame->Commit()) &&
           SUCCEEDED(enc->Commit());
}

}  // namespace

void CaptureWindow::openBorder(const Id& item) {
    if (!scenes_.item(scenes_.current(), item)) return;
    borderFor_ = item;
    selected_ = item;
    resetScroll();
    invalidate();
}

void CaptureWindow::setItemBorder(const Id& item, const border::Style& b) {
    scenes_.setItemBorder(scenes_.current(), item, b);
    scenesChanged();
}

void CaptureWindow::saveBorderPng(const Id& itemId) {
    const SceneItem* item = scenes_.item(scenes_.current(), itemId);
    if (!item) return;
    const ItemSize sz = shownSize(item->transform, sizeOf(item->source));
    if (sz.w < 2 || sz.h < 2) {
        flash(L"The item doesn't have a size yet (its picture hasn't arrived), so there's nothing to fit a border to.", 6);
        return;
    }
    border::Style b = item->border;
    b.on = true;
    int w = 0, h = 0;
    const auto px = border::render(b, (int)std::lround(sz.w), (int)std::lround(sz.h), w, h);
    PWSTR pics = nullptr;
    std::wstring folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pics))) folder = std::wstring(pics) + L"\\NorthRay Capture";
    CoTaskMemFree(pics);
    if (folder.empty()) return;
    SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    wchar_t stamp[32];
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    wcsftime(stamp, 32, L"%Y-%m-%d %H.%M.%S", &tmv);
    const std::wstring path = folder + L"\\Border " + std::to_wstring(w) + L"x" + std::to_wstring(h) + L" - " + stamp + L".png";
    if (!writePng(path, px, w, h)) {
        flash(L"The PNG couldn't be written to your Pictures folder.", 6);
        return;
    }
    log::info("Border saved as PNG: " + narrow(path));
    flash(L"Saved " + std::to_wstring(w) + L"×" + std::to_wstring(h) + L" PNG (clear inside) in Pictures › NorthRay Capture.", 8);
}

void CaptureWindow::drawBorderEditor(const Rect& area) {
    // Follows the selection, so another camera can be done without closing.
    if (!selected_.empty() && selected_ != borderFor_ && scenes_.item(scenes_.current(), selected_)) borderFor_ = selected_;
    const SceneItem* item = scenes_.item(scenes_.current(), borderFor_);
    if (!item) {
        borderFor_.clear();
        return;
    }
    const Id id = item->id;
    border::Style b = item->border;
    const Source* src = scenes_.source(item->source);
    const ItemSize sz = shownSize(item->transform, sizeOf(item->source));
    auto set = [this, id](border::Style s) { setItemBorder(id, s); };

    gfx_.fillRounded(area, 10, pal_.surface);
    const float top = beginScroll(area);
    const float pad = 18;
    float y = top + 14;
    const float x0 = area.x + pad, W = area.w - pad * 2;
    // Title row.
    gfx_.text(L"Border: " + (src ? widen(src->name) : std::wstring(L"item")), {x0, y, W - 470, 30}, Font::Title, pal_.ink, {Align::Left, false, true, true});
    toggle("brd:on", x0 + W - 460, y + 4, b.on, [b, set]() mutable {
        b.on = !b.on;
        set(b);
    });
    gfx_.text(b.on ? L"On" : L"Off", {x0 + W - 412, y + 2, 40, 26}, Font::Body, pal_.inkMuted, {Align::Left, false, true});
    button("brd:png", {x0 + W - 360, y, 160, 32}, L"Save as PNG", [this, id] { saveBorderPng(id); });
    button("brd:done", {x0 + W - 190, y, 190, 32}, L"Done", [this] {
        borderFor_.clear();
        invalidate();
    }, true);
    y += 42;
    y += gfx_.text(L"Shown live in the preview above. Click another item there to give it a border too. The ring is drawn just inside the "
                   L"item's edge; the glow goes round it.",
                   {x0, y, W, 40}, Font::Small, pal_.inkMuted) + 10;

    // Presets.
    {
        float px = x0;
        const auto& ps = border::presets();
        for (std::size_t i = 0; i < ps.size(); ++i) {
            const std::wstring name = widen(ps[i].name);
            const float bw = 30 + (float)name.size() * 7.4f;
            if (px + bw > x0 + W) {
                px = x0;
                y += 40;
            }
            button("brd:preset:" + std::to_string(i), {px, y, bw, 32}, name, [this, id, i] {
                const SceneItem* it = scenes_.item(scenes_.current(), id);
                if (!it) return;
                const border::Preset& p = border::presets()[i];
                if (p.square) {
                    // A circle needs a square: crop the picture's middle and make the item square, same height, same centre.
                    const ItemSize s = sizeOf(it->source);
                    if (s.w > 0 && s.h > 0) {
                        Transform t = it->transform;
                        const ItemSize shown = shownSize(t, s);
                        const float cx = t.x + shown.w / 2, cy = t.y + shown.h / 2;
                        t.cropLeft = t.cropRight = t.cropTop = t.cropBottom = 0;
                        if (s.w > s.h) t.cropLeft = t.cropRight = (s.w - s.h) / 2;
                        else t.cropTop = t.cropBottom = (s.h - s.w) / 2;
                        const float side = std::min(shown.w, shown.h);
                        t.width = t.height = side;
                        t.x = cx - side / 2;
                        t.y = cy - side / 2;
                        setTransform(id, t);
                    }
                }
                setItemBorder(id, p.style);
            });
            px += bw + 8;
        }
        y += 46;
    }

    // Two columns of values.
    const float colGap = 32, colW = (W - colGap) / 2;
    auto sliderRow = [&](float cx, float& cy, const std::string& key, const std::wstring& label, float v, float lo, float hi, const wchar_t* unit,
                         bool enabled, std::function<void(border::Style&, float)> apply) {
        gfx_.text(label, {cx, cy, 130, 26}, Font::Body, enabled ? pal_.ink : pal_.inkFaint, {Align::Left, false, true, true});
        if (enabled)
            slider("brd:" + key, {cx + 136, cy + 6, colW - 136 - 86, 16}, (v - lo) / (hi - lo), [this, id, lo, hi, apply](float t) {
                const SceneItem* it = scenes_.item(scenes_.current(), id);
                if (!it) return;
                border::Style s = it->border;
                const float nv = std::round(lo + std::clamp(t, 0.0f, 1.0f) * (hi - lo));
                apply(s, nv);
                if (!(s == it->border)) setItemBorder(id, s);
            });
        wchar_t t[32];
        swprintf(t, 32, L"%.0f%ls", v, unit);
        gfx_.text(t, {cx + colW - 80, cy, 80, 26}, Font::Body, enabled ? pal_.ink : pal_.inkFaint, {Align::Right, false, true});
        cy += 36;
    };
    auto colourRow = [&](float cx, float& cy, const std::string& key, const std::wstring& label, const Rgba& c, bool enabled,
                         std::function<void(border::Style&, const Rgba&)> apply) {
        gfx_.text(label, {cx, cy, 130, 26}, Font::Body, enabled ? pal_.ink : pal_.inkFaint, {Align::Left, false, true, true});
        const Rect sw{cx + 136, cy + 1, 26, 24};
        gfx_.fillRounded(sw, 6, toColor(c));
        gfx_.strokeRounded(sw, 6, pal_.rule, 1);
        if (enabled)
            button("brd:" + key, {cx + 172, cy - 2, colW - 172, 30}, widen(sourceutil::colourText(c)), [this, id, apply] {
                std::vector<MenuItem> items;
                for (int i = 0; i < (int)(sizeof kColours / sizeof kColours[0]); ++i) items.push_back(MenuItem::item(kColours[i].name, 100 + (UINT)i));
                items.push_back(MenuItem::sep());
                items.push_back(MenuItem::item(L"Another colour…", 99, L"#RRGGBB"));
                POINT pt;
                GetCursorPos(&pt);
                const UINT cmd = cjui::showMenu(hwnd(), items, pt, pal_);
                auto applyText = [this, id, apply](const std::string& text) {
                    Rgba v;
                    const SceneItem* it = scenes_.item(scenes_.current(), id);
                    if (!it) return;
                    if (!sourceutil::parseColour(text, v)) {
                        flash(L"Type a colour like #3FCF8E (or #3FCF8E80 for see-through).", 5);
                        return;
                    }
                    border::Style s = it->border;
                    apply(s, v);
                    setItemBorder(id, s);
                };
                if (cmd >= 100 && cmd < 100 + sizeof kColours / sizeof kColours[0]) applyText(kColours[cmd - 100].hex);
                else if (cmd == 99) askText(L"A colour (#RRGGBB, or #RRGGBBAA)", L"#", [applyText](const std::wstring& t) { applyText(narrow(t)); });
            });
        cy += 36;
    };

    float ly = y, ry = y;
    const float lx = x0, rx = x0 + colW + colGap;
    gfx_.text(L"RING AND CORNERS", {lx, ly, colW, 18}, Font::Small, pal_.inkFaint);
    ly += 24;
    sliderRow(lx, ly, "width", L"Thickness", b.width, 0, 60, L" px", true, [](border::Style& s, float v) { s.width = v; });
    sliderRow(lx, ly, "round", L"Roundness", b.roundness, 0, 100, L"%", true, [](border::Style& s, float v) { s.roundness = v; });
    colourRow(lx, ly, "c1", b.gradient ? L"From colour" : L"Colour", b.colour, true, [](border::Style& s, const Rgba& c) { s.colour = c; });
    gfx_.text(L"Gradient", {lx, ly, 130, 26}, Font::Body, pal_.ink, {Align::Left, false, true});
    toggle("brd:grad", lx + 136, ly + 2, b.gradient, [b, set]() mutable {
        b.gradient = !b.gradient;
        set(b);
    });
    ly += 36;
    colourRow(lx, ly, "c2", L"To colour", b.colour2, b.gradient, [](border::Style& s, const Rgba& c) { s.colour2 = c; });
    sliderRow(lx, ly, "angle", L"Direction", b.angle, 0, 360, L"°", b.gradient, [](border::Style& s, float v) { s.angle = v; });

    gfx_.text(L"GLOW OR SHADOW", {rx, ry, colW, 18}, Font::Small, pal_.inkFaint);
    ry += 24;
    sliderRow(rx, ry, "glow", L"Size", b.glow, 0, 120, L" px", true, [](border::Style& s, float v) { s.glow = v; });
    const bool g = b.glow > 0;
    colourRow(rx, ry, "gc", L"Colour", b.glowColour, g, [](border::Style& s, const Rgba& c) { s.glowColour = c; });
    sliderRow(rx, ry, "gop", L"Strength", b.glowOpacity, 0, 100, L"%", g, [](border::Style& s, float v) { s.glowOpacity = v; });
    sliderRow(rx, ry, "ox", L"Move across", b.offsetX, -60, 60, L" px", g, [](border::Style& s, float v) { s.offsetX = v; });
    sliderRow(rx, ry, "oy", L"Move down", b.offsetY, -60, 60, L" px", g, [](border::Style& s, float v) { s.offsetY = v; });
    y = std::max(ly, ry) + 4;
    wchar_t info[200];
    swprintf(info, 200, L"The item is %.0f × %.0f on the canvas. Save as PNG makes a %.0f × %.0f picture with the inside left clear.", sz.w, sz.h,
             sz.w + 2 * border::margin([&] { border::Style s = b; s.on = true; return s; }()),
             sz.h + 2 * border::margin([&] { border::Style s = b; s.on = true; return s; }()));
    y += gfx_.text(info, {x0, y, W, 40}, Font::Small, pal_.inkMuted) + 12;
    endScroll(y - top);
}

}  // namespace ncap
