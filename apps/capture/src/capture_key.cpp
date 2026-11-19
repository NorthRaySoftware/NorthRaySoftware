// NorthRay Capture's window: green screen on a scene item (its menu's Green screen submenu), and the
// eyedropper that picks the screen's colour from the preview.
//
// The eyedropper: the next click in the preview names a canvas pixel. The item's key is switched off
// for a moment, and a one-off frame sink on the engine thread copies that one pixel of the composed
// canvas (BGRA) into a 1x1 staging texture and reads it. A few frames are skipped first so the
// canvas is the one drawn without the key. The key then comes back on with that colour.
#include "capture_window.hpp"

#include "log.hpp"

#include "cjui/menu.hpp"

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>

namespace ncap {

using cjui::MenuItem;
using cjui::narrow;
using cjui::widen;
using Microsoft::WRL::ComPtr;

namespace {
enum : UINT {
    CmdKeyOff = 6900, CmdKeyGreen, CmdKeyBlue, CmdKeyPick, CmdKeyCustom,
    CmdKeySim0 = 6910, CmdKeySoft0 = 6920, CmdKeySpill0 = 6930,
    CmdPtrOff = 6950, CmdPtrOn, CmdPtrClicks, CmdPtrColour, CmdPtrSecond, CmdPtrSize0 = 6960, CmdPtrStrength0 = 6970,
};
const int kSims[] = {20, 30, 40, 50, 60}, kSofts[] = {2, 5, 8, 15, 25}, kSpills[] = {0, 25, 50, 75, 100};

struct PickState {
    std::atomic<int> skip{0};
    std::atomic<bool> done{false};
    int x = 0, y = 0;
    std::atomic<std::uint32_t> bgra{0};
};
PickState gPick;
}  // namespace

std::vector<MenuItem> CaptureWindow::keyMenuItems(const SceneItem& item) {
    const keying::Key& k = item.key;
    std::vector<MenuItem> m;
    m.push_back(MenuItem::item(L"Off", CmdKeyOff, {}, true, !k.on));
    m.push_back(MenuItem::item(L"Green screen", CmdKeyGreen, L"#00B140", true, k.on && k.colour == keying::kGreen));
    m.push_back(MenuItem::item(L"Blue screen", CmdKeyBlue, L"#0047BB", true, k.on && k.colour == keying::kBlue));
    m.push_back(MenuItem::item(L"Pick the colour from the preview…", CmdKeyPick, L"click your screen"));
    m.push_back(MenuItem::item(L"Another colour…", CmdKeyCustom, k.on ? widen(sourceutil::colourText(k.colour)) : L""));
    m.push_back(MenuItem::sep());
    std::vector<MenuItem> sim, soft, spill;
    for (int i = 0; i < 5; ++i) {
        sim.push_back(MenuItem::item(std::to_wstring(kSims[i]) + L"%", CmdKeySim0 + (UINT)i, i == 0 ? L"only the exact colour" : i == 4 ? L"a wide range" : L"",
                                     true, std::lround(k.similarity) == kSims[i]));
        soft.push_back(MenuItem::item(std::to_wstring(kSofts[i]) + L"%", CmdKeySoft0 + (UINT)i, i == 0 ? L"hard" : i == 4 ? L"very soft" : L"", true,
                                      std::lround(k.smoothness) == kSofts[i]));
        spill.push_back(MenuItem::item(std::to_wstring(kSpills[i]) + L"%", CmdKeySpill0 + (UINT)i, i == 0 ? L"none" : L"", true,
                                       std::lround(k.spill) == kSpills[i]));
    }
    m.push_back(MenuItem::sub(L"How much is cleared", sim));
    m.push_back(MenuItem::sub(L"Edge softness", soft));
    m.push_back(MenuItem::sub(L"Take out the green tint (spill)", spill));
    return m;
}

// --- the pointer highlight (0.12) ---

namespace {
const int kPtrSizes[] = {32, 48, 72, 100}, kPtrStrengths[] = {0, 20, 35, 55};
}

std::vector<MenuItem> CaptureWindow::pointerMenuItems(const SceneItem& item) {
    const pointer::Style& p = item.pointer;
    std::vector<MenuItem> m;
    m.push_back(MenuItem::item(L"Off", CmdPtrOff, {}, true, !p.on));
    m.push_back(MenuItem::item(L"On", CmdPtrOn, L"a halo round the pointer", true, p.on));
    m.push_back(MenuItem::item(L"Rings when you click", CmdPtrClicks, L"right clicks in the second colour", true, p.clicks));
    m.push_back(MenuItem::sep());
    std::vector<MenuItem> size, strength;
    static const wchar_t* sizeNames[] = {L"Small", L"Medium", L"Large", L"Very large"};
    static const wchar_t* strengthNames[] = {L"None (clicks only)", L"Faint", L"Medium", L"Strong"};
    for (int i = 0; i < 4; ++i) {
        size.push_back(MenuItem::item(sizeNames[i], CmdPtrSize0 + (UINT)i, {}, true, std::lround(p.size) == kPtrSizes[i]));
        strength.push_back(MenuItem::item(strengthNames[i], CmdPtrStrength0 + (UINT)i, {}, true, std::lround(p.strength) == kPtrStrengths[i]));
    }
    m.push_back(MenuItem::sub(L"Size", size));
    m.push_back(MenuItem::sub(L"Halo", strength));
    m.push_back(MenuItem::item(L"Colour…", CmdPtrColour, widen(sourceutil::colourText(p.colour))));
    m.push_back(MenuItem::item(L"Right-click colour…", CmdPtrSecond, widen(sourceutil::colourText(p.second))));
    return m;
}

bool CaptureWindow::pointerCommand(UINT cmd, const Id& itemId) {
    if (cmd < CmdPtrOff || cmd >= CmdPtrStrength0 + 4) return false;
    const Id scene = scenes_.current();
    const SceneItem* item = scenes_.item(scene, itemId);
    if (!item) return true;
    pointer::Style p = item->pointer;
    if (cmd == CmdPtrColour || cmd == CmdPtrSecond) {
        const bool second = cmd == CmdPtrSecond;
        askText(second ? L"Right-click colour (#RRGGBB)" : L"Pointer colour (#RRGGBB)", widen(sourceutil::colourText(second ? p.second : p.colour)),
                [this, scene, itemId, second](const std::wstring& v) {
                    sourceutil::Rgba c;
                    if (!sourceutil::parseColour(narrow(v), c)) {
                        flash(L"Type a colour like #FFD640.", 5);
                        return;
                    }
                    const SceneItem* it = scenes_.item(scene, itemId);
                    if (!it) return;
                    pointer::Style np = it->pointer;
                    (second ? np.second : np.colour) = {c.r, c.g, c.b, 255};
                    np.on = true;
                    scenes_.setItemPointer(scene, itemId, np);
                    scenesChanged();
                });
        return true;
    }
    if (cmd == CmdPtrOff) p.on = false;
    else if (cmd == CmdPtrOn) p.on = true;
    else if (cmd == CmdPtrClicks) p.clicks = !p.clicks, p.on = true;
    else if (cmd >= CmdPtrSize0 && cmd < CmdPtrSize0 + 4) p.size = (float)kPtrSizes[cmd - CmdPtrSize0], p.on = true;
    else if (cmd >= CmdPtrStrength0) p.strength = (float)kPtrStrengths[cmd - CmdPtrStrength0], p.on = true;
    scenes_.setItemPointer(scene, itemId, p);
    scenesChanged();
    return true;
}

bool CaptureWindow::keyCommand(UINT cmd, const Id& itemId) {
    if (cmd < CmdKeyOff || cmd >= CmdKeySpill0 + 5) return false;
    const SceneItem* item = scenes_.item(scenes_.current(), itemId);
    if (!item) return true;
    keying::Key k = item->key;
    const Id scene = scenes_.current();
    auto set = [&](const keying::Key& nk) {
        scenes_.setItemKey(scene, itemId, nk);
        scenesChanged();
    };
    if (cmd == CmdKeyOff) k.on = false;
    else if (cmd == CmdKeyGreen) k.on = true, k.colour = keying::kGreen;
    else if (cmd == CmdKeyBlue) k.on = true, k.colour = keying::kBlue;
    else if (cmd == CmdKeyPick) {
        keyPickItem_ = itemId;
        keyPickScene_ = scene;
        flash(L"Click your green screen in the preview (a spot without you in it). Esc to stop.", 8);
        return true;
    } else if (cmd == CmdKeyCustom) {
        askText(L"The screen's colour (#RRGGBB)", widen(sourceutil::colourText(k.colour)), [this, scene, itemId](const std::wstring& v) {
            sourceutil::Rgba c;
            if (!sourceutil::parseColour(narrow(v), c)) {
                flash(L"Type a colour like #00B140.", 5);
                return;
            }
            const SceneItem* it = scenes_.item(scene, itemId);
            if (!it) return;
            keying::Key nk = it->key;
            nk.on = true;
            nk.colour = {c.r, c.g, c.b, 255};
            scenes_.setItemKey(scene, itemId, nk);
            scenesChanged();
        });
        return true;
    } else if (cmd >= CmdKeySim0 && cmd < CmdKeySim0 + 5) k.similarity = (float)kSims[cmd - CmdKeySim0], k.on = true;
    else if (cmd >= CmdKeySoft0 && cmd < CmdKeySoft0 + 5) k.smoothness = (float)kSofts[cmd - CmdKeySoft0], k.on = true;
    else if (cmd >= CmdKeySpill0 && cmd < CmdKeySpill0 + 5) k.spill = (float)kSpills[cmd - CmdKeySpill0], k.on = true;
    set(k);
    return true;
}

// The click that picks: true if it was taken.
bool CaptureWindow::keyPickClick(float x, float y) {
    if (keyPickItem_.empty() || !previewInner_.contains(x, y)) return false;
    const Vec2 p = toCanvas(x, y);
    if (p.x < 0 || p.y < 0 || p.x >= scenes_.canvasWidth || p.y >= scenes_.canvasHeight) return true;
    const SceneItem* it = scenes_.item(keyPickScene_, keyPickItem_);
    if (!it) {
        keyPickItem_.clear();
        return true;
    }
    // The key off while the pixel is read (so it's the screen's own colour).
    keyPickWas_ = it->key;
    keying::Key off = it->key;
    off.on = false;
    scenes_.setItemKey(keyPickScene_, keyPickItem_, off);
    scenesChanged();
    gPick.x = (int)p.x, gPick.y = (int)p.y;
    gPick.done = false;
    gPick.skip = 4;
    engine_.setFrameSink("eyedropper", [](ID3D11Texture2D* canvas, LONGLONG) {
        if (gPick.done || gPick.skip-- > 0) return;
        ComPtr<ID3D11Device> dev;
        canvas->GetDevice(&dev);
        ComPtr<ID3D11DeviceContext> ctx;
        dev->GetImmediateContext(&ctx);
        D3D11_TEXTURE2D_DESC d{};
        canvas->GetDesc(&d);
        if ((UINT)gPick.x >= d.Width || (UINT)gPick.y >= d.Height) {
            gPick.done = true;
            return;
        }
        D3D11_TEXTURE2D_DESC s{};
        s.Width = s.Height = 1;
        s.MipLevels = s.ArraySize = 1;
        s.Format = d.Format;
        s.SampleDesc.Count = 1;
        s.Usage = D3D11_USAGE_STAGING;
        s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> st;
        if (FAILED(dev->CreateTexture2D(&s, nullptr, &st))) return;
        const D3D11_BOX box{(UINT)gPick.x, (UINT)gPick.y, 0, (UINT)gPick.x + 1, (UINT)gPick.y + 1, 1};
        ctx->CopySubresourceRegion(st.Get(), 0, 0, 0, 0, canvas, 0, &box);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
        gPick.bgra = *reinterpret_cast<const std::uint32_t*>(m.pData);
        ctx->Unmap(st.Get(), 0);
        gPick.done = true;
    });
    keyPickWaiting_ = true;
    return true;
}

// From the stats timer: a picked colour, or giving up after a few seconds.
void CaptureWindow::keyPickTick() {
    if (!keyPickWaiting_) return;
    if (++keyPickTicks_ > 8 && !gPick.done) gPick.done = true, gPick.bgra = 0xFFFFFFFF;   // no frame came
    if (!gPick.done) return;
    engine_.setFrameSink("eyedropper", nullptr);
    keyPickWaiting_ = false;
    keyPickTicks_ = 0;
    const std::uint32_t v = gPick.bgra;
    keying::Key k = keyPickWas_;
    if (v != 0xFFFFFFFF) {
        k.colour = {(std::uint8_t)(v >> 16), (std::uint8_t)(v >> 8), (std::uint8_t)v, 255};
        k.on = true;
        flash(L"Keying out " + widen(sourceutil::colourText(k.colour)) + L". Adjust How much is cleared in the item's Green screen menu if you need to.", 7);
        log::info("Green screen colour picked from the preview");
    } else {
        flash(L"The colour couldn't be read from the preview.", 5);
    }
    scenes_.setItemKey(keyPickScene_, keyPickItem_, k);
    scenesChanged();
    keyPickItem_.clear();
}

}  // namespace ncap
