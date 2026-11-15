// NorthRay Capture's window: a sound source's filters (system 7, "Audio Processing"), on a page of
// their own like Settings: each filter with its switch, its values on sliders (click a value to type
// it), its place in the order, and Remove; Add a filter; and starting points for a voice or a game.
#include "capture_window.hpp"

#include "log.hpp"
#include "audio/plugin_host.hpp"

#include "ncap/dsp.hpp"

#include "cjui/dialog.hpp"
#include "cjui/menu.hpp"

#include <algorithm>
#include <cmath>

namespace ncap {

using cjui::Align;
using cjui::Color;
using cjui::Font;
using cjui::MenuItem;
using cjui::narrow;
using cjui::Rect;
using cjui::widen;
using nlohmann::json;

namespace {

const wchar_t* kWhat[] = {
    L"Louder or quieter before anything else.",
    L"Silences the sound while it's quiet (breathing, a fan between words) and opens when you speak.",
    L"Lowers steady background noise (a fan, hiss, hum) while you talk. Adds about 11 ms of delay.",
    L"Makes quiet sound quieter still, more gently than a gate.",
    L"Evens out loud and quiet moments: sound above the level is turned down by the ratio.",
    L"A ceiling nothing goes over, so the stream never clips. Best last. Adds 1.5 ms of delay.",
    L"Bass, middle and treble.",
    L"Moves the sound to the left or right.",
    L"Holds the sound back, to line it up with a camera or capture card that's behind.",
};

dsp::Chain chainOf(const Source& s) {
    const json j = json::parse(s.settings, nullptr, false);
    return j.is_object() && j.contains("filters") ? dsp::fromJson(j["filters"].dump()) : dsp::Chain{};
}

std::wstring valueText(const dsp::Param& p, float v) {
    wchar_t b[48];
    const std::string unit = p.unit;
    if (unit == ":1") swprintf(b, 48, L"%.1f:1", v);
    else if (unit == "Hz") swprintf(b, 48, v >= 1000 ? L"%.1f kHz" : L"%.0f Hz", v >= 1000 ? v / 1000 : v);
    else if (unit == "%" && std::string(p.key) == "balance") {
        if (std::fabs(v) < 0.5f) swprintf(b, 48, L"centre");
        else swprintf(b, 48, L"%.0f%% %ls", std::fabs(v), v < 0 ? L"left" : L"right");
    } else if (unit == "dB" && std::string(p.key) == "strength") swprintf(b, 48, L"%.0f dB less", v);
    else if (unit == "dB") swprintf(b, 48, std::fabs(v) < 10 ? L"%+.1f dB" : L"%+.0f dB", v);
    else swprintf(b, 48, v < 10 ? L"%.1f %hs" : L"%.0f %hs", v, p.unit);
    return b;
}

}  // namespace

std::shared_ptr<dsp::Processor> CaptureWindow::filtersFor(const Source& s) {
    const json j = json::parse(s.settings, nullptr, false);
    const std::string text = j.is_object() && j.contains("filters") ? j["filters"].dump() : "[]";
    auto& slot = processors_[s.id];
    if (slot.first == text && (slot.second || text == "[]")) return slot.second;
    // A new chain: new running filters (the old ones finish their block on the mixer thread).
    const dsp::Chain chain = dsp::fromJson(text);
    slot.first = text;
    slot.second = chain.empty() ? nullptr : std::make_shared<dsp::Processor>(chain);
    return slot.second;
}

void CaptureWindow::openFilters(const Id& source) {
    if (!scenes_.source(source)) return;
    settingsOpen_ = true;
    filtersFor_ = source;
    resetScroll();
    invalidate();
}

void CaptureWindow::drawFilters(const Rect& area) {
    const Source* src = scenes_.source(filtersFor_);
    if (!src) return;
    const Id id = src->id;
    dsp::Chain chain = chainOf(*src);
    auto save = [this, id](const dsp::Chain& c) { setAudioSetting(id, "filters", json::parse(dsp::toJson(c))); };

    gfx_.fill(area, pal_.paper);
    const float colW = std::min(760.0f, area.w - 48);
    const float x = area.x + (area.w - colW) / 2;
    const float top = beginScroll(area);
    float y = top + 24;
    gfx_.text(L"Filters: " + widen(src->name), {x, y, colW - 130, 34}, Font::Title, pal_.ink, {Align::Left, false, true, true});
    button("flt:close", {x + colW - 110, y + 2, 110, 34}, L"Done", [this] { openSettings(false); }, true);
    y += 48;
    y += gfx_.text(L"Filters change this source's own sound, in order from the top, before its volume and where it's sent. The test "
                   L"clip in its menu records the result.",
                   {x, y, colW, 60}, Font::Small, pal_.inkMuted) + 16;

    // Starting points and adding.
    button("flt:add", {x, y, 170, 32}, L"Add a filter", [this, chain, save]() mutable {
        std::vector<MenuItem> items;
        for (int t = 0; t < dsp::kTypes; ++t) items.push_back(MenuItem::item(widen(dsp::label((dsp::Type)t)), 100 + (UINT)t));
        // Plugins the user has switched on.
        std::vector<std::string> plugs;
        for (auto& e : PluginHost::get().entries())
            if (e.enabled && e.loaded && plugs.size() < 50) plugs.push_back(e.file);
        std::vector<MenuItem> pm;
        for (std::size_t i = 0; i < plugs.size(); ++i) {
            const PluginEntry* e = PluginHost::get().entry(plugs[i]);
            pm.push_back(MenuItem::item(widen(e->info.name), 200 + (UINT)i, widen(e->info.author)));
        }
        if (pm.empty()) pm.push_back(MenuItem::item(L"None switched on (Settings \u203a Plugins)", 1, {}, false));
        items.push_back(MenuItem::sep());
        items.push_back(MenuItem::sub(L"Plugins", pm));
        POINT pt;
        GetCursorPos(&pt);
        const UINT cmd = cjui::showMenu(hwnd(), items, pt, pal_);
        const bool plugin = cmd >= 200 && cmd < 200 + plugs.size();
        if (!plugin && (cmd < 100 || cmd >= 100 + dsp::kTypes)) return;
        if (chain.size() >= 16) {
            flash(L"Sixteen filters is the most one source can have.", 5);
            return;
        }
        // A limiter belongs last; anything else goes before a limiter that's there.
        const dsp::Type t = plugin ? dsp::Type::Plugin : (dsp::Type)(cmd - 100);
        auto at = chain.end();
        if (t != dsp::Type::Limiter)
            for (auto it = chain.begin(); it != chain.end(); ++it)
                if (it->type == dsp::Type::Limiter) {
                    at = it;
                    break;
                }
        dsp::Filter nf = dsp::Filter::make(t);
        if (plugin) {
            nf.plugin = plugs[cmd - 200];
            if (const auto* ps = PluginHost::get().paramsOf(nf.plugin))
                for (auto& p : *ps) nf.values[p.key] = p.def;
        }
        chain.insert(at, nf);
        save(chain);
    });
    button("flt:preset", {x + 180, y, 220, 32}, L"Start from a preset", [this, chain, save]() mutable {
        const std::vector<MenuItem> items = {
            MenuItem::item(L"A microphone: clean and even", 1, L"suppression, gate, compressor, limiter"),
            MenuItem::item(L"Game or music: keep it in check", 2, L"gentle compressor, limiter"),
            MenuItem::item(L"Just a safety limiter", 3),
            MenuItem::sep(),
            MenuItem::item(L"Remove all the filters", 4, {}, !chain.empty()),
        };
        POINT pt;
        GetCursorPos(&pt);
        const UINT cmd = cjui::showMenu(hwnd(), items, pt, pal_);
        dsp::Chain c;
        if (cmd == 1) {
            c = {dsp::Filter::make(dsp::Type::NoiseSuppression), dsp::Filter::make(dsp::Type::NoiseGate), dsp::Filter::make(dsp::Type::Compressor),
                 dsp::Filter::make(dsp::Type::Limiter)};
            c[2].values["makeup"] = 4;
        } else if (cmd == 2) {
            c = {dsp::Filter::make(dsp::Type::Compressor), dsp::Filter::make(dsp::Type::Limiter)};
            c[0].values["threshold"] = -12;
            c[0].values["ratio"] = 2;
        } else if (cmd == 3) {
            c = {dsp::Filter::make(dsp::Type::Limiter)};
        } else if (cmd != 4) {
            return;
        }
        if (!chain.empty() && cmd != 4 &&
            cjui::messageBox(hwnd(), L"Replace this source's filters with the preset?", L"Filters", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
            return;
        save(c);
    });
    y += 48;

    if (chain.empty()) {
        y += gfx_.text(L"No filters yet. A microphone usually wants the microphone preset; anything can be adjusted afterwards.", {x, y, colW, 40},
                       Font::Body, pal_.inkMuted) + 12;
    }
    for (std::size_t i = 0; i < chain.size(); ++i) {
        const dsp::Filter f = chain[i];
        const std::string fid = "flt:" + std::to_string(i) + ":";
        static const std::vector<dsp::Param> kNone;
        const PluginEntry* pe = f.type == dsp::Type::Plugin ? PluginHost::get().entry(f.plugin) : nullptr;
        const std::vector<dsp::Param>* pp = f.type == dsp::Type::Plugin ? PluginHost::get().paramsOf(f.plugin) : nullptr;
        const auto& ps = f.type == dsp::Type::Plugin ? (pp ? *pp : kNone) : dsp::params(f.type);
        std::wstring what = f.type == dsp::Type::Plugin ? std::wstring() : std::wstring(kWhat[(int)f.type]);
        std::wstring title = widen(dsp::label(f.type));
        if (f.type == dsp::Type::Plugin) {
            title = L"Plugin: " + (pe && !pe->info.name.empty() ? widen(pe->info.name) : widen(f.plugin));
            if (pp)
                what = L"By " + widen(pe->info.author) + L", version " + widen(pe->info.version) + L" (" + widen(f.plugin) + L").";
            else
                what = L"Not running, so sound passes through unchanged: " +
                       (!pe ? std::wstring(L"its file isn't in the plugins folder.")
                        : !pe->enabled ? (pe->offReason.empty() ? std::wstring(L"it's switched off in Settings \u203a Plugins.") : widen(pe->offReason))
                                       : widen(pe->problem));
        }
        const float cardH = 66 + (float)ps.size() * 40;
        const Rect card{x, y, colW, cardH};
        gfx_.fillRounded(card, 10, pal_.surface);
        gfx_.strokeRounded(card, 10, f.enabled ? pal_.rule : pal_.rule.withAlpha(0.5f), 1);
        // Its name, switch, place and Remove.
        gfx_.text(std::to_wstring(i + 1) + L".  " + title, {x + 16, y + 10, colW - 330, 24}, Font::BodyStrong,
                  f.enabled ? pal_.ink : pal_.inkFaint, {Align::Left, false, true, true});
        toggle(fid + "on", x + colW - 300, y + 10, f.enabled, [chain, i, save]() mutable {
            chain[i].enabled = !chain[i].enabled;
            save(chain);
        });
        button(fid + "up", {x + colW - 240, y + 8, 40, 28}, L"↑", [chain, i, save]() mutable {
            if (i == 0) return;
            std::swap(chain[i], chain[i - 1]);
            save(chain);
        });
        button(fid + "down", {x + colW - 194, y + 8, 40, 28}, L"↓", [chain, i, save]() mutable {
            if (i + 1 >= chain.size()) return;
            std::swap(chain[i], chain[i + 1]);
            save(chain);
        });
        button(fid + "del", {x + colW - 144, y + 8, 128, 28}, L"Remove", [chain, i, save]() mutable {
            chain.erase(chain.begin() + (long)i);
            save(chain);
        });
        gfx_.text(what, {x + 16, y + 36, colW - 32, 20}, Font::Small, f.type == dsp::Type::Plugin && !pp ? cjui::hexColor("#E0566A") : pal_.inkMuted,
                  {Align::Left, false, true, true});
        float py = y + 64;
        for (const dsp::Param& p : ps) {
            const float v = std::clamp(f.values.count(p.key) ? f.values.at(p.key) : p.def, p.min, p.max);
            gfx_.text(widen(p.label), {x + 16, py, 160, 28}, Font::Body, pal_.ink, {Align::Left, false, true, true});
            const float frac = (v - p.min) / (p.max - p.min);
            slider(fid + p.key, {x + 180, py + 6, colW - 340, 16}, frac, [chain, i, key = std::string(p.key), p, save](float t) mutable {
                float nv = p.min + std::clamp(t, 0.0f, 1.0f) * (p.max - p.min);
                // Round to what the unit shows.
                const float step = std::string(p.unit) == "Hz" ? 10.0f : std::string(p.unit) == ":1" ? 0.1f : (p.max - p.min) > 100 ? 1.0f : 0.5f;
                nv = std::round(nv / step) * step;
                if (std::fabs(chain[i].get(key) - nv) < 1e-4f) return;
                chain[i].values[key] = nv;
                save(chain);
            });
            const Rect vr{x + colW - 150, py, 134, 28};
            const std::string vid = fid + p.key + ":v";
            addHit(vid, vr, [this, chain, i, key = std::string(p.key), p, save]() mutable {
                wchar_t now[32];
                swprintf(now, 32, L"%g", chain[i].get(key));
                askText(widen(p.label) + L" (" + widen(std::to_string((int)p.min)) + L" to " + widen(std::to_string((int)p.max)) + L" " + widen(p.unit) + L")",
                        now, [chain, i, key, p, save](const std::wstring& t) mutable {
                            wchar_t* end = nullptr;
                            const float nv = std::wcstof(t.c_str(), &end);
                            if (end == t.c_str() || !std::isfinite(nv)) return;
                            chain[i].values[key] = std::clamp(nv, p.min, p.max);
                            save(chain);
                        });
            });
            const float hv = hoverAmount(vid);
            if (hv > 0) gfx_.fillRounded(vr, 6, pal_.ink.withAlpha(0.06f * hv));
            gfx_.text(valueText(p, v), vr, Font::Body, pal_.ink, {Align::Right, false, true, true});
            py += 40;
        }
        y += cardH + 12;
    }
    if (!chain.empty()) {
        const auto running = filtersFor(*src);   // the chain running now (made once, not per frame)
        const int lat = running ? running->latencyFrames() : 0;
        if (lat > 0)
            y += gfx_.text(L"Together these hold this source back by " + std::to_wstring((lat * 1000 + 24000) / 48000) +
                               L" ms. Sources without them aren't delayed, so a microphone could be a little behind a game.",
                           {x, y, colW, 40}, Font::Small, pal_.inkMuted) + 8;
    }
    endScroll(y - top + 24);
}

}  // namespace ncap
